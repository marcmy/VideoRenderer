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
constexpr int kFullStage = 3, kAnchorStage = 5, kLevelStage = 6, kFieldStage = 7;
bool Finishing(int stage) { return stage == kFieldStage; }
int InputCount(int stage) { return stage < 0 ? 2 : Finishing(stage) ? 4 : stage == kAnchorStage ? 6 : stage == kLevelStage ? 3 : 3 + stage; }
int OutputScale(int stage) { return stage < 0 ? 32 : stage == kAnchorStage ? 4 : stage >= kFullStage ? 1 : 16 >> stage; }
int OutputChannels(int stage) { return stage < 0 ? 15 : stage == kAnchorStage ? 16 : stage == kFieldStage ? 8 : 28; }
// The coarse blocks run at the height the model needs, which may be more than the frame's.
int CoarseHeight(int height) { return (height + 127) / 128 * 128; }
int LevelRows(int level, int height) { return CoarseHeight(height) / levelScales[level]; }
int OutputRows(int stage, int height) { return OutputScale(stage) > 1 ? CoarseHeight(height) / OutputScale(stage) : height; }
// Each input's complete shape, including layouts that are linear memory and not NCHW.
Dims4 InputShape(int stage, int input, int height, int width)
{
    if (input == 0) return {1, 11, height, width};
    if (input == 1) return {2, height, width, 8};
    if (Finishing(stage)) return input == 2 ? Dims4{1, height, width, 4} : Dims4{1, 20, height / 2, width / 2};
    if (stage == kLevelStage) return {1, CoarseHeight(height) / 4, width / 4, 16};
    return {1, 13, LevelRows(input - 2, height), width / levelScales[input - 2]};
}
Dims4 DeclaredShape(int stage, int input)
{
    if (input == 0) return {1, 11, -1, -1};
    if (input == 1) return {2, -1, -1, 8};
    if (Finishing(stage)) return input == 2 ? Dims4{1, -1, -1, 4} : Dims4{1, 20, -1, -1};
    if (stage == kLevelStage) return {1, -1, -1, 16};
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
            // Stage 6's level arrives as the linear memory it declares; the others planar,
            // for TensorRT's reformat to kHWC8.
            if (stage == kLevelStage) return __half2float(host[2][(size_t(row) * lowWidth + col) * 16 + channel]);
            return __half2float(host[2 + level][(size_t(channel) * rows + row) * lowWidth + col]);
        };
        return (1 - fy) * ((1 - fx) * at(y0, x0) + fx * at(y0, x1)) + fy * ((1 - fx) * at(y1, x0) + fx * at(y1, x1));
    }
    float Flow(int channel, int x, int y) const {
        float flow = 0;
        for (int level = stage == kLevelStage ? 3 : 0; level <= std::min(stage, kFullStage); ++level) flow += levelScales[level] * Up(level, channel, x, y);
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
        if (channel < 24) return Up(std::min(stage, kFullStage), 4 + channel - 15, x, y);
        return Flow(channel - 24, x, y) / OutputScale(stage);
    }
    float Input(int channel, int x, int y) const {
        if (channel < 6) return Source(channel / 3, channel % 3, x, y);
        if (channel < 14) return Source((channel - 6) / 4, 4 + (channel - 6) % 4, x, y);
        return Timestep();
    }
    // Stage 7: final flow and mask, with the block's channels placed as in Finished().
    float Field(int channel, int x, int y) const {
        const auto block = [&](int c) {
            const int mapped = c * 4 + (y % 2) * 2 + x % 2;
            return __half2float(host[3][(size_t(mapped) * (height / 2) + y / 2) * (width / 2) + x / 2]);
        };
        if (channel < 4) return __half2float(host[2][(size_t(y) * width + x) * 4 + channel]) + block(channel);
        return channel == 4 ? block(4) : 0.0f;
    }
    // Stage 5: flow summed at a level 3 texel's centre, in level 3's unit; state its own.
    float Accumulated(int channel, int x, int y) const {
        const size_t rows = LevelRows(3, height), lowWidth = width / 4;
        const float own = __half2float(host[5][(channel * rows + y) * lowWidth + x]);
        if (channel >= 13) return 0.0f;
        if (channel >= 4) return own;
        float flow = 0;
        for (int level = 0; level < 3; ++level) {
            const int factor = levelScales[level], levelWidth = width / factor, levelRows = LevelRows(level, height);
            const float lx = std::clamp((x + .5f) * 4 / factor - .5f, 0.0f, float(levelWidth - 1));
            const float ly = std::clamp((y + .5f) * 4 / factor - .5f, 0.0f, float(levelRows - 1));
            const int x0 = int(lx), y0 = int(ly), x1 = std::min(x0 + 1, levelWidth - 1), y1 = std::min(y0 + 1, levelRows - 1);
            const float fx = lx - x0, fy = ly - y0;
            const auto at = [&](int row, int col) { return __half2float(host[2 + level][(size_t(channel) * levelRows + row) * levelWidth + col]); };
            flow += factor * ((1 - fy) * ((1 - fx) * at(y0, x0) + fx * at(y0, x1)) + fy * ((1 - fx) * at(y1, x0) + fx * at(y1, x1)));
        }
        return own + flow / 4;
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
    auto* creator = static_cast<IPluginCreatorV3One*>(getPluginRegistry()->getCreator("MpcvrRifeWarpConcat", "7", "mpcvr"));
    Check(creator != nullptr, "creator lookup");
    const int32_t invalidStage = 8;
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
    const bool hasFlow = stage == kFullStage || stage == kLevelStage;
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
        // Stages 5 and 7 store interleaved memory, each pixel's values together.
        if (stage == kAnchorStage || stage == kFieldStage) {
            for (int outY = 0; outY < outputHeight; ++outY) for (int outX = 0; outX < outputWidth; ++outX) {
                for (int c = 0; c < channels; ++c) {
                    if (stage == kAnchorStage && c >= 13) continue;
                    const float expected = Half(stage == kAnchorStage ? reference.Accumulated(c, outX, outY) : reference.Field(c, outX, outY));
                    const float error = std::abs(expected - __half2float(result[(size_t(outY) * outputWidth + outX) * channels + c]));
                    Check(std::isfinite(error), "finite result");
                    (c < 4 ? flowMaximum : tailMaximum) = std::max(c < 4 ? flowMaximum : tailMaximum, error);
                }
            }
            std::cout << "stage " << stage << ' ' << width << 'x' << height << " state max=" << tailMaximum << " flow max=" << flowMaximum << '\n';
            Check(tailMaximum <= 1.0f / 2048, "state reference tolerance");
            Check(flowMaximum <= 1.0f / 32, "flow reference tolerance");
            continue;
        }
        for (int outY = 0; outY < outputHeight; ++outY) for (int outX = 0; outX < outputWidth; ++outX) {
            const size_t p = size_t(outY) * outputWidth + outX;
            for (int c = 0; c < channels; ++c) {
                const float expected = Half(reference.Down([&](int x, int y) {
                    return stage < 0 ? reference.Input(c, x, y) : reference.Stage(c, x, y);
                }, outX, outY));
                const float error = std::abs(expected - __half2float(result[c * outputPlane + p]));
                Check(std::isfinite(error), "finite result");
                if (stage >= 0 && c < 14) { warpMaximum = std::max(warpMaximum, error); total += error; }
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
        const int warpChannels = 14;
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

float Lerp2(float tl, float tr, float bl, float br, float fx, float fy)
{
    return (1 - fy) * ((1 - fx) * tl + fx * tr) + fy * ((1 - fx) * bl + fx * br);
}

// Bilinear at a clamped position over interleaved values, in FP32 as the kernels compute.
float Interleaved(const std::vector<__half>& values, int stride, int channel, float u, float v, int rows, int cols)
{
    u = std::clamp(u, 0.0f, float(cols - 1));
    v = std::clamp(v, 0.0f, float(rows - 1));
    const int x0 = int(u), y0 = int(v), x1 = std::min(x0 + 1, cols - 1), y1 = std::min(y0 + 1, rows - 1);
    const auto at = [&](int y, int x) { return __half2float(values[(size_t(y) * cols + x) * stride + channel]); };
    return Lerp2(at(y0, x0), at(y0, x1), at(y1, x0), at(y1, x1), u - x0, v - y0);
}

// The two kernels that make a target from shared flow, against what they mean.
void TestRetime(cudaStream_t stream)
{
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> values(-1, 1), flows(-24, 24), unit(0, 1);
    for (const auto& [width, height] : std::vector<std::pair<int, int>>{{256, 128}, {640, 384}, {1920, 1088}}) {
        const int rows = CoarseHeight(height) / 4, cols = width / 4;
        std::vector<__half> anchor(size_t(rows) * cols * 16);
        for (size_t i = 0; i < anchor.size(); ++i) anchor[i] = __float2half_rn(i % 16 < 4 ? flows(rng) : i % 16 < 13 ? values(rng) : 0.0f);
        DeviceBuffer anchorDevice(anchor.size() * sizeof(__half)), levelDevice(anchor.size() * sizeof(__half)), slot(sizeof(__half));
        Cuda(cudaMemcpy(anchorDevice.pointer, anchor.data(), anchor.size() * sizeof(__half), cudaMemcpyHostToDevice));
        Cuda(cudaDeviceSynchronize());
        for (const float t : {.125f, .5f, .75f}) {
            Cuda(MpcvrRifeRetimeLevel(anchorDevice.pointer, levelDevice.pointer, rows, cols, t, slot.pointer, stream));
            Cuda(cudaStreamSynchronize(stream));
            std::vector<__half> level(anchor.size());
            __half stored;
            Cuda(cudaMemcpy(level.data(), levelDevice.pointer, level.size() * sizeof(__half), cudaMemcpyDeviceToHost));
            Cuda(cudaMemcpy(&stored, slot.pointer, sizeof(__half), cudaMemcpyDeviceToHost));
            Check(__half2float(stored) == Half(t), "level kernel stores the target's time");
            float flowMaximum = 0, stateMaximum = 0;
            for (int y = 0; y < rows; ++y) for (int x = 0; x < cols; ++x) {
                const auto own = [&](int c) { return __half2float(anchor[(size_t(y) * cols + x) * 16 + c]); };
                const float u = x - (t - .5f) * (own(2) - own(0)), v = y - (t - .5f) * (own(3) - own(1));
                for (int c = 0; c < 13; ++c) {
                    const float scale = c < 2 ? 2 * t : c < 4 ? 2 * (1 - t) : 1.0f;
                    const float error = std::abs(Half(scale * Interleaved(anchor, 16, c, u, v, rows, cols)) - __half2float(level[(size_t(y) * cols + x) * 16 + c]));
                    Check(std::isfinite(error), "finite level");
                    (c < 4 ? flowMaximum : stateMaximum) = std::max(c < 4 ? flowMaximum : stateMaximum, error);
                }
            }
            std::cout << "retime level " << width << 'x' << height << " t=" << t << " state max=" << stateMaximum << " flow max=" << flowMaximum << '\n';
            Check(stateMaximum <= 1.0f / 2048, "retimed state tolerance");
            Check(flowMaximum <= 1.0f / 32, "retimed flow tolerance");
        }
        // Whole frames: to linear memory, then through both surface formats.
        std::vector<__half> field(size_t(height) * width * 8), source(size_t(2) * height * width * 8);
        for (size_t i = 0; i < field.size(); ++i) field[i] = __float2half_rn(i % 8 < 4 ? flows(rng) : i % 8 == 4 ? values(rng) * 4 : 0.0f);
        for (size_t i = 0; i < source.size(); ++i) source[i] = __float2half_rn(i % 8 == 3 ? 0.0f : unit(rng));
        DeviceBuffer fieldDevice(field.size() * sizeof(__half)), sourceDevice(source.size() * sizeof(__half));
        DeviceBuffer linearDevice(size_t(height) * width * 4);
        Cuda(cudaMemcpy(fieldDevice.pointer, field.data(), field.size() * sizeof(__half), cudaMemcpyHostToDevice));
        Cuda(cudaMemcpy(sourceDevice.pointer, source.data(), source.size() * sizeof(__half), cudaMemcpyHostToDevice));
        Cuda(cudaDeviceSynchronize());
        const cudaChannelFormatDesc bgraFormat = cudaCreateChannelDesc<uchar4>(), floatFormat = cudaCreateChannelDesc(16, 16, 16, 16, cudaChannelFormatKindFloat);
        cudaArray_t bgraArray = nullptr, floatArray = nullptr;
        Cuda(cudaMallocArray(&bgraArray, &bgraFormat, width, height, cudaArraySurfaceLoadStore));
        Cuda(cudaMallocArray(&floatArray, &floatFormat, width, height, cudaArraySurfaceLoadStore));
        for (const float t : {.125f, .5f, .75f}) {
            cudaSurfaceObject_t bgraSurface = 0, floatSurface = 0;
            RifeFrameOutput toLinear, toBgra, toFloat;
            toLinear.linear = linearDevice.pointer;
            toLinear.pitch = size_t(width) * 4;
            toBgra.array = bgraArray;
            toFloat.array = floatArray;
            toFloat.rgba16 = true;
            Cuda(MpcvrRifeRetimeFrame(fieldDevice.pointer, sourceDevice.pointer, toLinear, height, width, t, stream, nullptr));
            Cuda(MpcvrRifeRetimeFrame(fieldDevice.pointer, sourceDevice.pointer, toBgra, height, width, t, stream, &bgraSurface));
            Cuda(MpcvrRifeRetimeFrame(fieldDevice.pointer, sourceDevice.pointer, toFloat, height, width, t, stream, &floatSurface));
            Cuda(cudaStreamSynchronize(stream));
            std::vector<uchar4> linear(size_t(height) * width), bgra(linear.size());
            std::vector<__half> floats(linear.size() * 4);
            Cuda(cudaMemcpy(linear.data(), linearDevice.pointer, linear.size() * sizeof(uchar4), cudaMemcpyDeviceToHost));
            Cuda(cudaMemcpy2DFromArray(bgra.data(), size_t(width) * sizeof(uchar4), bgraArray, 0, 0, size_t(width) * sizeof(uchar4), height, cudaMemcpyDeviceToHost));
            Cuda(cudaMemcpy2DFromArray(floats.data(), size_t(width) * 8, floatArray, 0, 0, size_t(width) * 8, height, cudaMemcpyDeviceToHost));
            Cuda(cudaDestroySurfaceObject(bgraSurface));
            Cuda(cudaDestroySurfaceObject(floatSurface));
            float maximum = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const size_t pixel = size_t(y) * width + x;
                const auto own = [&](int c) { return __half2float(field[pixel * 8 + c]); };
                const float u = x - (t - .5f) * (own(2) - own(0)), v = y - (t - .5f) * (own(3) - own(1));
                const float a = 2 * t, b = 2 * (1 - t);
                const float mask = 1 / (1 + std::exp(-Interleaved(field, 8, 4, u, v, height, width)));
                const float first = mask * b * b * b * b, second = (1 - mask) * a * a * a * a;
                float expected[3];
                for (int c = 0; c < 3; ++c) {
                    const auto warped = [&](int image) {
                        const float scale = image ? b : a;
                        const float flowX = scale * Interleaved(field, 8, image * 2, u, v, height, width);
                        const float flowY = scale * Interleaved(field, 8, image * 2 + 1, u, v, height, width);
                        const std::vector<__half>& all = source;
                        const float px = std::clamp(x + flowX, 0.0f, float(width - 1)), py = std::clamp(y + flowY, 0.0f, float(height - 1));
                        const int x0 = int(px), y0 = int(py), x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
                        const auto at = [&](int row, int col) { return __half2float(all[((size_t(image) * height + row) * width + col) * 8 + c]); };
                        return Lerp2(at(y0, x0), at(y0, x1), at(y1, x0), at(y1, x1), px - x0, py - y0);
                    };
                    expected[c] = std::clamp((warped(0) * first + warped(1) * second) / std::max(first + second, 1e-6f), 0.0f, 1.0f);
                }
                // Coordinates computed in another order can land a bit apart, so allow more than one FP16 step.
                for (int c = 0; c < 3; ++c) {
                    const float error = std::abs(expected[c] - __half2float(floats[pixel * 4 + c]));
                    Check(std::isfinite(error), "finite frame");
                    maximum = std::max(maximum, error);
                }
                Check(__half2float(floats[pixel * 4 + 3]) == 1.0f && linear[pixel].w == 255 && bgra[pixel].w == 255, "opaque alpha");
                Check(linear[pixel].x == bgra[pixel].x && linear[pixel].y == bgra[pixel].y && linear[pixel].z == bgra[pixel].z, "surface and linear targets agree");
                Check(std::abs(linear[pixel].z - expected[0] * 255) <= 1.5f && std::abs(linear[pixel].y - expected[1] * 255) <= 1.5f && std::abs(linear[pixel].x - expected[2] * 255) <= 1.5f, "BGRA order and value");
            }
            std::cout << "retime frame " << width << 'x' << height << " t=" << t << " max=" << maximum << '\n';
            Check(maximum <= 1.0f / 256, "retimed frame tolerance");
        }
        Cuda(cudaFreeArray(floatArray));
        Cuda(cudaFreeArray(bgraArray));
    }
}

// The frame kernel, on every target, from the warp source and from filtered surfaces. As the
// graph computes it: DepthToSpace (CRD, factor 2), Add, GridSample, Sigmoid, Mul, Sub, Mul, Add.
void TestFinish(cudaStream_t stream)
{
    std::mt19937 rng(78);
    std::uniform_real_distribution<float> flowValue(-24, 24), residual(-2, 2), maskValue(-4, 4), unit(0, 1);
    for (const auto& [width, height] : std::vector<std::pair<int, int>>{{256, 128}, {640, 384}, {1920, 1088}}) {
        const size_t pixels = size_t(height) * width;
        std::vector<__half> flows(pixels * 4), low(size_t(height / 2) * (width / 2) * 24), source(pixels * 2 * 8);
        std::vector<uchar4> bgraImage(pixels * 2);
        std::vector<__half> floatImage(pixels * 2 * 4);
        for (auto& value : flows) value = __float2half_rn(flowValue(rng));
        for (size_t i = 0; i < low.size(); ++i) low[i] = __float2half_rn(i % 24 < 16 ? residual(rng) : maskValue(rng));
        for (size_t p = 0; p < pixels * 2; ++p) {
            uchar4 bytes{};
            bytes.w = 255;
            for (int c = 0; c < 3; ++c) {
                const auto byte = static_cast<unsigned char>(unit(rng) * 255.0f + 0.5f);
                (c == 0 ? bytes.z : c == 1 ? bytes.y : bytes.x) = byte;
                source[p * 8 + c] = __float2half_rn(byte / 255.0f);
                floatImage[p * 4 + c] = source[p * 8 + c];
            }
            floatImage[p * 4 + 3] = __float2half_rn(1.0f);
            bgraImage[p] = bytes;
        }
        DeviceBuffer flowsDevice(flows.size() * sizeof(__half)), lowDevice(low.size() * sizeof(__half)), sourceDevice(source.size() * sizeof(__half));
        DeviceBuffer linearDevice(pixels * 4);
        Cuda(cudaMemcpy(flowsDevice.pointer, flows.data(), flows.size() * sizeof(__half), cudaMemcpyHostToDevice));
        Cuda(cudaMemcpy(lowDevice.pointer, low.data(), low.size() * sizeof(__half), cudaMemcpyHostToDevice));
        Cuda(cudaMemcpy(sourceDevice.pointer, source.data(), source.size() * sizeof(__half), cudaMemcpyHostToDevice));
        const cudaChannelFormatDesc bgraFormat = cudaCreateChannelDesc<uchar4>(), floatFormat = cudaCreateChannelDesc(16, 16, 16, 16, cudaChannelFormatKindFloat);
        cudaArray_t bgraArray = nullptr, floatArray = nullptr;
        Cuda(cudaMallocArray(&bgraArray, &bgraFormat, width, height, cudaArraySurfaceLoadStore));
        Cuda(cudaMallocArray(&floatArray, &floatFormat, width, height, cudaArraySurfaceLoadStore));
        // Two images as surfaces, as imported ones are read.
        cudaArray_t images[4]{};
        cudaTextureObject_t filtered[4]{};
        for (int k = 0; k < 4; ++k) {
            const bool isFloat = k >= 2;
            Cuda(cudaMallocArray(&images[k], isFloat ? &floatFormat : &bgraFormat, width, height));
            const void* data = isFloat ? static_cast<const void*>(floatImage.data() + pixels * 4 * (k - 2)) : static_cast<const void*>(bgraImage.data() + pixels * k);
            Cuda(cudaMemcpy2DToArray(images[k], 0, 0, data, size_t(width) * (isFloat ? 8 : 4), size_t(width) * (isFloat ? 8 : 4), height, cudaMemcpyHostToDevice));
            cudaResourceDesc resource{};
            resource.resType = cudaResourceTypeArray;
            resource.res.array.array = images[k];
            cudaTextureDesc filter{};
            filter.addressMode[0] = filter.addressMode[1] = cudaAddressModeClamp;
            filter.filterMode = cudaFilterModeLinear;
            filter.readMode = isFloat ? cudaReadModeElementType : cudaReadModeNormalizedFloat;
            Cuda(cudaCreateTextureObject(&filtered[k], &resource, &filter, nullptr));
        }
        for (const bool textured : {false, true}) {
            RifeFrameOutput toLinear, toBgra, toFloat;
            toLinear.linear = linearDevice.pointer;
            toLinear.pitch = size_t(width) * 4;
            toBgra.array = bgraArray;
            toFloat.array = floatArray;
            toFloat.rgba16 = true;
            if (textured) {
                toLinear.firstImage = toBgra.firstImage = filtered[0];
                toLinear.secondImage = toBgra.secondImage = filtered[1];
                toFloat.firstImage = filtered[2];
                toFloat.secondImage = filtered[3];
            }
            cudaSurfaceObject_t bgraSurface = 0, floatSurface = 0;
            Cuda(MpcvrRifeFinishFrame(flowsDevice.pointer, lowDevice.pointer, sourceDevice.pointer, toLinear, height, width, stream, nullptr));
            Cuda(MpcvrRifeFinishFrame(flowsDevice.pointer, lowDevice.pointer, sourceDevice.pointer, toBgra, height, width, stream, &bgraSurface));
            Cuda(MpcvrRifeFinishFrame(flowsDevice.pointer, lowDevice.pointer, sourceDevice.pointer, toFloat, height, width, stream, &floatSurface));
            Cuda(cudaStreamSynchronize(stream));
            std::vector<uchar4> linear(pixels), bgra(pixels);
            std::vector<__half> floats(pixels * 4);
            Cuda(cudaMemcpy(linear.data(), linearDevice.pointer, pixels * 4, cudaMemcpyDeviceToHost));
            Cuda(cudaMemcpy2DFromArray(bgra.data(), size_t(width) * 4, bgraArray, 0, 0, size_t(width) * 4, height, cudaMemcpyDeviceToHost));
            Cuda(cudaMemcpy2DFromArray(floats.data(), size_t(width) * 8, floatArray, 0, 0, size_t(width) * 8, height, cudaMemcpyDeviceToHost));
            Cuda(cudaDestroySurfaceObject(bgraSurface));
            Cuda(cudaDestroySurfaceObject(floatSurface));
            float maximum = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const size_t pixel = size_t(y) * width + x;
                const __half* block = low.data() + (size_t(y / 2) * (width / 2) + x / 2) * 24 + (y % 2) * 2 + x % 2;
                const float mask = 1 / (1 + std::exp(-__half2float(block[16])));
                float expected[3];
                for (int c = 0; c < 3; ++c) {
                    const auto warped = [&](int image) {
                        const float px = std::clamp(x + __half2float(flows[pixel * 4 + image * 2]) + __half2float(block[image * 8]), 0.0f, float(width - 1));
                        const float py = std::clamp(y + __half2float(flows[pixel * 4 + image * 2 + 1]) + __half2float(block[image * 8 + 4]), 0.0f, float(height - 1));
                        const int x0 = int(px), y0 = int(py), x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
                        const auto at = [&](int row, int col) { return __half2float(source[((size_t(image) * height + row) * width + col) * 8 + c]); };
                        return Lerp2(at(y0, x0), at(y0, x1), at(y1, x0), at(y1, x1), px - x0, py - y0);
                    };
                    expected[c] = std::clamp(warped(0) * mask + warped(1) * (1 - mask), 0.0f, 1.0f);
                }
                for (int c = 0; c < 3; ++c) {
                    const float error = std::abs(expected[c] - __half2float(floats[pixel * 4 + c]));
                    Check(std::isfinite(error), "finite finished frame");
                    maximum = std::max(maximum, error);
                }
                Check(__half2float(floats[pixel * 4 + 3]) == 1.0f && linear[pixel].w == 255 && bgra[pixel].w == 255, "finished frame is opaque");
                Check(linear[pixel].x == bgra[pixel].x && linear[pixel].y == bgra[pixel].y && linear[pixel].z == bgra[pixel].z, "finished surface and linear targets agree");
                // The filter's weights have eight fractional bits, on noise as harsh as images get.
                const float tolerance = textured ? 2.5f : 1.0f;
                Check(std::abs(linear[pixel].z - expected[0] * 255) <= tolerance && std::abs(linear[pixel].y - expected[1] * 255) <= tolerance
                    && std::abs(linear[pixel].x - expected[2] * 255) <= tolerance, "finished 8-bit frame");
            }
            std::cout << "finish frame " << width << 'x' << height << (textured ? " filtered" : " warp source") << " max=" << maximum << '\n';
            Check(maximum <= (textured ? 1.0f / 128 : 1.0f / 1024), "finished frame tolerance");
        }
        for (int k = 0; k < 4; ++k) {
            Cuda(cudaDestroyTextureObject(filtered[k]));
            Cuda(cudaFreeArray(images[k]));
        }
        Cuda(cudaFreeArray(floatArray));
        Cuda(cudaFreeArray(bgraArray));
    }
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
        for (const int stage : {-1, 0, 1, 2, 3, kAnchorStage, kLevelStage, kFieldStage}) {
            TestStage(stage, {{128, 64}, {128, 128}, {640, 1152}, {1792, 1024}, {1920, 1088}, {1920, 1152}}, stream);
        }
        TestRetime(stream);
        TestFinish(stream);
        Cuda(cudaStreamDestroy(stream));
        std::cout << "Fused warp: reference, dynamic shapes and serialization passed\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
