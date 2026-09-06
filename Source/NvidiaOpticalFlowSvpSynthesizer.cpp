#include "stdafx.h"
#include "NvidiaOpticalFlowSvpSynthesizer.h"
#include "NvidiaOpticalFlowSvpClassifyBytecode.h"
#include "NvidiaOpticalFlowSvpControlBytecode.h"
#include "NvidiaOpticalFlowSvpCoverageScatterBytecode.h"
#include "NvidiaOpticalFlowSvpCoverageFinishBytecode.h"
#include "NvidiaOpticalFlowSvpWarpBytecode.h"
#include "Helper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <vector>

namespace {

template <class T>
bool CreateConstantBuffer(ID3D11Device* device, CComPtr<ID3D11Buffer>& buffer,
    std::wstring& status, const wchar_t* label)
{
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(T);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const HRESULT hr = device->CreateBuffer(&desc, nullptr, &buffer);
    if (FAILED(hr)) {
        status = std::format(L"CreateBuffer({}) failed ({})", label, HR2Str(hr));
        return false;
    }
    return true;
}

bool CreateShader(ID3D11Device* device, const void* bytecode, const size_t bytecodeSize,
    CComPtr<ID3D11ComputeShader>& shader, std::wstring& status, const wchar_t* label)
{
    const HRESULT hr = device->CreateComputeShader(bytecode, bytecodeSize, nullptr, &shader);
    if (FAILED(hr)) {
        status = std::format(L"CreateComputeShader({}) failed ({})", label, HR2Str(hr));
        return false;
    }
    return true;
}

bool CreateStructuredUintBuffer(ID3D11Device* device, const UINT count,
    CComPtr<ID3D11Buffer>& buffer,
    CComPtr<ID3D11ShaderResourceView>& view,
    CComPtr<ID3D11UnorderedAccessView>& uav,
    std::wstring& status, const wchar_t* label)
{
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = count * sizeof(UINT);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(UINT);
    HRESULT hr = device->CreateBuffer(&desc, nullptr, &buffer);
    if (FAILED(hr)) {
        status = std::format(L"CreateBuffer({}) failed ({})", label, HR2Str(hr));
        return false;
    }
    hr = device->CreateShaderResourceView(buffer, nullptr, &view);
    if (FAILED(hr)) {
        status = std::format(L"CreateShaderResourceView({}) failed ({})", label, HR2Str(hr));
        return false;
    }
    hr = device->CreateUnorderedAccessView(buffer, nullptr, &uav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView({}) failed ({})", label, HR2Str(hr));
        return false;
    }
    return true;
}

void UnbindCompute(ID3D11DeviceContext* context)
{
    const std::array<ID3D11ShaderResourceView*, 8> nullSrvs = {};
    const std::array<ID3D11UnorderedAccessView*, 4> nullUavs = {};
    ID3D11Buffer* nullBuffer = nullptr;
    context->CSSetShaderResources(0, static_cast<UINT>(nullSrvs.size()), nullSrvs.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUavs.size()), nullUavs.data(), nullptr);
    context->CSSetConstantBuffers(0, 1, &nullBuffer);
    context->CSSetShader(nullptr, nullptr, 0);
}

} // namespace

bool CNvidiaOpticalFlowSvpSynthesizer::Initialize(ID3D11Device* device,
    const UINT frameWidth, const UINT frameHeight,
    const UINT flowWidth, const UINT flowHeight, std::wstring& status)
{
    Reset();
    if (!device || !frameWidth || !frameHeight || !flowWidth || !flowHeight) {
        status = L"Invalid SVP-style NVOF synthesis dimensions or device";
        return false;
    }

    m_frameWidth = frameWidth;
    m_frameHeight = frameHeight;
    m_flowWidth = flowWidth;
    m_flowHeight = flowHeight;

    if (!CreateShader(device, g_NvofSvpClassifyBytecode, sizeof(g_NvofSvpClassifyBytecode),
            m_classifyShader, status, L"SVP NVOF software-SAD classifier") ||
        !CreateShader(device, g_NvofSvpControlBytecode, sizeof(g_NvofSvpControlBytecode),
            m_controlShader, status, L"SVP scene/adaptive control") ||
        !CreateShader(device, g_NvofSvpCoverageScatterBytecode, sizeof(g_NvofSvpCoverageScatterBytecode),
            m_coverageScatterShader, status, L"SVP coverage scatter") ||
        !CreateShader(device, g_NvofSvpCoverageFinishBytecode, sizeof(g_NvofSvpCoverageFinishBytecode),
            m_coverageFinishShader, status, L"SVP coverage finish") ||
        !CreateShader(device, g_NvofSvpWarpBytecode, sizeof(g_NvofSvpWarpBytecode),
            m_warpShader, status, L"SVP direct 4x4 warp")) {
        Reset();
        return false;
    }

    if (!CreateConstantBuffer<ClassifyParameters>(device, m_classifyParameters, status, L"SVP classify params") ||
        !CreateConstantBuffer<ControlParameters>(device, m_controlParameters, status, L"SVP control params") ||
        !CreateConstantBuffer<CoverageScatterParameters>(device, m_coverageScatterParameters, status, L"SVP coverage scatter params") ||
        !CreateConstantBuffer<CoverageFinishParameters>(device, m_coverageFinishParameters, status, L"SVP coverage finish params") ||
        !CreateConstantBuffer<WarpParameters>(device, m_warpParameters, status, L"SVP warp params")) {
        Reset();
        return false;
    }

    if (!CreateStructuredUintBuffer(device, 4, m_classCounters,
            m_classCountersView, m_classCountersUav, status, L"SVP classifier counters") ||
        !CreateStructuredUintBuffer(device, 8, m_controlState,
            m_controlStateView, m_controlStateUav, status, L"SVP control state")) {
        Reset();
        return false;
    }

    const UINT cellCount = flowWidth * flowHeight;
    for (UINT index = 0; index < 2; ++index) {
        if (!CreateStructuredUintBuffer(device, cellCount, m_coverageAccum[index],
                m_coverageAccumView[index], m_coverageAccumUav[index], status, L"SVP coverage accumulation") ||
            !CreateStructuredUintBuffer(device, cellCount, m_coverageMask[index],
                m_coverageMaskView[index], m_coverageMaskUav[index], status, L"SVP coverage mask")) {
            Reset();
            return false;
        }
    }

    D3D11_BUFFER_DESC readbackDesc = {};
    readbackDesc.ByteWidth = 8 * sizeof(UINT);
    readbackDesc.Usage = D3D11_USAGE_STAGING;
    readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    readbackDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    readbackDesc.StructureByteStride = sizeof(UINT);
    for (UINT slot = 0; slot < TelemetrySlotCount; ++slot) {
        const HRESULT hr = device->CreateBuffer(&readbackDesc, nullptr, &m_controlReadback[slot]);
        if (FAILED(hr)) {
            status = std::format(L"CreateBuffer(SVP telemetry {}) failed ({})", slot, HR2Str(hr));
            Reset();
            return false;
        }
    }

    std::vector<UINT> pairLuma(511);
    for (UINT sum = 0; sum <= 510; ++sum) {
        const double normalized = static_cast<double>(sum) / 510.0;
        UINT value = static_cast<UINT>(std::pow(normalized, 1.5) * 255.0);
        if (value < 21) value = 20;
        pairLuma[sum] = value;
    }
    D3D11_TEXTURE1D_DESC lutDesc = {};
    lutDesc.Width = static_cast<UINT>(pairLuma.size());
    lutDesc.MipLevels = 1;
    lutDesc.ArraySize = 1;
    lutDesc.Format = DXGI_FORMAT_R32_UINT;
    lutDesc.Usage = D3D11_USAGE_IMMUTABLE;
    lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA lutData = {};
    lutData.pSysMem = pairLuma.data();
    lutData.SysMemPitch = static_cast<UINT>(pairLuma.size() * sizeof(UINT));
    HRESULT hr = device->CreateTexture1D(&lutDesc, &lutData, &m_pairLumaLut);
    if (FAILED(hr)) {
        status = std::format(L"CreateTexture1D(SVP pair-luma LUT) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateShaderResourceView(m_pairLumaLut, nullptr, &m_pairLumaLutView);
    if (FAILED(hr)) {
        status = std::format(L"CreateShaderResourceView(SVP pair-luma LUT) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

    return true;
}

void CNvidiaOpticalFlowSvpSynthesizer::Reset()
{
    m_pairLumaLutView.Release();
    m_pairLumaLut.Release();
    for (UINT index = 0; index < 2; ++index) {
        m_coverageMaskUav[index].Release();
        m_coverageMaskView[index].Release();
        m_coverageMask[index].Release();
        m_coverageAccumUav[index].Release();
        m_coverageAccumView[index].Release();
        m_coverageAccum[index].Release();
    }
    for (UINT slot = 0; slot < TelemetrySlotCount; ++slot) {
        m_controlReadback[slot].Release();
        m_telemetryPrimed[slot] = false;
    }
    m_telemetryWriteIndex = 0;
    m_controlStateUav.Release();
    m_controlStateView.Release();
    m_controlState.Release();
    m_classCountersUav.Release();
    m_classCountersView.Release();
    m_classCounters.Release();
    m_warpParameters.Release();
    m_coverageFinishParameters.Release();
    m_coverageScatterParameters.Release();
    m_controlParameters.Release();
    m_classifyParameters.Release();
    m_warpShader.Release();
    m_coverageFinishShader.Release();
    m_coverageScatterShader.Release();
    m_controlShader.Release();
    m_classifyShader.Release();
    m_frameWidth = m_frameHeight = m_flowWidth = m_flowHeight = 0;
    m_lastClass = 0;
    m_lastPhase = 128;
    m_lastAlgorithm = 21;
    m_lastConsidered = m_lastZeroSkipped = 0;
    m_lastM1Plus = m_lastM2Plus = m_lastScenePlus = 0;
    m_haveTelemetry = false;
}

std::wstring CNvidiaOpticalFlowSvpSynthesizer::GetTelemetryText() const
{
    if (!m_haveTelemetry) {
        return L"SVP control telemetry warming up";
    }
    return std::format(
        L"SVP C{}, algo{}, phase={}, considered={}, zero-skip={}, m1+={:.1f}%, m2+={:.1f}%, scene+={:.1f}%",
        m_lastClass, m_lastAlgorithm, m_lastPhase,
        m_lastConsidered, m_lastZeroSkipped,
        m_lastConsidered ? 100.0 * static_cast<double>(m_lastM1Plus) / m_lastConsidered : 0.0,
        m_lastConsidered ? 100.0 * static_cast<double>(m_lastM2Plus) / m_lastConsidered : 0.0,
        m_lastConsidered ? 100.0 * static_cast<double>(m_lastScenePlus) / m_lastConsidered : 0.0);
}

bool CNvidiaOpticalFlowSvpSynthesizer::Dispatch(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* previousFrame,
    ID3D11ShaderResourceView* nextFrame,
    ID3D11ShaderResourceView* previousLuma,
    ID3D11ShaderResourceView* nextLuma,
    ID3D11ShaderResourceView* flowBtoA,
    ID3D11ShaderResourceView* flowAtoB,
    ID3D11UnorderedAccessView* output,
    const float midpointTime,
    std::wstring& status)
{
    UNREFERENCED_PARAMETER(midpointTime);
    if (!context || !previousFrame || !nextFrame || !previousLuma || !nextLuma ||
            !flowBtoA || !flowAtoB || !output || !m_classifyShader || !m_controlShader ||
            !m_coverageScatterShader || !m_coverageFinishShader || !m_warpShader) {
        status = L"SVP-style NVOF synthesis resources are incomplete";
        return false;
    }

    const UINT borderX = std::max(1u, static_cast<UINT>(m_flowWidth * 0.04));
    const UINT borderY = std::max(1u, static_cast<UINT>(m_flowHeight * 0.04));
    const UINT zero[4] = {};
    context->ClearUnorderedAccessViewUint(m_classCountersUav, zero);

    const ClassifyParameters classifyValues = {
        m_flowWidth, m_flowHeight, m_frameWidth, m_frameHeight,
        borderX, borderY, {},
    };
    context->UpdateSubresource(m_classifyParameters, 0, nullptr, &classifyValues, 0, 0);
    ID3D11Buffer* classifyBuffer = m_classifyParameters;
    const std::array<ID3D11ShaderResourceView*, 4> classifyInputs = {
        flowAtoB, previousLuma, nextLuma, m_pairLumaLutView,
    };
    ID3D11UnorderedAccessView* classifyOutput = m_classCountersUav;
    context->CSSetShader(m_classifyShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &classifyBuffer);
    context->CSSetShaderResources(0, static_cast<UINT>(classifyInputs.size()), classifyInputs.data());
    context->CSSetUnorderedAccessViews(0, 1, &classifyOutput, nullptr);
    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);
    UnbindCompute(context);

    const ControlParameters controlValues = {m_flowWidth, m_flowHeight, borderX, borderY};
    context->UpdateSubresource(m_controlParameters, 0, nullptr, &controlValues, 0, 0);
    ID3D11Buffer* controlBuffer = m_controlParameters;
    ID3D11ShaderResourceView* controlInput = m_classCountersView;
    ID3D11UnorderedAccessView* controlOutput = m_controlStateUav;
    context->CSSetShader(m_controlShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &controlBuffer);
    context->CSSetShaderResources(0, 1, &controlInput);
    context->CSSetUnorderedAccessViews(0, 1, &controlOutput, nullptr);
    context->Dispatch(1, 1, 1);
    UnbindCompute(context);

    const UINT telemetrySlot = m_telemetryWriteIndex;
    if (m_telemetryPrimed[telemetrySlot]) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT mapHr = context->Map(m_controlReadback[telemetrySlot], 0,
            D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (SUCCEEDED(mapHr)) {
            const UINT* values = static_cast<const UINT*>(mapped.pData);
            m_lastClass = static_cast<int>(values[0]);
            m_lastAlgorithm = values[1];
            m_lastPhase = values[2];
            m_lastConsidered = values[3];
            m_lastZeroSkipped = values[4];
            m_lastM1Plus = values[5] + values[6] + values[7];
            m_lastM2Plus = values[6] + values[7];
            m_lastScenePlus = values[7];
            m_haveTelemetry = true;
            context->Unmap(m_controlReadback[telemetrySlot], 0);
        }
    }
    context->CopyResource(m_controlReadback[telemetrySlot], m_controlState);
    m_telemetryPrimed[telemetrySlot] = true;
    m_telemetryWriteIndex = (telemetrySlot + 1) % TelemetrySlotCount;

    for (UINT index = 0; index < 2; ++index) {
        context->ClearUnorderedAccessViewUint(m_coverageAccumUav[index], zero);
    }
    const CoverageScatterParameters scatterValues = {m_flowWidth, m_flowHeight, {}};
    context->UpdateSubresource(m_coverageScatterParameters, 0, nullptr, &scatterValues, 0, 0);
    ID3D11Buffer* scatterBuffer = m_coverageScatterParameters;
    const std::array<ID3D11ShaderResourceView*, 3> scatterInputs = {
        flowBtoA, flowAtoB, m_controlStateView,
    };
    const std::array<ID3D11UnorderedAccessView*, 2> scatterOutputs = {
        m_coverageAccumUav[0], m_coverageAccumUav[1],
    };
    context->CSSetShader(m_coverageScatterShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &scatterBuffer);
    context->CSSetShaderResources(0, static_cast<UINT>(scatterInputs.size()), scatterInputs.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(scatterOutputs.size()), scatterOutputs.data(), nullptr);
    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);
    UnbindCompute(context);

    // SVP Manager uses mask.cover=80 for requested algorithms >=21.
    const CoverageFinishParameters finishValues = {m_flowWidth, m_flowHeight, 80u, 0u};
    context->UpdateSubresource(m_coverageFinishParameters, 0, nullptr, &finishValues, 0, 0);
    ID3D11Buffer* finishBuffer = m_coverageFinishParameters;
    const std::array<ID3D11ShaderResourceView*, 2> finishInputs = {
        m_coverageAccumView[0], m_coverageAccumView[1],
    };
    const std::array<ID3D11UnorderedAccessView*, 2> finishOutputs = {
        m_coverageMaskUav[0], m_coverageMaskUav[1],
    };
    context->CSSetShader(m_coverageFinishShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &finishBuffer);
    context->CSSetShaderResources(0, static_cast<UINT>(finishInputs.size()), finishInputs.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(finishOutputs.size()), finishOutputs.data(), nullptr);
    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);
    UnbindCompute(context);

    const WarpParameters warpValues = {m_frameWidth, m_frameHeight, m_flowWidth, m_flowHeight, {}};
    context->UpdateSubresource(m_warpParameters, 0, nullptr, &warpValues, 0, 0);
    ID3D11Buffer* warpBuffer = m_warpParameters;
    const std::array<ID3D11ShaderResourceView*, 7> warpInputs = {
        previousFrame, nextFrame, flowBtoA, flowAtoB,
        m_coverageMaskView[0], m_coverageMaskView[1], m_controlStateView,
    };
    context->CSSetShader(m_warpShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &warpBuffer);
    context->CSSetShaderResources(0, static_cast<UINT>(warpInputs.size()), warpInputs.data());
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch((m_frameWidth + 7) / 8, (m_frameHeight + 7) / 8, 1);
    UnbindCompute(context);

    return true;
}
