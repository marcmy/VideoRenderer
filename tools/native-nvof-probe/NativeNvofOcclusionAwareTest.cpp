/*
 * Driver-only NVIDIA Optical Flow occlusion-aware midpoint test.
 *
 * Builds on NativeNvofOcclusionTest.cpp. The v5 baseline performs one
 * midpoint flow lookup and always blends two valid samples 50/50. This test
 * keeps the same layered scene and GPU-resident NVOF textures, but evaluates
 * multiple inverse-warp hypotheses for each source. Projection residual,
 * forward/backward agreement, and cross-frame photometric agreement select
 * the visible source when the two warps disagree.
 */

#define wmain NativeNvofOcclusionReferenceMain
#include "NativeNvofOcclusionTest.cpp"
#undef wmain

namespace {

constexpr char OcclusionAwareComputeShader[] = R"hlsl(
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
Texture2D<int2> ForwardFlow : register(t2);   // B -> A
Texture2D<int2> BackwardFlow : register(t3);  // A -> B
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> OutputFrame : register(u0);
RWTexture2D<float4> SelectionMap : register(u1);

struct Candidate
{
    float2 position;
    float4 color;
    float score;
    float projectionError;
    float consistencyError;
    float photometricError;
    float valid;
};

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

float ColorDifference(float4 first, float4 second)
{
    float3 difference = abs(first.rgb - second.rgb);
    return dot(difference, float3(0.299, 0.587, 0.114));
}

Candidate InvalidCandidate()
{
    Candidate candidate;
    candidate.position = 0.0;
    candidate.color = float4(0.0, 0.0, 0.0, 1.0);
    candidate.score = 10000.0;
    candidate.projectionError = 1000.0;
    candidate.consistencyError = 1000.0;
    candidate.photometricError = 1.0;
    candidate.valid = 0.0;
    return candidate;
}

Candidate EvaluateFirstCandidate(float2 sourcePosition, float2 targetPixel)
{
    Candidate candidate = InvalidCandidate();
    if (!IsValid(sourcePosition)) {
        return candidate;
    }

    float2 sourceToSecond = SampleFlow(true, sourcePosition);
    float2 projectedMidpoint =
        sourcePosition + MidpointTime * sourceToSecond;
    float2 matchingSecond = sourcePosition + sourceToSecond;

    candidate.position = sourcePosition;
    candidate.color = SampleFrame(FirstFrame, sourcePosition);
    candidate.projectionError = length(projectedMidpoint - targetPixel);
    candidate.consistencyError = 64.0;
    candidate.photometricError = 1.0;
    candidate.valid = 1.0;

    if (IsValid(matchingSecond)) {
        float2 secondToSource = SampleFlow(false, matchingSecond);
        float4 secondColor = SampleFrame(SecondFrame, matchingSecond);
        candidate.consistencyError =
            length(sourceToSecond + secondToSource);
        candidate.photometricError =
            ColorDifference(candidate.color, secondColor);
    }

    candidate.score =
        4.0 * candidate.projectionError +
        0.30 * candidate.consistencyError +
        10.0 * candidate.photometricError;
    return candidate;
}

Candidate EvaluateSecondCandidate(float2 sourcePosition, float2 targetPixel)
{
    Candidate candidate = InvalidCandidate();
    if (!IsValid(sourcePosition)) {
        return candidate;
    }

    float2 sourceToFirst = SampleFlow(false, sourcePosition);
    float2 projectedMidpoint =
        sourcePosition + (1.0 - MidpointTime) * sourceToFirst;
    float2 matchingFirst = sourcePosition + sourceToFirst;

    candidate.position = sourcePosition;
    candidate.color = SampleFrame(SecondFrame, sourcePosition);
    candidate.projectionError = length(projectedMidpoint - targetPixel);
    candidate.consistencyError = 64.0;
    candidate.photometricError = 1.0;
    candidate.valid = 1.0;

    if (IsValid(matchingFirst)) {
        float2 firstToSource = SampleFlow(true, matchingFirst);
        float4 firstColor = SampleFrame(FirstFrame, matchingFirst);
        candidate.consistencyError =
            length(sourceToFirst + firstToSource);
        candidate.photometricError =
            ColorDifference(candidate.color, firstColor);
    }

    candidate.score =
        4.0 * candidate.projectionError +
        0.30 * candidate.consistencyError +
        10.0 * candidate.photometricError;
    return candidate;
}

Candidate BetterCandidate(Candidate current, Candidate alternative)
{
    return alternative.score < current.score ? alternative : current;
}

Candidate FindFirstCandidate(float2 targetPixel)
{
    float2 position = targetPixel;
    Candidate best = EvaluateFirstCandidate(position, targetPixel);

    [unroll]
    for (int iteration = 0; iteration < 6; ++iteration) {
        float2 flow = SampleFlow(true, position);
        position = targetPixel - MidpointTime * flow;
        best = BetterCandidate(
            best, EvaluateFirstCandidate(position, targetPixel));
    }
    return best;
}

Candidate FindSecondCandidate(float2 targetPixel)
{
    float2 position = targetPixel;
    Candidate best = EvaluateSecondCandidate(position, targetPixel);

    [unroll]
    for (int iteration = 0; iteration < 6; ++iteration) {
        float2 flow = SampleFlow(false, position);
        position = targetPixel - (1.0 - MidpointTime) * flow;
        best = BetterCandidate(
            best, EvaluateSecondCandidate(position, targetPixel));
    }
    return best;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= FrameSize)) {
        return;
    }

    float2 pixel = float2(dispatchThreadId.xy);
    Candidate first = FindFirstCandidate(pixel);
    Candidate second = FindSecondCandidate(pixel);

    float4 result = float4(0.0, 0.0, 0.0, 1.0);
    float4 selection = float4(1.0, 0.0, 1.0, 1.0);

    if (first.valid > 0.5 && second.valid > 0.5) {
        float scoreDifference = abs(first.score - second.score);
        float colorDifference = ColorDifference(first.color, second.color);
        bool bothReliable =
            first.score < 1.5 && second.score < 1.5 &&
            colorDifference < 0.04;

        if (bothReliable || scoreDifference < 0.35) {
            float firstWeight = rcp(0.10 + first.score);
            float secondWeight = rcp(0.10 + second.score);
            result = (
                firstWeight * first.color +
                secondWeight * second.color) /
                (firstWeight + secondWeight);
            selection = float4(0.0, 1.0, 0.0, 1.0);
        } else if (first.score < second.score) {
            result = first.color;
            selection = float4(1.0, 0.0, 0.0, 1.0);
        } else {
            result = second.color;
            selection = float4(0.0, 0.35, 1.0, 1.0);
        }
    } else if (first.valid > 0.5) {
        result = first.color;
        selection = float4(1.0, 0.0, 0.0, 1.0);
    } else if (second.valid > 0.5) {
        result = second.color;
        selection = float4(0.0, 0.35, 1.0, 1.0);
    }

    float minimumScore = min(first.score, second.score);
    if (minimumScore > 20.0) {
        selection = lerp(selection, float4(1.0, 0.0, 1.0, 1.0), 0.65);
    }

    OutputFrame[dispatchThreadId.xy] = result;
    SelectionMap[dispatchThreadId.xy] = selection;
}
)hlsl";

bool CompileOcclusionAwareShader(
    ID3D11Device* device,
    ComPtr<ID3D11ComputeShader>& shader)
{
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> messages;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                       D3DCOMPILE_WARNINGS_ARE_ERRORS |
                       D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT compileResult = D3DCompile(
        OcclusionAwareComputeShader,
        sizeof(OcclusionAwareComputeShader) - 1,
        "NativeNvofOcclusionAware", nullptr, nullptr,
        "main", "cs_5_0", flags, 0, &bytecode, &messages);
    if (FAILED(compileResult)) {
        std::wcerr << L"D3DCompile(occlusion-aware shader) failed: "
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
            L"CreateComputeShader(occlusion-aware)", createResult);
    }
    return true;
}

void DispatchOcclusionAwareShader(
    ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader,
    ID3D11Buffer* parameters,
    ID3D11SamplerState* sampler,
    const std::array<ID3D11ShaderResourceView*, 4>& inputs,
    const std::array<ID3D11UnorderedAccessView*, 2>& outputs,
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
    context->CSSetUnorderedAccessViews(
        0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
    context->Dispatch(
        (TestWidth + OcclusionShaderGroupSize - 1) /
            OcclusionShaderGroupSize,
        (TestHeight + OcclusionShaderGroupSize - 1) /
            OcclusionShaderGroupSize,
        1);

    const std::array<ID3D11UnorderedAccessView*, 2> nullOutputs = {};
    const std::array<ID3D11ShaderResourceView*, 4> nullInputs = {};
    ID3D11Buffer* nullBuffer = nullptr;
    ID3D11SamplerState* nullSampler = nullptr;
    context->CSSetUnorderedAccessViews(
        0, static_cast<UINT>(nullOutputs.size()),
        nullOutputs.data(), nullptr);
    context->CSSetShaderResources(
        0, static_cast<UINT>(nullInputs.size()), nullInputs.data());
    context->CSSetConstantBuffers(0, 1, &nullBuffer);
    context->CSSetSamplers(0, 1, &nullSampler);
    context->CSSetShader(nullptr, nullptr, 0);
}

double ImprovementPercent(const double baseline, const double improved)
{
    if (baseline <= 0.0) {
        return improved <= 0.0 ? 100.0 : 0.0;
    }
    return 100.0 * (baseline - improved) / baseline;
}

} // namespace

int wmain()
{
#ifndef _WIN64
    std::wcerr << L"This test must be built as a 64-bit executable.\n";
    return 2;
#else
    std::wcout << L"Native NVIDIA Optical Flow occlusion-aware synthesis test\n"
               << L"=========================================================\n";

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

    ComPtr<ID3D11ComputeShader> baselineShader;
    ComPtr<ID3D11ComputeShader> awareShader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> parameters;
    if (!CompileOcclusionShader(device.Get(), baselineShader) ||
        !CompileOcclusionAwareShader(device.Get(), awareShader) ||
        !CreateOcclusionShaderResources(device.Get(), sampler, parameters)) {
        return 17;
    }

    ComPtr<ID3D11Texture2D> baselineTexture;
    ComPtr<ID3D11UnorderedAccessView> baselineView;
    ComPtr<ID3D11Texture2D> baselineStaging;
    ComPtr<ID3D11Texture2D> awareTexture;
    ComPtr<ID3D11UnorderedAccessView> awareView;
    ComPtr<ID3D11Texture2D> awareStaging;
    ComPtr<ID3D11Texture2D> selectionTexture;
    ComPtr<ID3D11UnorderedAccessView> selectionView;
    ComPtr<ID3D11Texture2D> selectionStaging;
    if (!CreateOcclusionGpuOutput(
            device.Get(), baselineTexture, baselineView, baselineStaging) ||
        !CreateOcclusionGpuOutput(
            device.Get(), awareTexture, awareView, awareStaging) ||
        !CreateOcclusionGpuOutput(
            device.Get(), selectionTexture, selectionView, selectionStaging)) {
        return 18;
    }

    const std::array<ID3D11ShaderResourceView*, 4> shaderInputs = {
        firstView.Get(), secondView.Get(),
        forwardView.Get(), backwardView.Get(),
    };
    DispatchOcclusionShader(
        context.Get(), baselineShader.Get(), parameters.Get(), sampler.Get(),
        shaderInputs, baselineView.Get(), flowWidth, flowHeight);
    const std::array<ID3D11UnorderedAccessView*, 2> awareOutputs = {
        awareView.Get(), selectionView.Get(),
    };
    DispatchOcclusionAwareShader(
        context.Get(), awareShader.Get(), parameters.Get(), sampler.Get(),
        shaderInputs, awareOutputs, flowWidth, flowHeight);
    std::wcout << L"D3D11 compute synthesis: baseline + occlusion-aware dispatched\n";

    std::vector<uint8_t> baselineRgba;
    std::vector<uint8_t> awareRgba;
    std::vector<uint8_t> selectionRgba;
    if (!ReadOcclusionRgbaTexture(
            context.Get(), baselineTexture.Get(),
            baselineStaging.Get(), baselineRgba) ||
        !ReadOcclusionRgbaTexture(
            context.Get(), awareTexture.Get(),
            awareStaging.Get(), awareRgba) ||
        !ReadOcclusionRgbaTexture(
            context.Get(), selectionTexture.Get(),
            selectionStaging.Get(), selectionRgba)) {
        return 19;
    }

    std::vector<nvof::FlowVector> forwardFlow;
    std::vector<nvof::FlowVector> backwardFlow;
    if (!ReadFlowTexture(
            context.Get(), forwardOutput.Get(), forwardStaging.Get(),
            flowWidth, flowHeight, forwardFlow) ||
        !ReadFlowTexture(
            context.Get(), backwardOutput.Get(), backwardStaging.Get(),
            flowWidth, flowHeight, backwardFlow)) {
        return 20;
    }

    double forwardObjectX = 0.0;
    double forwardObjectY = 0.0;
    double backwardObjectX = 0.0;
    double backwardObjectY = 0.0;
    FlowRegionStatistics(
        forwardFlow, flowWidth, flowHeight,
        ObjectB, 28, forwardObjectX, forwardObjectY);
    FlowRegionStatistics(
        backwardFlow, flowWidth, flowHeight,
        ObjectA, 28, backwardObjectX, backwardObjectY);

    const RectI expandedEnvelope = MotionEnvelope.Expanded(28);
    const auto StableBackground = [&](const int x, const int y) {
        return !expandedEnvelope.Contains(x, y);
    };
    const auto ObjectInterior = [&](const int x, const int y) {
        return ObjectMidpoint.Contains(x, y, 24);
    };
    const auto Boundary = [&](const int x, const int y) {
        return expandedEnvelope.Contains(x, y) &&
               !ObjectMidpoint.Contains(x, y, 24);
    };

    const ErrorSummary baselineBoundary = CalculateRegionError(
        baselineRgba, expectedRgba, Boundary);
    const ErrorSummary awareBackground = CalculateRegionError(
        awareRgba, expectedRgba, StableBackground);
    const ErrorSummary awareObject = CalculateRegionError(
        awareRgba, expectedRgba, ObjectInterior);
    const ErrorSummary awareBoundary = CalculateRegionError(
        awareRgba, expectedRgba, Boundary);

    const double boundaryMaeImprovement = ImprovementPercent(
        baselineBoundary.meanAbsoluteError,
        awareBoundary.meanAbsoluteError);
    const double boundaryP95Improvement = ImprovementPercent(
        static_cast<double>(baselineBoundary.percentile95),
        static_cast<double>(awareBoundary.percentile95));

    std::wcout << std::fixed << std::setprecision(2)
               << L"Known object translation A -> B: X=+"
               << ObjectShiftX << L", Y=+" << ObjectShiftY << L" px\n"
               << L"Object forward flow (B -> A): X="
               << forwardObjectX << L", Y=" << forwardObjectY << L" px\n"
               << L"Object backward flow (A -> B): X="
               << backwardObjectX << L", Y=" << backwardObjectY << L" px\n";
    PrintErrorSummary(L"Baseline occlusion/boundary", baselineBoundary);
    PrintErrorSummary(L"Aware stable background", awareBackground);
    PrintErrorSummary(L"Aware moving object interior", awareObject);
    PrintErrorSummary(L"Aware occlusion/boundary", awareBoundary);
    std::wcout << L"Boundary improvement: MAE="
               << boundaryMaeImprovement << L"%, P95="
               << boundaryP95Improvement << L"%\n";

    const std::vector<uint8_t> awareBgra =
        OcclusionSwapRedBlue(awareRgba);
    const std::vector<uint8_t> selectionBgra =
        OcclusionSwapRedBlue(selectionRgba);
    const std::vector<uint8_t> awareDifference =
        MakeDifferenceImage(awareRgba, expectedRgba);
    const std::vector<uint8_t> baselineDifference =
        MakeDifferenceImage(baselineRgba, expectedRgba);
    const std::filesystem::path outputDirectory =
        std::filesystem::current_path();
    const std::filesystem::path awarePath =
        outputDirectory / L"NativeNvofOcclusionAwareMidpoint.bmp";
    const std::filesystem::path expectedPath =
        outputDirectory / L"NativeNvofOcclusionAwareExpected.bmp";
    const std::filesystem::path awareDifferencePath =
        outputDirectory / L"NativeNvofOcclusionAwareDiff.bmp";
    const std::filesystem::path baselineDifferencePath =
        outputDirectory / L"NativeNvofOcclusionBaselineDiff.bmp";
    const std::filesystem::path selectionPath =
        outputDirectory / L"NativeNvofOcclusionSelection.bmp";

    const bool saved =
        SaveBitmap(awarePath, awareBgra) &&
        SaveBitmap(expectedPath, expectedBgra) &&
        SaveBitmap(awareDifferencePath, awareDifference) &&
        SaveBitmap(baselineDifferencePath, baselineDifference) &&
        SaveBitmap(selectionPath, selectionBgra);
    if (saved) {
        std::wcout << L"Occlusion-aware midpoint: "
                   << awarePath.wstring() << L'\n'
                   << L"Exact expected midpoint: "
                   << expectedPath.wstring() << L'\n'
                   << L"Aware amplified difference: "
                   << awareDifferencePath.wstring() << L'\n'
                   << L"Baseline amplified difference: "
                   << baselineDifferencePath.wstring() << L'\n'
                   << L"Source-selection map: "
                   << selectionPath.wstring() << L'\n';
    } else {
        std::wcerr << L"Warning: one or more v6 bitmaps could not be written.\n";
    }

    const bool flowPassed =
        std::abs(forwardObjectX + ObjectShiftX) <= 6.0 &&
        std::abs(forwardObjectY + ObjectShiftY) <= 6.0 &&
        std::abs(backwardObjectX - ObjectShiftX) <= 6.0 &&
        std::abs(backwardObjectY - ObjectShiftY) <= 6.0;
    const bool stableRegionsPassed =
        awareBackground.meanAbsoluteError <= 1.0 &&
        awareBackground.percentile95 <= 3 &&
        awareObject.meanAbsoluteError <= 2.0 &&
        awareObject.percentile95 <= 8;
    const bool boundaryImproved =
        awareBoundary.meanAbsoluteError <
            baselineBoundary.meanAbsoluteError * 0.90 &&
        awareBoundary.percentile95 <= baselineBoundary.percentile95;
    const bool passed =
        flowPassed && stableRegionsPassed && boundaryImproved;

    std::wcout << L"OCCLUSION-AWARE RESULT: "
               << (passed ? L"PASS" : L"FAIL") << L'\n';
    return passed ? 0 : 21;
#endif
}
