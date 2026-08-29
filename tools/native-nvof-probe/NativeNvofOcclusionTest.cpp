/*
 * Driver-only NVIDIA Optical Flow layered-motion/occlusion diagnostic.
 *
 * The previous probes use a single full-frame translation. This test adds a
 * textured foreground object moving over a different static background. It
 * keeps NVOF flow GPU-resident for midpoint synthesis, then measures static
 * background, moving-object interior, and occlusion-boundary errors
 * separately. Boundary error is diagnostic and is not required to be zero.
 */

#define wmain NativeNvofCpuMidpointReferenceMain
#include "NativeNvofMidpointTest.cpp"
#undef wmain

#include <d3dcompiler.h>

#include <array>
#include <functional>

namespace {

constexpr UINT OcclusionShaderGroupSize = 8;
constexpr int ObjectStartX = 176;
constexpr int ObjectStartY = 104;
constexpr int ObjectWidth = 208;
constexpr int ObjectHeight = 128;
constexpr int ObjectShiftX = 24;
constexpr int ObjectShiftY = 12;
constexpr int ObjectMidpointX = ObjectStartX + ObjectShiftX / 2;
constexpr int ObjectMidpointY = ObjectStartY + ObjectShiftY / 2;

struct RectI {
    int left;
    int top;
    int right;
    int bottom;

    bool Contains(const int x, const int y, const int inset = 0) const
    {
        return x >= left + inset && x < right - inset &&
               y >= top + inset && y < bottom - inset;
    }

    RectI Expanded(const int amount) const
    {
        return RectI {
            left - amount,
            top - amount,
            right + amount,
            bottom + amount,
        };
    }
};

constexpr RectI ObjectA {
    ObjectStartX,
    ObjectStartY,
    ObjectStartX + ObjectWidth,
    ObjectStartY + ObjectHeight,
};
constexpr RectI ObjectB {
    ObjectStartX + ObjectShiftX,
    ObjectStartY + ObjectShiftY,
    ObjectStartX + ObjectShiftX + ObjectWidth,
    ObjectStartY + ObjectShiftY + ObjectHeight,
};
constexpr RectI ObjectMidpoint {
    ObjectMidpointX,
    ObjectMidpointY,
    ObjectMidpointX + ObjectWidth,
    ObjectMidpointY + ObjectHeight,
};
constexpr RectI BackgroundProbeRegion { 40, 40, 144, 88 };
constexpr RectI MotionEnvelope {
    ObjectStartX,
    ObjectStartY,
    ObjectStartX + ObjectShiftX + ObjectWidth,
    ObjectStartY + ObjectShiftY + ObjectHeight,
};

struct OcclusionShaderParameters {
    uint32_t frameWidth;
    uint32_t frameHeight;
    uint32_t flowWidth;
    uint32_t flowHeight;
    float midpointTime;
    float gridSize;
    float padding0;
    float padding1;
};

static_assert(sizeof(OcclusionShaderParameters) == 32);

struct ErrorSummary {
    double meanAbsoluteError = 255.0;
    unsigned percentile95 = 255;
    unsigned maximumError = 255;
    size_t sampleCount = 0;
};

constexpr char OcclusionComputeShader[] = R"hlsl(
cbuffer Parameters : register(b0)
{
    uint2 FrameSize;
    uint2 FlowSize;
    float MidpointTime;
    float GridSize;
    float2 Padding;
};

Texture2D<float4> FirstFrame : register(t0);
Texture2D<float4> SecondFrame : register(t1);
Texture2D<int2> ForwardFlow : register(t2);
Texture2D<int2> BackwardFlow : register(t3);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> OutputFrame : register(u0);

int2 LoadFlowVector(bool backward, int2 coordinate)
{
    return backward
        ? BackwardFlow.Load(int3(coordinate, 0))
        : ForwardFlow.Load(int3(coordinate, 0));
}

float2 SampleFlow(bool backward, float2 pixel)
{
    float2 grid = pixel / GridSize;
    grid = clamp(grid, 0.0, float2(FlowSize - 1));

    int2 p0 = int2(floor(grid));
    int2 p1 = min(p0 + 1, int2(FlowSize - 1));
    float2 fraction = grid - float2(p0);

    float2 f00 = float2(LoadFlowVector(backward, int2(p0.x, p0.y))) / 32.0;
    float2 f10 = float2(LoadFlowVector(backward, int2(p1.x, p0.y))) / 32.0;
    float2 f01 = float2(LoadFlowVector(backward, int2(p0.x, p1.y))) / 32.0;
    float2 f11 = float2(LoadFlowVector(backward, int2(p1.x, p1.y))) / 32.0;

    float2 top = lerp(f00, f10, fraction.x);
    float2 bottom = lerp(f01, f11, fraction.x);
    return lerp(top, bottom, fraction.y);
}

bool IsValid(float2 position)
{
    return all(position >= 0.0) &&
           all(position <= float2(FrameSize - 1));
}

float4 SampleFrame(Texture2D<float4> frame, float2 position)
{
    float2 uv = (position + 0.5) / float2(FrameSize);
    return frame.SampleLevel(LinearClamp, uv, 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= FrameSize)) {
        return;
    }

    float2 pixel = float2(dispatchThreadId.xy);
    float2 inputToReference = SampleFlow(false, pixel);
    float2 referenceToInput = SampleFlow(true, pixel);

    float2 firstPosition = pixel - MidpointTime * referenceToInput;
    float2 secondPosition = pixel - MidpointTime * inputToReference;
    bool firstValid = IsValid(firstPosition);
    bool secondValid = IsValid(secondPosition);

    float4 result = float4(0.0, 0.0, 0.0, 1.0);
    if (firstValid && secondValid) {
        result = 0.5 * (
            SampleFrame(FirstFrame, firstPosition) +
            SampleFrame(SecondFrame, secondPosition));
    } else if (firstValid) {
        result = SampleFrame(FirstFrame, firstPosition);
    } else if (secondValid) {
        result = SampleFrame(SecondFrame, secondPosition);
    }

    OutputFrame[dispatchThreadId.xy] = result;
}
)hlsl";

bool OcclusionHResultFailure(
    const wchar_t* operation,
    const HRESULT result)
{
    std::wcerr << operation << L" failed: " << HResultText(result) << L'\n';
    return false;
}

std::vector<uint8_t> MakeLayeredFrame(
    const int objectX,
    const int objectY)
{
    std::vector<uint8_t> pixels(
        static_cast<size_t>(TestWidth) * TestHeight * 4);

    for (uint32_t y = 0; y < TestHeight; ++y) {
        for (uint32_t x = 0; x < TestWidth; ++x) {
            const uint32_t hash = PatternHash(x + 73u, y + 29u);
            const bool checker = (((x / 24) ^ (y / 24)) & 1u) != 0;
            const size_t offset =
                (static_cast<size_t>(y) * TestWidth + x) * 4;

            pixels[offset + 0] = static_cast<uint8_t>(
                24u + (hash & 0x3fu) + (checker ? 32u : 0u));
            pixels[offset + 1] = static_cast<uint8_t>(
                36u + ((hash >> 8) & 0x4fu));
            pixels[offset + 2] = static_cast<uint8_t>(
                28u + ((hash >> 16) & 0x3fu) + (checker ? 0u : 28u));
            pixels[offset + 3] = 255;
        }
    }

    for (int localY = 0; localY < ObjectHeight; ++localY) {
        const int y = objectY + localY;
        for (int localX = 0; localX < ObjectWidth; ++localX) {
            const int x = objectX + localX;
            if (x < 0 || y < 0 ||
                x >= static_cast<int>(TestWidth) ||
                y >= static_cast<int>(TestHeight)) {
                continue;
            }

            const uint32_t hash = PatternHash(
                static_cast<uint32_t>(localX),
                static_cast<uint32_t>(localY));
            const bool stripe =
                (((localX / 12) + (localY / 10)) & 1) != 0;
            const bool border =
                localX < 4 || localY < 4 ||
                localX >= ObjectWidth - 4 ||
                localY >= ObjectHeight - 4;
            const size_t offset =
                (static_cast<size_t>(y) * TestWidth +
                 static_cast<uint32_t>(x)) * 4;

            pixels[offset + 0] = border ? 250 : static_cast<uint8_t>(
                112u + (hash & 0x6fu));
            pixels[offset + 1] = border ? 40 : static_cast<uint8_t>(
                44u + ((hash >> 8) & 0x6fu) + (stripe ? 64u : 0u));
            pixels[offset + 2] = border ? 230 : static_cast<uint8_t>(
                104u + ((hash >> 16) & 0x6fu) + (stripe ? 0u : 32u));
            pixels[offset + 3] = 255;
        }
    }

    return pixels;
}

bool CreateOcclusionShaderResourceView(
    ID3D11Device* device,
    ID3D11Resource* resource,
    ComPtr<ID3D11ShaderResourceView>& view)
{
    const HRESULT result =
        device->CreateShaderResourceView(resource, nullptr, &view);
    if (FAILED(result)) {
        return OcclusionHResultFailure(
            L"CreateShaderResourceView", result);
    }
    return true;
}

bool CompileOcclusionShader(
    ID3D11Device* device,
    ComPtr<ID3D11ComputeShader>& shader)
{
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> messages;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                       D3DCOMPILE_WARNINGS_ARE_ERRORS |
                       D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT compileResult = D3DCompile(
        OcclusionComputeShader, sizeof(OcclusionComputeShader) - 1,
        "NativeNvofOcclusion", nullptr, nullptr,
        "main", "cs_5_0", flags, 0, &bytecode, &messages);
    if (FAILED(compileResult)) {
        std::wcerr << L"D3DCompile(occlusion compute shader) failed: "
                   << HResultText(compileResult) << L'\n';
        if (messages && messages->GetBufferPointer()) {
            const auto* text = static_cast<const char*>(
                messages->GetBufferPointer());
            const size_t length = messages->GetBufferSize();
            for (size_t index = 0; index < length; ++index) {
                std::wcerr << static_cast<wchar_t>(
                    static_cast<unsigned char>(text[index]));
            }
            std::wcerr << L'\n';
        }
        return false;
    }

    const HRESULT createResult = device->CreateComputeShader(
        bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
        nullptr, &shader);
    if (FAILED(createResult)) {
        return OcclusionHResultFailure(
            L"CreateComputeShader", createResult);
    }
    return true;
}

bool CreateOcclusionGpuOutput(
    ID3D11Device* device,
    ComPtr<ID3D11Texture2D>& output,
    ComPtr<ID3D11UnorderedAccessView>& outputView,
    ComPtr<ID3D11Texture2D>& staging)
{
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = TestWidth;
    description.Height = TestHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS |
                            D3D11_BIND_SHADER_RESOURCE;

    HRESULT result = device->CreateTexture2D(
        &description, nullptr, &output);
    if (FAILED(result)) {
        return OcclusionHResultFailure(
            L"CreateTexture2D(occlusion midpoint)", result);
    }

    result = device->CreateUnorderedAccessView(
        output.Get(), nullptr, &outputView);
    if (FAILED(result)) {
        return OcclusionHResultFailure(
            L"CreateUnorderedAccessView(occlusion midpoint)", result);
    }

    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    result = device->CreateTexture2D(
        &description, nullptr, &staging);
    if (FAILED(result)) {
        return OcclusionHResultFailure(
            L"CreateTexture2D(occlusion staging)", result);
    }
    return true;
}

bool CreateOcclusionShaderResources(
    ID3D11Device* device,
    ComPtr<ID3D11SamplerState>& sampler,
    ComPtr<ID3D11Buffer>& parameters)
{
    D3D11_SAMPLER_DESC samplerDescription = {};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT result = device->CreateSamplerState(
        &samplerDescription, &sampler);
    if (FAILED(result)) {
        return OcclusionHResultFailure(L"CreateSamplerState", result);
    }

    D3D11_BUFFER_DESC bufferDescription = {};
    bufferDescription.ByteWidth = sizeof(OcclusionShaderParameters);
    bufferDescription.Usage = D3D11_USAGE_DEFAULT;
    bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    result = device->CreateBuffer(
        &bufferDescription, nullptr, &parameters);
    if (FAILED(result)) {
        return OcclusionHResultFailure(
            L"CreateBuffer(occlusion parameters)", result);
    }
    return true;
}

void DispatchOcclusionShader(
    ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader,
    ID3D11Buffer* parameters,
    ID3D11SamplerState* sampler,
    const std::array<ID3D11ShaderResourceView*, 4>& inputs,
    ID3D11UnorderedAccessView* output,
    const uint32_t flowWidth,
    const uint32_t flowHeight)
{
    const OcclusionShaderParameters values {
        TestWidth,
        TestHeight,
        flowWidth,
        flowHeight,
        MidpointTime,
        static_cast<float>(GridSize),
        0.0f,
        0.0f,
    };
    context->UpdateSubresource(
        parameters, 0, nullptr, &values, 0, 0);

    context->CSSetShader(shader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &parameters);
    context->CSSetSamplers(0, 1, &sampler);
    context->CSSetShaderResources(
        0, static_cast<UINT>(inputs.size()), inputs.data());
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch(
        (TestWidth + OcclusionShaderGroupSize - 1) /
            OcclusionShaderGroupSize,
        (TestHeight + OcclusionShaderGroupSize - 1) /
            OcclusionShaderGroupSize,
        1);

    ID3D11UnorderedAccessView* nullOutput = nullptr;
    const std::array<ID3D11ShaderResourceView*, 4> nullInputs = {};
    ID3D11Buffer* nullBuffer = nullptr;
    ID3D11SamplerState* nullSampler = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &nullOutput, nullptr);
    context->CSSetShaderResources(
        0, static_cast<UINT>(nullInputs.size()), nullInputs.data());
    context->CSSetConstantBuffers(0, 1, &nullBuffer);
    context->CSSetSamplers(0, 1, &nullSampler);
    context->CSSetShader(nullptr, nullptr, 0);
}

bool ReadOcclusionRgbaTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* output,
    ID3D11Texture2D* staging,
    std::vector<uint8_t>& pixels)
{
    context->CopyResource(staging, output);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT result =
        context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        return OcclusionHResultFailure(
            L"Map(occlusion midpoint)", result);
    }

    pixels.resize(static_cast<size_t>(TestWidth) * TestHeight * 4);
    for (uint32_t y = 0; y < TestHeight; ++y) {
        const auto* source = static_cast<const uint8_t*>(mapped.pData) +
            static_cast<size_t>(y) * mapped.RowPitch;
        std::copy_n(
            source, static_cast<size_t>(TestWidth) * 4,
            pixels.data() + static_cast<size_t>(y) * TestWidth * 4);
    }
    context->Unmap(staging, 0);
    return true;
}

std::vector<uint8_t> OcclusionSwapRedBlue(
    const std::vector<uint8_t>& pixels)
{
    std::vector<uint8_t> swapped = pixels;
    for (size_t offset = 0; offset < swapped.size(); offset += 4) {
        std::swap(swapped[offset + 0], swapped[offset + 2]);
    }
    return swapped;
}

void FlowRegionStatistics(
    const std::vector<nvof::FlowVector>& flow,
    const uint32_t flowWidth,
    const uint32_t flowHeight,
    const RectI& region,
    const int inset,
    double& medianX,
    double& medianY)
{
    std::vector<double> xValues;
    std::vector<double> yValues;

    for (uint32_t y = 0; y < flowHeight; ++y) {
        for (uint32_t x = 0; x < flowWidth; ++x) {
            const int pixelX = static_cast<int>(x * GridSize + GridSize / 2);
            const int pixelY = static_cast<int>(y * GridSize + GridSize / 2);
            if (!region.Contains(pixelX, pixelY, inset)) {
                continue;
            }

            const nvof::FlowVector& vector =
                flow[static_cast<size_t>(y) * flowWidth + x];
            xValues.push_back(static_cast<double>(vector.x) / 32.0);
            yValues.push_back(static_cast<double>(vector.y) / 32.0);
        }
    }

    medianX = Median(std::move(xValues));
    medianY = Median(std::move(yValues));
}

ErrorSummary CalculateRegionError(
    const std::vector<uint8_t>& actual,
    const std::vector<uint8_t>& expected,
    const std::function<bool(int, int)>& includePixel)
{
    ErrorSummary summary;
    std::vector<unsigned> errors;
    uint64_t totalError = 0;

    for (uint32_t y = 0; y < TestHeight; ++y) {
        for (uint32_t x = 0; x < TestWidth; ++x) {
            if (!includePixel(static_cast<int>(x), static_cast<int>(y))) {
                continue;
            }

            const size_t offset =
                (static_cast<size_t>(y) * TestWidth + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel) {
                const unsigned error = static_cast<unsigned>(std::abs(
                    static_cast<int>(actual[offset + channel]) -
                    static_cast<int>(expected[offset + channel])));
                errors.push_back(error);
                totalError += error;
            }
        }
    }

    summary.sampleCount = errors.size();
    if (errors.empty()) {
        return summary;
    }

    std::sort(errors.begin(), errors.end());
    summary.meanAbsoluteError = static_cast<double>(totalError) /
        static_cast<double>(errors.size());
    const size_t percentileIndex = std::min(
        errors.size() - 1,
        (errors.size() * 95) / 100);
    summary.percentile95 = errors[percentileIndex];
    summary.maximumError = errors.back();
    return summary;
}

std::vector<uint8_t> MakeRegionMap()
{
    std::vector<uint8_t> pixels(
        static_cast<size_t>(TestWidth) * TestHeight * 4, 0);
    const RectI expandedEnvelope = MotionEnvelope.Expanded(28);

    for (uint32_t y = 0; y < TestHeight; ++y) {
        for (uint32_t x = 0; x < TestWidth; ++x) {
            const int pixelX = static_cast<int>(x);
            const int pixelY = static_cast<int>(y);
            const bool objectInterior =
                ObjectMidpoint.Contains(pixelX, pixelY, 24);
            const bool boundary =
                expandedEnvelope.Contains(pixelX, pixelY) &&
                !objectInterior;
            const bool stableBackground = !boundary && !objectInterior;
            const size_t offset =
                (static_cast<size_t>(y) * TestWidth + x) * 4;

            pixels[offset + 0] = objectInterior ? 255 : 0;
            pixels[offset + 1] = stableBackground ? 190 : 0;
            pixels[offset + 2] = boundary ? 255 : 0;
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}

std::vector<uint8_t> MakeConsistencyMap(
    const std::vector<nvof::FlowVector>& forwardFlow,
    const std::vector<nvof::FlowVector>& backwardFlow,
    const uint32_t flowWidth,
    const uint32_t flowHeight,
    double& medianConsistency,
    double& percentile95Consistency)
{
    std::vector<uint8_t> pixels(
        static_cast<size_t>(TestWidth) * TestHeight * 4, 0);
    std::vector<double> consistencyValues;
    consistencyValues.reserve(
        static_cast<size_t>(TestWidth) * TestHeight);

    for (uint32_t y = 0; y < TestHeight; ++y) {
        for (uint32_t x = 0; x < TestWidth; ++x) {
            const Float2 forward = SampleFlow(
                forwardFlow, flowWidth, flowHeight,
                static_cast<float>(x), static_cast<float>(y));
            const float mappedX = static_cast<float>(x) + forward.x;
            const float mappedY = static_cast<float>(y) + forward.y;
            const Float2 backward = SampleFlow(
                backwardFlow, flowWidth, flowHeight,
                mappedX, mappedY);
            const double consistency = std::hypot(
                static_cast<double>(forward.x + backward.x),
                static_cast<double>(forward.y + backward.y));
            consistencyValues.push_back(consistency);

            const uint8_t intensity = static_cast<uint8_t>(std::lround(
                std::clamp(consistency * 24.0, 0.0, 255.0)));
            const size_t offset =
                (static_cast<size_t>(y) * TestWidth + x) * 4;
            pixels[offset + 0] = intensity;
            pixels[offset + 1] = intensity;
            pixels[offset + 2] = intensity;
            pixels[offset + 3] = 255;
        }
    }

    medianConsistency = Median(consistencyValues);
    std::sort(consistencyValues.begin(), consistencyValues.end());
    const size_t index = std::min(
        consistencyValues.size() - 1,
        (consistencyValues.size() * 95) / 100);
    percentile95Consistency = consistencyValues[index];
    return pixels;
}

void PrintErrorSummary(
    const wchar_t* name,
    const ErrorSummary& summary)
{
    std::wcout << name << L": MAE=" << summary.meanAbsoluteError
               << L", P95=" << summary.percentile95
               << L", max=" << summary.maximumError
               << L" / 255\n";
}

} // namespace

int wmain()
{
#ifndef _WIN64
    std::wcerr << L"This test must be built as a 64-bit executable.\n";
    return 2;
#else
    std::wcout << L"Native NVIDIA Optical Flow occlusion diagnostic\n"
               << L"================================================\n";

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::wstring adapterName;
    if (!CreateDevice(device, context, adapterName)) {
        return 3;
    }
    std::wcout << L"Adapter: " << adapterName << L'\n';

    ModuleGuard module { LoadDriverModule() };
    if (!module.module) {
        std::wcerr << L"Could not load System32\\nvofapi64.dll.\n";
        return 4;
    }

    const auto getVersion =
        reinterpret_cast<nvof::GetMaxSupportedApiVersionFn>(
            GetProcAddress(module.module, "NvOFGetMaxSupportedApiVersion"));
    const auto createInstance =
        reinterpret_cast<nvof::CreateInstanceD3D11Fn>(
            GetProcAddress(module.module, "NvOFAPICreateInstanceD3D11"));
    if (!getVersion || !createInstance) {
        std::wcerr << L"Required NVOF exports are missing.\n";
        return 5;
    }

    uint32_t driverVersion = 0;
    nvof::Status status = getVersion(&driverVersion);
    if (status != nvof::Success || driverVersion < nvof::ApiVersion50) {
        std::wcerr << L"The installed driver does not support NVOF API 5.0.\n";
        return 6;
    }
    std::wcout << L"Driver NVOF API: " << (driverVersion >> 4)
               << L'.' << (driverVersion & 0x0f) << L'\n';

    nvof::D3D11FunctionList api = {};
    status = createInstance(nvof::ApiVersion50, &api);
    if (status != nvof::Success || !api.createOpticalFlowD3D11 ||
        !api.initialize || !api.registerResourceD3D11 ||
        !api.unregisterResourceD3D11 || !api.execute || !api.destroy) {
        std::wcerr << L"Could not create the NVOF D3D11 function table.\n";
        return 7;
    }

    SessionGuard session;
    session.destroy = api.destroy;
    status = api.createOpticalFlowD3D11(
        device.Get(), context.Get(), &session.handle);
    if (status != nvof::Success || !session.handle) {
        PrintFailure(L"createOpticalFlowD3D11", status, api, session.handle);
        return 8;
    }
    std::wcout << L"D3D11 NVOF session: created\n";

    nvof::InitParams init = {};
    init.width = TestWidth;
    init.height = TestHeight;
    init.outputGridSize = nvof::OutputGrid4;
    init.hintGridSize = nvof::HintGridUndefined;
    init.mode = nvof::ModeOpticalFlow;
    init.performance = nvof::PerfSlow;
    init.enableExternalHints = nvof::False;
    init.enableOutputCost = nvof::False;
    init.disparityRange = nvof::StereoRangeUndefined;
    init.enableRoi = nvof::False;
    init.predictionDirection = nvof::PredictionBoth;
    init.enableGlobalFlow = nvof::False;
    init.inputBufferFormat = nvof::BufferFormatAbgr8;

    status = api.initialize(session.handle, &init);
    if (status != nvof::Success) {
        PrintFailure(L"nvOFInit(forward/backward)", status, api, session.handle);
        return 9;
    }

    const uint32_t flowWidth = (TestWidth + GridSize - 1) / GridSize;
    const uint32_t flowHeight = (TestHeight + GridSize - 1) / GridSize;
    std::wcout << L"NVOF initialized: " << TestWidth << L'x' << TestHeight
               << L", 4x4 grid, forward + backward "
               << flowWidth << L'x' << flowHeight << L'\n';

    const std::vector<uint8_t> firstBgra =
        MakeLayeredFrame(ObjectStartX, ObjectStartY);
    const std::vector<uint8_t> secondBgra = MakeLayeredFrame(
        ObjectStartX + ObjectShiftX,
        ObjectStartY + ObjectShiftY);
    const std::vector<uint8_t> expectedBgra = MakeLayeredFrame(
        ObjectMidpointX, ObjectMidpointY);
    const std::vector<uint8_t> expectedRgba =
        OcclusionSwapRedBlue(expectedBgra);

    ComPtr<ID3D11Texture2D> firstFrame;
    ComPtr<ID3D11Texture2D> secondFrame;
    ComPtr<ID3D11Texture2D> forwardOutput;
    ComPtr<ID3D11Texture2D> forwardStaging;
    ComPtr<ID3D11Texture2D> backwardOutput;
    ComPtr<ID3D11Texture2D> backwardStaging;
    if (!CreateInputTexture(device.Get(), firstBgra, firstFrame) ||
        !CreateInputTexture(device.Get(), secondBgra, secondFrame) ||
        !CreateFlowTextures(
            device.Get(), flowWidth, flowHeight,
            forwardOutput, forwardStaging) ||
        !CreateFlowTextures(
            device.Get(), flowWidth, flowHeight,
            backwardOutput, backwardStaging)) {
        return 10;
    }

    ResourceGuard firstResource;
    ResourceGuard secondResource;
    ResourceGuard forwardResource;
    ResourceGuard backwardResource;
    firstResource.unregisterResource = api.unregisterResourceD3D11;
    secondResource.unregisterResource = api.unregisterResourceD3D11;
    forwardResource.unregisterResource = api.unregisterResourceD3D11;
    backwardResource.unregisterResource = api.unregisterResourceD3D11;

    status = api.registerResourceD3D11(
        session.handle, firstFrame.Get(), &firstResource.handle);
    if (status != nvof::Success) {
        PrintFailure(L"register first frame", status, api, session.handle);
        return 11;
    }
    status = api.registerResourceD3D11(
        session.handle, secondFrame.Get(), &secondResource.handle);
    if (status != nvof::Success) {
        PrintFailure(L"register second frame", status, api, session.handle);
        return 12;
    }
    status = api.registerResourceD3D11(
        session.handle, forwardOutput.Get(), &forwardResource.handle);
    if (status != nvof::Success) {
        PrintFailure(L"register forward output", status, api, session.handle);
        return 13;
    }
    status = api.registerResourceD3D11(
        session.handle, backwardOutput.Get(), &backwardResource.handle);
    if (status != nvof::Success) {
        PrintFailure(L"register backward output", status, api, session.handle);
        return 14;
    }
    std::wcout << L"D3D11 resources: registered\n";

    nvof::ExecuteInputParams input = {};
    input.inputFrame = secondResource.handle;
    input.referenceFrame = firstResource.handle;
    input.disableTemporalHints = nvof::True;

    nvof::ExecuteOutputParams output = {};
    output.outputBuffer = forwardResource.handle;
    output.backwardOutputBuffer = backwardResource.handle;

    status = api.execute(session.handle, &input, &output);
    if (status != nvof::Success) {
        PrintFailure(L"nvOFExecute(forward/backward)", status, api, session.handle);
        return 15;
    }
    std::wcout << L"nvOFExecute: layered forward/backward submitted\n";

    ComPtr<ID3D11ShaderResourceView> firstView;
    ComPtr<ID3D11ShaderResourceView> secondView;
    ComPtr<ID3D11ShaderResourceView> forwardView;
    ComPtr<ID3D11ShaderResourceView> backwardView;
    if (!CreateOcclusionShaderResourceView(
            device.Get(), firstFrame.Get(), firstView) ||
        !CreateOcclusionShaderResourceView(
            device.Get(), secondFrame.Get(), secondView) ||
        !CreateOcclusionShaderResourceView(
            device.Get(), forwardOutput.Get(), forwardView) ||
        !CreateOcclusionShaderResourceView(
            device.Get(), backwardOutput.Get(), backwardView)) {
        return 16;
    }

    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> parameters;
    ComPtr<ID3D11Texture2D> gpuMidpoint;
    ComPtr<ID3D11UnorderedAccessView> gpuMidpointView;
    ComPtr<ID3D11Texture2D> gpuMidpointStaging;
    if (!CompileOcclusionShader(device.Get(), shader) ||
        !CreateOcclusionShaderResources(
            device.Get(), sampler, parameters) ||
        !CreateOcclusionGpuOutput(
            device.Get(), gpuMidpoint,
            gpuMidpointView, gpuMidpointStaging)) {
        return 17;
    }

    const std::array<ID3D11ShaderResourceView*, 4> shaderInputs = {
        firstView.Get(),
        secondView.Get(),
        forwardView.Get(),
        backwardView.Get(),
    };
    DispatchOcclusionShader(
        context.Get(), shader.Get(), parameters.Get(), sampler.Get(),
        shaderInputs, gpuMidpointView.Get(), flowWidth, flowHeight);
    std::wcout << L"D3D11 compute midpoint: layered scene dispatched\n";

    std::vector<uint8_t> actualRgba;
    if (!ReadOcclusionRgbaTexture(
            context.Get(), gpuMidpoint.Get(),
            gpuMidpointStaging.Get(), actualRgba)) {
        return 18;
    }

    std::vector<nvof::FlowVector> forwardFlow;
    std::vector<nvof::FlowVector> backwardFlow;
    if (!ReadFlowTexture(
            context.Get(), forwardOutput.Get(), forwardStaging.Get(),
            flowWidth, flowHeight, forwardFlow) ||
        !ReadFlowTexture(
            context.Get(), backwardOutput.Get(), backwardStaging.Get(),
            flowWidth, flowHeight, backwardFlow)) {
        return 19;
    }

    double forwardObjectX = 0.0;
    double forwardObjectY = 0.0;
    double backwardObjectX = 0.0;
    double backwardObjectY = 0.0;
    double forwardBackgroundX = 0.0;
    double forwardBackgroundY = 0.0;
    double backwardBackgroundX = 0.0;
    double backwardBackgroundY = 0.0;
    FlowRegionStatistics(
        forwardFlow, flowWidth, flowHeight,
        ObjectB, 28, forwardObjectX, forwardObjectY);
    FlowRegionStatistics(
        backwardFlow, flowWidth, flowHeight,
        ObjectA, 28, backwardObjectX, backwardObjectY);
    FlowRegionStatistics(
        forwardFlow, flowWidth, flowHeight,
        BackgroundProbeRegion, 0,
        forwardBackgroundX, forwardBackgroundY);
    FlowRegionStatistics(
        backwardFlow, flowWidth, flowHeight,
        BackgroundProbeRegion, 0,
        backwardBackgroundX, backwardBackgroundY);

    const RectI expandedEnvelope = MotionEnvelope.Expanded(28);
    const ErrorSummary stableBackgroundError = CalculateRegionError(
        actualRgba, expectedRgba,
        [&](const int x, const int y) {
            return !expandedEnvelope.Contains(x, y);
        });
    const ErrorSummary objectInteriorError = CalculateRegionError(
        actualRgba, expectedRgba,
        [&](const int x, const int y) {
            return ObjectMidpoint.Contains(x, y, 24);
        });
    const ErrorSummary boundaryError = CalculateRegionError(
        actualRgba, expectedRgba,
        [&](const int x, const int y) {
            return expandedEnvelope.Contains(x, y) &&
                   !ObjectMidpoint.Contains(x, y, 24);
        });

    double medianConsistency = 0.0;
    double percentile95Consistency = 0.0;
    const std::vector<uint8_t> consistencyMap = MakeConsistencyMap(
        forwardFlow, backwardFlow, flowWidth, flowHeight,
        medianConsistency, percentile95Consistency);

    std::wcout << std::fixed << std::setprecision(2)
               << L"Known object translation A -> B: X=+"
               << ObjectShiftX << L", Y=+" << ObjectShiftY << L" px\n"
               << L"Object forward flow (B -> A): X="
               << forwardObjectX << L", Y=" << forwardObjectY << L" px\n"
               << L"Object backward flow (A -> B): X="
               << backwardObjectX << L", Y=" << backwardObjectY << L" px\n"
               << L"Static background forward flow: X="
               << forwardBackgroundX << L", Y="
               << forwardBackgroundY << L" px\n"
               << L"Static background backward flow: X="
               << backwardBackgroundX << L", Y="
               << backwardBackgroundY << L" px\n";
    PrintErrorSummary(L"Stable background synthesis", stableBackgroundError);
    PrintErrorSummary(L"Moving object interior synthesis", objectInteriorError);
    PrintErrorSummary(L"Occlusion/boundary synthesis (diagnostic)", boundaryError);
    std::wcout << L"Forward/backward consistency: median="
               << medianConsistency << L" px, P95="
               << percentile95Consistency << L" px\n";

    const std::vector<uint8_t> actualBgra =
        OcclusionSwapRedBlue(actualRgba);
    const std::vector<uint8_t> difference =
        MakeDifferenceImage(actualRgba, expectedRgba);
    const std::vector<uint8_t> regionMap = MakeRegionMap();
    const std::filesystem::path outputDirectory =
        std::filesystem::current_path();
    const std::filesystem::path actualPath =
        outputDirectory / L"NativeNvofOcclusionMidpoint.bmp";
    const std::filesystem::path expectedPath =
        outputDirectory / L"NativeNvofOcclusionExpected.bmp";
    const std::filesystem::path differencePath =
        outputDirectory / L"NativeNvofOcclusionDiff.bmp";
    const std::filesystem::path regionPath =
        outputDirectory / L"NativeNvofOcclusionRegions.bmp";
    const std::filesystem::path consistencyPath =
        outputDirectory / L"NativeNvofOcclusionConsistency.bmp";

    const bool saved =
        SaveBitmap(actualPath, actualBgra) &&
        SaveBitmap(expectedPath, expectedBgra) &&
        SaveBitmap(differencePath, difference) &&
        SaveBitmap(regionPath, regionMap) &&
        SaveBitmap(consistencyPath, consistencyMap);
    if (saved) {
        std::wcout << L"Layered GPU midpoint: " << actualPath.wstring() << L'\n'
                   << L"Exact expected midpoint: "
                   << expectedPath.wstring() << L'\n'
                   << L"Amplified difference: "
                   << differencePath.wstring() << L'\n'
                   << L"Evaluation regions: "
                   << regionPath.wstring() << L'\n'
                   << L"Flow consistency map: "
                   << consistencyPath.wstring() << L'\n';
    } else {
        std::wcerr << L"Warning: one or more occlusion bitmaps could not be written.\n";
    }

    const bool objectFlowPassed =
        std::abs(forwardObjectX + ObjectShiftX) <= 6.0 &&
        std::abs(forwardObjectY + ObjectShiftY) <= 6.0 &&
        std::abs(backwardObjectX - ObjectShiftX) <= 6.0 &&
        std::abs(backwardObjectY - ObjectShiftY) <= 6.0;
    const bool backgroundFlowPassed =
        std::abs(forwardBackgroundX) <= 2.0 &&
        std::abs(forwardBackgroundY) <= 2.0 &&
        std::abs(backwardBackgroundX) <= 2.0 &&
        std::abs(backwardBackgroundY) <= 2.0;
    const bool stableSynthesisPassed =
        stableBackgroundError.meanAbsoluteError <= 4.0 &&
        stableBackgroundError.percentile95 <= 10 &&
        objectInteriorError.meanAbsoluteError <= 12.0 &&
        objectInteriorError.percentile95 <= 32;
    const bool passed =
        objectFlowPassed && backgroundFlowPassed && stableSynthesisPassed;

    std::wcout << L"OCCLUSION DIAGNOSTIC: "
               << (passed ? L"PASS" : L"FAIL") << L'\n';
    return passed ? 0 : 20;
#endif
}
