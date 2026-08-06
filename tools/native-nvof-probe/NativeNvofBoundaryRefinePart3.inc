int wmain()
{
#ifndef _WIN64
    std::wcerr << L"This test must be built as a 64-bit executable.\n";
    return 2;
#else
    std::wcout << L"Native NVIDIA Optical Flow boundary refinement test\n"
               << L"==================================================\n";

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

    const auto getVersion = reinterpret_cast<nvof::GetMaxSupportedApiVersionFn>(
        GetProcAddress(module.module, "NvOFGetMaxSupportedApiVersion"));
    const auto createInstance = reinterpret_cast<nvof::CreateInstanceD3D11Fn>(
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
        ObjectStartX + ObjectShiftX, ObjectStartY + ObjectShiftY);
    const std::vector<uint8_t> expectedBgra =
        MakeLayeredFrame(ObjectMidpointX, ObjectMidpointY);
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
        !CreateFlowTextures(device.Get(), flowWidth, flowHeight,
            forwardOutput, forwardStaging) ||
        !CreateFlowTextures(device.Get(), flowWidth, flowHeight,
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
    if (status != nvof::Success) return 11;
    status = api.registerResourceD3D11(
        session.handle, secondFrame.Get(), &secondResource.handle);
    if (status != nvof::Success) return 12;
    status = api.registerResourceD3D11(
        session.handle, forwardOutput.Get(), &forwardResource.handle);
    if (status != nvof::Success) return 13;
    status = api.registerResourceD3D11(
        session.handle, backwardOutput.Get(), &backwardResource.handle);
    if (status != nvof::Success) return 14;
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
    if (!CreateOcclusionShaderResourceView(device.Get(), firstFrame.Get(), firstView) ||
        !CreateOcclusionShaderResourceView(device.Get(), secondFrame.Get(), secondView) ||
        !CreateOcclusionShaderResourceView(device.Get(), forwardOutput.Get(), forwardView) ||
        !CreateOcclusionShaderResourceView(device.Get(), backwardOutput.Get(), backwardView)) {
        return 16;
    }

    ComPtr<ID3D11ComputeShader> v6Shader;
    ComPtr<ID3D11ComputeShader> searchShader;
    ComPtr<ID3D11ComputeShader> repairShader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> parameters;
    if (!CompileOcclusionAwareShader(device.Get(), v6Shader) ||
        !CompileBoundaryShader(device.Get(), BoundarySearchComputeShader,
            sizeof(BoundarySearchComputeShader) - 1,
            "NativeNvofBoundarySearch", searchShader) ||
        !CompileBoundaryShader(device.Get(), BoundaryRepairComputeShader,
            sizeof(BoundaryRepairComputeShader) - 1,
            "NativeNvofBoundaryRepair", repairShader) ||
        !CreateOcclusionShaderResources(device.Get(), sampler, parameters)) {
        return 17;
    }

    ComPtr<ID3D11Texture2D> v6Texture, v6Selection;
    ComPtr<ID3D11UnorderedAccessView> v6View, v6SelectionView;
    ComPtr<ID3D11Texture2D> v6Staging, v6SelectionStaging;
    ComPtr<ID3D11Texture2D> searchTexture, searchSelection, confidenceTexture;
    ComPtr<ID3D11UnorderedAccessView> searchView, searchSelectionView, confidenceView;
    ComPtr<ID3D11Texture2D> searchStaging, searchSelectionStaging, confidenceStaging;
    ComPtr<ID3D11Texture2D> refinedTexture, repairMap;
    ComPtr<ID3D11UnorderedAccessView> refinedView, repairMapView;
    ComPtr<ID3D11Texture2D> refinedStaging, repairMapStaging;

    if (!CreateOcclusionGpuOutput(device.Get(), v6Texture, v6View, v6Staging) ||
        !CreateOcclusionGpuOutput(device.Get(), v6Selection, v6SelectionView, v6SelectionStaging) ||
        !CreateOcclusionGpuOutput(device.Get(), searchTexture, searchView, searchStaging) ||
        !CreateOcclusionGpuOutput(device.Get(), searchSelection, searchSelectionView, searchSelectionStaging) ||
        !CreateOcclusionGpuOutput(device.Get(), confidenceTexture, confidenceView, confidenceStaging) ||
        !CreateOcclusionGpuOutput(device.Get(), refinedTexture, refinedView, refinedStaging) ||
        !CreateOcclusionGpuOutput(device.Get(), repairMap, repairMapView, repairMapStaging)) {
        return 18;
    }

    const std::array<ID3D11ShaderResourceView*, 4> flowInputs = {
        firstView.Get(), secondView.Get(), forwardView.Get(), backwardView.Get(),
    };
    const std::array<ID3D11UnorderedAccessView*, 2> v6Outputs = {
        v6View.Get(), v6SelectionView.Get(),
    };
    DispatchOcclusionAwareShader(
        context.Get(), v6Shader.Get(), parameters.Get(), sampler.Get(),
        flowInputs, v6Outputs, flowWidth, flowHeight);

    const std::array<ID3D11UnorderedAccessView*, 3> searchOutputs = {
        searchView.Get(), searchSelectionView.Get(), confidenceView.Get(),
    };
    DispatchBoundarySearch(
        context.Get(), searchShader.Get(), parameters.Get(), sampler.Get(),
        flowInputs, searchOutputs, flowWidth, flowHeight);

    ComPtr<ID3D11ShaderResourceView> searchFrameView;
    ComPtr<ID3D11ShaderResourceView> searchMapView;
    ComPtr<ID3D11ShaderResourceView> confidenceMapView;
    if (!CreateOcclusionShaderResourceView(device.Get(), searchTexture.Get(), searchFrameView) ||
        !CreateOcclusionShaderResourceView(device.Get(), searchSelection.Get(), searchMapView) ||
        !CreateOcclusionShaderResourceView(device.Get(), confidenceTexture.Get(), confidenceMapView)) {
        return 19;
    }
    const std::array<ID3D11ShaderResourceView*, 3> repairInputs = {
        searchFrameView.Get(), searchMapView.Get(), confidenceMapView.Get(),
    };
    const std::array<ID3D11UnorderedAccessView*, 2> repairOutputs = {
        refinedView.Get(), repairMapView.Get(),
    };
    DispatchBoundaryRepair(
        context.Get(), repairShader.Get(), parameters.Get(),
        repairInputs, repairOutputs);
    std::wcout << L"D3D11 compute synthesis: v6 + edge search + repair dispatched\n";

    std::vector<uint8_t> v6Rgba, searchRgba, refinedRgba;
    std::vector<uint8_t> selectionRgba, confidenceRgba, repairRgba;
    if (!ReadOcclusionRgbaTexture(context.Get(), v6Texture.Get(), v6Staging.Get(), v6Rgba) ||
        !ReadOcclusionRgbaTexture(context.Get(), searchTexture.Get(), searchStaging.Get(), searchRgba) ||
        !ReadOcclusionRgbaTexture(context.Get(), refinedTexture.Get(), refinedStaging.Get(), refinedRgba) ||
        !ReadOcclusionRgbaTexture(context.Get(), searchSelection.Get(), searchSelectionStaging.Get(), selectionRgba) ||
        !ReadOcclusionRgbaTexture(context.Get(), confidenceTexture.Get(), confidenceStaging.Get(), confidenceRgba) ||
        !ReadOcclusionRgbaTexture(context.Get(), repairMap.Get(), repairMapStaging.Get(), repairRgba)) {
        return 20;
    }

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

    const ErrorSummary v6Boundary =
        CalculateRegionError(v6Rgba, expectedRgba, Boundary);
    const ErrorSummary searchBoundary =
        CalculateRegionError(searchRgba, expectedRgba, Boundary);
    const ErrorSummary refinedBackground =
        CalculateRegionError(refinedRgba, expectedRgba, StableBackground);
    const ErrorSummary refinedObject =
        CalculateRegionError(refinedRgba, expectedRgba, ObjectInterior);
    const ErrorSummary refinedBoundary =
        CalculateRegionError(refinedRgba, expectedRgba, Boundary);

    const double searchMaeImprovement = ImprovementPercent(
        v6Boundary.meanAbsoluteError, searchBoundary.meanAbsoluteError);
    const double refinedMaeImprovement = ImprovementPercent(
        v6Boundary.meanAbsoluteError, refinedBoundary.meanAbsoluteError);
    const double refinedP95Improvement = ImprovementPercent(
        static_cast<double>(v6Boundary.percentile95),
        static_cast<double>(refinedBoundary.percentile95));
