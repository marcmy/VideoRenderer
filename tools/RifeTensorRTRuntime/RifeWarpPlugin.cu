#include "RifeWarpPlugin.h"

#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <array>
#include <cstdint>
#include <new>
#include <string_view>

namespace mpcvr_rife_warp {
using namespace nvinfer1;
constexpr char kName[] = "MpcvrRifeWarpConcat";
constexpr char kVersion[] = "1";
constexpr char kNamespace[] = "mpcvr";

// Fuse four graph operations: normalized flow coordinates, bilinear border
// warp, batch-to-channel reshape and concatenation of the next block's input.
// Inputs are raw NCHW [1,11,H,W], encoded images [2,4,H,W], flow [1,4,H,W],
// recurrent state [1,9,H,W] and next-block flow [1,4,H,W]. Output is [1,28,H,W].
// Keeping both FP16 coordinate-rounding boundaries matters at large widths.
__global__ void WarpConcat(const __half* raw, const __half* features, const __half* flow,
    const __half* state, const __half* tail, __half* output, int height, int width)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x >= width) return;
    const int y = blockIdx.y;
    const int batch = blockIdx.z;
    const size_t plane = size_t(height) * width;
    const size_t pixel = size_t(y) * width + x;
    const float gx = __half2float(__hadd(raw[7 * plane + pixel],
        __hmul(flow[(batch * 2) * plane + pixel], raw[9 * plane + pixel])));
    const float gy = __half2float(__hadd(raw[8 * plane + pixel],
        __hmul(flow[(batch * 2 + 1) * plane + pixel], raw[10 * plane + pixel])));
    const float u = fminf(fmaxf((gx + 1.0f) * 0.5f * (width - 1), 0.0f), float(width - 1));
    const float v = fminf(fmaxf((gy + 1.0f) * 0.5f * (height - 1), 0.0f), float(height - 1));
    const int x0 = int(u), y0 = int(v), x1 = min(x0 + 1, width - 1), y1 = min(y0 + 1, height - 1);
    const float fx = u - x0, fy = v - y0;
    const float a = (1 - fx) * (1 - fy), b = fx * (1 - fy), c = (1 - fx) * fy, d = fx * fy;
#pragma unroll
    for (int channel = 0; channel < 7; ++channel) {
        const __half* source = channel < 3 ? raw + (batch * 3 + channel) * plane
            : features + (batch * 4 + channel - 3) * plane;
        const float value = a * __half2float(source[size_t(y0) * width + x0])
            + b * __half2float(source[size_t(y0) * width + x1])
            + c * __half2float(source[size_t(y1) * width + x0])
            + d * __half2float(source[size_t(y1) * width + x1]);
        output[(batch * 7 + channel) * plane + pixel] = __float2half_rn(value);
    }
    // Split the unwarped tail across the same two batches to keep both busy.
#pragma unroll
    for (int channel = 0; channel < 7; ++channel) {
        const int index = batch * 7 + channel;
        output[(14 + index) * plane + pixel] = index == 0 ? raw[6 * plane + pixel]
            : index < 10 ? state[(index - 1) * plane + pixel] : tail[(index - 10) * plane + pixel];
    }
}

bool ValidInputs(PluginTensorDesc const* input, int32_t count) noexcept
{
    if (!input || count != 5) return false;
    constexpr int batches[]{1, 2, 1, 1, 1}, channels[]{11, 4, 4, 9, 4};
    const auto& shape = input[0].dims;
    if (shape.nbDims != 4 || shape.d[2] < 1 || shape.d[2] > 16384
            || shape.d[3] < 1 || shape.d[3] > 16384) return false;
    for (int i = 0; i < count; ++i) {
        const auto& desc = input[i];
        if (desc.type != DataType::kHALF || desc.format != TensorFormat::kLINEAR
                || desc.dims.nbDims != 4 || desc.dims.d[0] != batches[i] || desc.dims.d[1] != channels[i]
                || desc.dims.d[2] != shape.d[2] || desc.dims.d[3] != shape.d[3]) return false;
    }
    return true;
}

class WarpPlugin final : public IPluginV3, public IPluginV3OneCore,
    public IPluginV3OneBuild, public IPluginV3OneRuntime {
public:
    IPluginCapability* getCapabilityInterface(PluginCapabilityType type) noexcept override {
        switch (type) {
        case PluginCapabilityType::kCORE: return static_cast<IPluginV3OneCore*>(this);
        case PluginCapabilityType::kBUILD: return static_cast<IPluginV3OneBuild*>(this);
        case PluginCapabilityType::kRUNTIME: return static_cast<IPluginV3OneRuntime*>(this);
        default: return nullptr;
        }
    }
    IPluginV3* clone() noexcept override { return new (std::nothrow) WarpPlugin; }
    char const* getPluginName() const noexcept override { return kName; }
    char const* getPluginVersion() const noexcept override { return kVersion; }
    char const* getPluginNamespace() const noexcept override { return kNamespace; }
    int32_t getNbOutputs() const noexcept override { return 1; }
    int32_t configurePlugin(DynamicPluginTensorDesc const*, int32_t inputs,
        DynamicPluginTensorDesc const*, int32_t outputs) noexcept override {
        return inputs == 5 && outputs == 1 ? 0 : -1;
    }
    int32_t getOutputDataTypes(DataType* output, int32_t outputs, DataType const* input,
        int32_t inputs) const noexcept override {
        if (inputs != 5 || outputs != 1) return -1;
        for (int i = 0; i < inputs; ++i) if (input[i] != DataType::kHALF) return -1;
        output[0] = DataType::kHALF;
        return 0;
    }
    int32_t getOutputShapes(DimsExprs const* input, int32_t inputs, DimsExprs const*,
        int32_t shapeInputs, DimsExprs* output, int32_t outputs, IExprBuilder& builder) noexcept override {
        if (inputs != 5 || outputs != 1 || shapeInputs || input[0].nbDims != 4) return -1;
        output[0] = input[0];
        output[0].d[1] = builder.constant(28);
        return 0;
    }
    bool supportsFormatCombination(int32_t position, DynamicPluginTensorDesc const* io,
        int32_t inputs, int32_t outputs) noexcept override {
        return inputs == 5 && outputs == 1 && position >= 0 && position < 6
            && io[position].desc.type == DataType::kHALF && io[position].desc.format == TensorFormat::kLINEAR;
    }
    int32_t onShapeChange(PluginTensorDesc const* input, int32_t inputs,
        PluginTensorDesc const* output, int32_t outputs) noexcept override {
        if (!ValidInputs(input, inputs) || outputs != 1) return -1;
        const auto& shape = output[0].dims;
        return output[0].type == DataType::kHALF && output[0].format == TensorFormat::kLINEAR
            && shape.nbDims == 4 && shape.d[0] == 1 && shape.d[1] == 28
            && shape.d[2] == input[0].dims.d[2] && shape.d[3] == input[0].dims.d[3] ? 0 : -1;
    }
    int32_t enqueue(PluginTensorDesc const* desc, PluginTensorDesc const*, void const* const* input,
        void* const* output, void*, cudaStream_t stream) noexcept override {
        const int height = static_cast<int>(desc[0].dims.d[2]), width = static_cast<int>(desc[0].dims.d[3]);
        WarpConcat<<<dim3((unsigned(width) + 127) / 128, unsigned(height), 2), 128, 0, stream>>>(
            static_cast<const __half*>(input[0]), static_cast<const __half*>(input[1]),
            static_cast<const __half*>(input[2]), static_cast<const __half*>(input[3]),
            static_cast<const __half*>(input[4]), static_cast<__half*>(output[0]), height, width);
        return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
    }
    IPluginV3* attachToContext(IPluginResourceContext*) noexcept override { return clone(); }
    PluginFieldCollection const* getFieldsToSerialize() noexcept override {
        static PluginFieldCollection const fields{0, nullptr};
        return &fields;
    }
};

class WarpCreator final : public IPluginCreatorV3One {
public:
    IPluginV3* createPlugin(char const*, PluginFieldCollection const*, TensorRTPhase) noexcept override {
        return new (std::nothrow) WarpPlugin;
    }
    PluginFieldCollection const* getFieldNames() noexcept override {
        static PluginFieldCollection const fields{0, nullptr};
        return &fields;
    }
    char const* getPluginName() const noexcept override { return kName; }
    char const* getPluginVersion() const noexcept override { return kVersion; }
    char const* getPluginNamespace() const noexcept override { return kNamespace; }
};

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

bool FuseRifeSynthesisWarps(nvinfer1::INetworkDefinition& network) noexcept
{
    using namespace mpcvr_rife_warp;
    // Exact export names are safe only behind the model hash gate in
    // FeatureReuseEngine. Check the complete match before rewriting anything.
    constexpr std::array<const char*, 4> concatNames{"/Concat_2", "/Concat_3", "/Concat_4", "/Concat_5"};
    constexpr std::array<const char*, 4> flowNames{
        "/block0/Mul_output_0", "/Add_1_output_0", "/Add_3_output_0", "/Add_5_output_0"};
    constexpr std::array<const char*, 4> pluginNames{
        "mpcvr_rife_warp0", "mpcvr_rife_warp1", "mpcvr_rife_warp2", "mpcvr_rife_warp3"};
    auto* raw = FindTensor(network, "input");
    auto* features = FindTensor(network, "/encode/cnn3/ConvTranspose_output_0");
    if (!raw || !features) return false;
    struct Match { ILayer* concat = nullptr; ITensor* flow = nullptr; };
    std::array<Match, 4> matches{};
    for (size_t i = 0; i < matches.size(); ++i) {
        matches[i].flow = FindTensor(network, flowNames[i]);
        for (int j = 0; j < network.getNbLayers(); ++j) {
            auto* layer = network.getLayer(j);
            if (std::string_view(layer->getName()) == concatNames[i]) matches[i].concat = layer;
        }
        const auto* concat = matches[i].concat;
        if (!matches[i].flow || !concat || concat->getType() != LayerType::kCONCATENATION
                || concat->getNbInputs() != 4 || concat->getNbOutputs() != 1) return false;
    }
    for (size_t i = 0; i < matches.size(); ++i) {
        auto* concat = matches[i].concat;
        ITensor* input[]{raw, features, matches[i].flow, concat->getInput(2), concat->getInput(3)};
        WarpPlugin plugin;
        auto* fused = network.addPluginV3(input, 5, nullptr, 0, plugin);
        if (!fused) return false;
        // addPluginV3 clones the plugin; the network owns that clone.
        fused->setName(pluginNames[i]);
        auto* old = concat->getOutput(0);
        for (int j = 0; j < network.getNbLayers(); ++j) {
            auto* layer = network.getLayer(j);
            for (int k = 0; k < layer->getNbInputs(); ++k) {
                if (layer->getInput(k) == old) layer->setInput(k, *fused->getOutput(0));
            }
        }
    }
    return true;
}
