#include "stdafx.h"
#include <uuids.h>
#include <Mferror.h>
#include <Mfidl.h>
#include <algorithm>
#include "Helper.h"
#include "Times.h"
#include "VideoRenderer.h"
#include "DX11VideoProcessor.h"

bool CDX11VideoProcessor::PrepareRifeSource(
    IMediaSample* pSample,
    ID3D11Texture2D* target,
    REFERENCE_TIME& sourceTime)
{
    sourceTime = INVALID_TIME;
#ifdef _WIN64
    if (!pSample || !target || !m_pDevice || !m_pDeviceContext || !m_pDXGISwapChain1) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    target->GetDesc(&desc);
    const CSize rifeSize = GetRifeFrameSize();
    const UINT width = static_cast<UINT>(std::max<LONG>(0, rifeSize.cx));
    const UINT height = static_cast<UINT>(std::max<LONG>(0, rifeSize.cy));
    if (!width || !height || desc.Width != width || desc.Height != height
            || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1) {
        return false;
    }

    REFERENCE_TIME rtStart = 0, rtEnd = 0;
    if (FAILED(pSample->GetTime(&rtStart, &rtEnd))) {
        rtStart = m_pFilter->m_FrameStats.GeTimestamp();
    }
    sourceTime = rtStart;
    m_rtStart = rtStart;

    HRESULT hr = CopySample(pSample);
    if (FAILED(hr)) {
        return false;
    }

    CComPtr<ID3D11RenderTargetView> targetView;
    hr = m_pDevice->CreateRenderTargetView(target, nullptr, &targetView);
    if (FAILED(hr)) {
        return false;
    }
    const FLOAT clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_pDeviceContext->ClearRenderTargetView(targetView, clearColor);

    // Process() deliberately stops before the normal Render() subtitle/OSD
    // composition. RIFE therefore sees only the video image; subtitles and
    // statistics are drawn later when the prepared texture is presented.
    hr = Process(target, m_srcRect, m_videoRect, false);
    if (FAILED(hr)) {
        return false;
    }

    // Publish D3D11 writes before handing the source to NVOF/CUDA on the
    // worker thread. Interop APIs provide resource ownership synchronization.
    m_pDeviceContext->Flush();
    return true;
#else
    UNREFERENCED_PARAMETER(pSample);
    UNREFERENCED_PARAMETER(target);
    return false;
#endif
}

bool CDX11VideoProcessor::ReserveRifePresentationSurface(
    ID3D11Texture2D* source,
    UINT& sourceSurface)
{
    sourceSurface = UINT_MAX;
#ifdef _WIN64
    if (!source || !m_pDevice || !m_pDeviceContext) {
        return false;
    }

    CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

    UINT freeSurface = UINT_MAX;
    for (UINT i = 0; i < FrameInterpolationSurfaceCount; ++i) {
        if (!m_FrameInterpolationPresentationSurfaces[i].inUse) {
            freeSurface = i;
            break;
        }
    }
    if (freeSurface == UINT_MAX) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    if (!desc.Width || !desc.Height || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
            || desc.SampleDesc.Count != 1) {
        return false;
    }

    auto& surface = m_FrameInterpolationPresentationSurfaces[freeSurface];
    const HRESULT hr = surface.texture.CheckCreate(
        m_pDevice, desc.Format, desc.Width, desc.Height, Tex2D_DefaultShader);
    if (FAILED(hr)) {
        return false;
    }

    m_pDeviceContext->CopyResource(surface.texture.pTexture, source);
    m_pDeviceContext->Flush();
    surface.inUse = true;
    sourceSurface = freeSurface;
    return true;
#else
    UNREFERENCED_PARAMETER(source);
    return false;
#endif
}
