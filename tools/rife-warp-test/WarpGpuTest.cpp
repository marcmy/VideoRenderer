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

constexpr int batches[]{1, 2, 1, 1, 1}, channels[]{11, 4, 4, 9, 4};
constexpr const char* names[]{"raw", "features", "flow", "state", "tail"};

// Independent CPU reference: warp every pixel as the graph does, keeping its FP16
// rounding steps, then apply half_pixel linear Resize at exactly 1/scale.
void TestScale(const int scale, const std::vector<std::pair<int, int>>& sizes, cudaStream_t stream)
{
    std::unique_ptr<IBuilder> builder(createInferBuilder(logger));
    std::unique_ptr<INetworkDefinition> net(builder->createNetworkV2(0));
    std::unique_ptr<IBuilderConfig> config(builder->createBuilderConfig());
    auto* profile = builder->createOptimizationProfile();
    std::array<ITensor*, 5> inputs{};
    for (int i = 0; i < 5; ++i) {
        inputs[i] = net->addInput(names[i], DataType::kHALF, Dims4{batches[i], channels[i], -1, -1});
        Check(profile->setDimensions(names[i], OptProfileSelector::kMIN, Dims4{batches[i], channels[i], scale, scale}), "min");
        Check(profile->setDimensions(names[i], OptProfileSelector::kOPT, Dims4{batches[i], channels[i], 1152, 640}), "opt");
        Check(profile->setDimensions(names[i], OptProfileSelector::kMAX, Dims4{batches[i], channels[i], 1152, 1920}), "max");
    }
    // An unknown graph must fail the matcher without changing the network.
    const int layers = net->getNbLayers();
    Check(!FuseRifeSynthesisWarps(*net) && net->getNbLayers() == layers, "unknown graph preserved");
    auto* creator = static_cast<IPluginCreatorV3One*>(getPluginRegistry()->getCreator("MpcvrRifeWarpConcat", "2", "mpcvr"));
    Check(creator != nullptr, "creator lookup");
    const int32_t invalidScale = 3;
    PluginField invalidField("scale", &invalidScale, PluginFieldType::kINT32, 1);
    PluginFieldCollection invalidFields{1, &invalidField};
    Check(!std::unique_ptr<IPluginV3>(creator->createPlugin("warp", &invalidFields, TensorRTPhase::kBUILD)), "invalid scale rejected");
    const int32_t scaleValue = scale;
    PluginField field("scale", &scaleValue, PluginFieldType::kINT32, 1);
    PluginFieldCollection fields{1, &field};
    std::unique_ptr<IPluginV3> plugin(creator->createPlugin("warp", &fields, TensorRTPhase::kBUILD));
    Check(plugin != nullptr, "plugin creation");
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
    std::mt19937 rng(123 + scale);
    std::uniform_real_distribution<float> values(-1, 1), displacement(-60, 60);
    for (const auto& [width, height] : sizes) {
        const size_t plane = size_t(width) * height;
        const int outputWidth = width / scale, outputHeight = height / scale;
        const size_t outputPlane = size_t(outputWidth) * outputHeight;
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
        DeviceBuffer output(28 * outputPlane * sizeof(__half));
        Check(context->setTensorAddress("output", output.pointer), "output binding");
        Check(context->enqueueV3(stream), "plugin inference"); Cuda(cudaStreamSynchronize(stream));
        std::vector<__half> result(28 * outputPlane);
        Cuda(cudaMemcpy(result.data(), output.pointer, result.size() * sizeof(__half), cudaMemcpyDeviceToHost));
        const int w = width, h = height;
        const auto warpedSample = [&](int b, int c, int px, int py) {
            const size_t p = size_t(py) * w + px;
            const auto raw = [&](int channel) { return __half2float(host[0][channel * plane + p]); };
            const float gx = Half(raw(7) + Half(__half2float(host[2][b * 2 * plane + p]) * raw(9)));
            const float gy = Half(raw(8) + Half(__half2float(host[2][(b * 2 + 1) * plane + p]) * raw(10)));
            const float u = std::clamp((gx + 1) * .5f * (w - 1), 0.0f, float(w - 1));
            const float v = std::clamp((gy + 1) * .5f * (h - 1), 0.0f, float(h - 1));
            const int x = int(u), y = int(v), xx = std::min(x + 1, w - 1), yy = std::min(y + 1, h - 1);
            const float fx = u - x, fy = v - y;
            const auto* source = c < 3 ? host[0].data() + (b * 3 + c) * plane : host[1].data() + (b * 4 + c - 3) * plane;
            const auto at = [&](int row, int col) { return __half2float(source[size_t(row) * w + col]); };
            return Half((1 - fx) * (1 - fy) * at(y, x) + fx * (1 - fy) * at(y, xx)
                + (1 - fx) * fy * at(yy, x) + fx * fy * at(yy, xx));
        };
        const auto tailSample = [&](int c, int px, int py) {
            const size_t p = size_t(py) * w + px;
            return c == 0 ? host[0][6 * plane + p] : c < 10 ? host[3][(c - 1) * plane + p] : host[4][(c - 10) * plane + p];
        };
        // Sample positions from the ONNX half_pixel formula, written independently
        // of the kernel's offsets: x_in = (x_out + 0.5) * scale - 0.5.
        const auto reference = [&](auto&& sample, int outX, int outY) {
            if (scale == 1) return sample(outX, outY);
            const double cx = (outX + .5) * scale - .5, cy = (outY + .5) * scale - .5;
            const int x0 = int(std::floor(cx)), y0 = int(std::floor(cy));
            Check(cx - x0 == .5 && cy - y0 == .5 && x0 + 1 < w && y0 + 1 < h, "half_pixel centre");
            return Half(.25f * (sample(x0, y0) + sample(x0 + 1, y0) + sample(x0, y0 + 1) + sample(x0 + 1, y0 + 1)));
        };
        float warpMaximum = 0, tailMaximum = 0; double total = 0;
        for (int outY = 0; outY < outputHeight; ++outY) for (int outX = 0; outX < outputWidth; ++outX) {
            const size_t p = size_t(outY) * outputWidth + outX;
            for (int b = 0; b < 2; ++b) for (int c = 0; c < 7; ++c) {
                const float expected = reference([&](int x, int y) { return warpedSample(b, c, x, y); }, outX, outY);
                const float error = std::abs(expected - __half2float(result[(b * 7 + c) * outputPlane + p]));
                Check(std::isfinite(error), "finite warp result"); warpMaximum = std::max(warpMaximum, error); total += error;
            }
            for (int c = 0; c < 14; ++c) {
                const float expected = reference([&](int x, int y) { return __half2float(tailSample(c, x, y)); }, outX, outY);
                const float error = std::abs(expected - __half2float(result[(14 + c) * outputPlane + p]));
                Check(std::isfinite(error), "finite tail result"); tailMaximum = std::max(tailMaximum, error);
            }
        }
        std::cout << "scale " << scale << ' ' << width << 'x' << height << " warp max=" << warpMaximum
                  << " mean=" << total / (outputPlane * 14) << " tail max=" << tailMaximum << '\n';
        // Values span [-1,1]. A scale-1 result must match exactly. Downscaled warps
        // round once where the graph rounds each warp and then its resize, so they
        // may differ by one FP16 step, which is 2^-11 just below 1.
        Check(scale > 1 ? warpMaximum <= 1.0f / 2048 : warpMaximum == 0, "warp reference tolerance");
        Check(tailMaximum == 0, "tail reference");
        Check(total / (outputPlane * 14) < 1.0 / 8192, "mean reference tolerance");
    }
    if (scale > 1) {
        // Graph rounding differs for a non-multiple size, so it must not run.
        std::unique_ptr<IExecutionContext> rejected(engine->createExecutionContext());
        std::array<std::unique_ptr<DeviceBuffer>, 5> device;
        const int width = scale * 2 + 1, height = scale * 2;
        bool shaped = true;
        for (int i = 0; i < 5; ++i) {
            shaped = shaped && rejected->setInputShape(names[i], Dims4{batches[i], channels[i], height, width});
            device[i] = std::make_unique<DeviceBuffer>(size_t(width) * height * batches[i] * channels[i] * sizeof(__half));
            rejected->setTensorAddress(names[i], device[i]->pointer);
        }
        DeviceBuffer output(28 * size_t(width) * height * sizeof(__half));
        rejected->setTensorAddress("output", output.pointer);
        Check(!shaped || !rejected->enqueueV3(stream), "non-multiple size rejected");
        cudaGetLastError();
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
        TestScale(1, {{1, 1}, {128, 128}, {257, 129}, {640, 1152}, {1920, 1152}}, stream);
        for (const int scale : {4, 8, 16}) {
            TestScale(scale, {{scale, scale}, {128, 128}, {640, 1152}, {1920, 1152}, {1792, 1024}}, stream);
        }
        Cuda(cudaStreamDestroy(stream));
        std::cout << "Fused warp: reference, dynamic shapes and serialization passed\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
