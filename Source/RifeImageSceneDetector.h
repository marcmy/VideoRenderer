/*
 * (C) 2026 see Authors.txt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <d3d11_4.h>
#include <atlbase.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include "RifePrecision.h"

// Full-image difference reduced on the GPU to one sum/count per 16x16 tile.
struct RifeImageSampleGrid {
    UINT width = 0, height = 0, columns = 0, rows = 0;
    static RifeImageSampleGrid ForSize(UINT w, UINT h) {
        return {w, h, (w + 15) / 16, (h + 15) / 16};
    }
    UINT Count() const { return columns * rows; }
};

struct RifeImageSceneStatistics {
    uint64_t difference = 0, samples = 0;
    void Add(const uint8_t* a, const uint8_t* b) {
        // RGB textures are already normalized by the video processor. Keep
        // GPU and CPU fallback arithmetic exact and ignore alpha.
        const int ya = 722 * a[0] + 7152 * a[1] + 2126 * a[2];
        const int yb = 722 * b[0] + 7152 * b[1] + 2126 * b[2];
        difference += std::abs(ya - yb);
        ++samples;
    }
    bool Finish(bool& likelyCut, double threshold = 0.15) const {
        likelyCut = false;
        if (!samples) return false;
        // SVP's misc.SCDetect convention: mean absolute normalized luma
        // difference strictly exceeds the selected threshold.
        likelyCut = static_cast<double>(difference)
            / (static_cast<double>(samples) * 2550000.0) > std::clamp(threshold, 0.0, 1.0);
        return true;
    }
};

class CRifeImageSceneDetector {
public:
    using ShaderCompiler = HRESULT (*)(const std::string&, const D3D_SHADER_MACRO*, LPCSTR, ID3DBlob**);

    explicit CRifeImageSceneDetector(ShaderCompiler compiler) : m_compiler(compiler) {}

    bool Analyze(ID3D11Device* device, ID3D11Texture2D* first, ID3D11Texture2D* second,
        bool& likelyCut, double threshold = 0.15, UINT contentWidth = 0, UINT contentHeight = 0)
    {
        likelyCut = false;
        m_compactReadbackUsed = false;
        if (!device || !first || !second) return false;
        D3D11_TEXTURE2D_DESC desc = {}, secondDesc = {};
        first->GetDesc(&desc);
        second->GetDesc(&secondDesc);
        if (desc.Width != secondDesc.Width || desc.Height != secondDesc.Height
                || !RifeSupportedSurfaceFormat(desc.Format) || secondDesc.Format != desc.Format) {
            return false;
        }
        // TensorRT padding is not part of the picture being compared.
        contentWidth = contentWidth ? std::min(contentWidth, desc.Width) : desc.Width;
        contentHeight = contentHeight ? std::min(contentHeight, desc.Height) : desc.Height;
        const auto grid = RifeImageSampleGrid::ForSize(contentWidth, contentHeight);
        if (!grid.Count() || !EnsureResources(device, desc.Width, desc.Height, contentWidth, contentHeight, desc.Format)) return false;

        RifeImageSceneStatistics statistics;
        if (Gather(first, second, grid, statistics)) {
            m_compactReadbackUsed = true;
            return statistics.Finish(likelyCut, threshold);
        }
        // Use the same full-image difference on older feature levels or shader/view
        // setup failure, including when used as NVOF's existing image fallback.
        return AnalyzeFullFrames(first, second, grid, likelyCut, threshold);
    }

    bool UsedCompactReadback() const { return m_compactReadbackUsed; }

    void Reset()
    {
        m_views = {};
        m_nextView = 0;
        m_constants.Release();
        m_samplesView.Release();
        m_samples.Release();
        m_readback.Release();
        m_shader.Release();
        m_recording.Release();
        m_secondStaging.Release();
        m_firstStaging.Release();
        m_multithread.Release();
        m_context.Release();
        m_device.Release();
        m_width = m_height = m_contentWidth = m_contentHeight = 0;
        m_gatherUnavailable = m_compactReadbackUsed = false;
    }

private:
    struct ContextLock {
        ID3D11Multithread* multithread;
        explicit ContextLock(ID3D11Multithread* value) : multithread(value) { if (multithread) multithread->Enter(); }
        ~ContextLock() { if (multithread) multithread->Leave(); }
    };
    struct ViewEntry {
        CComPtr<ID3D11Texture2D> texture;
        CComPtr<ID3D11ShaderResourceView> view;
    };

    bool EnsureResources(ID3D11Device* device, UINT width, UINT height, UINT contentWidth, UINT contentHeight, DXGI_FORMAT format)
    {
        if (m_device == device && m_width == width && m_height == height && m_context
                && m_contentWidth == contentWidth && m_contentHeight == contentHeight && m_format == format) return true;
        Reset();
        m_device = device;
        m_width = width; m_height = height; m_format = format;
        m_contentWidth = contentWidth; m_contentHeight = contentHeight;
        device->GetImmediateContext(&m_context);
        if (!m_context) return false;
        if (SUCCEEDED(m_context->QueryInterface(IID_PPV_ARGS(&m_multithread)))) {
            m_multithread->SetMultithreadProtected(TRUE);
        }
        return true;
    }

    bool EnsureGather(const RifeImageSampleGrid& grid)
    {
        if (m_shader && m_recording && m_samples && m_samplesView && m_readback && m_constants) return true;
        if (m_gatherUnavailable) return false;
        m_gatherUnavailable = true;
        if (!m_compiler || m_device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return false;
        static const std::string source = R"(
Texture2D<float4> firstFrame : register(t0);
Texture2D<float4> secondFrame : register(t1);
RWStructuredBuffer<uint2> samples : register(u0);
cbuffer Grid : register(b0) { uint width; uint height; uint columns; uint rows; };
groupshared uint differences[256];
groupshared uint counts[256];
int Luma(float4 value) {
    uint3 rgb = (uint3)round(saturate(value.rgb) * 255.0);
    return rgb.r * 2126 + rgb.g * 7152 + rgb.b * 722;
}
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID,
          uint index : SV_GroupIndex) {
    bool valid = id.x < width && id.y < height;
    uint difference = 0;
    if (valid) {
        int3 position = int3(id.xy, 0);
        difference = abs(Luma(firstFrame.Load(position)) - Luma(secondFrame.Load(position)));
    }
    differences[index] = difference;
    counts[index] = valid ? 1 : 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (index < stride) {
            differences[index] += differences[index + stride];
            counts[index] += counts[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) samples[group.y * columns + group.x] = uint2(differences[0], counts[0]);
}
)";
        CComPtr<ID3DBlob> code;
        if (FAILED(m_compiler(source, nullptr, "cs_5_0", &code))
                || FAILED(m_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_shader))
                || FAILED(m_device->CreateDeferredContext(0, &m_recording))) return false;

        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = grid.Count() * 2 * sizeof(uint32_t);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = 2 * sizeof(uint32_t);
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_samples))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = grid.Count();
        if (FAILED(m_device->CreateUnorderedAccessView(m_samples, &view, &m_samplesView))) return false;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_readback))) return false;
        desc = {};
        desc.ByteWidth = sizeof(RifeImageSampleGrid);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA data = {&grid, 0, 0};
        if (FAILED(m_device->CreateBuffer(&desc, &data, &m_constants))) return false;
        m_gatherUnavailable = false;
        return true;
    }

    ID3D11ShaderResourceView* SourceView(ID3D11Texture2D* texture)
    {
        for (const auto& entry : m_views) {
            if (entry.texture == texture) return entry.view;
        }
        CComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(m_device->CreateShaderResourceView(texture, nullptr, &view))) return nullptr;
        auto& entry = m_views[m_nextView++ % m_views.size()];
        entry.texture = texture;
        entry.view = view;
        return view;
    }

    bool Gather(ID3D11Texture2D* first, ID3D11Texture2D* second,
        const RifeImageSampleGrid& grid, RifeImageSceneStatistics& statistics)
    {
        if (!EnsureGather(grid)) return false;
        ID3D11ShaderResourceView* views[] = {SourceView(first), SourceView(second)};
        if (!views[0] || !views[1]) return false;
        // Record on a private context and restore the shared context's state.
        // A worker must not leak its CS bindings or unbind the presenter's RTV.
        m_recording->OMSetRenderTargets(0, nullptr, nullptr);
        m_recording->CSSetShader(m_shader, nullptr, 0);
        m_recording->CSSetShaderResources(0, 2, views);
        ID3D11UnorderedAccessView* output = m_samplesView;
        m_recording->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        ID3D11Buffer* constants = m_constants;
        m_recording->CSSetConstantBuffers(0, 1, &constants);
        m_recording->Dispatch(grid.columns, grid.rows, 1);
        CComPtr<ID3D11CommandList> commands;
        if (FAILED(m_recording->FinishCommandList(FALSE, &commands))) return false;
        {
            ContextLock lock(m_multithread);
            m_context->ExecuteCommandList(commands, TRUE);
            m_context->CopyResource(m_readback, m_samples);
            m_context->Flush();
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_context->Map(m_readback, 0, D3D11_MAP_READ, 0, &mapped))) return false;
        const auto* pairs = static_cast<const uint32_t*>(mapped.pData);
        for (UINT i = 0; i < grid.Count(); ++i) {
            statistics.difference += pairs[i * 2];
            statistics.samples += pairs[i * 2 + 1];
        }
        m_context->Unmap(m_readback, 0);
        return true;
    }

    bool AnalyzeFullFrames(ID3D11Texture2D* first, ID3D11Texture2D* second,
        const RifeImageSampleGrid& grid, bool& likelyCut, double threshold)
    {
        if (!m_firstStaging || !m_secondStaging) {
            m_firstStaging.Release(); m_secondStaging.Release();
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = m_width; desc.Height = m_height;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = m_format;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_firstStaging))
                    || FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_secondStaging))) return false;
        }
        {
            ContextLock lock(m_multithread);
            m_context->CopyResource(m_firstStaging, first);
            m_context->CopyResource(m_secondStaging, second);
            m_context->Flush();
        }
        D3D11_MAPPED_SUBRESOURCE a = {}, b = {};
        if (FAILED(m_context->Map(m_firstStaging, 0, D3D11_MAP_READ, 0, &a))) return false;
        if (FAILED(m_context->Map(m_secondStaging, 0, D3D11_MAP_READ, 0, &b))) {
            m_context->Unmap(m_firstStaging, 0);
            return false;
        }
        RifeImageSceneStatistics statistics;
        for (UINT y = 0; y < grid.height; ++y) {
            const auto* rowA = static_cast<const uint8_t*>(a.pData) + static_cast<size_t>(y) * a.RowPitch;
            const auto* rowB = static_cast<const uint8_t*>(b.pData) + static_cast<size_t>(y) * b.RowPitch;
            for (UINT x = 0; x < grid.width; ++x) {
                if (m_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    uint8_t a8[4], b8[4];
                    RifeAnalysisBgra(reinterpret_cast<const uint16_t*>(rowA + size_t(x) * 8), a8);
                    RifeAnalysisBgra(reinterpret_cast<const uint16_t*>(rowB + size_t(x) * 8), b8);
                    statistics.Add(a8, b8);
                } else statistics.Add(rowA + static_cast<size_t>(x) * 4, rowB + static_cast<size_t>(x) * 4);
            }
        }
        m_context->Unmap(m_secondStaging, 0);
        m_context->Unmap(m_firstStaging, 0);
        return statistics.Finish(likelyCut, threshold);
    }

    ShaderCompiler m_compiler;
    CComPtr<ID3D11Device> m_device;
    CComPtr<ID3D11DeviceContext> m_context, m_recording;
    CComPtr<ID3D11Multithread> m_multithread;
    CComPtr<ID3D11ComputeShader> m_shader;
    CComPtr<ID3D11Buffer> m_samples, m_readback, m_constants;
    CComPtr<ID3D11UnorderedAccessView> m_samplesView;
    CComPtr<ID3D11Texture2D> m_firstStaging, m_secondStaging;
    std::array<ViewEntry, 16> m_views;
    size_t m_nextView = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    UINT m_width = 0, m_height = 0;
    UINT m_contentWidth = 0, m_contentHeight = 0;
    bool m_gatherUnavailable = false, m_compactReadbackUsed = false;
};

static_assert(sizeof(RifeImageSampleGrid) == 16);
