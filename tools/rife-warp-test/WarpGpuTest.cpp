#include "../RifeTensorRTRuntime/RifeWarpPlugin.h"
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace nvinfer1;
struct Logger : ILogger {
    void log(Severity severity, char const* message) noexcept override {
        if (severity <= Severity::kWARNING) std::cerr << message << '\n';
    }
} logger;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Cuda(cudaError_t result) { Check(result == cudaSuccess, cudaGetErrorString(result)); }
float Half(float value) { return __half2float(__float2half_rn(value)); }
struct DeviceBuffer {
    void* pointer = nullptr;
    explicit DeviceBuffer(size_t bytes) { Cuda(cudaMalloc(&pointer, bytes)); }
    ~DeviceBuffer() { cudaFree(pointer); }
};

constexpr int levelScales[]{32, 16, 8, 4};
constexpr int kTailStage = 4;
int InputCount(int stage) { return stage < 0 ? 2 : stage == kTailStage ? 3 : 3 + stage; }
int OutputScale(int stage) { return stage < 0 ? 32 : stage >= 3 ? 1 : 16 >> stage; }
int OutputChannels(int stage) { return stage < 0 ? 15 : stage == kTailStage ? 3 : 28; }
// [batch, channels, divisor] for each input position.
std::array<int, 3> InputLayout(int stage, int input)
{
    if (input == 0) return {1, 11, 1};
    if (stage == kTailStage) return input == 1 ? std::array<int, 3>{1, 28, 1} : std::array<int, 3>{1, 20, 2};
    if (input == 1) return {2, 4, 1};
    return {1, 13, levelScales[input - 2]};
}

// Independent CPU reference, written from the ONNX operator definitions.
struct Reference {
    int width, height, stage;
    const std::vector<std::vector<__half>>& host;
    float Raw(int channel, int x, int y) const { return __half2float(host[0][(size_t(channel) * height + y) * width + x]); }
    float Feature(int batch, int channel, int x, int y) const {
        return __half2float(host[1][((size_t(batch) * 4 + channel) * height + y) * width + x]);
    }
    // Resize, mode linear, half_pixel: x_original = (x_resized + 0.5) / scale_factor - 0.5.
    float Up(int level, int channel, int x, int y) const {
        const int factor = levelScales[level], lowWidth = width / factor, lowHeight = height / factor;
        const float lx = std::clamp((x + .5f) / factor - .5f, 0.0f, float(lowWidth - 1));
        const float ly = std::clamp((y + .5f) / factor - .5f, 0.0f, float(lowHeight - 1));
        const int x0 = int(lx), y0 = int(ly), x1 = std::min(x0 + 1, lowWidth - 1), y1 = std::min(y0 + 1, lowHeight - 1);
        const float fx = lx - x0, fy = ly - y0;
        const auto at = [&](int row, int col) {
            return __half2float(host[2 + level][(size_t(channel) * lowHeight + row) * lowWidth + col]);
        };
        return (1 - fy) * ((1 - fx) * at(y0, x0) + fx * at(y0, x1)) + fy * ((1 - fx) * at(y1, x0) + fx * at(y1, x1));
    }
    float Flow(int channel, int x, int y) const {
        float flow = 0;
        for (int level = 0; level <= stage; ++level) {
            const float scaled = Half(Half(Up(level, channel, x, y)) * levelScales[level]);
            flow = level ? Half(flow + scaled) : scaled;
        }
        return flow;
    }
    float Warped(int batch, int channel, int x, int y) const {
        const float gx = Half(Raw(7, x, y) + Half(Flow(batch * 2, x, y) * Raw(9, x, y)));
        const float gy = Half(Raw(8, x, y) + Half(Flow(batch * 2 + 1, x, y) * Raw(10, x, y)));
        const float u = std::clamp((gx + 1) * .5f * (width - 1), 0.0f, float(width - 1));
        const float v = std::clamp((gy + 1) * .5f * (height - 1), 0.0f, float(height - 1));
        const int x0 = int(u), y0 = int(v), x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
        const float fx = u - x0, fy = v - y0;
        const auto at = [&](int row, int col) {
            return channel < 3 ? Raw(batch * 3 + channel, col, row) : Feature(batch, channel - 3, col, row);
        };
        return Half((1 - fx) * (1 - fy) * at(y0, x0) + fx * (1 - fy) * at(y0, x1)
            + (1 - fx) * fy * at(y1, x0) + fx * fy * at(y1, x1));
    }
    // The graph's concat channel at one full-resolution pixel.
    float Stage(int channel, int x, int y) const {
        if (channel < 14) return Warped(channel / 7, channel % 7, x, y);
        if (channel == 14) return Raw(6, x, y);
        if (channel < 24) return Half(Up(stage, 4 + channel - 15, x, y));
        return Half(Flow(channel - 24, x, y) / OutputScale(stage));
    }
    float Input(int channel, int x, int y) const {
        if (channel < 6) return Raw(channel, x, y);
        if (channel < 14) return Feature((channel - 6) / 4, (channel - 6) % 4, x, y);
        return Raw(6, x, y);
    }
    // Stage 4 from the graph's operators: DepthToSpace with mode CRD and factor 2, Add,
    // Mul, GridSample of the two images, Sigmoid, Mul, Sub, Mul, Add. Input 1 is stage
    // 3's concat and input 2 the ConvTranspose, whose first 20 channels matter.
    float Finished(int channel, int x, int y) const {
        const auto full = [&](int c) { return __half2float(host[1][(size_t(c) * height + y) * width + x]); };
        const auto block = [&](int c) {
            const int lowWidth = width / 2, lowHeight = height / 2;
            const int mapped = c * 4 + (y % 2) * 2 + x % 2;
            return __half2float(host[2][(size_t(mapped) * lowHeight + y / 2) * lowWidth + x / 2]);
        };
        const auto warped = [&](int batch) {
            const float flowX = Half(full(24 + batch * 2) + block(batch * 2));
            const float flowY = Half(full(25 + batch * 2) + block(batch * 2 + 1));
            const float gx = Half(Raw(7, x, y) + Half(flowX * Raw(9, x, y)));
            const float gy = Half(Raw(8, x, y) + Half(flowY * Raw(10, x, y)));
            const float u = std::clamp((gx + 1) * .5f * (width - 1), 0.0f, float(width - 1));
            const float v = std::clamp((gy + 1) * .5f * (height - 1), 0.0f, float(height - 1));
            const int x0 = int(u), y0 = int(v), x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
            const float fx = u - x0, fy = v - y0;
            const auto at = [&](int row, int col) { return Raw(batch * 3 + channel, col, row); };
            return Half((1 - fx) * (1 - fy) * at(y0, x0) + fx * (1 - fy) * at(y0, x1)
                + (1 - fx) * fy * at(y1, x0) + fx * fy * at(y1, x1));
        };
        const float mask = Half(1 / (1 + std::exp(-block(4))));
        return Half(Half(warped(0) * mask) + Half(warped(1) * Half(1 - mask)));
    }
    // Resize at exactly 1/scale: x_original = (x_resized + 0.5) * scale - 0.5.
    float Down(const std::function<float(int, int)>& sample, int outputX, int outputY) const {
        const int scale = OutputScale(stage);
        if (scale == 1) return sample(outputX, outputY);
        const double cx = (outputX + .5) * scale - .5, cy = (outputY + .5) * scale - .5;
        const int x0 = int(std::floor(cx)), y0 = int(std::floor(cy));
        Check(cx - x0 == .5 && cy - y0 == .5 && x0 + 1 < width && y0 + 1 < height, "half_pixel centre");
        return Half(.25f * (sample(x0, y0) + sample(x0 + 1, y0) + sample(x0, y0 + 1) + sample(x0 + 1, y0 + 1)));
    }
};

void TestStage(const int stage, const std::vector<std::pair<int, int>>& sizes, cudaStream_t stream)
{
    std::unique_ptr<IBuilder> builder(createInferBuilder(logger));
    std::unique_ptr<INetworkDefinition> net(builder->createNetworkV2(0));
    std::unique_ptr<IBuilderConfig> config(builder->createBuilderConfig());
    auto* profile = builder->createOptimizationProfile();
    const int inputCount = InputCount(stage);
    std::vector<std::string> names;
    std::vector<ITensor*> inputs;
    for (int i = 0; i < inputCount; ++i) {
        const auto [batch, channels, divisor] = InputLayout(stage, i);
        names.push_back("input" + std::to_string(i));
        inputs.push_back(net->addInput(names[i].c_str(), DataType::kHALF, Dims4{batch, channels, -1, -1}));
        Check(profile->setDimensions(names[i].c_str(), OptProfileSelector::kMIN, Dims4{batch, channels, 32 / divisor, 32 / divisor}), "min");
        Check(profile->setDimensions(names[i].c_str(), OptProfileSelector::kOPT, Dims4{batch, channels, 1152 / divisor, 640 / divisor}), "opt");
        Check(profile->setDimensions(names[i].c_str(), OptProfileSelector::kMAX, Dims4{batch, channels, 1152 / divisor, 1920 / divisor}), "max");
    }
    // An unknown graph must fail the matcher without changing the network.
    const int layers = net->getNbLayers();
    std::vector<uint16_t> fusedWeights;
    Check(!FuseRifeSynthesisWarps(*net, fusedWeights) && net->getNbLayers() == layers, "unknown graph preserved");
    auto* creator = static_cast<IPluginCreatorV3One*>(getPluginRegistry()->getCreator("MpcvrRifeWarpConcat", "4", "mpcvr"));
    Check(creator != nullptr, "creator lookup");
    const int32_t invalidStage = 5;
    PluginField invalidField("stage", &invalidStage, PluginFieldType::kINT32, 1);
    PluginFieldCollection invalidFields{1, &invalidField};
    Check(!std::unique_ptr<IPluginV3>(creator->createPlugin("warp", &invalidFields, TensorRTPhase::kBUILD)), "invalid stage rejected");
    const int32_t stageValue = stage;
    PluginField field("stage", &stageValue, PluginFieldType::kINT32, 1);
    PluginFieldCollection fields{1, &field};
    std::unique_ptr<IPluginV3> plugin(creator->createPlugin("warp", &fields, TensorRTPhase::kBUILD));
    Check(plugin != nullptr, "plugin creation");
    auto* layer = net->addPluginV3(inputs.data(), inputCount, nullptr, 0, *plugin);
    Check(layer != nullptr, "plugin layer");
    Check(layer->getNbOutputs() == 1, "output count");
    // Inputs and outputs stay linear here, so TensorRT's own reformats check that the
    // plugin's interleaved inputs and outputs really have the layout they declare.
    layer->getOutput(0)->setName("output0");
    net->markOutput(*layer->getOutput(0));
    Check(config->addOptimizationProfile(profile) >= 0, "profile");
    std::unique_ptr<IHostMemory> plan(builder->buildSerializedNetwork(*net, *config));
    Check(plan != nullptr, "serialize plugin engine");
    std::unique_ptr<IRuntime> runtime(createInferRuntime(logger));
    std::unique_ptr<ICudaEngine> engine(runtime->deserializeCudaEngine(plan->data(), plan->size()));
    Check(engine != nullptr, "deserialize plugin engine");
    std::unique_ptr<IExecutionContext> context(engine->createExecutionContext());
    std::mt19937 rng(321 + stage);
    std::uniform_real_distribution<float> values(-1, 1);
    const int scale = OutputScale(stage), channels = OutputChannels(stage);
    for (const auto& [width, height] : sizes) {
        const size_t plane = size_t(width) * height;
        const int outputWidth = width / scale, outputHeight = height / scale;
        const size_t outputPlane = size_t(outputWidth) * outputHeight;
        std::vector<std::vector<__half>> host(inputCount);
        std::vector<std::unique_ptr<DeviceBuffer>> device(inputCount);
        for (int i = 0; i < inputCount; ++i) {
            const auto [batch, inputChannels, divisor] = InputLayout(stage, i);
            const int h = height / divisor, w = width / divisor;
            host[i].resize(size_t(batch) * inputChannels * h * w);
            for (auto& value : host[i]) value = __float2half_rn(values(rng));
            Check(context->setInputShape(names[i].c_str(), Dims4{batch, inputChannels, h, w}), "input shape");
            device[i] = std::make_unique<DeviceBuffer>(host[i].size() * sizeof(__half));
            Check(context->setTensorAddress(names[i].c_str(), device[i]->pointer), "input binding");
        }
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const size_t p = size_t(y) * width + x;
            host[0][7 * plane + p] = __float2half_rn(2.0f * x / (width - 1) - 1);
            host[0][8 * plane + p] = __float2half_rn(2.0f * y / (height - 1) - 1);
            host[0][9 * plane + p] = __float2half_rn(2.0f / (width - 1));
            host[0][10 * plane + p] = __float2half_rn(2.0f / (height - 1));
        }
        for (int i = 0; i < inputCount; ++i) {
            Cuda(cudaMemcpy(device[i]->pointer, host[i].data(), host[i].size() * sizeof(__half), cudaMemcpyHostToDevice));
        }
        DeviceBuffer output(channels * outputPlane * sizeof(__half));
        Check(context->setTensorAddress("output0", output.pointer), "output binding");
        Check(context->enqueueV3(stream), "plugin inference"); Cuda(cudaStreamSynchronize(stream));
        std::vector<__half> result(channels * outputPlane);
        Cuda(cudaMemcpy(result.data(), output.pointer, result.size() * sizeof(__half), cudaMemcpyDeviceToHost));
        const Reference reference{width, height, stage, host};
        float warpMaximum = 0, tailMaximum = 0, flowMaximum = 0; double total = 0;
        for (int outY = 0; outY < outputHeight; ++outY) for (int outX = 0; outX < outputWidth; ++outX) {
            const size_t p = size_t(outY) * outputWidth + outX;
            for (int c = 0; c < channels; ++c) {
                const float expected = reference.Down([&](int x, int y) {
                    return stage < 0 ? reference.Input(c, x, y)
                        : stage == kTailStage ? reference.Finished(c, x, y) : reference.Stage(c, x, y);
                }, outX, outY);
                const float error = std::abs(expected - __half2float(result[c * outputPlane + p]));
                Check(std::isfinite(error), "finite result");
                if (stage == kTailStage || (stage >= 0 && c < 14)) { warpMaximum = std::max(warpMaximum, error); total += error; }
                else if (stage >= 0 && c >= 24) flowMaximum = std::max(flowMaximum, error);
                else tailMaximum = std::max(tailMaximum, error);
            }
        }
        const int warpChannels = stage == kTailStage ? channels : 14;
        std::cout << "stage " << stage << ' ' << width << 'x' << height << " warp max=" << warpMaximum
                  << " mean=" << (stage < 0 ? 0 : total / (outputPlane * warpChannels)) << " tail max=" << tailMaximum
                  << " flow max=" << flowMaximum << '\n';
        // Full-resolution output keeps the graph's rounding and must match exactly.
        // Downscaled output rounds once where the graph rounds each sample and then the
        // resize, so it may differ by one FP16 step: 2^-11 just below 1 for images and
        // state; flow in these random tests stays under 16, where a step is 2^-7.
        // The final blend's exponential may differ in its last bit from the CPU's. A mask
        // that then rounds the other way moves the blend by up to two steps near 1.
        const bool exact = scale == 1 && stage != kTailStage;
        Check(exact ? warpMaximum == 0 : warpMaximum <= (stage == kTailStage ? 1.0f / 1024 : 1.0f / 2048), "warp reference tolerance");
        Check(exact ? tailMaximum == 0 : tailMaximum <= 1.0f / 2048, "tail reference tolerance");
        Check(exact ? flowMaximum == 0 : flowMaximum <= 1.0f / 128, "flow reference tolerance");
        Check(total / (outputPlane * warpChannels) < 1.0 / 8192, "mean reference tolerance");
    }
    // Levels would not divide exactly, so a non-multiple size must not run.
    std::unique_ptr<IExecutionContext> rejected(engine->createExecutionContext());
    std::vector<std::unique_ptr<DeviceBuffer>> device(inputCount);
    const int width = 48, height = 64;
    bool shaped = true;
    for (int i = 0; i < inputCount; ++i) {
        const auto [batch, inputChannels, divisor] = InputLayout(stage, i);
        shaped = shaped && rejected->setInputShape(names[i].c_str(), Dims4{batch, inputChannels, height / divisor, width / divisor});
        device[i] = std::make_unique<DeviceBuffer>(size_t(batch) * inputChannels * height * width * sizeof(__half));
        rejected->setTensorAddress(names[i].c_str(), device[i]->pointer);
    }
    DeviceBuffer output(channels * size_t(width) * height * sizeof(__half));
    rejected->setTensorAddress("output0", output.pointer);
    Check(!shaped || !rejected->enqueueV3(stream), "non-multiple size rejected");
    cudaGetLastError();
}
}

// Explicitly invoked native GPU test. No player, installed runtime or engine
// cache is opened. Validate the actual production plugin, including dynamic
// shapes, border sampling and serialization, against an independent CPU loop.
int main()
{
    try {
        Check(RegisterRifeWarpPlugin(), "plugin registration");
        cudaStream_t stream = nullptr; Cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        for (const int stage : {-1, 0, 1, 2, 3, kTailStage}) {
            TestStage(stage, {{32, 32}, {128, 64}, {640, 1152}, {1792, 1024}, {1920, 1152}}, stream);
        }
        Cuda(cudaStreamDestroy(stream));
        std::cout << "Fused warp: reference, dynamic shapes and serialization passed\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
