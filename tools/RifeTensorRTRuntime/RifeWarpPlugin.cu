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
constexpr char kVersion[] = "5";
constexpr char kNamespace[] = "mpcvr";

// RIFE 4.25 Lite's refinement stages rebuild full-resolution flow and state from
// each block's low-resolution output, warp both images and their features at every
// pixel, then mostly shrink the result again. These kernels evaluate the same
// operations only at the pixels the next block reads, directly from those outputs.
//
// Arithmetic is FP32, and values are rounded only where they are stored. The graph's
// FP16 grid + flow * scale snaps coordinates to about half a pixel near the picture's
// sides; x + flow is what that expression means, and is what the model was trained on.
//
// Input 0 is the raw tensor [1,11,H,W], read only for the uniform timestep in its
// first pixel. Input 1 is the warp source, [2,H,W,8] as linear memory: one 16-byte
// texel per pixel per image holding r g b 0 f0 f1 f2 f3, which FeatureReuseEngine
// fills once per image. Loading a texel at once costs far less than one plane per value.
//
// Stage -1 feeds block 0: inputs raw and source; output [1,15,C/32,W/32], the graph's
//   input concat after its half_pixel linear Resize.
// Stages 0-2 feed blocks 1-3: inputs raw, source and block outputs [1,13,C/s,W/s] in
//   kHWC8 for s = 32, 16, 8, 4 up to this stage; output [1,28,C/o,W/o] in kHWC8 for
//   o = 16, 8, 4.
// Stage 3 feeds block 4: inputs as above; output 0 is [1,28,H,W] in kHWC8. Output 1 is
//   the accumulated flow, [1,H,W,4] as linear memory. At 64 bytes per pixel, reading
//   flow back from output 0 would touch every cache line of that tensor again.
// Stage 4 finishes the frame: inputs raw, source, stage 3's flow and block 4's
//   ConvTranspose [1,20,H/2,W/2] in kHWC8, pruned to the channels used; output [1,3,H,W].
//
// H may be any multiple of 64, but the coarse blocks 0-3 run at C, H rounded up to a
// multiple of 128, the height the model needs. They reach far enough for their flow to
// depend on where the picture ends, so a shorter picture would move flow everywhere.
// Those rows cost little there. Block 4 reaches only tens of pixels and most of the
// frame's time is spent in it and in stages 3 and 4, so those run at H. Rows from H to
// C read the last source row, which FeatureReuseEngine's padding keeps black.
constexpr int kInputStage = -1, kFullStage = 3, kTailStage = 4;

constexpr bool ValidStage(int32_t stage) noexcept { return stage >= kInputStage && stage <= kTailStage; }
constexpr int32_t InputCount(int32_t stage) noexcept { return stage == kInputStage ? 2 : stage == kTailStage ? 4 : 3 + stage; }
constexpr int32_t OutputScale(int32_t stage) noexcept { return stage == kInputStage ? 32 : stage >= kFullStage ? 1 : 16 >> stage; }
constexpr int32_t OutputChannels(int32_t stage) noexcept { return stage == kInputStage ? 15 : stage == kTailStage ? 3 : 28; }
constexpr int kFullPixelChannels = 32, kTailLowPixelChannels = 24, kTailLowChannels = 20;
__host__ __device__ constexpr int32_t LevelScale(int32_t level) noexcept { return 32 >> level; }
__host__ __device__ constexpr int CoarseHeight(int height) noexcept { return (height + 127) / 128 * 128; }

struct LowLevels { const unsigned* level[4]; };

struct LowSample { int x0, x1, y0, y1; float fx, fy; };

__device__ __forceinline__ float Half(unsigned bits) { return __half2float(__ushort_as_half(static_cast<unsigned short>(bits))); }
__device__ __forceinline__ float Channel(const uint4& v, int c)
{
    const unsigned w = c < 2 ? v.x : c < 4 ? v.y : c < 6 ? v.z : v.w;
    return Half(c & 1 ? w >> 16 : w & 0xffff);
}

// half_pixel coordinates, clamped at the border: x_low = (x + 0.5) / scale - 0.5.
__device__ __forceinline__ LowSample LocateLow(int x, int y, int scale, int lowWidth, int lowHeight)
{
    const float lx = fminf(fmaxf((x + 0.5f) / scale - 0.5f, 0.0f), float(lowWidth - 1));
    const float ly = fminf(fmaxf((y + 0.5f) / scale - 0.5f, 0.0f), float(lowHeight - 1));
    const int x0 = int(lx), y0 = int(ly);
    return {x0, min(x0 + 1, lowWidth - 1), y0, min(y0 + 1, lowHeight - 1), lx - x0, ly - y0};
}

// Block outputs are kHWC8: 13 channels padded to 16, 32 bytes per pixel.
constexpr int kLevelWordsPerPixel = 8;

__device__ __forceinline__ float Lerp2(float tl, float tr, float bl, float br, const LowSample& s)
{
    return (1 - s.fy) * ((1 - s.fx) * tl + s.fx * tr) + s.fy * ((1 - s.fx) * bl + s.fx * br);
}

// Accumulated flow for one batch, in full-resolution pixels.
template <int Stage>
__device__ __forceinline__ void FlowAt(const LowLevels& levels, int batch, int x, int y, int height, int width,
    float& flowX, float& flowY)
{
    flowX = 0;
    flowY = 0;
#pragma unroll
    for (int level = 0; level <= Stage; ++level) {
        const int scale = LevelScale(level), lowWidth = width / scale, lowHeight = CoarseHeight(height) / scale;
        const LowSample s = LocateLow(x, y, scale, lowWidth, lowHeight);
        const unsigned* low = levels.level[level] + batch;
        const unsigned tl = low[(size_t(s.y0) * lowWidth + s.x0) * kLevelWordsPerPixel];
        const unsigned tr = low[(size_t(s.y0) * lowWidth + s.x1) * kLevelWordsPerPixel];
        const unsigned bl = low[(size_t(s.y1) * lowWidth + s.x0) * kLevelWordsPerPixel];
        const unsigned br = low[(size_t(s.y1) * lowWidth + s.x1) * kLevelWordsPerPixel];
        flowX += scale * Lerp2(Half(tl & 0xffff), Half(tr & 0xffff), Half(bl & 0xffff), Half(br & 0xffff), s);
        flowY += scale * Lerp2(Half(tl >> 16), Half(tr >> 16), Half(bl >> 16), Half(br >> 16), s);
    }
}

// Add one pixel's bilinear border samples to sum.
// Source texels are r g b 0 f0 f1 f2 f3 per image.
__device__ __forceinline__ void AddWarpedPixel(const uint4* source, float flowX, float flowY,
    int batch, int x, int y, int height, int width, float* sum)
{
    const size_t plane = size_t(height) * width;
    const float u = fminf(fmaxf(x + flowX, 0.0f), float(width - 1));
    const float v = fminf(fmaxf(y + flowY, 0.0f), float(height - 1));
    const int x0 = int(u), y0 = int(v), x1 = min(x0 + 1, width - 1), y1 = min(y0 + 1, height - 1);
    const float fx = u - x0, fy = v - y0;
    const float a = (1 - fx) * (1 - fy), b = fx * (1 - fy), c = (1 - fx) * fy, d = fx * fy;
    const uint4* image = source + size_t(batch) * plane;
    const uint4 tl = image[size_t(y0) * width + x0], tr = image[size_t(y0) * width + x1];
    const uint4 bl = image[size_t(y1) * width + x0], br = image[size_t(y1) * width + x1];
#pragma unroll
    for (int channel = 0; channel < 7; ++channel) {
        const int slot = channel < 3 ? channel : channel + 1;
        sum[channel] += a * Channel(tl, slot) + b * Channel(tr, slot) + c * Channel(bl, slot) + d * Channel(br, slot);
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
__global__ void WarpFull(const uint4* source, LowLevels levels, const __half* timestep, __half* output, uint2* flows, int height, int width)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x >= width) return;
    const int y = blockIdx.y;
    const size_t pixel = size_t(y) * width + x;
    __half out[kFullPixelChannels];
#pragma unroll
    for (int batch = 0; batch < 2; ++batch) {
        float flowX, flowY;
        FlowAt<3>(levels, batch, x, y, height, width, flowX, flowY);
        float warped[7] = {};
        AddWarpedPixel(source, flowX, flowY, batch, x, y, height, width, warped);
#pragma unroll
        for (int channel = 0; channel < 7; ++channel) out[batch * 7 + channel] = __float2half_rn(warped[channel]);
        out[24 + batch * 2] = __float2half_rn(flowX);
        out[25 + batch * 2] = __float2half_rn(flowY);
    }
    out[14] = *timestep;
    const int lowWidth = width / 4, lowHeight = CoarseHeight(height) / 4;
    const LowSample s = LocateLow(x, y, 4, lowWidth, lowHeight);
    const auto* low = reinterpret_cast<const uint4*>(levels.level[3]);
    const uint4* t0 = low + (size_t(s.y0) * lowWidth + s.x0) * 2, *t1 = low + (size_t(s.y0) * lowWidth + s.x1) * 2;
    const uint4* b0 = low + (size_t(s.y1) * lowWidth + s.x0) * 2, *b1 = low + (size_t(s.y1) * lowWidth + s.x1) * 2;
    const uint4 tl0 = t0[0], tr0 = t1[0], bl0 = b0[0], br0 = b1[0], tl1 = t0[1], tr1 = t1[1], bl1 = b0[1], br1 = b1[1];
#pragma unroll
    for (int channel = 0; channel < 9; ++channel) {
        const int c = 4 + channel;
        const float value = c < 8
            ? Lerp2(Channel(tl0, c), Channel(tr0, c), Channel(bl0, c), Channel(br0, c), s)
            : Lerp2(Channel(tl1, c - 8), Channel(tr1, c - 8), Channel(bl1, c - 8), Channel(br1, c - 8), s);
        out[15 + channel] = __float2half_rn(value);
    }
#pragma unroll
    for (int channel = 28; channel < kFullPixelChannels; ++channel) out[channel] = __float2half_rn(0.0f);
    auto* target = reinterpret_cast<uint4*>(output + pixel * kFullPixelChannels);
#pragma unroll
    for (int group = 0; group < 4; ++group) target[group] = Pack8(out + group * 8);
    const auto bits = [](__half a, __half b) { return unsigned(__half_as_ushort(a)) | unsigned(__half_as_ushort(b)) << 16; };
    flows[pixel] = make_uint2(bits(out[24], out[25]), bits(out[26], out[27]));
}

// Stage 4: block 4's DepthToSpace (CRD, factor 2) at the channels used, its flow
// residual, the final warp of both images and the mask blend, rounding as the graph does.
__global__ void FinishFrame(const uint4* source, const uint2* flows, const __half* low, __half* output, int height, int width)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x >= width) return;
    const int y = blockIdx.y;
    const size_t plane = size_t(height) * width;
    const size_t pixel = size_t(y) * width + x;
    const __half* lowPixel = low + (size_t(y / 2) * (width / 2) + x / 2) * kTailLowPixelChannels + (y & 1) * 2 + (x & 1);
    const uint2 accumulated = flows[pixel];
    float warped[2][3];
    __half result[3];
#pragma unroll
    for (int batch = 0; batch < 2; ++batch) {
        const unsigned word = batch ? accumulated.y : accumulated.x;
        const float flowX = Half(word & 0xffff) + __half2float(lowPixel[batch * 8]);
        const float flowY = Half(word >> 16) + __half2float(lowPixel[batch * 8 + 4]);
        const float u = fminf(fmaxf(x + flowX, 0.0f), float(width - 1));
        const float v = fminf(fmaxf(y + flowY, 0.0f), float(height - 1));
        const int x0 = int(u), y0 = int(v), x1 = min(x0 + 1, width - 1), y1 = min(y0 + 1, height - 1);
        const float fx = u - x0, fy = v - y0;
        const float a = (1 - fx) * (1 - fy), b = fx * (1 - fy), c = (1 - fx) * fy, d = fx * fy;
        // Only the first eight bytes of each texel hold the image.
        const auto* image = reinterpret_cast<const uint2*>(source + size_t(batch) * plane);
        const uint2 tl = image[(size_t(y0) * width + x0) * 2], tr = image[(size_t(y0) * width + x1) * 2];
        const uint2 bl = image[(size_t(y1) * width + x0) * 2], br = image[(size_t(y1) * width + x1) * 2];
        const auto rgb = [](const uint2& t, int c) { return c == 0 ? Half(t.x & 0xffff) : c == 1 ? Half(t.x >> 16) : Half(t.y & 0xffff); };
#pragma unroll
        for (int channel = 0; channel < 3; ++channel) {
            warped[batch][channel] = a * rgb(tl, channel) + b * rgb(tr, channel) + c * rgb(bl, channel) + d * rgb(br, channel);
        }
    }
    const float mask = 1.0f / (1.0f + expf(-__half2float(lowPixel[16])));
#pragma unroll
    for (int channel = 0; channel < 3; ++channel) {
        result[channel] = __float2half_rn(warped[0][channel] * mask + warped[1][channel] * (1.0f - mask));
    }
    output[pixel] = result[0];
    output[plane + pixel] = result[1];
    output[2 * plane + pixel] = result[2];
}

// Stages 0-2: each output sample is the mean of the four pixels around its centre,
// which is what a half_pixel linear Resize at exactly 1/scale computes. Output is
// kHWC8, 28 channels padded to 32, written with 16-byte stores.
template <int Stage>
__global__ void WarpDown(const uint4* source, LowLevels levels, const __half* timestep, __half* output, int height, int width)
{
    const int scale = 16 >> Stage;
    const int outputWidth = width / scale;
    const int outputX = blockIdx.x * blockDim.x + threadIdx.x;
    if (outputX >= outputWidth) return;
    const int outputY = blockIdx.y;
    const size_t outputPixel = size_t(outputY) * outputWidth + outputX;
    const int left = outputX * scale + scale / 2 - 1, top = outputY * scale + scale / 2 - 1;
    const int lowScale = LevelScale(Stage), lowWidth = width / lowScale, lowHeight = CoarseHeight(height) / lowScale;
    float warped[2][7] = {}, flow[4] = {}, state[9] = {};
    const auto* low = reinterpret_cast<const uint4*>(levels.level[Stage]);
#pragma unroll
    for (int offset = 0; offset < 4; ++offset) {
        const int x = left + (offset & 1), y = top + (offset >> 1);
#pragma unroll
        for (int batch = 0; batch < 2; ++batch) {
            float flowX, flowY;
            FlowAt<Stage>(levels, batch, x, y, height, width, flowX, flowY);
            AddWarpedPixel(source, flowX, flowY, batch, x, y, height, width, warped[batch]);
            flow[batch * 2] += flowX;
            flow[batch * 2 + 1] += flowY;
        }
        const LowSample s = LocateLow(x, y, lowScale, lowWidth, lowHeight);
        const uint4* t0 = low + (size_t(s.y0) * lowWidth + s.x0) * 2, *t1 = low + (size_t(s.y0) * lowWidth + s.x1) * 2;
        const uint4* b0 = low + (size_t(s.y1) * lowWidth + s.x0) * 2, *b1 = low + (size_t(s.y1) * lowWidth + s.x1) * 2;
        const uint4 tl0 = t0[0], tr0 = t1[0], bl0 = b0[0], br0 = b1[0], tl1 = t0[1], tr1 = t1[1], bl1 = b0[1], br1 = b1[1];
#pragma unroll
        for (int channel = 0; channel < 9; ++channel) {
            const int c = 4 + channel;
            state[channel] += c < 8
                ? Lerp2(Channel(tl0, c), Channel(tr0, c), Channel(bl0, c), Channel(br0, c), s)
                : Lerp2(Channel(tl1, c - 8), Channel(tr1, c - 8), Channel(bl1, c - 8), Channel(br1, c - 8), s);
        }
    }
    __half out[kFullPixelChannels];
#pragma unroll
    for (int batch = 0; batch < 2; ++batch) {
#pragma unroll
        for (int channel = 0; channel < 7; ++channel) out[batch * 7 + channel] = __float2half_rn(warped[batch][channel] * 0.25f);
    }
    out[14] = *timestep;
#pragma unroll
    for (int channel = 0; channel < 9; ++channel) out[15 + channel] = __float2half_rn(state[channel] * 0.25f);
    // The next block receives flow divided by this output's scale.
    const float toOutput = 0.25f / scale;
#pragma unroll
    for (int channel = 0; channel < 4; ++channel) out[24 + channel] = __float2half_rn(flow[channel] * toOutput);
#pragma unroll
    for (int channel = 28; channel < kFullPixelChannels; ++channel) out[channel] = __float2half_rn(0.0f);
    auto* target = reinterpret_cast<uint4*>(output + outputPixel * kFullPixelChannels);
#pragma unroll
    for (int group = 0; group < 4; ++group) target[group] = Pack8(out + group * 8);
}

// Stage -1: images, features and timestep at 1/32, for block 0.
__global__ void DownscaleInput(const uint4* source, const __half* timestep, __half* output, int height, int width)
{
    constexpr int scale = 32;
    const int outputWidth = width / scale;
    const int outputX = blockIdx.x * blockDim.x + threadIdx.x;
    if (outputX >= outputWidth) return;
    const size_t plane = size_t(height) * width;
    const size_t outputPlane = size_t(CoarseHeight(height) / scale) * outputWidth;
    const size_t outputPixel = size_t(blockIdx.y) * outputWidth + outputX;
    const int left = outputX * scale + scale / 2 - 1, top = blockIdx.y * scale + scale / 2 - 1;
    const size_t row0 = size_t(min(top, height - 1)) * width, row1 = size_t(min(top + 1, height - 1)) * width;
    const size_t pixels[4]{row0 + left, row0 + left + 1, row1 + left, row1 + left + 1};
#pragma unroll
    for (int image = 0; image < 2; ++image) {
        const uint4* texels = source + size_t(image) * plane;
        const uint4 t0 = texels[pixels[0]], t1 = texels[pixels[1]], t2 = texels[pixels[2]], t3 = texels[pixels[3]];
#pragma unroll
        for (int slot = 0; slot < 8; ++slot) {
            if (slot == 3) continue;
            const float sum = Channel(t0, slot) + Channel(t1, slot) + Channel(t2, slot) + Channel(t3, slot);
            const int channel = slot < 3 ? image * 3 + slot : 6 + image * 4 + slot - 4;
            output[channel * outputPlane + outputPixel] = __float2half_rn(sum * 0.25f);
        }
    }
    output[14 * outputPlane + outputPixel] = *timestep;
}

TensorFormat InputFormat(int32_t stage, int32_t input) noexcept
{
    if (stage == kTailStage && input == 2) return TensorFormat::kLINEAR;
    return input >= 2 ? TensorFormat::kHWC8 : TensorFormat::kLINEAR;
}

TensorFormat OutputFormat(int32_t stage) noexcept
{
    return stage >= 0 && stage <= kFullStage ? TensorFormat::kHWC8 : TensorFormat::kLINEAR;
}

bool ValidInputs(PluginTensorDesc const* input, int32_t count, int32_t stage) noexcept
{
    if (!input || count != InputCount(stage)) return false;
    const auto& shape = input[0].dims;
    // Levels must divide exactly, in both directions.
    if (shape.nbDims != 4 || shape.d[2] < 64 || shape.d[2] > 16384 || shape.d[3] < 128 || shape.d[3] > 16384
            || shape.d[2] % 64 != 0 || shape.d[3] % 128 != 0) return false;
    for (int i = 0; i < count; ++i) {
        const auto& desc = input[i];
        if (desc.type != DataType::kHALF || desc.format != InputFormat(stage, i) || desc.dims.nbDims != 4) return false;
        if (i == 1) {
            if (desc.dims.d[0] != 2 || desc.dims.d[1] != shape.d[2] || desc.dims.d[2] != shape.d[3] || desc.dims.d[3] != 8) return false;
            continue;
        }
        if (stage == kTailStage && i == 2) {
            if (desc.dims.d[0] != 1 || desc.dims.d[1] != shape.d[2] || desc.dims.d[2] != shape.d[3] || desc.dims.d[3] != 4) return false;
            continue;
        }
        int scale = 1, channels = 11;
        if (stage == kTailStage) {
            if (i) { channels = kTailLowChannels; scale = 2; }
        } else if (i) {
            scale = LevelScale(i - 2);
            channels = 13;
        }
        const int64_t rows = (stage != kTailStage && i ? CoarseHeight(int(shape.d[2])) : shape.d[2]) / scale;
        if (desc.dims.d[0] != 1 || desc.dims.d[1] != channels
                || desc.dims.d[2] != rows || desc.dims.d[3] != shape.d[3] / scale) return false;
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
    int32_t getNbOutputs() const noexcept override { return m_stage == kFullStage ? 2 : 1; }
    int32_t configurePlugin(DynamicPluginTensorDesc const*, int32_t inputs,
        DynamicPluginTensorDesc const*, int32_t outputs) noexcept override {
        return inputs == InputCount(m_stage) && outputs == getNbOutputs() ? 0 : -1;
    }
    int32_t getOutputDataTypes(DataType* output, int32_t outputs, DataType const* input,
        int32_t inputs) const noexcept override {
        if (inputs != InputCount(m_stage) || outputs != getNbOutputs()) return -1;
        for (int i = 0; i < inputs; ++i) if (input[i] != DataType::kHALF) return -1;
        for (int i = 0; i < outputs; ++i) output[i] = DataType::kHALF;
        return 0;
    }
    int32_t getOutputShapes(DimsExprs const* input, int32_t inputs, DimsExprs const*,
        int32_t shapeInputs, DimsExprs* output, int32_t outputs, IExprBuilder& builder) noexcept override {
        if (inputs != InputCount(m_stage) || outputs != getNbOutputs() || shapeInputs || input[0].nbDims != 4) return -1;
        if (m_stage == kFullStage) {
            output[1].nbDims = 4;
            output[1].d[0] = builder.constant(1);
            output[1].d[1] = input[0].d[2];
            output[1].d[2] = input[0].d[3];
            output[1].d[3] = builder.constant(4);
        }
        output[0] = input[0];
        output[0].d[1] = builder.constant(OutputChannels(m_stage));
        const int32_t outputScale = OutputScale(m_stage);
        if (outputScale > 1) {
            auto const* scale = builder.constant(outputScale);
            // CoarseHeight(H) / scale, as an expression TensorRT can carry through the blocks.
            output[0].d[2] = builder.operation(DimensionOperation::kPROD,
                *builder.operation(DimensionOperation::kCEIL_DIV, *input[0].d[2], *builder.constant(128)), *builder.constant(128 / outputScale));
            output[0].d[3] = builder.operation(DimensionOperation::kFLOOR_DIV, *input[0].d[3], *scale);
        }
        return 0;
    }
    bool supportsFormatCombination(int32_t position, DynamicPluginTensorDesc const* io,
        int32_t inputs, int32_t outputs) noexcept override {
        return inputs == InputCount(m_stage) && outputs == getNbOutputs()
            && position >= 0 && position < inputs + outputs
            && io[position].desc.type == DataType::kHALF
            && io[position].desc.format == (position < inputs ? InputFormat(m_stage, position)
                : position == inputs + 1 ? TensorFormat::kLINEAR : OutputFormat(m_stage));
    }
    int32_t onShapeChange(PluginTensorDesc const* input, int32_t inputs,
        PluginTensorDesc const* output, int32_t outputs) noexcept override {
        if (!ValidInputs(input, inputs, m_stage) || outputs != getNbOutputs()) return -1;
        const auto& source = input[0].dims;
        const auto& shape = output[0].dims;
        const int scale = OutputScale(m_stage);
        if (m_stage == kFullStage) {
            const auto& flows = output[1].dims;
            if (output[1].type != DataType::kHALF || output[1].format != TensorFormat::kLINEAR || flows.nbDims != 4
                    || flows.d[0] != 1 || flows.d[1] != source.d[2] || flows.d[2] != source.d[3] || flows.d[3] != 4) return -1;
        }
        return output[0].type == DataType::kHALF && output[0].format == OutputFormat(m_stage)
            && shape.nbDims == 4 && shape.d[0] == 1 && shape.d[1] == OutputChannels(m_stage)
            && shape.d[2] == (scale > 1 ? CoarseHeight(int(source.d[2])) : source.d[2]) / scale && shape.d[3] == source.d[3] / scale ? 0 : -1;
    }
    int32_t enqueue(PluginTensorDesc const* desc, PluginTensorDesc const*, void const* const* input,
        void* const* output, void*, cudaStream_t stream) noexcept override {
        const int height = static_cast<int>(desc[0].dims.d[2]), width = static_cast<int>(desc[0].dims.d[3]);
        const auto* timestep = static_cast<const __half*>(input[0]) + 6 * size_t(height) * width;
        const auto* source = static_cast<const uint4*>(input[1]);
        LowLevels levels{};
        for (int level = 0; level <= m_stage && level < 4; ++level) levels.level[level] = static_cast<const unsigned*>(input[2 + level]);
        auto* result = static_cast<__half*>(output[0]);
        const int scale = OutputScale(m_stage);
        const dim3 blocks((unsigned(width / scale) + 127) / 128, unsigned((scale > 1 ? CoarseHeight(height) : height) / scale), 1);
        switch (m_stage) {
        case kInputStage: DownscaleInput<<<blocks, 128, 0, stream>>>(source, timestep, result, height, width); break;
        case 0: WarpDown<0><<<blocks, 128, 0, stream>>>(source, levels, timestep, result, height, width); break;
        case 1: WarpDown<1><<<blocks, 128, 0, stream>>>(source, levels, timestep, result, height, width); break;
        case 2: WarpDown<2><<<blocks, 128, 0, stream>>>(source, levels, timestep, result, height, width); break;
        case kFullStage: WarpFull<<<blocks, 128, 0, stream>>>(source, levels, timestep, result, static_cast<uint2*>(output[1]), height, width); break;
        default: FinishFrame<<<blocks, 128, 0, stream>>>(source, static_cast<const uint2*>(input[2]), static_cast<const __half*>(input[3]), result, height, width); break;
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
    // FeatureReuseEngine, which chooses a size this plugin accepts. Check the
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
    if (!raw || !FindTensor(network, "/encode/cnn3/ConvTranspose_output_0") || network.getNbOutputs() != 1) return false;
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
    auto* warpSource = network.addInput(kRifeWarpSourceName, DataType::kHALF, Dims4{2, -1, -1, 8});
    if (!warpSource) return false;
    ITensor* flows = nullptr;
    for (int stage = kInputStage; stage <= kTailStage; ++stage) {
        ITensor* input[6]{raw, warpSource, levels[0], levels[1], levels[2], levels[3]};
        if (stage == kTailStage) {
            input[2] = flows;
            input[3] = pruned->getOutput(0);
        }
        WarpPlugin plugin(stage);
        auto* fused = network.addPluginV3(input, InputCount(stage), nullptr, 0, plugin);
        if (!fused) return false;
        // addPluginV3 clones the plugin; the network owns that clone.
        fused->setName(pluginNames[stage + 1]);
        if (stage < kTailStage) {
            ReplaceTensor(network, replaced[stage + 1], *fused->getOutput(0));
            if (stage == kFullStage) flows = fused->getOutput(1);
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
