/*
 * (C) 2026 see Authors.txt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <d3dcompiler.h>
#include "NvofAnalysisInput.h"
#include "SvpSceneMotion.h"
#include <vector>

// GPU area reduction -> bounded readback -> GPL SVPflow1 CPU motion search.
// A private recorder and restored immediate-context state isolate worker use.
class CSvpSceneDetector {
public:
    using ShaderCompiler = CNvofAnalysisInput::ShaderCompiler;
    explicit CSvpSceneDetector(ShaderCompiler compiler) : m_input(compiler) {}
    void Reset() {
        m_input.Reset(); m_motion.Reset(); m_output = {}; m_staging = {};
        m_device.Release(); m_context.Release(); m_multithread.Release();
        m_pixels = {}; m_size = {}; m_width = m_height = 0;
    }
    bool Analyze(ID3D11Device* device, ID3D11Texture2D* first, ID3D11Texture2D* second,
        UINT width, UINT height, SvpSceneResult& result) {
        result = {};
        const auto size = NvofAnalysisSize::ForContent(width, height);
        if (!device || !first || !second || size.width < 32 || size.height < 32
            || size.width > 4096 || size.height > 4096
            || static_cast<uint64_t>(size.width) * size.height > 1024 * 1024) return false;
        if (!EnsureResources(device, width, height, size)) return false;
        if (!m_input.Prepare(device, first, second, m_output[0], m_output[1], width, height)) return false;
        {
            ContextLock lock(m_multithread);
            for (UINT i = 0; i < 2; ++i) m_context->CopyResource(m_staging[i], m_output[i]);
            for (UINT i = 0; i < 2; ++i) {
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (FAILED(m_context->Map(m_staging[i], 0, D3D11_MAP_READ, 0, &mapped))) return false;
                for (UINT y = 0; y < size.height; ++y) {
                    memcpy(m_pixels[i].data() + static_cast<size_t>(y) * size.width * 4,
                        static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                        size.width * 4);
                }
                m_context->Unmap(m_staging[i], 0);
            }
        }
        // Never hold the shared renderer context while searching on the CPU.
        return m_motion.Analyze(m_pixels[0].data(), size.width * 4, m_pixels[1].data(), size.width * 4,
            size.width, size.height, result);
    }
    NvofAnalysisSize Size() const { return m_size; }
private:
    struct ContextLock {
        ID3D11Multithread* value;
        explicit ContextLock(ID3D11Multithread* value) : value(value) { value->Enter(); }
        ~ContextLock() { value->Leave(); }
    };
    bool EnsureResources(ID3D11Device* device, UINT width, UINT height, NvofAnalysisSize size) {
        if (m_device == device && m_width == width && m_height == height && m_staging[1]) return true;
        Reset(); m_device = device; m_width = width; m_height = height; m_size = size;
        device->GetImmediateContext(&m_context);
        if (!m_context || FAILED(m_context->QueryInterface(IID_PPV_ARGS(&m_multithread)))) return false;
        m_multithread->SetMultithreadProtected(TRUE);
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = size.width; desc.Height = size.height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (auto& texture : m_output) if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) { Reset(); return false; }
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& texture : m_staging) if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) { Reset(); return false; }
        try {
            for (auto& pixels : m_pixels) pixels.resize(static_cast<size_t>(size.width) * size.height * 4);
        } catch (const std::bad_alloc&) { Reset(); return false; }
        return true;
    }
    CNvofAnalysisInput m_input;
    CSvpSceneMotion m_motion;
    CComPtr<ID3D11Device> m_device;
    CComPtr<ID3D11DeviceContext> m_context;
    CComPtr<ID3D11Multithread> m_multithread;
    std::array<CComPtr<ID3D11Texture2D>, 2> m_output, m_staging;
    std::array<std::vector<uint8_t>, 2> m_pixels;
    NvofAnalysisSize m_size;
    UINT m_width = 0, m_height = 0;
};
