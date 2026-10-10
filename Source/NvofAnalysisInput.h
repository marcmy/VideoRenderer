/*
 * (C) 2026 see Authors.txt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <d3d11_4.h>
#include <atlbase.h>
#include <array>
#include <algorithm>
#include <string>
#include "RifePrecision.h"

struct NvofAnalysisSize {
    UINT width = 0, height = 0;
    static constexpr NvofAnalysisSize ForContent(UINT width, UINT height) {
        // Keep at least 40x32 flow cells when reducing. Small pictures remain
        // at native size; never pad or upscale an optical-flow input.
        UINT divisor = 1;
        if (width / 4 >= 160 && height / 4 >= 128) divisor = 4;
        else if (width / 2 >= 160 && height / 2 >= 128) divisor = 2;
        return {(width + divisor - 1) / divisor, (height + divisor - 1) / divisor};
    }
    std::array<float, 2> SourceVector(float x, float y, UINT contentWidth, UINT contentHeight) const {
        return {x * static_cast<float>(contentWidth) / width,
                y * static_cast<float>(contentHeight) / height};
    }
};

// GPU-only area reduction of both source pictures. The private deferred
// context prevents interference with renderer state on the shared context.
// No frame readback or CUDA ownership changes occur here.
class CNvofAnalysisInput {
public:
    using ShaderCompiler = HRESULT (*)(const std::string&, const D3D_SHADER_MACRO*, LPCSTR, ID3DBlob**);
    explicit CNvofAnalysisInput(ShaderCompiler compiler) : m_compiler(compiler) {}

    void Reset() {
        m_sources = {}; m_targets = {}; m_nextSource = m_nextTarget = 0;
        m_constants.Release(); m_vertex.Release(); m_pixel.Release();
        m_rasterizer.Release(); m_recording.Release(); m_multithread.Release();
        m_context.Release(); m_device.Release();
        m_contentWidth = m_contentHeight = 0; m_size = {}; m_shaderUnavailable = false;
    }

    bool Prepare(ID3D11Device* device, ID3D11Texture2D* first, ID3D11Texture2D* second,
        ID3D11Texture2D* firstOutput, ID3D11Texture2D* secondOutput,
        UINT contentWidth, UINT contentHeight)
    {
        const auto size = NvofAnalysisSize::ForContent(contentWidth, contentHeight);
        if (!device || !contentWidth || !contentHeight || !first || !second || !firstOutput || !secondOutput
                || first == firstOutput || first == secondOutput || second == firstOutput || second == secondOutput
                || firstOutput == secondOutput) return false;
        D3D11_TEXTURE2D_DESC a = {}, b = {}, c = {}, d = {};
        first->GetDesc(&a); second->GetDesc(&b); firstOutput->GetDesc(&c); secondOutput->GetDesc(&d);
        if (a.Width != b.Width || a.Height != b.Height || contentWidth > a.Width || contentHeight > a.Height
                || a.Format != b.Format || !RifeSupportedSurfaceFormat(a.Format)
                || c.Format != DXGI_FORMAT_B8G8R8A8_UNORM || d.Format != c.Format
                || c.Width != size.width || d.Width != size.width || c.Height != size.height || d.Height != size.height) return false;
        for (ID3D11Texture2D* texture : {first, second, firstOutput, secondOutput}) {
            D3D11_TEXTURE2D_DESC desc = {}; texture->GetDesc(&desc);
            CComPtr<ID3D11Device> owner; texture->GetDevice(&owner);
            if (owner != device
                    || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1) return false;
        }
        if (m_device != device || m_contentWidth != contentWidth || m_contentHeight != contentHeight) {
            Reset(); m_device = device; m_contentWidth = contentWidth; m_contentHeight = contentHeight; m_size = size;
            device->GetImmediateContext(&m_context);
            if (!m_context || FAILED(m_context->QueryInterface(IID_PPV_ARGS(&m_multithread)))) return false;
            m_multithread->SetMultithreadProtected(TRUE);
        }
        if (a.Format == DXGI_FORMAT_B8G8R8A8_UNORM && size.width == contentWidth && size.height == contentHeight) {
            const D3D11_BOX box = {0, 0, 0, contentWidth, contentHeight, 1};
            ContextLock lock(m_multithread);
            m_context->CopySubresourceRegion(firstOutput, 0, 0, 0, 0, first, 0, &box);
            m_context->CopySubresourceRegion(secondOutput, 0, 0, 0, 0, second, 0, &box);
            return true;
        }
        if (!(a.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !(b.BindFlags & D3D11_BIND_SHADER_RESOURCE)
                || !(c.BindFlags & D3D11_BIND_RENDER_TARGET) || !(d.BindFlags & D3D11_BIND_RENDER_TARGET)) return false;
        if (!EnsureShaders()) return false;
        // Retain both views while looking up the pair: a cache miss for the
        // second texture may evict the first texture's cached entry.
        CComPtr<ID3D11ShaderResourceView> firstView = SourceView(first), secondView = SourceView(second);
        CComPtr<ID3D11RenderTargetView> firstTarget = TargetView(firstOutput), secondTarget = TargetView(secondOutput);
        ID3D11ShaderResourceView* sources[] = {firstView, secondView};
        ID3D11RenderTargetView* targets[] = {firstTarget, secondTarget};
        if (!sources[0] || !sources[1] || !targets[0] || !targets[1]) return false;
        m_recording->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_recording->VSSetShader(m_vertex, nullptr, 0);
        m_recording->PSSetShader(m_pixel, nullptr, 0);
        m_recording->RSSetState(m_rasterizer);
        const D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(size.width), static_cast<float>(size.height), 0, 1};
        m_recording->RSSetViewports(1, &viewport);
        ID3D11Buffer* constants = m_constants;
        m_recording->PSSetConstantBuffers(0, 1, &constants);
        for (UINT i = 0; i < 2; ++i) {
            m_recording->OMSetRenderTargets(1, &targets[i], nullptr);
            m_recording->PSSetShaderResources(0, 1, &sources[i]);
            m_recording->Draw(3, 0);
        }
        CComPtr<ID3D11CommandList> commands;
        if (FAILED(m_recording->FinishCommandList(FALSE, &commands))) return false;
        ContextLock lock(m_multithread);
        m_context->ExecuteCommandList(commands, TRUE);
        return true;
    }

private:
    struct ContextLock {
        ID3D11Multithread* value;
        explicit ContextLock(ID3D11Multithread* value) : value(value) { value->Enter(); }
        ~ContextLock() { value->Leave(); }
    };
    struct SourceEntry { CComPtr<ID3D11Texture2D> texture; CComPtr<ID3D11ShaderResourceView> view; };
    struct TargetEntry { CComPtr<ID3D11Texture2D> texture; CComPtr<ID3D11RenderTargetView> view; };

    ID3D11ShaderResourceView* SourceView(ID3D11Texture2D* texture) {
        for (const auto& entry : m_sources) if (entry.texture == texture) return entry.view;
        CComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(m_device->CreateShaderResourceView(texture, nullptr, &view))) return nullptr;
        auto& entry = m_sources[m_nextSource++ % m_sources.size()];
        entry.texture = texture; entry.view = view; return view;
    }
    ID3D11RenderTargetView* TargetView(ID3D11Texture2D* texture) {
        for (const auto& entry : m_targets) if (entry.texture == texture) return entry.view;
        CComPtr<ID3D11RenderTargetView> view;
        if (FAILED(m_device->CreateRenderTargetView(texture, nullptr, &view))) return nullptr;
        auto& entry = m_targets[m_nextTarget++ % m_targets.size()];
        entry.texture = texture; entry.view = view; return view;
    }
    bool EnsureShaders() {
        if (m_vertex && m_pixel && m_constants && m_recording && m_rasterizer) return true;
        if (m_shaderUnavailable) return false;
        m_shaderUnavailable = true;
        m_vertex.Release(); m_pixel.Release(); m_constants.Release(); m_recording.Release(); m_rasterizer.Release();
        if (!m_compiler || m_device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return false;
        static const std::string vertex = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
})";
        static const std::string pixel = R"(
Texture2D<float4> frame : register(t0);
cbuffer Geometry : register(b0) { uint contentWidth; uint contentHeight; uint outputWidth; uint outputHeight; };
float4 main(float4 position : SV_Position) : SV_Target {
    float2 scale = float2(contentWidth, contentHeight) / float2(outputWidth, outputHeight);
    float2 low = floor(position.xy) * scale;
    float2 high = min(low + scale, float2(contentWidth, contentHeight));
    float4 sum = 0;
    // Area weights preserve odd right/bottom edges and suppress aliasing.
    // Load never visits TensorRT's padded rows or columns.
    for (uint y = (uint)floor(low.y); y < (uint)ceil(high.y); ++y) {
        float wy = max(0, min(high.y, y + 1.0) - max(low.y, (float)y));
        for (uint x = (uint)floor(low.x); x < (uint)ceil(high.x); ++x) {
            float wx = max(0, min(high.x, x + 1.0) - max(low.x, (float)x));
            sum += frame.Load(int3(x, y, 0)) * (wx * wy);
        }
    }
    return sum / ((high.x - low.x) * (high.y - low.y));
})";
        CComPtr<ID3DBlob> code;
        if (FAILED(m_compiler(vertex, nullptr, "vs_5_0", &code))
                || FAILED(m_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_vertex))) return false;
        code.Release();
        if (FAILED(m_compiler(pixel, nullptr, "ps_5_0", &code))
                || FAILED(m_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_pixel))
                || FAILED(m_device->CreateDeferredContext(0, &m_recording))) return false;
        D3D11_RASTERIZER_DESC rasterizer = {};
        rasterizer.FillMode = D3D11_FILL_SOLID; rasterizer.CullMode = D3D11_CULL_NONE; rasterizer.DepthClipEnable = TRUE;
        if (FAILED(m_device->CreateRasterizerState(&rasterizer, &m_rasterizer))) return false;
        const std::array<UINT, 4> geometry = {m_contentWidth, m_contentHeight, m_size.width, m_size.height};
        D3D11_BUFFER_DESC desc = {}; desc.ByteWidth = sizeof(geometry);
        desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA data = {geometry.data(), 0, 0};
        if (FAILED(m_device->CreateBuffer(&desc, &data, &m_constants))) return false;
        m_shaderUnavailable = false;
        return true;
    }
    ShaderCompiler m_compiler;
    CComPtr<ID3D11Device> m_device;
    CComPtr<ID3D11DeviceContext> m_context, m_recording;
    CComPtr<ID3D11Multithread> m_multithread;
    CComPtr<ID3D11VertexShader> m_vertex;
    CComPtr<ID3D11PixelShader> m_pixel;
    CComPtr<ID3D11Buffer> m_constants;
    CComPtr<ID3D11RasterizerState> m_rasterizer;
    std::array<SourceEntry, 12> m_sources;
    std::array<TargetEntry, 2> m_targets;
    size_t m_nextSource = 0, m_nextTarget = 0;
    UINT m_contentWidth = 0, m_contentHeight = 0;
    NvofAnalysisSize m_size;
    bool m_shaderUnavailable = false;
};
