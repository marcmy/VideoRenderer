#include "stdafx.h"
#include "NvidiaOpticalFlowDenseSynthesizer.h"
#include "NvidiaOpticalFlowDenseSeedBytecode.h"
#include "NvidiaOpticalFlowDenseRegionGateBytecode.h"
#include "NvidiaOpticalFlowDenseJumpBytecode.h"
#include "NvidiaOpticalFlowDenseUpsampleBytecode.h"
#include "NvidiaOpticalFlowDenseWarpBytecode.h"
#include "Helper.h"

#include <algorithm>
#include <array>
#include <format>

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

void UnbindCompute(ID3D11DeviceContext* context)
{
    const std::array<ID3D11ShaderResourceView*, 5> nullSrvs = {};
    const std::array<ID3D11UnorderedAccessView*, 3> nullUavs = {};
    context->CSSetShaderResources(0, static_cast<UINT>(nullSrvs.size()), nullSrvs.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUavs.size()), nullUavs.data(), nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
}

} // namespace

bool CNvidiaOpticalFlowDenseSynthesizer::Initialize(ID3D11Device* device,
    const UINT frameWidth, const UINT frameHeight,
    const UINT flowWidth, const UINT flowHeight, std::wstring& status)
{
    Reset();
    if (!device || !frameWidth || !frameHeight || !flowWidth || !flowHeight) {
        status = L"Invalid dense NVOF synthesis dimensions or device";
        return false;
    }

    m_frameWidth = frameWidth;
    m_frameHeight = frameHeight;
    m_flowWidth = flowWidth;
    m_flowHeight = flowHeight;

    if (!CreateShader(device, g_NvofDenseSeedBytecode, sizeof(g_NvofDenseSeedBytecode),
            m_seedShader, status, L"dense NVOF validation") ||
        !CreateShader(device, g_NvofDenseRegionGateBytecode, sizeof(g_NvofDenseRegionGateBytecode),
            m_regionGateShader, status, L"dense NVOF regional frame gate") ||
        !CreateShader(device, g_NvofDenseJumpBytecode, sizeof(g_NvofDenseJumpBytecode),
            m_jumpShader, status, L"dense NVOF jump flood") ||
        !CreateShader(device, g_NvofDenseUpsampleBytecode, sizeof(g_NvofDenseUpsampleBytecode),
            m_denseShader, status, L"dense NVOF edge-aware upsample") ||
        !CreateShader(device, g_NvofDenseWarpBytecode, sizeof(g_NvofDenseWarpBytecode),
            m_warpShader, status, L"dense NVOF midpoint warp")) {
        Reset();
        return false;
    }

    D3D11_TEXTURE2D_DESC seedDesc = {};
    seedDesc.Width = flowWidth;
    seedDesc.Height = flowHeight;
    seedDesc.MipLevels = 1;
    seedDesc.ArraySize = 1;
    seedDesc.Format = DXGI_FORMAT_R32_UINT;
    seedDesc.SampleDesc.Count = 1;
    seedDesc.Usage = D3D11_USAGE_DEFAULT;
    seedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (auto index = 0u; index < 2u; ++index) {
        HRESULT hr = device->CreateTexture2D(&seedDesc, nullptr, &m_seedTextures[index]);
        if (FAILED(hr)) {
            status = std::format(L"CreateTexture2D(dense NVOF seed {}) failed ({})", index, HR2Str(hr));
            Reset();
            return false;
        }
        hr = device->CreateShaderResourceView(m_seedTextures[index], nullptr, &m_seedViews[index]);
        if (FAILED(hr)) {
            status = std::format(L"CreateShaderResourceView(dense NVOF seed {}) failed ({})", index, HR2Str(hr));
            Reset();
            return false;
        }
        hr = device->CreateUnorderedAccessView(m_seedTextures[index], nullptr, &m_seedUavs[index]);
        if (FAILED(hr)) {
            status = std::format(L"CreateUnorderedAccessView(dense NVOF seed {}) failed ({})", index, HR2Str(hr));
            Reset();
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC qualityDesc = {};
    qualityDesc.Width = 1;
    qualityDesc.Height = 1;
    qualityDesc.MipLevels = 1;
    qualityDesc.ArraySize = 1;
    qualityDesc.Format = DXGI_FORMAT_R32_UINT;
    qualityDesc.SampleDesc.Count = 1;
    qualityDesc.Usage = D3D11_USAGE_DEFAULT;
    qualityDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = device->CreateTexture2D(&qualityDesc, nullptr, &m_qualityTexture);
    if (FAILED(hr)) {
        status = std::format(L"CreateTexture2D(dense NVOF quality counter) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateShaderResourceView(m_qualityTexture, nullptr, &m_qualityView);
    if (FAILED(hr)) {
        status = std::format(L"CreateShaderResourceView(dense NVOF quality counter) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateUnorderedAccessView(m_qualityTexture, nullptr, &m_qualityUav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView(dense NVOF quality counter) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

    hr = device->CreateTexture2D(&seedDesc, nullptr, &m_unsafeCellTexture);
    if (FAILED(hr)) {
        status = std::format(L"CreateTexture2D(dense NVOF unsafe-cell map) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateShaderResourceView(m_unsafeCellTexture, nullptr, &m_unsafeCellView);
    if (FAILED(hr)) {
        status = std::format(L"CreateShaderResourceView(dense NVOF unsafe-cell map) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateUnorderedAccessView(m_unsafeCellTexture, nullptr, &m_unsafeCellUav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView(dense NVOF unsafe-cell map) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

    hr = device->CreateTexture2D(&qualityDesc, nullptr, &m_regionRejectTexture);
    if (FAILED(hr)) {
        status = std::format(L"CreateTexture2D(dense NVOF regional reject flag) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateShaderResourceView(m_regionRejectTexture, nullptr, &m_regionRejectView);
    if (FAILED(hr)) {
        status = std::format(L"CreateShaderResourceView(dense NVOF regional reject flag) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateUnorderedAccessView(m_regionRejectTexture, nullptr, &m_regionRejectUav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView(dense NVOF regional reject flag) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

    D3D11_TEXTURE2D_DESC denseDesc = {};
    denseDesc.Width = frameWidth;
    denseDesc.Height = frameHeight;
    denseDesc.MipLevels = 1;
    denseDesc.ArraySize = 1;
    denseDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    denseDesc.SampleDesc.Count = 1;
    denseDesc.Usage = D3D11_USAGE_DEFAULT;
    denseDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    hr = device->CreateTexture2D(&denseDesc, nullptr, &m_denseFlowTexture);
    if (FAILED(hr)) {
        status = std::format(L"CreateTexture2D(dense NVOF full-resolution flow) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateShaderResourceView(m_denseFlowTexture, nullptr, &m_denseFlowView);
    if (FAILED(hr)) {
        status = std::format(L"CreateShaderResourceView(dense NVOF full-resolution flow) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateUnorderedAccessView(m_denseFlowTexture, nullptr, &m_denseFlowUav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView(dense NVOF full-resolution flow) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

    if (!CreateConstantBuffer<SeedParameters>(device, m_seedParameters, status, L"dense NVOF seed params") ||
        !CreateConstantBuffer<RegionGateParameters>(device, m_regionGateParameters, status, L"dense NVOF regional-gate params") ||
        !CreateConstantBuffer<JumpParameters>(device, m_jumpParameters, status, L"dense NVOF jump params") ||
        !CreateConstantBuffer<DenseParameters>(device, m_denseParameters, status, L"dense NVOF upsample params") ||
        !CreateConstantBuffer<WarpParameters>(device, m_warpParameters, status, L"dense NVOF warp params")) {
        Reset();
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&samplerDesc, &m_linearSampler);
    if (FAILED(hr)) {
        status = std::format(L"CreateSamplerState(dense NVOF) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

    return true;
}

void CNvidiaOpticalFlowDenseSynthesizer::Reset()
{
    m_linearSampler.Release();
    m_warpParameters.Release();
    m_denseParameters.Release();
    m_jumpParameters.Release();
    m_seedParameters.Release();
    m_regionGateParameters.Release();
    m_denseFlowUav.Release();
    m_denseFlowView.Release();
    m_denseFlowTexture.Release();
    m_qualityUav.Release();
    m_qualityView.Release();
    m_qualityTexture.Release();
    m_regionRejectUav.Release();
    m_regionRejectView.Release();
    m_regionRejectTexture.Release();
    m_unsafeCellUav.Release();
    m_unsafeCellView.Release();
    m_unsafeCellTexture.Release();
    for (auto index = 0u; index < 2u; ++index) {
        m_seedUavs[index].Release();
        m_seedViews[index].Release();
        m_seedTextures[index].Release();
    }
    m_warpShader.Release();
    m_denseShader.Release();
    m_jumpShader.Release();
    m_regionGateShader.Release();
    m_seedShader.Release();
    m_frameWidth = m_frameHeight = m_flowWidth = m_flowHeight = 0;
}

bool CNvidiaOpticalFlowDenseSynthesizer::Dispatch(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* previousFrame,
    ID3D11ShaderResourceView* nextFrame,
    ID3D11ShaderResourceView* forwardFlowBtoA,
    ID3D11ShaderResourceView* backwardFlowAtoB,
    ID3D11UnorderedAccessView* output,
    const float midpointTime,
    std::wstring& status)
{
    if (!context || !previousFrame || !nextFrame || !forwardFlowBtoA || !backwardFlowAtoB || !output ||
            !m_seedShader || !m_regionGateShader || !m_jumpShader || !m_denseShader || !m_warpShader ||
            !m_qualityUav || !m_qualityView || !m_unsafeCellUav || !m_unsafeCellView ||
            !m_regionRejectUav || !m_regionRejectView) {
        status = L"Dense NVOF synthesis resources are incomplete";
        return false;
    }

    const UINT zero[4] = {};
    context->ClearUnorderedAccessViewUint(m_qualityUav, zero);
    context->ClearUnorderedAccessViewUint(m_regionRejectUav, zero);

    const SeedParameters seedValues = {
        m_flowWidth, m_flowHeight, 4.0f, 20.0f,
        20.0f, {0.0f, 0.0f, 0.0f},
    };
    context->UpdateSubresource(m_seedParameters, 0, nullptr, &seedValues, 0, 0);
    ID3D11Buffer* seedBuffer = m_seedParameters;
    const std::array<ID3D11ShaderResourceView*, 2> seedInputs = {
        forwardFlowBtoA, backwardFlowAtoB,
    };
    const std::array<ID3D11UnorderedAccessView*, 3> seedOutputs = {
        m_seedUavs[0], m_qualityUav, m_unsafeCellUav,
    };
    context->CSSetShader(m_seedShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &seedBuffer);
    context->CSSetShaderResources(0, static_cast<UINT>(seedInputs.size()), seedInputs.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(seedOutputs.size()), seedOutputs.data(), nullptr);
    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);
    UnbindCompute(context);

    // A local catastrophic cluster can be visually unacceptable even when it
    // occupies far less than the global 25% threshold. Reject the entire
    // inserted midpoint rather than compositing real-frame patches locally.
    const RegionGateParameters regionValues = {
        m_flowWidth, m_flowHeight, 8u, 3u,
    };
    context->UpdateSubresource(m_regionGateParameters, 0, nullptr, &regionValues, 0, 0);
    ID3D11Buffer* regionBuffer = m_regionGateParameters;
    ID3D11ShaderResourceView* regionInput = m_unsafeCellView;
    ID3D11UnorderedAccessView* regionOutput = m_regionRejectUav;
    context->CSSetShader(m_regionGateShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &regionBuffer);
    context->CSSetShaderResources(0, 1, &regionInput);
    context->CSSetUnorderedAccessViews(0, 1, &regionOutput, nullptr);
    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);
    UnbindCompute(context);

    UINT jumpStep = 1;
    const UINT maxDimension = std::max(m_flowWidth, m_flowHeight);
    while (jumpStep < maxDimension) {
        jumpStep <<= 1;
    }
    jumpStep >>= 1;

    auto seedRead = 0u;
    auto seedWrite = 1u;
    while (jumpStep >= 1) {
        const JumpParameters jumpValues = {
            m_flowWidth, m_flowHeight, jumpStep, 0,
        };
        context->UpdateSubresource(m_jumpParameters, 0, nullptr, &jumpValues, 0, 0);
        ID3D11Buffer* jumpBuffer = m_jumpParameters;
        ID3D11ShaderResourceView* jumpInput = m_seedViews[seedRead];
        ID3D11UnorderedAccessView* jumpOutput = m_seedUavs[seedWrite];
        context->CSSetShader(m_jumpShader, nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &jumpBuffer);
        context->CSSetShaderResources(0, 1, &jumpInput);
        context->CSSetUnorderedAccessViews(0, 1, &jumpOutput, nullptr);
        context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);
        UnbindCompute(context);
        std::swap(seedRead, seedWrite);
        if (jumpStep == 1) {
            break;
        }
        jumpStep >>= 1;
    }

    const DenseParameters denseValues = {
        m_frameWidth, m_frameHeight, m_flowWidth, m_flowHeight,
        4.0f, 1.25f, 0.10f, 8.0f,
    };
    context->UpdateSubresource(m_denseParameters, 0, nullptr, &denseValues, 0, 0);
    ID3D11Buffer* denseBuffer = m_denseParameters;
    const std::array<ID3D11ShaderResourceView*, 3> denseInputs = {
        nextFrame, forwardFlowBtoA, m_seedViews[seedRead],
    };
    ID3D11UnorderedAccessView* denseOutput = m_denseFlowUav;
    context->CSSetShader(m_denseShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &denseBuffer);
    context->CSSetShaderResources(0, static_cast<UINT>(denseInputs.size()), denseInputs.data());
    context->CSSetUnorderedAccessViews(0, 1, &denseOutput, nullptr);
    context->Dispatch((m_frameWidth + 7) / 8, (m_frameHeight + 7) / 8, 1);
    UnbindCompute(context);

    const WarpParameters warpValues = {
        m_frameWidth, m_frameHeight, m_flowWidth * m_flowHeight, 0.25f,
        midpointTime, {0.0f, 0.0f, 0.0f},
    };
    context->UpdateSubresource(m_warpParameters, 0, nullptr, &warpValues, 0, 0);
    ID3D11Buffer* warpBuffer = m_warpParameters;
    const std::array<ID3D11ShaderResourceView*, 5> warpInputs = {
        previousFrame, nextFrame, m_denseFlowView, m_qualityView, m_regionRejectView,
    };
    ID3D11SamplerState* sampler = m_linearSampler;
    context->CSSetShader(m_warpShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &warpBuffer);
    context->CSSetSamplers(0, 1, &sampler);
    context->CSSetShaderResources(0, static_cast<UINT>(warpInputs.size()), warpInputs.data());
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch((m_frameWidth + 7) / 8, (m_frameHeight + 7) / 8, 1);
    UnbindCompute(context);
    return true;
}
