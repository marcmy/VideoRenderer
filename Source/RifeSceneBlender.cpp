#include "stdafx.h"
#include "RifeSceneBlender.h"
#include "Shaders.h"

#include <algorithm>
#include <array>

namespace {

constexpr char kVertexShader[] = R"(
struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut output;
    output.uv = uv;
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return output;
}
)";

constexpr char kPixelShader[] = R"(
Texture2D<float4> First : register(t0);
Texture2D<float4> Second : register(t1);
SamplerState SourceSampler : register(s0);

cbuffer BlendConstants : register(b0) {
    float Amount;
    float3 Padding;
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 a = First.SampleLevel(SourceSampler, uv, 0.0);
    float4 b = Second.SampleLevel(SourceSampler, uv, 0.0);
    return lerp(a, b, saturate(Amount));
}
)";

bool CompatibleTexture(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& expected)
{
    if (!texture) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    return desc.Width == expected.Width
        && desc.Height == expected.Height
        && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
        && desc.SampleDesc.Count == 1;
}

} // namespace

struct CRifeSceneBlender::Impl
{
    CComPtr<ID3D11Device> device;
    CComPtr<ID3D11DeviceContext> immediate;
    CComPtr<ID3D11DeviceContext> deferred;
    CComPtr<ID3D11VertexShader> vertexShader;
    CComPtr<ID3D11PixelShader> pixelShader;
    CComPtr<ID3D11SamplerState> sampler;
    CComPtr<ID3D11Buffer> constants;

    void Reset()
    {
        constants.Release();
        sampler.Release();
        pixelShader.Release();
        vertexShader.Release();
        deferred.Release();
        immediate.Release();
        device.Release();
    }

    bool Initialize(ID3D11Device* requestedDevice)
    {
        if (!requestedDevice) {
            return false;
        }
        if (device == requestedDevice && immediate && deferred && vertexShader
                && pixelShader && sampler && constants) {
            return true;
        }

        Reset();
        device = requestedDevice;
        device->GetImmediateContext(&immediate);
        if (!immediate || FAILED(device->CreateDeferredContext(0, &deferred))) {
            Reset();
            return false;
        }

        CComPtr<ID3DBlob> vertexCode;
        CComPtr<ID3DBlob> pixelCode;
        if (FAILED(CompileShader(kVertexShader, nullptr, "vs_4_0", &vertexCode))
                || FAILED(CompileShader(kPixelShader, nullptr, "ps_4_0", &pixelCode))) {
            Reset();
            return false;
        }
        if (FAILED(device->CreateVertexShader(
                vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &vertexShader))
                || FAILED(device->CreatePixelShader(
                    pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &pixelShader))) {
            Reset();
            return false;
        }

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&samplerDesc, &sampler))) {
            Reset();
            return false;
        }

        D3D11_BUFFER_DESC constantDesc = {};
        constantDesc.ByteWidth = 16;
        constantDesc.Usage = D3D11_USAGE_DYNAMIC;
        constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&constantDesc, nullptr, &constants))) {
            Reset();
            return false;
        }

        CComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(immediate->QueryInterface(IID_PPV_ARGS(&multithread))) && multithread) {
            multithread->SetMultithreadProtected(TRUE);
        }
        return true;
    }

    bool Blend(
        ID3D11Texture2D* first,
        ID3D11Texture2D* second,
        ID3D11Texture2D* output,
        float timestep)
    {
        if (!first || !second || !output || !deferred || !immediate) {
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        output->GetDesc(&desc);
        if (!desc.Width || !desc.Height || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                || desc.SampleDesc.Count != 1
                || !CompatibleTexture(first, desc) || !CompatibleTexture(second, desc)) {
            return false;
        }

        CComPtr<ID3D11ShaderResourceView> firstView;
        CComPtr<ID3D11ShaderResourceView> secondView;
        CComPtr<ID3D11RenderTargetView> outputView;
        if (FAILED(device->CreateShaderResourceView(first, nullptr, &firstView))
                || FAILED(device->CreateShaderResourceView(second, nullptr, &secondView))
                || FAILED(device->CreateRenderTargetView(output, nullptr, &outputView))) {
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(deferred->Map(constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        const std::array<float, 4> values = {
            std::clamp(timestep, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f
        };
        memcpy(mapped.pData, values.data(), sizeof(values));
        deferred->Unmap(constants, 0);

        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(desc.Width);
        viewport.Height = static_cast<float>(desc.Height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        ID3D11ShaderResourceView* resources[] = {firstView, secondView};
        ID3D11Buffer* constantBuffers[] = {constants};
        ID3D11SamplerState* samplers[] = {sampler};
        ID3D11RenderTargetView* targets[] = {outputView};

        deferred->ClearState();
        deferred->IASetInputLayout(nullptr);
        deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        deferred->RSSetViewports(1, &viewport);
        deferred->VSSetShader(vertexShader, nullptr, 0);
        deferred->PSSetShader(pixelShader, nullptr, 0);
        deferred->PSSetShaderResources(0, 2, resources);
        deferred->PSSetSamplers(0, 1, samplers);
        deferred->PSSetConstantBuffers(0, 1, constantBuffers);
        deferred->OMSetRenderTargets(1, targets, nullptr);
        deferred->Draw(3, 0);

        ID3D11ShaderResourceView* nullResources[2] = {};
        ID3D11RenderTargetView* nullTarget = nullptr;
        deferred->PSSetShaderResources(0, 2, nullResources);
        deferred->OMSetRenderTargets(1, &nullTarget, nullptr);

        CComPtr<ID3D11CommandList> commands;
        if (FAILED(deferred->FinishCommandList(FALSE, &commands)) || !commands) {
            return false;
        }

        // Executing one command list is atomic from MPCVR's point of view and
        // TRUE restores the renderer's immediate-context state afterward.
        immediate->ExecuteCommandList(commands, TRUE);
        immediate->Flush();
        return true;
    }
};

CRifeSceneBlender::CRifeSceneBlender()
    : m_impl(std::make_unique<Impl>())
{
}

CRifeSceneBlender::~CRifeSceneBlender() = default;

bool CRifeSceneBlender::Blend(
    ID3D11Device* device,
    ID3D11Texture2D* first,
    ID3D11Texture2D* second,
    ID3D11Texture2D* output,
    float timestep)
{
    return m_impl && m_impl->Initialize(device)
        && m_impl->Blend(first, second, output, timestep);
}

void CRifeSceneBlender::Reset() noexcept
{
    if (m_impl) {
        m_impl->Reset();
    }
}
