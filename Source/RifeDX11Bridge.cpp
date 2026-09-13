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
    const CSize contentSize = GetRifeContentSize();
    const CSize rifeSize = GetRifeFrameSize();
    const UINT width = static_cast<UINT>(std::max<LONG>(0, rifeSize.cx));
    const UINT height = static_cast<UINT>(std::max<LONG>(0, rifeSize.cy));
    if (contentSize.cx <= 0 || contentSize.cy <= 0 || !width || !height
            || desc.Width != width || desc.Height != height
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
        RecordRifeD3DFailure(RIFE_D3D_FAILURE_PREPARE_COPY_SAMPLE, hr);
        return false;
    }

    CComPtr<ID3D11RenderTargetView> targetView;
    hr = m_pDevice->CreateRenderTargetView(target, nullptr, &targetView);
    if (FAILED(hr)) {
        RecordRifeD3DFailure(RIFE_D3D_FAILURE_PREPARE_CREATE_RTV, hr);
        return false;
    }
    const FLOAT clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_pDeviceContext->ClearRenderTargetView(targetView, clearColor);

    // Process() deliberately stops before the normal Render() subtitle/OSD
    // composition. RIFE therefore sees only the video image; subtitles and
    // statistics are drawn later when the prepared texture is presented.
    const CRect contentRect(0, 0, contentSize.cx, contentSize.cy);
    hr = Process(target, m_srcRect, contentRect, false, true);
    if (FAILED(hr)) {
        RecordRifeD3DFailure(RIFE_D3D_FAILURE_PREPARE_PROCESS, hr);
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
        auto& candidate = m_FrameInterpolationPresentationSurfaces[i];
        if (candidate.inUse) {
            continue;
        }
        if (candidate.retirePending) {
            if (!candidate.retireQuery) {
                candidate.retirePending = false;
            } else {
                const HRESULT queryHr = m_pDeviceContext->GetData(
                    candidate.retireQuery, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (queryHr == S_FALSE) {
                    continue;
                }
                if (FAILED(queryHr)) {
                    RecordRifeD3DFailure(RIFE_D3D_FAILURE_PRESENT_RETIRE_QUERY, queryHr);
                    continue;
                }
                candidate.retirePending = false;
            }
        }
        freeSurface = i;
        break;
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
        m_pDevice, desc.Format, desc.Width, desc.Height, Tex2D_DefaultShaderRTarget);
    if (FAILED(hr)) {
        RecordRifeD3DFailure(RIFE_D3D_FAILURE_PRESENT_CREATE_TEXTURE, hr);
        return false;
    }

    if (!surface.retireQuery) {
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_EVENT;
        const HRESULT queryHr = m_pDevice->CreateQuery(&queryDesc, &surface.retireQuery);
        if (FAILED(queryHr)) {
            RecordRifeD3DFailure(RIFE_D3D_FAILURE_PRESENT_CREATE_QUERY, queryHr);
            return false;
        }
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

bool CDX11VideoProcessor::AcquireRifePresentationSurface(
    const UINT width,
    const UINT height,
    ID3D11Texture2D** target,
    UINT& sourceSurface)
{
    sourceSurface = UINT_MAX;
    if (target) {
        *target = nullptr;
    }
#ifdef _WIN64
    if (!target || !width || !height || !m_pDevice || !m_pDeviceContext) {
        return false;
    }

    CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

    UINT freeSurface = UINT_MAX;
    for (UINT i = 0; i < FrameInterpolationSurfaceCount; ++i) {
        auto& candidate = m_FrameInterpolationPresentationSurfaces[i];
        if (candidate.inUse) {
            continue;
        }
        if (candidate.retirePending) {
            if (!candidate.retireQuery) {
                candidate.retirePending = false;
            } else {
                const HRESULT queryHr = m_pDeviceContext->GetData(
                    candidate.retireQuery, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (queryHr == S_FALSE) {
                    continue;
                }
                if (FAILED(queryHr)) {
                    RecordRifeD3DFailure(RIFE_D3D_FAILURE_PRESENT_RETIRE_QUERY, queryHr);
                    continue;
                }
                candidate.retirePending = false;
            }
        }
        freeSurface = i;
        break;
    }
    if (freeSurface == UINT_MAX) {
        return false;
    }

    auto& surface = m_FrameInterpolationPresentationSurfaces[freeSurface];
    const HRESULT hr = surface.texture.CheckCreate(
        m_pDevice, DXGI_FORMAT_B8G8R8A8_UNORM, width, height, Tex2D_DefaultShaderRTarget);
    if (FAILED(hr)) {
        RecordRifeD3DFailure(RIFE_D3D_FAILURE_PRESENT_CREATE_TEXTURE, hr);
        return false;
    }

    if (!surface.retireQuery) {
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_EVENT;
        const HRESULT queryHr = m_pDevice->CreateQuery(&queryDesc, &surface.retireQuery);
        if (FAILED(queryHr)) {
            RecordRifeD3DFailure(RIFE_D3D_FAILURE_PRESENT_CREATE_QUERY, queryHr);
            return false;
        }
    }

    surface.inUse = true;
    sourceSurface = freeSurface;
    *target = surface.texture.pTexture;
    (*target)->AddRef();
    return true;
#else
    UNREFERENCED_PARAMETER(width);
    UNREFERENCED_PARAMETER(height);
    UNREFERENCED_PARAMETER(target);
    return false;
#endif
}
