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
constexpr int kFullStage = 3, kTailStage = 4;
int InputCount(int stage) { return stage < 0 ? 2 : stage == kTailStage ? 4 : 3 + stage; }
int OutputScale(int stage) { return stage < 0 ? 32 : stage >= kFullStage ? 1 : 16 >> stage; }
int OutputChannels(int stage) { return stage < 0 ? 15 : stage == kTailStage ? 3 : 28; }
// The coarse blocks run at the height the model needs, which may be more than the frame's.
int CoarseHeight(int height) { return (height + 127) / 128 * 128; }
int LevelRows(int level, int height) { return CoarseHeight(height) / levelScales[level]; }
int OutputRows(int stage, int height) { return OutputScale(stage) > 1 ? CoarseHeight(height) / OutputScale(stage) : height; }
// Each input's complete shape, including layouts that are linear memory and not NCHW.
Dims4 InputShape(int stage, int input, int height, int width)
{
    if (input == 0) return {1, 11, height, width};
    if (input == 1) return {2, height, width, 8};
    if (stage == kTailStage) return input == 2 ? Dims4{1, height, width, 4} : Dims4{1, 20, height / 2, width / 2};
    return {1, 13, LevelRows(input - 2, height), width / levelScales[input - 2]};
}
Dims4 DeclaredShape(int stage, int input)
{
    if (input == 0) return {1, 11, -1, -1};
    if (input == 1) return {2, -1, -1, 8};
    if (stage == kTailStage) return input == 2 ? Dims4{1, -1, -1, 4} : Dims4{1, 20, -1, -1};
    return {1, 13, -1, -1};
}
size_t Elements(const Dims4& d) { return size_t(d.d[0]) * d.d[1] * d.d[2] * d.d[3]; }

// Independent CPU reference, written from the ONNX operator definitions but evaluated
// in FP32 with x + flow as the sampling position, which is what grid + flow * scale means.
struct Reference {
    int width, height, stage;
    const std::vector<std::vector<__half>>& host;
    float Timestep() const { return __half2float(host[0][6 * size_t(height) * width]); }
    float Source(int image, int slot, int x, int y) const {
        return __half2float(host[1][((size_t(image) * height + std::min(y, height - 1)) * width + x) * 8 + slot]);
    }
    // Resize, mode linear, half_pixel: x_original = (x_resized + 0.5) / scale_factor - 0.5.
    float Up(int level, int channel, int x, int y) const {
        const int factor = levelScales[level], lowWidth = width / factor, rows = LevelRows(level, height), lowHeight = rows;
        const float lx = std::clamp((x + .5f) / factor - .5f, 0.0f, float(lowWidth - 1));
        const float ly = std::clamp((y + .5f) / factor - .5f, 0.0f, float(lowHeight - 1));
        const int x0 = int(lx), y0 = int(ly), x1 = std::min(x0 + 1, lowWidth - 1), y1 = std::min(y0 + 1, lowHeight - 1);
        const float fx = lx - x0, fy = ly - y0;
        const auto at = [&](int row, int col) {
            return __half2float(host[2 + level][(size_t(channel) * rows + row) * lowWidth + col]);
        };
        return (1 - fy) * ((1 - fx) * at(y0, x0) + fx * at(y0, x1)) + fy * ((1 - fx) * at(y1, x0) + fx * at(y1, x1));
    }
    float Flow(int channel, int x, int y) const {
        float flow = 0;
        for (int level = 0; level <= std::min(stage, kFullStage); ++level) flow += levelScales[level] * Up(level, channel, x, y);
        return flow;
    }
    float Bilinear(float flowX, float flowY, int x, int y, const std::function<float(int, int)>& at) const {
        const float u = std::clamp(x + flowX, 0.0f, float(width - 1));
        const float v = std::clamp(y + flowY, 0.0f, float(height - 1));
        const int x0 = int(u), y0 = int(v), x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
        const float fx = u - x0, fy = v - y0;
        return (1 - fx) * (1 - fy) * at(y0, x0) + fx * (1 - fy) * at(y0, x1)
            + (1 - fx) * fy * at(y1, x0) + fx * fy * at(y1, x1);
    }
    float Warped(int batch, int channel, int x, int y) const {
        const int slot = channel < 3 ? channel : channel + 1;
        return Bilinear(Flow(batch * 2, x, y), Flow(batch * 2 + 1, x, y), x, y,
            [&](int row, int col) { return Source(batch, slot, col, row); });
    }
    // The graph's concat channel at one full-resolution pixel.
    float Stage(int channel, int x, int y) const {
        if (channel < 14) return Warped(channel / 7, channel % 7, x, y);
        if (channel == 14) return Timestep();
        if (channel < 24) return Up(stage, 4 + channel - 15, x, y);
        return Flow(channel - 24, x, y) / OutputScale(stage);
    }
    float Input(int channel, int x, int y) const {
        if (channel < 6) return Source(channel / 3, channel % 3, x, y);
        if (channel < 14) return Source((channel - 6) / 4, 4 + (channel - 6) % 4, x, y);
        return Timestep();
    }
    // Stage 4 from the graph's operators: DepthToSpace with mode CRD and factor 2, Add,
    // GridSample of the two images, Sigmoid, Mul, Sub, Mul, Add. Input 2 is stage 3's
    // accumulated flow and input 3 the ConvTranspose, whose first 20 channels matter.
    float Finished(int channel, int x, int y) const {
        const auto accumulated = [&](int c) { return __half2float(host[2][(size_t(y) * width + x) * 4 + c]); };
        const auto block = [&](int c) {
            const int lowWidth = width / 2, lowHeight = height / 2;
            const int mapped = c * 4 + (y % 2) * 2 + x % 2;
            return __half2float(host[3][(size_t(mapped) * lowHeight + y / 2) * lowWidth + x / 2]);
        };
        const auto warped = [&](int batch) {
            return Bilinear(accumulated(batch * 2) + block(batch * 2), accumulated(batch * 2 + 1) + block(batch * 2 + 1), x, y,
                [&](int row, int col) { return Source(batch, channel, col, row); });
        };
        const float mask = 1 / (1 + std::exp(-block(4)));
        return warped(0) * mask + warped(1) * (1 - mask);
    }
    // Resize at exactly 1/scale: x_original = (x_resized + 0.5) * scale - 0.5.
    float Down(const std::function<float(int, int)>& sample, int outputX, int outputY) const {
        const int scale = OutputScale(stage);
        if (scale == 1) return sample(outputX, outputY);
        const double cx = (outputX + .5) * scale - .5, cy = (outputY + .5) * scale - .5;
        const int x0 = int(std::floor(cx)), y0 = int(std::floor(cy));
        Check(cx - x0 == .5 && cy - y0 == .5 && x0 + 1 < width && y0 + 1 < CoarseHeight(height), "half_pixel centre");
        return .25f * (sample(x0, y0) + sample(x0 + 1, y0) + sample(x0, y0 + 1) + sample(x0 + 1, y0 + 1));
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
        names.push_back("input" + std::to_string(i));
        inputs.push_back(net->addInput(names[i].c_str(), DataType::kHALF, DeclaredShape(stage, i)));
        Check(profile->setDimensions(names[i].c_str(), OptProfileSelector::kMIN, InputShape(stage, i, 64, 128)), "min");
        Check(profile->setDimensions(names[i].c_str(), OptProfileSelector::kOPT, InputShape(stage, i, 1152, 640)), "opt");
        Check(profile->setDimensions(names[i].c_str(), OptProfileSelector::kMAX, InputShape(stage, i, 1152, 1920)), "max");
    }
    // An unknown graph must fail the matcher without changing the network.
    const int layers = net->getNbLayers(), networkInputs = net->getNbInputs();
    std::vector<uint16_t> fusedWeights;
    Check(!FuseRifeSynthesisWarps(*net, fusedWeights) && net->getNbLayers() == layers && net->getNbInputs() == networkInputs, "unknown graph preserved");
    auto* creator = static_cast<IPluginCreatorV3One*>(getPluginRegistry()->getCreator("MpcvrRifeWarpConcat", "5", "mpcvr"));
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
    const bool hasFlow = stage == kFullStage;
    Check(layer->getNbOutputs() == (hasFlow ? 2 : 1), "output count");
    // Inputs and outputs stay linear here, so TensorRT's own reformats check that the
    // plugin's interleaved inputs and outputs really have the layout they declare.
    layer->getOutput(0)->setName("output0");
    net->markOutput(*layer->getOutput(0));
    if (hasFlow) {
        layer->getOutput(1)->setName("output1");
        net->markOutput(*layer->getOutput(1));
    }
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
        const int outputWidth = width / scale, outputHeight = OutputRows(stage, height);
        const size_t outputPlane = size_t(outputWidth) * outputHeight;
        std::vector<std::vector<__half>> host(inputCount);
        std::vector<std::unique_ptr<DeviceBuffer>> device(inputCount);
        for (int i = 0; i < inputCount; ++i) {
            const Dims4 shape = InputShape(stage, i, height, width);
            host[i].resize(Elements(shape));
            for (auto& value : host[i]) value = __float2half_rn(values(rng));
            Check(context->setInputShape(names[i].c_str(), shape), "input shape");
            device[i] = std::make_unique<DeviceBuffer>(host[i].size() * sizeof(__half));
            Check(context->setTensorAddress(names[i].c_str(), device[i]->pointer), "input binding");
        }
        // Only the first pixel of the timestep plane is to be read; the rest stay random.
        host[0][6 * plane] = __float2half_rn(.375f);
        for (int i = 0; i < inputCount; ++i) {
            Cuda(cudaMemcpy(device[i]->pointer, host[i].data(), host[i].size() * sizeof(__half), cudaMemcpyHostToDevice));
        }
        // A copy from pageable memory may return before its transfer ends, and this test's
        // non-blocking stream does not wait for the default stream that copy ran on.
        Cuda(cudaDeviceSynchronize());
        DeviceBuffer output(channels * outputPlane * sizeof(__half));
        Check(context->setTensorAddress("output0", output.pointer), "output binding");
        DeviceBuffer flowOutput(hasFlow ? plane * 4 * sizeof(__half) : 4);
        if (hasFlow) Check(context->setTensorAddress("output1", flowOutput.pointer), "flow output binding");
        Check(context->enqueueV3(stream), "plugin inference"); Cuda(cudaStreamSynchronize(stream));
        std::vector<__half> result(channels * outputPlane);
        Cuda(cudaMemcpy(result.data(), output.pointer, result.size() * sizeof(__half), cudaMemcpyDeviceToHost));
        const Reference reference{width, height, stage, host};
        float warpMaximum = 0, tailMaximum = 0, flowMaximum = 0; double total = 0;
        for (int outY = 0; outY < outputHeight; ++outY) for (int outX = 0; outX < outputWidth; ++outX) {
            const size_t p = size_t(outY) * outputWidth + outX;
            for (int c = 0; c < channels; ++c) {
                const float expected = Half(reference.Down([&](int x, int y) {
                    return stage < 0 ? reference.Input(c, x, y)
                        : stage == kTailStage ? reference.Finished(c, x, y) : reference.Stage(c, x, y);
                }, outX, outY));
                const float error = std::abs(expected - __half2float(result[c * outputPlane + p]));
                Check(std::isfinite(error), "finite result");
                if (stage == kTailStage || (stage >= 0 && c < 14)) { warpMaximum = std::max(warpMaximum, error); total += error; }
                else if (stage >= 0 && c >= 24) flowMaximum = std::max(flowMaximum, error);
                else tailMaximum = std::max(tailMaximum, error);
            }
        }
        if (hasFlow) {
            std::vector<__half> flows(plane * 4);
            Cuda(cudaMemcpy(flows.data(), flowOutput.pointer, flows.size() * sizeof(__half), cudaMemcpyDeviceToHost));
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) for (int c = 0; c < 4; ++c) {
                const float error = std::abs(Half(reference.Flow(c, x, y)) - __half2float(flows[(size_t(y) * width + x) * 4 + c]));
                flowMaximum = std::max(flowMaximum, error);
            }
        }
        const int warpChannels = stage == kTailStage ? channels : 14;
        std::cout << "stage " << stage << ' ' << width << 'x' << height << " warp max=" << warpMaximum
                  << " mean=" << (stage < 0 ? 0 : total / (outputPlane * warpChannels)) << " tail max=" << tailMaximum
                  << " flow max=" << flowMaximum << '\n';
        // Evaluation order and the exponential's last bit can round a stored value the
        // other way: one FP16 step, 2^-11 just below 1 for images and state. Flow in
        // these random tests stays under 64, where a step is 2^-5.
        Check(warpMaximum <= 1.0f / 2048, "warp reference tolerance");
        Check(tailMaximum <= 1.0f / 2048, "tail reference tolerance");
        Check(flowMaximum <= 1.0f / 32, "flow reference tolerance");
        Check(total / (outputPlane * warpChannels) < 1.0 / 8192, "mean reference tolerance");
    }
    // Levels would not divide exactly, so a non-multiple size must not run.
    std::unique_ptr<IExecutionContext> rejected(engine->createExecutionContext());
    std::vector<std::unique_ptr<DeviceBuffer>> device(inputCount);
    const int width = 192, height = 128;
    bool shaped = true;
    for (int i = 0; i < inputCount; ++i) {
        const Dims4 shape = InputShape(stage, i, height, width);
        shaped = shaped && rejected->setInputShape(names[i].c_str(), shape);
        device[i] = std::make_unique<DeviceBuffer>(size_t(2) * 13 * height * width * sizeof(__half));
        rejected->setTensorAddress(names[i].c_str(), device[i]->pointer);
    }
    DeviceBuffer output(channels * size_t(width) * height * sizeof(__half));
    rejected->setTensorAddress("output0", output.pointer);
    DeviceBuffer flowOutput(size_t(width) * height * 4 * sizeof(__half));
    if (hasFlow) rejected->setTensorAddress("output1", flowOutput.pointer);
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
            TestStage(stage, {{128, 64}, {128, 128}, {640, 1152}, {1792, 1024}, {1920, 1088}, {1920, 1152}}, stream);
        }
        Cuda(cudaStreamDestroy(stream));
        std::cout << "Fused warp: reference, dynamic shapes and serialization passed\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
