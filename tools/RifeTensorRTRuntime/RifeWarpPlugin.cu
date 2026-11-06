#include "RifeWarpPlugin.h"

#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>

namespace mpcvr_rife_warp {
using namespace nvinfer1;
constexpr char kName[] = "MpcvrRifeWarpConcat";
constexpr char kVersion[] = "4";
constexpr char kNamespace[] = "mpcvr";

// RIFE 4.25 Lite's refinement stages rebuild full-resolution flow and state from
// each block's low-resolution output, warp both images and their features at every
// pixel, then mostly shrink the result again. These kernels evaluate the same graph
// operations only at the pixels the next block reads, directly from those outputs.
//
// Stage -1 feeds block 0: inputs raw NCHW [1,11,H,W] and features [2,4,H,W]; output
//   [1,15,H/32,W/32], the graph's input concat after its half_pixel linear Resize.
// Stages 0-2 feed blocks 1-3: inputs raw, features and block outputs [1,13,H/s,W/s]
//   for s = 32, 16, 8, 4 up to this stage; output [1,28,H/o,W/o] for o = 16, 8, 4.
// Stage 3 feeds block 4: inputs as above; output [1,28,H,W] in kHWC8, the format
//   block 4's first convolution reads, which also carries the accumulated flow.
// Stage 4 finishes the frame: inputs raw, stage 3's output and block 4's ConvTranspose
//   [1,20,H/2,W/2] in kHWC8, pruned to the channels used; output [1,3,H,W].
constexpr int kInputStage = -1, kFullStage = 3, kTailStage = 4;

constexpr bool ValidStage(int32_t stage) noexcept { return stage >= kInputStage && stage <= kTailStage; }
constexpr int32_t InputCount(int32_t stage) noexcept { return stage == kInputStage ? 2 : stage == kTailStage ? 3 : 3 + stage; }
constexpr int32_t OutputScale(int32_t stage) noexcept { return stage == kInputStage ? 32 : stage >= kFullStage ? 1 : 16 >> stage; }
constexpr int32_t OutputChannels(int32_t stage) noexcept { return stage == kInputStage ? 15 : stage == kTailStage ? 3 : 28; }
constexpr int kFullPixelChannels = 32, kTailLowPixelChannels = 24, kTailLowChannels = 20;
__host__ __device__ constexpr int32_t LevelScale(int32_t level) noexcept { return 32 >> level; }

struct LowLevels { const __half* level[4]; };

struct LowSample { int x0, x1, y0, y1; float fx, fy; };

// half_pixel coordinates, clamped at the border: x_low = (x + 0.5) / scale - 0.5.
__device__ __forceinline__ LowSample LocateLow(int x, int y, int scale, int lowWidth, int lowHeight)
{
    const float lx = fminf(fmaxf((x + 0.5f) / scale - 0.5f, 0.0f), float(lowWidth - 1));
    const float ly = fminf(fmaxf((y + 0.5f) / scale - 0.5f, 0.0f), float(lowHeight - 1));
    const int x0 = int(lx), y0 = int(ly);
    return {x0, min(x0 + 1, lowWidth - 1), y0, min(y0 + 1, lowHeight - 1), lx - x0, ly - y0};
}

__device__ __forceinline__ float ReadLow(const __half* low, int channel, int lowWidth, int lowHeight, const LowSample& s)
{
    const __half* plane = low + size_t(channel) * lowHeight * lowWidth;
    const __half* top = plane + size_t(s.y0) * lowWidth, *bottom = plane + size_t(s.y1) * lowWidth;
    return (1 - s.fy) * ((1 - s.fx) * __half2float(top[s.x0]) + s.fx * __half2float(top[s.x1]))
        + s.fy * ((1 - s.fx) * __half2float(bottom[s.x0]) + s.fx * __half2float(bottom[s.x1]));
}

// Accumulated flow for one batch, keeping the graph's FP16 rounding after each
// upsample and each addition. Each level's multiply by its scale is exact.
template <int Stage>
__device__ __forceinline__ void FlowAt(const LowLevels& levels, int batch, int x, int y, int height, int width,
    __half& flowX, __half& flowY)
{
#pragma unroll
    for (int level = 0; level <= Stage; ++level) {
        const int scale = LevelScale(level), lowWidth = width / scale, lowHeight = height / scale;
        const LowSample s = LocateLow(x, y, scale, lowWidth, lowHeight);
        const float upX = __half2float(__float2half_rn(ReadLow(levels.level[level], batch * 2, lowWidth, lowHeight, s)));
        const float upY = __half2float(__float2half_rn(ReadLow(levels.level[level], batch * 2 + 1, lowWidth, lowHeight, s)));
        const __half scaledX = __float2half_rn(upX * scale), scaledY = __float2half_rn(upY * scale);
        flowX = level ? __hadd(flowX, scaledX) : scaledX;
        flowY = level ? __hadd(flowY, scaledY) : scaledY;
    }
}

// Add one pixel's bilinear border samples to sum. Keeping both FP16
// coordinate-rounding boundaries matters at large widths.
__device__ __forceinline__ void AddWarpedPixel(const __half* raw, const __half* features, __half flowX, __half flowY,
    int batch, int x, int y, int height, int width, float* sum)
{
    const size_t plane = size_t(height) * width;
    const size_t pixel = size_t(y) * width + x;
    const float gx = __half2float(__hadd(raw[7 * plane + pixel], __hmul(flowX, raw[9 * plane + pixel])));
    const float gy = __half2float(__hadd(raw[8 * plane + pixel], __hmul(flowY, raw[10 * plane + pixel])));
    const float u = fminf(fmaxf((gx + 1.0f) * 0.5f * (width - 1), 0.0f), float(width - 1));
    const float v = fminf(fmaxf((gy + 1.0f) * 0.5f * (height - 1), 0.0f), float(height - 1));
    const int x0 = int(u), y0 = int(v), x1 = min(x0 + 1, width - 1), y1 = min(y0 + 1, height - 1);
    const float fx = u - x0, fy = v - y0;
    const float a = (1 - fx) * (1 - fy), b = fx * (1 - fy), c = (1 - fx) * fy, d = fx * fy;
#pragma unroll
    for (int channel = 0; channel < 7; ++channel) {
        const __half* source = channel < 3 ? raw + (batch * 3 + channel) * plane
            : features + (batch * 4 + channel - 3) * plane;
        sum[channel] += a * __half2float(source[size_t(y0) * width + x0])
            + b * __half2float(source[size_t(y0) * width + x1])
            + c * __half2float(source[size_t(y1) * width + x0])
            + d * __half2float(source[size_t(y1) * width + x1]);
    }
}

// Eight FP16 values as one 16-byte store. Writing a pixel's channels one at a time
// costs about four times as much in this interleaved format.
__device__ __forceinline__ uint4 Pack8(const __half* v)
{
    const auto bits = [](__half a, __half b) { return unsigned(__half_as_ushort(a)) | unsigned(__half_as_ushort(b)) << 16; };
    return make_uint4(bits(v[0], v[1]), bits(v[2], v[3]), bits(v[4], v[5]), bits(v[6], v[7]));
}

// Stage 3: every pixel, no downscale. Output is kHWC8, 28 channels padded to 32.
__global__ void WarpFull(const __half* raw, const __half* features, LowLevels levels, __half* output, int height, int width)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x >= width) return;
    const int y = blockIdx.y;
    const size_t plane = size_t(height) * width;
    const size_t pixel = size_t(y) * width + x;
    __half out[kFullPixelChannels];
#pragma unroll
    for (int batch = 0; batch < 2; ++batch) {
        __half flowX, flowY;
        FlowAt<3>(levels, batch, x, y, height, width, flowX, flowY);
        float warped[7] = {};
        AddWarpedPixel(raw, features, flowX, flowY, batch, x, y, height, width, warped);
#pragma unroll
        for (int channel = 0; channel < 7; ++channel) out[batch * 7 + channel] = __float2half_rn(warped[channel]);
        out[24 + batch * 2] = flowX;
        out[25 + batch * 2] = flowY;
    }
    out[14] = raw[6 * plane + pixel];
    const int lowWidth = width / 4, lowHeight = height / 4;
    const LowSample s = LocateLow(x, y, 4, lowWidth, lowHeight);
#pragma unroll
    for (int channel = 0; channel < 9; ++channel) {
        out[15 + channel] = __float2half_rn(ReadLow(levels.level[3], 4 + channel, lowWidth, lowHeight, s));
    }
#pragma unroll
    for (int channel = 28; channel < kFullPixelChannels; ++channel) out[channel] = __float2half_rn(0.0f);
    auto* target = reinterpret_cast<uint4*>(output + pixel * kFullPixelChannels);
#pragma unroll
    for (int group = 0; group < 4; ++group) target[group] = Pack8(out + group * 8);
}

// Stage 4: block 4's DepthToSpace (CRD, factor 2) at the channels used, its flow
// residual, the final warp of both images and the mask blend, rounding as the graph does.
__global__ void FinishFrame(const __half* raw, const __half* full, const __half* low, __half* output, int height, int width)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x >= width) return;
    const int y = blockIdx.y;
    const size_t plane = size_t(height) * width;
    const size_t pixel = size_t(y) * width + x;
    const __half* lowPixel = low + (size_t(y / 2) * (width / 2) + x / 2) * kTailLowPixelChannels + (y & 1) * 2 + (x & 1);
    const __half* fullPixel = full + pixel * kFullPixelChannels;
    const __half gridX = raw[7 * plane + pixel], gridY = raw[8 * plane + pixel];
    const __half scaleX = raw[9 * plane + pixel], scaleY = raw[10 * plane + pixel];
    float warped[2][3];
    __half result[3];
#pragma unroll
    for (int batch = 0; batch < 2; ++batch) {
        const __half flowX = __hadd(fullPixel[24 + batch * 2], lowPixel[batch * 8]);
        const __half flowY = __hadd(fullPixel[25 + batch * 2], lowPixel[batch * 8 + 4]);
        const float gx = __half2float(__hadd(gridX, __hmul(flowX, scaleX)));
        const float gy = __half2float(__hadd(gridY, __hmul(flowY, scaleY)));
        const float u = fminf(fmaxf((gx + 1.0f) * 0.5f * (width - 1), 0.0f), float(width - 1));
        const float v = fminf(fmaxf((gy + 1.0f) * 0.5f * (height - 1), 0.0f), float(height - 1));
        const int x0 = int(u), y0 = int(v), x1 = min(x0 + 1, width - 1), y1 = min(y0 + 1, height - 1);
        const float fx = u - x0, fy = v - y0;
        const float a = (1 - fx) * (1 - fy), b = fx * (1 - fy), c = (1 - fx) * fy, d = fx * fy;
#pragma unroll
        for (int channel = 0; channel < 3; ++channel) {
            const __half* source = raw + (batch * 3 + channel) * plane;
            warped[batch][channel] = __half2float(__float2half_rn(a * __half2float(source[size_t(y0) * width + x0])
                + b * __half2float(source[size_t(y0) * width + x1])
                + c * __half2float(source[size_t(y1) * width + x0])
                + d * __half2float(source[size_t(y1) * width + x1])));
        }
    }
    const float mask = __half2float(__float2half_rn(1.0f / (1.0f + expf(-__half2float(lowPixel[16])))));
    const float inverse = __half2float(__float2half_rn(1.0f - mask));
#pragma unroll
    for (int channel = 0; channel < 3; ++channel) {
        const float first = __half2float(__float2half_rn(warped[0][channel] * mask));
        const float second = __half2float(__float2half_rn(warped[1][channel] * inverse));
        result[channel] = __float2half_rn(first + second);
    }
    output[pixel] = result[0];
    output[plane + pixel] = result[1];
    output[2 * plane + pixel] = result[2];
}

// Each batch also writes part of the unwarped tail, to keep both busy: batch 0 the
// timestep plane and state 0-4, batch 1 state 5-8, and each its own two flow channels.
constexpr int kStatePerBatch0 = 5;

// Stages 0-2: each output sample is the mean of the four pixels around its centre,
// which is what a half_pixel linear Resize at exactly 1/scale computes. Rounding once
// per output replaces rounding each warp and upsample and then the resize.
template <int Stage>
__global__ void WarpDown(const __half* raw, const __half* features, LowLevels levels, __half* output, int height, int width)
{
    const int scale = 16 >> Stage;
    const int outputWidth = width / scale;
    const int outputX = blockIdx.x * blockDim.x + threadIdx.x;
    if (outputX >= outputWidth) return;
    const int outputY = blockIdx.y;
    const int batch = blockIdx.z;
    const size_t plane = size_t(height) * width;
    const size_t outputPlane = size_t(height / scale) * outputWidth;
    const size_t outputPixel = size_t(outputY) * outputWidth + outputX;
    const int left = outputX * scale + scale / 2 - 1, top = outputY * scale + scale / 2 - 1;
    const int lowScale = LevelScale(Stage), lowWidth = width / lowScale, lowHeight = height / lowScale;
    const int first = batch ? kStatePerBatch0 : 0, count = batch ? 9 - kStatePerBatch0 : kStatePerBatch0;
    float warped[7] = {}, state[kStatePerBatch0] = {}, flow[2] = {}, timestep = 0;
#pragma unroll
    for (int offset = 0; offset < 4; ++offset) {
        const int x = left + (offset & 1), y = top + (offset >> 1);
        __half flowX, flowY;
        FlowAt<Stage>(levels, batch, x, y, height, width, flowX, flowY);
        AddWarpedPixel(raw, features, flowX, flowY, batch, x, y, height, width, warped);
        flow[0] += __half2float(flowX);
        flow[1] += __half2float(flowY);
        const LowSample s = LocateLow(x, y, lowScale, lowWidth, lowHeight);
        for (int channel = 0; channel < count; ++channel) {
            state[channel] += ReadLow(levels.level[Stage], 4 + first + channel, lowWidth, lowHeight, s);
        }
        if (!batch) timestep += __half2float(raw[6 * plane + size_t(y) * width + x]);
    }
#pragma unroll
    for (int channel = 0; channel < 7; ++channel) {
        output[(batch * 7 + channel) * outputPlane + outputPixel] = __float2half_rn(warped[channel] * 0.25f);
    }
    if (!batch) output[14 * outputPlane + outputPixel] = __float2half_rn(timestep * 0.25f);
    for (int channel = 0; channel < count; ++channel) {
        output[(15 + first + channel) * outputPlane + outputPixel] = __float2half_rn(state[channel] * 0.25f);
    }
    // The next block receives flow divided by this output's scale.
    const float toOutput = 0.25f / scale;
    output[(24 + batch * 2) * outputPlane + outputPixel] = __float2half_rn(flow[0] * toOutput);
    output[(25 + batch * 2) * outputPlane + outputPixel] = __float2half_rn(flow[1] * toOutput);
}

// Stage -1: images, features and timestep at 1/32, for block 0.
__global__ void DownscaleInput(const __half* raw, const __half* features, __half* output, int height, int width)
{
    constexpr int scale = 32;
    const int outputWidth = width / scale;
    const int outputX = blockIdx.x * blockDim.x + threadIdx.x;
    if (outputX >= outputWidth) return;
    const size_t plane = size_t(height) * width;
    const size_t outputPlane = size_t(height / scale) * outputWidth;
    const size_t outputPixel = size_t(blockIdx.y) * outputWidth + outputX;
    const int left = outputX * scale + scale / 2 - 1, top = blockIdx.y * scale + scale / 2 - 1;
    const size_t pixels[4]{size_t(top) * width + left, size_t(top) * width + left + 1,
        size_t(top + 1) * width + left, size_t(top + 1) * width + left + 1};
#pragma unroll
    for (int channel = 0; channel < 15; ++channel) {
        const __half* source = channel < 6 ? raw + channel * plane
            : channel < 14 ? features + (channel - 6) * plane : raw + 6 * plane;
        const float sum = __half2float(source[pixels[0]]) + __half2float(source[pixels[1]])
            + __half2float(source[pixels[2]]) + __half2float(source[pixels[3]]);
        output[channel * outputPlane + outputPixel] = __float2half_rn(sum * 0.25f);
    }
}

TensorFormat InputFormat(int32_t stage, int32_t input) noexcept
{
    return stage == kTailStage && input > 0 ? TensorFormat::kHWC8 : TensorFormat::kLINEAR;
}

TensorFormat OutputFormat(int32_t stage) noexcept
{
    return stage == kFullStage ? TensorFormat::kHWC8 : TensorFormat::kLINEAR;
}

bool ValidInputs(PluginTensorDesc const* input, int32_t count, int32_t stage) noexcept
{
    if (!input || count != InputCount(stage)) return false;
    const auto& shape = input[0].dims;
    // Every level must be an exact division, which padded RIFE sizes already are.
    if (shape.nbDims != 4 || shape.d[2] < 32 || shape.d[2] > 16384 || shape.d[3] < 32 || shape.d[3] > 16384
            || shape.d[2] % 32 != 0 || shape.d[3] % 32 != 0) return false;
    for (int i = 0; i < count; ++i) {
        const auto& desc = input[i];
        int scale = 1, batches = 1, channels = 11;
        if (stage == kTailStage) {
            if (i) { channels = i == 1 ? 28 : kTailLowChannels; scale = i == 1 ? 1 : 2; }
        } else if (i) {
            scale = i < 2 ? 1 : LevelScale(i - 2);
            batches = i == 1 ? 2 : 1;
            channels = i == 1 ? 4 : 13;
        }
        if (desc.type != DataType::kHALF || desc.format != InputFormat(stage, i)
                || desc.dims.nbDims != 4 || desc.dims.d[0] != batches || desc.dims.d[1] != channels
                || desc.dims.d[2] != shape.d[2] / scale || desc.dims.d[3] != shape.d[3] / scale) return false;
    }
    return true;
}

class WarpPlugin final : public IPluginV3, public IPluginV3OneCore,
    public IPluginV3OneBuild, public IPluginV3OneRuntime {
public:
    explicit WarpPlugin(int32_t stage) noexcept : m_stage(stage) {}
    IPluginCapability* getCapabilityInterface(PluginCapabilityType type) noexcept override {
        switch (type) {
        case PluginCapabilityType::kCORE: return static_cast<IPluginV3OneCore*>(this);
        case PluginCapabilityType::kBUILD: return static_cast<IPluginV3OneBuild*>(this);
        case PluginCapabilityType::kRUNTIME: return static_cast<IPluginV3OneRuntime*>(this);
        default: return nullptr;
        }
    }
    IPluginV3* clone() noexcept override { return new (std::nothrow) WarpPlugin(m_stage); }
    char const* getPluginName() const noexcept override { return kName; }
    char const* getPluginVersion() const noexcept override { return kVersion; }
    char const* getPluginNamespace() const noexcept override { return kNamespace; }
    int32_t getNbOutputs() const noexcept override { return 1; }
    int32_t configurePlugin(DynamicPluginTensorDesc const*, int32_t inputs,
        DynamicPluginTensorDesc const*, int32_t outputs) noexcept override {
        return inputs == InputCount(m_stage) && outputs == 1 ? 0 : -1;
    }
    int32_t getOutputDataTypes(DataType* output, int32_t outputs, DataType const* input,
        int32_t inputs) const noexcept override {
        if (inputs != InputCount(m_stage) || outputs != 1) return -1;
        for (int i = 0; i < inputs; ++i) if (input[i] != DataType::kHALF) return -1;
        for (int i = 0; i < outputs; ++i) output[i] = DataType::kHALF;
        return 0;
    }
    int32_t getOutputShapes(DimsExprs const* input, int32_t inputs, DimsExprs const*,
        int32_t shapeInputs, DimsExprs* output, int32_t outputs, IExprBuilder& builder) noexcept override {
        if (inputs != InputCount(m_stage) || outputs != 1 || shapeInputs || input[0].nbDims != 4) return -1;
        output[0] = input[0];
        output[0].d[1] = builder.constant(OutputChannels(m_stage));
        const int32_t outputScale = OutputScale(m_stage);
        if (outputScale > 1) {
            auto const* scale = builder.constant(outputScale);
            output[0].d[2] = builder.operation(DimensionOperation::kFLOOR_DIV, *input[0].d[2], *scale);
            output[0].d[3] = builder.operation(DimensionOperation::kFLOOR_DIV, *input[0].d[3], *scale);
        }
        return 0;
    }
    bool supportsFormatCombination(int32_t position, DynamicPluginTensorDesc const* io,
        int32_t inputs, int32_t outputs) noexcept override {
        return inputs == InputCount(m_stage) && outputs == 1
            && position >= 0 && position < inputs + outputs
            && io[position].desc.type == DataType::kHALF
            && io[position].desc.format == (position < inputs ? InputFormat(m_stage, position) : OutputFormat(m_stage));
    }
    int32_t onShapeChange(PluginTensorDesc const* input, int32_t inputs,
        PluginTensorDesc const* output, int32_t outputs) noexcept override {
        if (!ValidInputs(input, inputs, m_stage) || outputs != 1) return -1;
        const auto& source = input[0].dims;
        const auto& shape = output[0].dims;
        const int scale = OutputScale(m_stage);
        return output[0].type == DataType::kHALF && output[0].format == OutputFormat(m_stage)
            && shape.nbDims == 4 && shape.d[0] == 1 && shape.d[1] == OutputChannels(m_stage)
            && shape.d[2] == source.d[2] / scale && shape.d[3] == source.d[3] / scale ? 0 : -1;
    }
    int32_t enqueue(PluginTensorDesc const* desc, PluginTensorDesc const*, void const* const* input,
        void* const* output, void*, cudaStream_t stream) noexcept override {
        const int height = static_cast<int>(desc[0].dims.d[2]), width = static_cast<int>(desc[0].dims.d[3]);
        const auto* raw = static_cast<const __half*>(input[0]);
        const auto* features = static_cast<const __half*>(input[1]);
        LowLevels levels{};
        for (int level = 0; level <= m_stage && level < 4; ++level) levels.level[level] = static_cast<const __half*>(input[2 + level]);
        auto* result = static_cast<__half*>(output[0]);
        const int scale = OutputScale(m_stage);
        const dim3 blocks((unsigned(width / scale) + 127) / 128, unsigned(height / scale), m_stage >= 0 && m_stage < kFullStage ? 2 : 1);
        switch (m_stage) {
        case kInputStage: DownscaleInput<<<blocks, 128, 0, stream>>>(raw, features, result, height, width); break;
        case 0: WarpDown<0><<<blocks, 128, 0, stream>>>(raw, features, levels, result, height, width); break;
        case 1: WarpDown<1><<<blocks, 128, 0, stream>>>(raw, features, levels, result, height, width); break;
        case 2: WarpDown<2><<<blocks, 128, 0, stream>>>(raw, features, levels, result, height, width); break;
        case kFullStage: WarpFull<<<blocks, 128, 0, stream>>>(raw, features, levels, result, height, width); break;
        default: FinishFrame<<<blocks, 128, 0, stream>>>(raw, features, static_cast<const __half*>(input[2]), result, height, width); break;
        }
        return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
    }
    IPluginV3* attachToContext(IPluginResourceContext*) noexcept override { return clone(); }
    PluginFieldCollection const* getFieldsToSerialize() noexcept override { return &m_fields; }
private:
    int32_t m_stage;
    PluginField m_field{"stage", &m_stage, PluginFieldType::kINT32, 1};
    PluginFieldCollection m_fields{1, &m_field};
};

class WarpCreator final : public IPluginCreatorV3One {
public:
    IPluginV3* createPlugin(char const*, PluginFieldCollection const* fields, TensorRTPhase) noexcept override {
        if (!fields || fields->nbFields != 1 || std::string_view(fields->fields[0].name) != "stage"
                || fields->fields[0].type != PluginFieldType::kINT32 || fields->fields[0].length != 1) return nullptr;
        const int32_t stage = *static_cast<const int32_t*>(fields->fields[0].data);
        return ValidStage(stage) ? new (std::nothrow) WarpPlugin(stage) : nullptr;
    }
    PluginFieldCollection const* getFieldNames() noexcept override {
        static PluginField const field{"stage", nullptr, PluginFieldType::kINT32, 1};
        static PluginFieldCollection const fields{1, &field};
        return &fields;
    }
    char const* getPluginName() const noexcept override { return kName; }
    char const* getPluginVersion() const noexcept override { return kVersion; }
    char const* getPluginNamespace() const noexcept override { return kNamespace; }
};

ILayer* FindLayer(INetworkDefinition& network, std::string_view name) noexcept
{
    for (int i = 0; i < network.getNbLayers(); ++i) {
        auto* layer = network.getLayer(i);
        if (std::string_view(layer->getName()) == name) return layer;
    }
    return nullptr;
}

ITensor* FindTensor(INetworkDefinition& network, std::string_view name) noexcept
{
    for (int i = 0; i < network.getNbInputs(); ++i) {
        auto* tensor = network.getInput(i);
        if (tensor && tensor->getName() == name) return tensor;
    }
    for (int i = 0; i < network.getNbLayers(); ++i) {
        auto* layer = network.getLayer(i);
        for (int j = 0; j < layer->getNbOutputs(); ++j) {
            auto* tensor = layer->getOutput(j);
            if (tensor && tensor->getName() == name) return tensor;
        }
    }
    return nullptr;
}

void ReplaceTensor(INetworkDefinition& network, const ITensor* old, ITensor& replacement) noexcept
{
    for (int j = 0; j < network.getNbLayers(); ++j) {
        auto* layer = network.getLayer(j);
        for (int k = 0; k < layer->getNbInputs(); ++k) {
            if (layer->getInput(k) == old) layer->setInput(k, replacement);
        }
    }
}
} // namespace mpcvr_rife_warp

bool RegisterRifeWarpPlugin() noexcept
{
    using namespace mpcvr_rife_warp;
    // The renderer can unload a runtime DLL while TensorRT remains loaded.
    // Do not leave a creator pointing into an unloaded module in its registry.
    struct Registration {
        WarpCreator creator;
        bool registered = ::getPluginRegistry()->registerCreator(creator, kNamespace);
        ~Registration() {
            if (registered) ::getPluginRegistry()->deregisterCreator(creator);
        }
    };
    static Registration registration;
    return registration.registered;
}

bool FuseRifeSynthesisWarps(nvinfer1::INetworkDefinition& network, std::vector<uint16_t>& weightStorage) noexcept
{
    using namespace mpcvr_rife_warp;
    // Exact export names are safe only behind the model hash gate in
    // FeatureReuseEngine, whose padded sizes divide by every level here. Check the
    // complete match before rewriting anything. Layers this leaves without consumers
    // are removed by TensorRT when it builds the engine.
    constexpr std::array<const char*, 4> blockOutputs{
        "/block0/lastconv/lastconv.1/DepthToSpace_output_0", "/block1/lastconv/lastconv.1/DepthToSpace_output_0",
        "/block2/lastconv/lastconv.1/DepthToSpace_output_0", "/block3/lastconv/lastconv.1/DepthToSpace_output_0"};
    constexpr std::array<const char*, 5> replacedOutputs{"/block0/Resize_output_0", "/block1/Resize_output_0",
        "/block2/Resize_output_0", "/block3/Resize_output_0", "/Concat_5_output_0"};
    constexpr std::array<const char*, 6> pluginNames{"mpcvr_rife_input", "mpcvr_rife_warp0",
        "mpcvr_rife_warp1", "mpcvr_rife_warp2", "mpcvr_rife_warp3", "mpcvr_rife_finish"};
    auto* raw = FindTensor(network, "input");
    auto* features = FindTensor(network, "/encode/cnn3/ConvTranspose_output_0");
    if (!raw || !features || network.getNbOutputs() != 1) return false;
    std::array<ITensor*, 4> levels{};
    for (size_t i = 0; i < levels.size(); ++i) {
        levels[i] = FindTensor(network, blockOutputs[i]);
        if (!levels[i]) return false;
    }
    std::array<ITensor*, 5> replaced{};
    for (size_t i = 0; i < replaced.size(); ++i) {
        replaced[i] = FindTensor(network, replacedOutputs[i]);
        if (!replaced[i]) return false;
    }
    // Block 4's output has 13 channels at full resolution and the frame uses five:
    // flow and mask. DepthToSpace (CRD, factor 2) places channel c in inputs 4c to
    // 4c+3, so the first 20 of its ConvTranspose's 52 maps are the ones that matter.
    constexpr int kInputMaps = 24, kOutputMaps = 52, kKernel = 4;
    auto* deconv = FindLayer(network, "/block4/lastconv/lastconv.0/ConvTranspose");
    if (!deconv || deconv->getType() != LayerType::kDECONVOLUTION || deconv->getNbInputs() != 1) return false;
    auto* original = static_cast<IDeconvolutionLayer*>(deconv);
    const Weights kernel = original->getKernelWeights(), bias = original->getBiasWeights();
    const Dims kernelSize = original->getKernelSizeNd();
    // Weights are CKRS: input channel, output map, filter height and width.
    if (original->getNbOutputMaps() != kOutputMaps || original->getNbGroups() != 1 || kernelSize.nbDims != 2
            || kernelSize.d[0] != kKernel || kernelSize.d[1] != kKernel
            || kernel.type != DataType::kHALF || bias.type != DataType::kHALF || !kernel.values || !bias.values
            || kernel.count != kInputMaps * kOutputMaps * kKernel * kKernel || bias.count != kOutputMaps) return false;
    constexpr int kFilterValues = kKernel * kKernel;
    try {
        weightStorage.resize(size_t(kInputMaps) * kTailLowChannels * kFilterValues);
    } catch (...) { return false; }
    const auto* source = static_cast<const uint16_t*>(kernel.values);
    for (int input = 0; input < kInputMaps; ++input) {
        std::copy_n(source + size_t(input) * kOutputMaps * kFilterValues, size_t(kTailLowChannels) * kFilterValues,
            weightStorage.data() + size_t(input) * kTailLowChannels * kFilterValues);
    }
    const Weights prunedKernel{DataType::kHALF, weightStorage.data(), int64_t(weightStorage.size())};
    // The bias's first values are contiguous; the network holds its original.
    const Weights prunedBias{DataType::kHALF, bias.values, kTailLowChannels};
    auto* pruned = network.addDeconvolutionNd(*original->getInput(0), kTailLowChannels, kernelSize, prunedKernel, prunedBias);
    if (!pruned) return false;
    pruned->setName("mpcvr_rife_block4_lastconv");
    pruned->setStrideNd(original->getStrideNd());
    pruned->setPaddingNd(original->getPaddingNd());
    pruned->setPrePadding(original->getPrePadding());
    pruned->setPostPadding(original->getPostPadding());
    pruned->setDilationNd(original->getDilationNd());
    pruned->setPaddingMode(original->getPaddingMode());
    pruned->setNbGroups(1);
    ITensor* full = nullptr;
    for (int stage = kInputStage; stage <= kTailStage; ++stage) {
        ITensor* input[6]{raw, features, levels[0], levels[1], levels[2], levels[3]};
        if (stage == kTailStage) {
            input[1] = full;
            input[2] = pruned->getOutput(0);
        }
        WarpPlugin plugin(stage);
        auto* fused = network.addPluginV3(input, InputCount(stage), nullptr, 0, plugin);
        if (!fused) return false;
        // addPluginV3 clones the plugin; the network owns that clone.
        fused->setName(pluginNames[stage + 1]);
        if (stage < kTailStage) {
            ReplaceTensor(network, replaced[stage + 1], *fused->getOutput(0));
            if (stage == kFullStage) full = fused->getOutput(0);
        } else {
            // Callers bind the output by name, so the new tensor takes it over.
            auto* output = network.getOutput(0);
            const std::string name = output->getName();
            network.unmarkOutput(*output);
            output->setName("mpcvr_rife_replaced_output");
            fused->getOutput(0)->setName(name.c_str());
            network.markOutput(*fused->getOutput(0));
        }
    }
    return true;
}
