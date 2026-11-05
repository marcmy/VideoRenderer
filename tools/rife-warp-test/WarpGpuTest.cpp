#include "../RifeTensorRTRuntime/RifeWarpPlugin.h"
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
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
}

// Explicitly invoked native GPU test. No player, installed runtime or engine
// cache is opened. Validate the actual production plugin, including dynamic
// shapes, border sampling and serialization, against an independent CPU loop.
int main()
{
    try {
        using namespace nvinfer1;
        Check(RegisterRifeWarpPlugin(), "plugin registration");
        std::unique_ptr<IBuilder> builder(createInferBuilder(logger));
        std::unique_ptr<INetworkDefinition> net(builder->createNetworkV2(0));
        std::unique_ptr<IBuilderConfig> config(builder->createBuilderConfig());
        auto* profile = builder->createOptimizationProfile();
        constexpr int batches[]{1, 2, 1, 1, 1}, channels[]{11, 4, 4, 9, 4};
        constexpr const char* names[]{"raw", "features", "flow", "state", "tail"};
        std::array<ITensor*, 5> inputs{};
        for (int i = 0; i < 5; ++i) {
            inputs[i] = net->addInput(names[i], DataType::kHALF, Dims4{batches[i], channels[i], -1, -1});
            Check(profile->setDimensions(names[i], OptProfileSelector::kMIN, Dims4{batches[i], channels[i], 1, 1}), "min");
            Check(profile->setDimensions(names[i], OptProfileSelector::kOPT, Dims4{batches[i], channels[i], 1152, 640}), "opt");
            Check(profile->setDimensions(names[i], OptProfileSelector::kMAX, Dims4{batches[i], channels[i], 1152, 1920}), "max");
        }
        // An unknown graph must fail the matcher without changing the network.
        const int layers = net->getNbLayers();
        Check(!FuseRifeSynthesisWarps(*net) && net->getNbLayers() == layers, "unknown graph preserved");
        auto* creator = static_cast<IPluginCreatorV3One*>(getPluginRegistry()->getCreator("MpcvrRifeWarpConcat", "1", "mpcvr"));
        Check(creator != nullptr, "creator lookup");
        PluginFieldCollection fields{0, nullptr};
        std::unique_ptr<IPluginV3> plugin(creator->createPlugin("warp", &fields, TensorRTPhase::kBUILD));
        auto* layer = net->addPluginV3(inputs.data(), 5, nullptr, 0, *plugin);
        Check(layer != nullptr, "plugin layer");
        layer->getOutput(0)->setName("output");
        net->markOutput(*layer->getOutput(0));
        Check(config->addOptimizationProfile(profile) >= 0, "profile");
        std::unique_ptr<IHostMemory> plan(builder->buildSerializedNetwork(*net, *config));
        Check(plan != nullptr, "serialize plugin engine");
        std::unique_ptr<IRuntime> runtime(createInferRuntime(logger));
        std::unique_ptr<ICudaEngine> engine(runtime->deserializeCudaEngine(plan->data(), plan->size()));
        Check(engine != nullptr, "deserialize plugin engine");
        std::unique_ptr<IExecutionContext> context(engine->createExecutionContext());
        cudaStream_t stream = nullptr; Cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        std::mt19937 rng(123);
        std::uniform_real_distribution<float> values(-1, 1), displacement(-60, 60);
        for (const auto [width, height] : {std::pair{1, 1}, {128, 128}, {257, 129}, {640, 1152}, {1920, 1152}}) {
            const size_t plane = size_t(width) * height;
            std::array<std::vector<__half>, 5> host;
            std::array<std::unique_ptr<DeviceBuffer>, 5> device;
            for (int i = 0; i < 5; ++i) {
                host[i].resize(plane * batches[i] * channels[i]);
                for (auto& value : host[i]) value = __float2half_rn(i == 2 ? displacement(rng) : values(rng));
                Check(context->setInputShape(names[i], Dims4{batches[i], channels[i], height, width}), "input shape");
                device[i] = std::make_unique<DeviceBuffer>(host[i].size() * sizeof(__half));
                Check(context->setTensorAddress(names[i], device[i]->pointer), "input binding");
            }
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const size_t p = size_t(y) * width + x;
                host[0][7 * plane + p] = __float2half_rn(width > 1 ? 2.0f * x / (width - 1) - 1 : 0);
                host[0][8 * plane + p] = __float2half_rn(height > 1 ? 2.0f * y / (height - 1) - 1 : 0);
                host[0][9 * plane + p] = __float2half_rn(width > 1 ? 2.0f / (width - 1) : 0);
                host[0][10 * plane + p] = __float2half_rn(height > 1 ? 2.0f / (height - 1) : 0);
            }
            for (int i = 0; i < 5; ++i) Cuda(cudaMemcpy(device[i]->pointer, host[i].data(), host[i].size() * sizeof(__half), cudaMemcpyHostToDevice));
            DeviceBuffer output(28 * plane * sizeof(__half));
            Check(context->setTensorAddress("output", output.pointer), "output binding");
            Check(context->enqueueV3(stream), "plugin inference"); Cuda(cudaStreamSynchronize(stream));
            std::vector<__half> result(28 * plane);
            Cuda(cudaMemcpy(result.data(), output.pointer, result.size() * sizeof(__half), cudaMemcpyDeviceToHost));
            float maximum = 0; double total = 0;
            for (size_t p = 0; p < plane; ++p) {
                const auto raw = [&](int c) { return __half2float(host[0][c * plane + p]); };
                for (int b = 0; b < 2; ++b) {
                    const float gx = Half(raw(7) + Half(__half2float(host[2][b * 2 * plane + p]) * raw(9)));
                    const float gy = Half(raw(8) + Half(__half2float(host[2][(b * 2 + 1) * plane + p]) * raw(10)));
                    const float u = std::clamp((gx + 1) * .5f * (width - 1), 0.0f, float(width - 1));
                    const float v = std::clamp((gy + 1) * .5f * (height - 1), 0.0f, float(height - 1));
                    const int x = int(u), y = int(v), xx = std::min(x + 1, width - 1), yy = std::min(y + 1, height - 1);
                    const float fx = u - x, fy = v - y;
                    for (int c = 0; c < 7; ++c) {
                        const auto* source = c < 3 ? host[0].data() + (b * 3 + c) * plane : host[1].data() + (b * 4 + c - 3) * plane;
                        const auto at = [&](int row, int col) { return __half2float(source[size_t(row) * width + col]); };
                        const float expected = Half((1 - fx) * (1 - fy) * at(y, x) + fx * (1 - fy) * at(y, xx)
                            + (1 - fx) * fy * at(yy, x) + fx * fy * at(yy, xx));
                        const float error = std::abs(expected - __half2float(result[(b * 7 + c) * plane + p]));
                        Check(std::isfinite(error), "finite warp result"); maximum = std::max(maximum, error); total += error;
                    }
                }
                for (int c = 0; c < 14; ++c) {
                    const __half expected = c == 0 ? host[0][6 * plane + p]
                        : c < 10 ? host[3][(c - 1) * plane + p] : host[4][(c - 10) * plane + p];
                    Check(__half_as_ushort(result[(14 + c) * plane + p]) == __half_as_ushort(expected), "tail copied exactly");
                }
            }
            std::cout << width << 'x' << height << " max=" << maximum << " mean=" << total / (plane * 14) << '\n';
            Check(maximum <= .0015f && total / (plane * 14) < .00002, "independent CPU reference tolerance");
        }
        Cuda(cudaStreamDestroy(stream));
        std::cout << "Fused warp: reference, dynamic shapes and serialization passed\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
