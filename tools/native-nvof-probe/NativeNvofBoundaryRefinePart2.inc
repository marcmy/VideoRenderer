constexpr char BoundaryRepairComputeShader[] = R"hlsl(
cbuffer Parameters : register(b0)
{
    uint2 FrameSize;
    uint2 FlowSize;
    float MidpointTime;
    float GridSize;
    float2 Padding;
};

Texture2D<float4> SearchFrame : register(t0);
Texture2D<float4> SelectionMap : register(t1);
Texture2D<float4> ConfidenceMap : register(t2);
RWTexture2D<float4> OutputFrame : register(u0);
RWTexture2D<float4> RepairMap : register(u1);

int SelectionClass(float4 value)
{
    if (value.g > 0.75 && value.r < 0.25 && value.b < 0.25) {
        return 0; // trusted blend
    }
    if (value.r > 0.75 && value.g < 0.25 && value.b < 0.45) {
        return 1; // first frame
    }
    if (value.b > 0.75 && value.r < 0.25 && value.g < 0.55) {
        return 2; // second frame
    }
    return 3; // low-confidence / unresolved
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= FrameSize)) {
        return;
    }

    int2 pixel = int2(dispatchThreadId.xy);
    float4 original = SearchFrame.Load(int3(pixel, 0));
    float4 confidence = ConfidenceMap.Load(int3(pixel, 0));
    int currentClass = SelectionClass(SelectionMap.Load(int3(pixel, 0)));

    // The repair pass is intentionally conservative: only candidates whose
    // minimum source score saturated the confidence channel are eligible.
    if (confidence.r < 0.98 && currentClass != 3) {
        OutputFrame[pixel] = original;
        RepairMap[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float bestCost = 10000.0;
    float4 bestColor = original;
    bool found = false;

    [unroll]
    for (int offsetY = -4; offsetY <= 4; ++offsetY) {
        [unroll]
        for (int offsetX = -4; offsetX <= 4; ++offsetX) {
            if (offsetX == 0 && offsetY == 0) {
                continue;
            }

            int2 candidatePixel = clamp(
                pixel + int2(offsetX, offsetY),
                int2(0, 0), int2(FrameSize) - 1);
            float4 candidateConfidence =
                ConfidenceMap.Load(int3(candidatePixel, 0));
            int candidateClass = SelectionClass(
                SelectionMap.Load(int3(candidatePixel, 0)));
            if (candidateClass == 3 || candidateConfidence.r >= 0.90) {
                continue;
            }

            float classPenalty = 0.0;
            if (currentClass != 3 && candidateClass != currentClass) {
                classPenalty = candidateClass == 0 ? 0.35 : 1.25;
            }
            float spatialPenalty =
                0.03 * length(float2(offsetX, offsetY));
            float cost =
                8.0 * candidateConfidence.r +
                1.5 * candidateConfidence.b +
                classPenalty + spatialPenalty;
            if (cost < bestCost) {
                bestCost = cost;
                bestColor = SearchFrame.Load(int3(candidatePixel, 0));
                found = true;
            }
        }
    }

    if (found) {
        OutputFrame[pixel] = bestColor;
        RepairMap[pixel] = float4(1.0, 1.0, 1.0, 1.0);
    } else {
        OutputFrame[pixel] = original;
        RepairMap[pixel] = float4(0.0, 0.0, 0.0, 1.0);
    }
}
)hlsl";

bool CompileBoundaryShader(
    ID3D11Device* device,
    const char* source,
    const size_t sourceSize,
    const char* sourceName,
    ComPtr<ID3D11ComputeShader>& shader)
{
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> messages;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                       D3DCOMPILE_WARNINGS_ARE_ERRORS |
                       D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT compileResult = D3DCompile(
        source, sourceSize, sourceName, nullptr, nullptr,
        "main", "cs_5_0", flags, 0, &bytecode, &messages);
    if (FAILED(compileResult)) {
        std::wcerr << L"D3DCompile(";
        for (const char* p = sourceName; *p; ++p) {
            std::wcerr << static_cast<wchar_t>(*p);
        }
        std::wcerr << L") failed: " << HResultText(compileResult) << L'\n';
        if (messages && messages->GetBufferPointer()) {
            const auto* text = static_cast<const char*>(
                messages->GetBufferPointer());
            for (size_t index = 0; index < messages->GetBufferSize(); ++index) {
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
            L"CreateComputeShader(boundary refinement)", createResult);
    }
    return true;
}

void DispatchBoundarySearch(
    ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader,
    ID3D11Buffer* parameters,
    ID3D11SamplerState* sampler,
    const std::array<ID3D11ShaderResourceView*, 4>& inputs,
    const std::array<ID3D11UnorderedAccessView*, 3>& outputs,
    const uint32_t flowWidth,
    const uint32_t flowHeight)
{
    const OcclusionShaderParameters values {
        TestWidth, TestHeight, flowWidth, flowHeight,
        MidpointTime, static_cast<float>(GridSize), 0.0f, 0.0f,
    };
    context->UpdateSubresource(parameters, 0, nullptr, &values, 0, 0);
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &parameters);
    context->CSSetSamplers(0, 1, &sampler);
    context->CSSetShaderResources(
        0, static_cast<UINT>(inputs.size()), inputs.data());
    context->CSSetUnorderedAccessViews(
        0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
    context->Dispatch(
        (TestWidth + OcclusionShaderGroupSize - 1) / OcclusionShaderGroupSize,
        (TestHeight + OcclusionShaderGroupSize - 1) / OcclusionShaderGroupSize,
        1);

    const std::array<ID3D11UnorderedAccessView*, 3> nullOutputs = {};
    const std::array<ID3D11ShaderResourceView*, 4> nullInputs = {};
    ID3D11Buffer* nullBuffer = nullptr;
    ID3D11SamplerState* nullSampler = nullptr;
    context->CSSetUnorderedAccessViews(
        0, static_cast<UINT>(nullOutputs.size()), nullOutputs.data(), nullptr);
    context->CSSetShaderResources(
        0, static_cast<UINT>(nullInputs.size()), nullInputs.data());
    context->CSSetConstantBuffers(0, 1, &nullBuffer);
    context->CSSetSamplers(0, 1, &nullSampler);
    context->CSSetShader(nullptr, nullptr, 0);
}

void DispatchBoundaryRepair(
    ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader,
    ID3D11Buffer* parameters,
    const std::array<ID3D11ShaderResourceView*, 3>& inputs,
    const std::array<ID3D11UnorderedAccessView*, 2>& outputs)
{
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &parameters);
    context->CSSetShaderResources(
        0, static_cast<UINT>(inputs.size()), inputs.data());
    context->CSSetUnorderedAccessViews(
        0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
    context->Dispatch(
        (TestWidth + OcclusionShaderGroupSize - 1) / OcclusionShaderGroupSize,
        (TestHeight + OcclusionShaderGroupSize - 1) / OcclusionShaderGroupSize,
        1);

    const std::array<ID3D11UnorderedAccessView*, 2> nullOutputs = {};
    const std::array<ID3D11ShaderResourceView*, 3> nullInputs = {};
    ID3D11Buffer* nullBuffer = nullptr;
    context->CSSetUnorderedAccessViews(
        0, static_cast<UINT>(nullOutputs.size()), nullOutputs.data(), nullptr);
    context->CSSetShaderResources(
        0, static_cast<UINT>(nullInputs.size()), nullInputs.data());
    context->CSSetConstantBuffers(0, 1, &nullBuffer);
    context->CSSetShader(nullptr, nullptr, 0);
}

} // namespace

