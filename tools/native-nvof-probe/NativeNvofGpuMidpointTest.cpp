/*
 * Driver-only NVIDIA Optical Flow GPU midpoint synthesis test.
 *
 * Reuses the proven NVOF setup and synthetic test pattern from the CPU
 * midpoint probe, but keeps the forward/backward flow textures GPU-resident
 * and performs flow upsampling, backward warping, and blending in a D3D11
 * compute shader.
 */

#define wmain NativeNvofCpuMidpointReferenceMain
#include "NativeNvofMidpointTest.cpp"
#undef wmain

#include <d3dcompiler.h>

namespace {

constexpr UINT ShaderGroupSize = 8;

struct ShaderParameters {
    uint32_t frameWidth;
    uint32_t frameHeight;
    uint32_t flowWidth;
    uint32_t flowHeight;
    float midpointTime;
    float gridSize;
    float padding0;
    float padding1;
};

static_assert(sizeof(ShaderParameters) == 32);

constexpr char MidpointComputeShader[] = R"hlsl(
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

bool PrintHResultFailure(const wchar_t* operation, const HRESULT result)
{
    std::wcerr << operation << L" failed: " << HResultText(result) << L'\n';
    return false;
}

bool CreateShaderResourceView(
    ID3D11Device* device,
    ID3D11Resource* resource,
    ComPtr<ID3D11ShaderResourceView>& view)
{
    const HRESULT result =
        device->CreateShaderResourceView(resource, nullptr, &view);
    if (FAILED(result)) {
        return PrintHResultFailure(L"CreateShaderResourceView", result);
    }
    return true;
}

bool CompileMidpointShader(
    ID3D11Device* device,
    ComPtr<ID3D11ComputeShader>& shader)
{
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> messages;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                       D3DCOMPILE_WARNINGS_ARE_ERRORS |
                       D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT compileResult = D3DCompile(
        MidpointComputeShader, sizeof(MidpointComputeShader) - 1,
        "NativeNvofGpuMidpoint", nullptr, nullptr,
        "main", "cs_5_0", flags, 0, &bytecode, &messages);
    if (FAILED(compileResult)) {
        std::wcerr << L"D3DCompile(midpoint compute shader) failed: "
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
        return PrintHResultFailure(L"CreateComputeShader", createResult);
    }
    return true;
}

bool CreateGpuOutput(
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
        return PrintHResultFailure(L"CreateTexture2D(GPU midpoint)", result);
    }

    result = device->CreateUnorderedAccessView(
        output.Get(), nullptr, &outputView);
    if (FAILED(result)) {
        return PrintHResultFailure(
            L"CreateUnorderedAccessView(GPU midpoint)", result);
    }

    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    result = device->CreateTexture2D(
        &description, nullptr, &staging);
    if (FAILED(result)) {
        return PrintHResultFailure(
            L"CreateTexture2D(GPU midpoint staging)", result);
    }
    return true;
}

bool CreateShaderResources(
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
        return PrintHResultFailure(L"CreateSamplerState", result);
    }

    D3D11_BUFFER_DESC bufferDescription = {};
    bufferDescription.ByteWidth = sizeof(ShaderParameters);
    bufferDescription.Usage = D3D11_USAGE_DEFAULT;
    bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    result = device->CreateBuffer(
        &bufferDescription, nullptr, &parameters);
    if (FAILED(result)) {
        return PrintHResultFailure(L"CreateBuffer(parameters)", result);
    }
    return true;
}

bool DispatchMidpointShader(
    ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader,
    ID3D11Buffer* parameters,
    ID3D11SamplerState* sampler,
    const std::array<ID3D11ShaderResourceView*, 4>& inputs,
    ID3D11UnorderedAccessView* output,
    const uint32_t flowWidth,
    const uint32_t flowHeight)
{
    const ShaderParameters values {
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
        (TestWidth + ShaderGroupSize - 1) / ShaderGroupSize,
        (TestHeight + ShaderGroupSize - 1) / ShaderGroupSize,
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
    return true;
}

bool ReadRgbaTexture(
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
        return PrintHResultFailure(L"Map(GPU midpoint)", result);
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

std::vector<uint8_t> SwapRedBlue(const std::vector<uint8_t>& pixels)
{
    std::vector<uint8_t> swapped = pixels;
    for (size_t offset = 0; offset < swapped.size(); offset += 4) {
        std::swap(swapped[offset + 0], swapped[offset + 2]);
    }
    return swapped;
}

} // namespace

int wmain()
{
#ifndef _WIN64
    std::wcerr << L"This test must be built as a 64-bit executable.\n";
    return 2;
#else
    std::wcout << L"Native NVIDIA Optical Flow GPU midpoint test\n"
               << L"============================================\n";

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

    const std::vector<uint8_t> firstPixels = MakeFirstFrame();
    const std::vector<uint8_t> secondPixels =
        ShiftRight(firstPixels, ShiftPixels);
    const std::vector<uint8_t> expectedBgra =
        ShiftRight(firstPixels, MidpointShiftPixels);
    const std::vector<uint8_t> expectedRgba = SwapRedBlue(expectedBgra);

    ComPtr<ID3D11Texture2D> firstFrame;
    ComPtr<ID3D11Texture2D> secondFrame;
    ComPtr<ID3D11Texture2D> forwardOutput;
    ComPtr<ID3D11Texture2D> forwardStaging;
    ComPtr<ID3D11Texture2D> backwardOutput;
    ComPtr<ID3D11Texture2D> backwardStaging;
    if (!CreateInputTexture(device.Get(), firstPixels, firstFrame) ||
        !CreateInputTexture(device.Get(), secondPixels, secondFrame) ||
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
    std::wcout << L"nvOFExecute: forward/backward submitted\n";

    ComPtr<ID3D11ShaderResourceView> firstView;
    ComPtr<ID3D11ShaderResourceView> secondView;
    ComPtr<ID3D11ShaderResourceView> forwardView;
    ComPtr<ID3D11ShaderResourceView> backwardView;
    if (!CreateShaderResourceView(device.Get(), firstFrame.Get(), firstView) ||
        !CreateShaderResourceView(device.Get(), secondFrame.Get(), secondView) ||
        !CreateShaderResourceView(device.Get(), forwardOutput.Get(), forwardView) ||
        !CreateShaderResourceView(device.Get(), backwardOutput.Get(), backwardView)) {
        return 16;
    }

    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> parameters;
    ComPtr<ID3D11Texture2D> gpuMidpoint;
    ComPtr<ID3D11UnorderedAccessView> gpuMidpointView;
    ComPtr<ID3D11Texture2D> gpuMidpointStaging;
    if (!CompileMidpointShader(device.Get(), shader) ||
        !CreateShaderResources(device.Get(), sampler, parameters) ||
        !CreateGpuOutput(
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
    DispatchMidpointShader(
        context.Get(), shader.Get(), parameters.Get(), sampler.Get(),
        shaderInputs, gpuMidpointView.Get(), flowWidth, flowHeight);
    std::wcout << L"D3D11 compute midpoint: dispatched with GPU-resident flow\n";

    std::vector<uint8_t> actualRgba;
    if (!ReadRgbaTexture(
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

    double forwardMedianX = 0.0;
    double forwardMedianY = 0.0;
    double backwardMedianX = 0.0;
    double backwardMedianY = 0.0;
    FlowStatistics(
        forwardFlow, flowWidth, flowHeight,
        forwardMedianX, forwardMedianY);
    FlowStatistics(
        backwardFlow, flowWidth, flowHeight,
        backwardMedianX, backwardMedianY);

    double meanAbsoluteError = 0.0;
    unsigned percentile95 = 0;
    unsigned maximumError = 0;
    ErrorStatistics(
        actualRgba, expectedRgba,
        meanAbsoluteError, percentile95, maximumError);

    std::wcout << std::fixed << std::setprecision(2)
               << L"Forward flow (B -> A): X=" << forwardMedianX
               << L", Y=" << forwardMedianY << L" px\n"
               << L"Backward flow (A -> B): X=" << backwardMedianX
               << L", Y=" << backwardMedianY << L" px\n"
               << L"Expected GPU midpoint translation: +"
               << MidpointShiftPixels << L" px X\n"
               << L"Safe-region mean absolute error: "
               << meanAbsoluteError << L" / 255\n"
               << L"Safe-region 95th percentile error: "
               << percentile95 << L" / 255\n"
               << L"Safe-region maximum error: "
               << maximumError << L" / 255\n";

    const std::vector<uint8_t> actualBgra = SwapRedBlue(actualRgba);
    const std::vector<uint8_t> difference =
        MakeDifferenceImage(actualRgba, expectedRgba);
    const std::filesystem::path outputDirectory =
        std::filesystem::current_path();
    const std::filesystem::path actualPath =
        outputDirectory / L"NativeNvofGpuMidpoint.bmp";
    const std::filesystem::path expectedPath =
        outputDirectory / L"NativeNvofGpuMidpointExpected.bmp";
    const std::filesystem::path differencePath =
        outputDirectory / L"NativeNvofGpuMidpointDiff.bmp";

    const bool savedActual = SaveBitmap(actualPath, actualBgra);
    const bool savedExpected = SaveBitmap(expectedPath, expectedBgra);
    const bool savedDifference = SaveBitmap(differencePath, difference);
    if (savedActual && savedExpected && savedDifference) {
        std::wcout << L"GPU midpoint: " << actualPath.wstring() << L'\n'
                   << L"Expected midpoint: " << expectedPath.wstring() << L'\n'
                   << L"Amplified difference: "
                   << differencePath.wstring() << L'\n';
    } else {
        std::wcerr << L"Warning: one or more GPU midpoint bitmaps could not be written.\n";
    }

    const bool flowPassed =
        forwardMedianX <= -8.0 && forwardMedianX >= -24.0 &&
        backwardMedianX >= 8.0 && backwardMedianX <= 24.0 &&
        std::abs(forwardMedianY) <= 4.0 &&
        std::abs(backwardMedianY) <= 4.0;
    const bool imagePassed =
        meanAbsoluteError <= 5.0 && percentile95 <= 12;
    const bool passed = flowPassed && imagePassed;

    std::wcout << L"GPU MIDPOINT RESULT: "
               << (passed ? L"PASS" : L"FAIL") << L'\n';
    return passed ? 0 : 20;
#endif
}
