#include "stdafx.h"
#include <uuids.h>
#include <Mferror.h>
#include <Mfidl.h>
#include <algorithm>
#include "Helper.h"
#include "Times.h"
#include "VideoRenderer.h"
#include "DX11VideoProcessor.h"
#include "RifePrecision.h"
#include "RifeVideoProcessorPolicy.h"
#include "RifeVsrInputProbe.h"

DWORD CDX11VideoProcessor::GetRifeVsrInputProbeMode()
{
#if MPCVR_TEST_VSR_INPUT_PROBE
    const auto now = GetTickCount64();
    if (!m_RifeVsrInputProbeReadTick || now - m_RifeVsrInputProbeReadTick >= 1000) {
        DWORD mode = 0, bytes = sizeof(mode);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\MPC Video Renderer\\Diagnostics",
                L"RTXVSRAfterRifeInput", RRF_RT_REG_DWORD, nullptr, &mode, &bytes) != ERROR_SUCCESS) mode = 0;
        m_RifeVsrInputProbeMode = mode <= 2 ? mode : 0;
        m_RifeVsrInputProbeReadTick = now;
    }
    return m_RifeVsrInputProbeMode;
#else
    return 0;
#endif
}

void CDX11VideoProcessor::UpdateRifeVsrProbeSourceRequest(const bool rifeSourcePreparation)
{
    const bool suppress = GetRifeVsrInputProbeMode() && rifeSourcePreparation
        && m_VendorId == PCIV_NVIDIA && m_bVPScaling && m_bVPUseSuperRes
        && m_iMaxineOperation == MAXINE_OPERATION_Disabled
        && !SourceIsHDR() && !m_bVPUseRTXVideoHDR;
    if (suppress) {
        // Disable even after VP reinitialization. Keep the accepted user request
        // flag: it still enables the independent post-RIFE comparison processor.
        m_bRifeVsrProbeSourceDisabled = m_D3D11VP.SetSuperRes(SUPERRES_Disable) == S_OK;
    } else if (m_bRifeVsrProbeSourceDisabled) {
        const int preset = m_bVPScaling && m_iMaxineOperation == MAXINE_OPERATION_Disabled
            ? m_iVPSuperRes : SUPERRES_Disable;
        m_bVPUseSuperRes = m_D3D11VP.SetSuperRes(preset) == S_OK;
        m_bRifeVsrProbeSourceDisabled = false;
    }
}

bool CDX11VideoProcessor::GetRifeVsrProbeDecoderInput(CComPtr<ID3D11Texture2D>& texture,
        CComPtr<IMediaSample>& sample, UINT& arraySlice)
{
    return m_D3D11VP.GetLatestDecoderInput(texture, sample, arraySlice);
}

bool CDX11VideoProcessor::TryRifeVideoProcessorUpscale(Tex2D_t& input,
        ID3D11Texture2D* target, const CRect& contentRect, const CRect& dstRect)
{
    const bool requested = m_VendorId == PCIV_NVIDIA && m_bVPScaling && m_bVPUseSuperRes;
    const bool maxineSelected = m_iMaxineOperation != MAXINE_OPERATION_Disabled;
    const bool rgbSdr = input.desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
        && !SourceIsHDR() && !m_bVPUseRTXVideoHDR;
    if (!CanUseRifeVideoProcessorUpscale(requested, maxineSelected, rgbSdr,
            {static_cast<uint32_t>(contentRect.Width()), static_cast<uint32_t>(contentRect.Height())},
            {static_cast<uint32_t>(dstRect.Width()), static_cast<uint32_t>(dstRect.Height())})) {
        if (requested && !maxineSelected && !rgbSdr) {
            m_strRifeUpscaleStatus = L"RIFE input is not SDR BGRA8; shader resizing retained";
        }
        return false;
    }

    const RifeSpatialSize contentSize = {
        static_cast<uint32_t>(contentRect.Width()), static_cast<uint32_t>(contentRect.Height())};
    const auto driverContent = ResolveRifeVsrContentSize(contentSize,
        {m_srcRectWidth, m_srcRectHeight}, m_srcAnamorphic, m_iRotation);
    const bool restoreEncodedGrid = driverContent != contentSize;
    auto nv12Size = RifeVsrNv12SurfaceSize(restoreEncodedGrid
        ? driverContent : RifeSpatialSize{input.desc.Width, input.desc.Height});
    const DWORD probeMode = GetRifeVsrInputProbeMode();
    CComPtr<ID3D11Texture2D> decodedInput;
    CComPtr<IMediaSample> decodedOwner;
    UINT decodedSlice = 0;
    if (probeMode) {
        if (!m_bRifeVsrProbeSourceDisabled) {
            m_strRifeUpscaleStatus = L"input probe waiting for source VSR suppression";
            return false;
        }
        if (!GetRifeVsrProbeDecoderInput(decodedInput, decodedOwner, decodedSlice)) {
            m_strRifeUpscaleStatus = L"input probe unavailable: GPU decoder NV12 sample required";
            return false;
        }
        D3D11_TEXTURE2D_DESC decodedDesc = {};
        decodedInput->GetDesc(&decodedDesc);
        const bool bt709Limited = m_srcExFmt.NominalRange == DXVA2_NominalRange_16_235
            && m_srcExFmt.VideoTransferMatrix == DXVA2_VideoTransferMatrix_BT709
            && (m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_22
                || m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_709);
        if (decodedDesc.Format != DXGI_FORMAT_NV12 || decodedDesc.Usage != D3D11_USAGE_DEFAULT
                || decodedDesc.CPUAccessFlags || decodedDesc.SampleDesc.Count != 1
                || decodedDesc.MipLevels != 1 || decodedSlice >= decodedDesc.ArraySize
                || !CanCompareRifeVsrInputs(driverContent, {m_srcRectWidth, m_srcRectHeight},
                    {decodedDesc.Width, decodedDesc.Height}, m_iRotation,
                    m_SampleFormat == D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, bt709Limited,
                    m_srcRect == CRect(0, 0, m_srcRectWidth, m_srcRectHeight))) {
            m_strRifeUpscaleStatus = L"input probe unavailable: needs full source, unrotated progressive BT.709 NV12";
            return false;
        }
        // Both modes use the decoder's storage alignment, including padding.
        // The visible crop, output geometry and VSR enumerator stay identical.
        nv12Size = {decodedDesc.Width, decodedDesc.Height};
    }
    if (!nv12Size.width || !nv12Size.height) {
        m_strRifeUpscaleStatus = L"NV12 bridge dimensions invalid; shader resizing retained";
        return false;
    }
    const CSize driverSize(driverContent.width, driverContent.height);
    const CSize storageSize(nv12Size.width, nv12Size.height);
    const CRect driverRect = restoreEncodedGrid || probeMode
        ? CRect(0, 0, driverSize.cx, driverSize.cy) : contentRect;
    const bool changed = !m_bRifeUpscaleAttempted
        || input.desc.Width != m_RifeUpscaleInputDesc.Width
        || input.desc.Height != m_RifeUpscaleInputDesc.Height
        || input.desc.Format != m_RifeUpscaleInputDesc.Format
        || contentRect != m_RifeUpscaleContentRect
        || driverSize != m_RifeVsrContentSize
        || storageSize != m_RifeVsrStorageSize
        || dstRect.Size() != m_RifeUpscaleOutputSize
        || m_RifeUpscaleSuperRes != m_iVPSuperRes;
    if (changed) {
        m_RifeVsrConvertVP.ReleaseVideoProcessor();
        m_RifeUpscaleVP.ReleaseVideoProcessor();
        m_TexRifeVsrNV12.Release();
        m_RifeUpscaleInputDesc = input.desc;
        m_RifeUpscaleContentRect = contentRect;
        m_RifeVsrContentSize = driverSize;
        m_RifeVsrStorageSize = storageSize;
        m_RifeUpscaleOutputSize = dstRect.Size();
        m_RifeUpscaleSuperRes = m_iVPSuperRes;
        m_bRifeUpscaleAttempted = true;
        HRESULT hr = S_OK;
        if (!m_RifeVsrConvertVP.IsVideoDeviceOk()) {
            hr = m_RifeVsrConvertVP.InitVideoDevice(m_pDevice, m_pDeviceContext, m_VendorId);
        }
        if (SUCCEEDED(hr) && !m_RifeUpscaleVP.IsVideoDeviceOk()) {
            hr = m_RifeUpscaleVP.InitVideoDevice(m_pDevice, m_pDeviceContext, m_VendorId);
        }
        if (SUCCEEDED(hr)) {
            hr = m_TexRifeVsrNV12.CheckCreate(m_pDevice, DXGI_FORMAT_NV12,
                nv12Size.width, nv12Size.height, Tex2D_DefaultRTarget);
        }
        DXGI_FORMAT outputFormat = DXGI_FORMAT_NV12;
        if (SUCCEEDED(hr)) {
            // RIFE output already has ProcAmp, source color conversion and
            // rotation applied. Convert its full-range RGB to fresh BT.709 NV12;
            // reusing original YUV metadata would apply source conversion twice.
            hr = m_RifeVsrConvertVP.InitVideoProcessor(input.desc.Format,
                input.desc.Width, input.desc.Height, {}, DEINT_Disable, false,
                outputFormat, CSize(nv12Size.width, nv12Size.height), true);
        }
        if (SUCCEEDED(hr)) hr = m_RifeVsrConvertVP.InitInputTextures(m_pDevice, false);
        if (SUCCEEDED(hr)) hr = m_RifeVsrConvertVP.SetSuperRes(SUPERRES_Disable);
        if (SUCCEEDED(hr)) {
            DXVA2_ExtendedFormat nv12Color = {};
            nv12Color.NominalRange = DXVA2_NominalRange_16_235;
            nv12Color.VideoTransferMatrix = DXVA2_VideoTransferMatrix_BT709;
            nv12Color.VideoTransferFunction = DXVA2_VideoTransFunc_22;
            outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            hr = m_RifeUpscaleVP.InitVideoProcessor(DXGI_FORMAT_NV12,
                nv12Size.width, nv12Size.height, nv12Color, DEINT_Disable, false,
                outputFormat, dstRect.Size());
        }
        if (SUCCEEDED(hr)) hr = m_RifeUpscaleVP.InitInputTextures(m_pDevice, false);
        if (SUCCEEDED(hr)) hr = m_RifeUpscaleVP.SetSuperRes(m_iVPSuperRes, driverRect.Size());
        if (hr != S_OK) {
            DLog(L"RIFE native RTX VSR initialization failed: {}", HR2Str(hr));
            m_RifeVsrConvertVP.ReleaseVideoProcessor();
            m_RifeUpscaleVP.ReleaseVideoProcessor();
        }
    }
    if (!m_RifeVsrConvertVP.IsReady() || !m_RifeUpscaleVP.IsReady()) {
        m_strRifeUpscaleStatus = L"NV12 bridge unavailable; shader resizing retained";
        return false;
    }

    D3D11_TEXTURE2D_DESC targetDesc = {};
    target->GetDesc(&targetDesc);
    const bool direct = targetDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
        && dstRect.left >= 0 && dstRect.top >= 0
        && dstRect.right <= static_cast<LONG>(targetDesc.Width)
        && dstRect.bottom <= static_cast<LONG>(targetDesc.Height);
    ID3D11Texture2D* vpTarget = target;
    CRect vpRect = dstRect;
    if (!direct) {
        if (FAILED(m_TexRifeUpscaleOutput.CheckCreate(m_pDevice,
                DXGI_FORMAT_B8G8R8A8_UNORM, dstRect.Width(), dstRect.Height(), Tex2D_DefaultShaderRTarget))) {
            m_strRifeUpscaleStatus = L"Could not create the RGB upscale target; shader resizing retained";
            return false;
        }
        vpTarget = m_TexRifeUpscaleOutput.pTexture;
        vpRect = CRect(0, 0, dstRect.Width(), dstRect.Height());
    }
    ID3D11ShaderResourceView* nullViews[3] = {};
    m_pDeviceContext->PSSetShaderResources(0, std::size(nullViews), nullViews);
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    const CRect fullInput(0, 0, input.desc.Width, input.desc.Height);
    m_RifeVsrConvertVP.SetInputVideoData(input.pTexture, nullptr, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    // Crop model padding before restoring encoded proportions. The same VP
    // pass converts to NV12, so this requires no additional pass or readback.
    HRESULT hr = m_RifeVsrConvertVP.SetRectangles(
        restoreEncodedGrid || probeMode ? contentRect : fullInput,
        restoreEncodedGrid || probeMode ? driverRect : fullInput);
    if (SUCCEEDED(hr)) hr = m_RifeVsrConvertVP.Process(m_TexRifeVsrNV12.pTexture,
        D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, false);
    if (FAILED(hr)) {
        m_strRifeUpscaleStatus = L"RGB to NV12 conversion failed; shader resizing retained";
        return false;
    }
    // Both passes use the same immediate D3D11 context. The NV12 render-target
    // write precedes the upscale read without a CPU readback or GPU-wide wait.
    // Run conversion in A too. Only the final input binding differs; A repeats
    // the latest decoded frame at RIFE cadence and is not an interpolation test.
    if (probeMode == 1) {
        m_RifeUpscaleVP.SetInputVideoData(decodedInput, decodedOwner, decodedSlice, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    } else {
        m_RifeUpscaleVP.SetInputVideoData(m_TexRifeVsrNV12.pTexture, nullptr, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    }
    hr = m_RifeUpscaleVP.SetRectangles(driverRect, vpRect);
    if (SUCCEEDED(hr)) {
        // Match Chromium's presenter: reapply the request immediately before
        // this processor's blit, after all other VP passes on the context.
        // Initialization-time acceptance does not prove per-frame activation.
        hr = m_RifeUpscaleVP.SetSuperRes(m_iVPSuperRes, driverRect.Size());
        if (hr != S_OK) {
            m_strRifeUpscaleStatus = L"Per-frame RTX VSR request rejected; shader resizing retained";
            return false;
        }
    }
    if (SUCCEEDED(hr)) hr = m_RifeUpscaleVP.Process(vpTarget, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, false);
    if (SUCCEEDED(hr) && !direct) {
        hr = ResizeShaderPass(m_TexRifeUpscaleOutput, target, vpRect, dstRect, 0, false);
    }
    if (FAILED(hr)) {
        m_strRifeUpscaleStatus = L"Video-processor upscale failed; shader resizing retained";
        return false;
    }
    m_bRifeUpscaleUsed = true;
    // Extension acceptance is a request, not a driver activity query.
    m_strRifeUpscaleStatus = std::format(L"requested per frame after RIFE (NV12 {}x{}; activity unverified)",
        driverSize.cx, driverSize.cy);
    if (probeMode) {
        m_strRifeUpscaleStatus = std::format(L"probe {}: {} NV12 {}x{} [{}x{}]; activity unverified",
            probeMode == 1 ? L"A" : L"B", probeMode == 1 ? L"decoded" : L"RIFE",
            driverSize.cx, driverSize.cy, storageSize.cx, storageSize.cy);
    }
    return true;
}

DXGI_FORMAT CDX11VideoProcessor::GetRifeSurfaceFormat() const
{
    const auto inputFormat = m_D3D11VP.IsReady() ? m_D3D11OutputFmt : m_InternalTexFmt;
    return m_pFilter->m_Sets.bRifePreservePrecision && m_srcParams.CDepth > 8
        && inputFormat != DXGI_FORMAT_B8G8R8A8_UNORM
        ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
}

CSize CDX11VideoProcessor::GetRifeContentSize() const
{
    const auto source = GetRifeSourceContentSize();
    const auto& settings = m_pFilter->m_Sets;
    const auto size = ResolveRifeWorkingSize(
        {static_cast<uint32_t>(std::max<LONG>(0, source.cx)), static_cast<uint32_t>(std::max<LONG>(0, source.cy))},
        {static_cast<uint32_t>(std::max<LONG>(0, m_videoRect.Width())), static_cast<uint32_t>(std::max<LONG>(0, m_videoRect.Height()))},
        settings.iRifeProcessingResolution, settings.iRifeProcessingLimit);
    return CSize(static_cast<int>(size.width), static_cast<int>(size.height));
}

CSize CDX11VideoProcessor::GetRifePresentationContentSize(const UINT sourceSurface) const
{
    // Window changes must not reinterpret the padding of a previously prepared
    // frame. Source-only NvOFFRUC surfaces retain their existing geometry path.
    if (sourceSurface < FrameInterpolationSurfaceCount) {
        const auto size = m_FrameInterpolationPresentationSurfaces[sourceSurface].rifeContentSize;
        if (size.cx > 0 && size.cy > 0) return size;
    }
    return GetRifeSourceContentSize();
}

REFERENCE_TIME CDX11VideoProcessor::RifePresentationPreparationLeadTime(const REFERENCE_TIME frameInterval)
{
#if defined(_WIN64) && defined(MPCVR_TEST_MAXINE_PREPARE_AHEAD) && MPCVR_TEST_MAXINE_PREPARE_AHEAD
    if (m_pFilter->m_filterState != State_Running || m_videoRect.IsRectEmpty()) {
        return 0;
    }
    CSize target;
    bool upscale = false;
    if (!GetMaxineVSRTargetSizeForInput(m_videoRect, GetRifeContentSize(), true, target, upscale)) {
        return 0;
    }
    const auto cost = [](const CNvidiaMaxineVSR& effect) {
        return std::max(effect.GetAverageGpuProcessTimeMs(), effect.GetLastProcessTimeMs());
    };
    double enhancementMs = 0;
    if (m_iMaxineOperation == MAXINE_OPERATION_Upscale && upscale) enhancementMs += cost(m_MaxineVSR);
    if (m_iMaxineDenoise != MAXINE_FILTER_Off && (m_iMaxineOperation == MAXINE_OPERATION_Upscale
            || m_iMaxineOperation == MAXINE_OPERATION_Denoise)) enhancementMs += cost(m_MaxineDenoise);
    if (m_iMaxineDeblur != MAXINE_FILTER_Off && (m_iMaxineOperation == MAXINE_OPERATION_Upscale
            || m_iMaxineOperation == MAXINE_OPERATION_Deblur)) enhancementMs += cost(m_MaxineDeblur);
    return RifePresentationPreparationLead(frameInterval, enhancementMs, true);
#else
    UNREFERENCED_PARAMETER(frameInterval);
    return 0;
#endif
}

bool CDX11VideoProcessor::PrepareRifePresentationSource(const UINT sourceSurface, const REFERENCE_TIME frameTime)
{
    m_RifePreparedMaxineKey.Clear();
#ifdef _WIN64
    if (sourceSurface >= FrameInterpolationSurfaceCount || !m_pDevice || !m_pDeviceContext
            || m_pFrameInterpolationTexture || m_pFilter->m_filterState != State_Running) return false;
    auto& surface = m_FrameInterpolationPresentationSurfaces[sourceSurface];
    if (!surface.inUse || !surface.texture.pTexture || !surface.texture.pShaderResource
            || surface.cudaOutput) return false;
    const CSize contentSize = GetRifePresentationContentSize(sourceSurface);
    CRect inputRect(0, 0, contentSize.cx, contentSize.cy);
    CSize target;
    bool upscale = false;
    if (inputRect.IsRectEmpty() || !GetMaxineVSRTargetSizeForInput(
            m_videoRect, contentSize, true, target, upscale)) return false;
    Tex2D_t* input = &surface.texture;
    m_bMaxineVSRUsed = false;
    m_MaxineVSRInputSize = CSize(0, 0);
    m_MaxineVSRSize = CSize(0, 0);
    m_iMaxineResolvedMode = -1;
    m_strMaxinePipeline.clear();
    // Preserve the existing RIFE-to-Maxine staging and every-frame D3D wait.
    // This is enhancement only: no backbuffer, subtitle, OSD or Present call.
    m_pFrameInterpolationTexture = surface.texture.pTexture;
    const bool enhanced = ApplyMaxine(input, inputRect, contentSize, target, upscale, true);
    m_pFrameInterpolationTexture = nullptr;
    const auto Finish = [&](const bool ready) {
        // Even failed enhancement can have queued a read from the source.
        // Cancellation must leave that slot protected until the GPU retires it.
        if (surface.retireQuery) {
            m_pDeviceContext->End(surface.retireQuery);
            surface.retirePending = true;
            surface.retireStartTick = GetPreciseTick();
        }
        // Submit the private snapshot before the deadline. No CPU wait here.
        m_pDeviceContext->Flush();
        return ready;
    };
    if (!enhanced) return Finish(false);
    if (FAILED(m_TexRifePreparedMaxine.CheckCreate(m_pDevice, input->desc.Format,
            input->desc.Width, input->desc.Height, Tex2D_DefaultShaderRTarget))) return Finish(false);
    // Effects and UI redraws reuse Maxine's working textures. Keep one private
    // snapshot so that releasing the renderer lock cannot replace this image.
    m_pDeviceContext->CopyResource(m_TexRifePreparedMaxine.pTexture, input->pTexture);
    m_RifePreparedMaxineInputRect = inputRect;
    m_RifePreparedMaxineDestRect = m_videoRect;
    m_RifePreparedMaxineSize = m_MaxineVSRSize;
    m_RifePreparedMaxineMode = m_iMaxineResolvedMode;
    m_RifePreparedMaxineOversampleClamped = m_bMaxineOversampleClamped;
    m_RifePreparedMaxinePipeline = m_strMaxinePipeline;
    m_RifePreparedMaxineStatus = m_strMaxineVSRStatus;
    m_RifePreparedMaxineKey = {m_FrameInterpolationGeneration.load(), frameTime, sourceSurface};
    return Finish(true);
#else
    UNREFERENCED_PARAMETER(sourceSurface);
    UNREFERENCED_PARAMETER(frameTime);
    return false;
#endif
}

bool CDX11VideoProcessor::PrepareRifeSource(
    IMediaSample* pSample,
    ID3D11Texture2D* target,
    REFERENCE_TIME& sourceTime,
    const UINT alignment)
{
    sourceTime = INVALID_TIME;
#ifdef _WIN64
    if (!pSample || !target || !m_pDevice || !m_pDeviceContext || !m_pDXGISwapChain1) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    target->GetDesc(&desc);
    const CSize contentSize = GetRifeContentSize();
    const CSize rifeSize = GetRifeFrameSize(alignment);
    const UINT width = static_cast<UINT>(std::max<LONG>(0, rifeSize.cx));
    const UINT height = static_cast<UINT>(std::max<LONG>(0, rifeSize.cy));
    if (contentSize.cx <= 0 || contentSize.cy <= 0 || !width || !height
            || desc.Width != width || desc.Height != height
            || desc.Format != GetRifeSurfaceFormat() || desc.SampleDesc.Count != 1) {
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

    // The worker copies this source through the same multithread-protected
    // immediate context before CUDA maps its private inference textures.
    // CUDA/D3D11 interop supplies the ownership synchronization at that map,
    // so forcing an immediate-context flush here only serializes the graphics
    // queue once per decoded source frame.
    return true;
#else
    UNREFERENCED_PARAMETER(pSample);
    UNREFERENCED_PARAMETER(target);
    return false;
#endif
}

bool CDX11VideoProcessor::ReserveRifePresentationSurface(
    ID3D11Texture2D* source,
    UINT& sourceSurface,
    const CSize contentSize)
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
                    m_RifePresentationRetireBusyChecks.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (FAILED(queryHr)) {
                    RecordRifeD3DFailure(RIFE_D3D_FAILURE_PRESENT_RETIRE_QUERY, queryHr);
                    continue;
                }
                candidate.retirePending = false;
                if (candidate.retireStartTick) {
                    const auto elapsedTicks = GetPreciseTick() - candidate.retireStartTick;
                    const uint64_t elapsedUs = static_cast<uint64_t>(
                        elapsedTicks * 1000000 / GetPreciseTicksPerSecondI());
                    candidate.retireStartTick = 0;
                    m_RifePresentationRetireLastUs.store(elapsedUs, std::memory_order_relaxed);
                    m_RifePresentationRetireCount.fetch_add(1, std::memory_order_relaxed);
                    m_RifePresentationRetireTiming.AddMicroseconds(elapsedUs);
                    uint64_t observedMax = m_RifePresentationRetireMaxUs.load(std::memory_order_relaxed);
                    while (observedMax < elapsedUs
                            && !m_RifePresentationRetireMaxUs.compare_exchange_weak(
                                observedMax, elapsedUs, std::memory_order_relaxed)) {
                    }
                }
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
    if (!desc.Width || !desc.Height || !RifeSupportedSurfaceFormat(desc.Format)
            || desc.SampleDesc.Count != 1) {
        return false;
    }

    auto& surface = m_FrameInterpolationPresentationSurfaces[freeSurface];
    surface.cudaOutput.reset();
    surface.rifeContentSize = contentSize;
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

    // Presentation consumes this texture through the same immediate context,
    // which preserves CopyResource ordering.  Avoid flushing once per queued
    // output frame; Maxine/CUDA interop will synchronize at its resource map
    // when enabled, while the ordinary D3D presentation path is ordered by the
    // immediate context itself.
    m_pDeviceContext->CopyResource(surface.texture.pTexture, source);
    surface.inUse = true;
    sourceSurface = freeSurface;
    return true;
#else
    UNREFERENCED_PARAMETER(source);
    UNREFERENCED_PARAMETER(contentSize);
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
                    m_RifePresentationRetireBusyChecks.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (FAILED(queryHr)) {
                    RecordRifeD3DFailure(RIFE_D3D_FAILURE_PRESENT_RETIRE_QUERY, queryHr);
                    continue;
                }
                candidate.retirePending = false;
                if (candidate.retireStartTick) {
                    const auto elapsedTicks = GetPreciseTick() - candidate.retireStartTick;
                    const uint64_t elapsedUs = static_cast<uint64_t>(
                        elapsedTicks * 1000000 / GetPreciseTicksPerSecondI());
                    candidate.retireStartTick = 0;
                    m_RifePresentationRetireLastUs.store(elapsedUs, std::memory_order_relaxed);
                    m_RifePresentationRetireCount.fetch_add(1, std::memory_order_relaxed);
                    m_RifePresentationRetireTiming.AddMicroseconds(elapsedUs);
                    uint64_t observedMax = m_RifePresentationRetireMaxUs.load(std::memory_order_relaxed);
                    while (observedMax < elapsedUs
                            && !m_RifePresentationRetireMaxUs.compare_exchange_weak(
                                observedMax, elapsedUs, std::memory_order_relaxed)) {
                    }
                }
            }
        }
        freeSurface = i;
        break;
    }
    if (freeSurface == UINT_MAX) {
        return false;
    }

    auto& surface = m_FrameInterpolationPresentationSurfaces[freeSurface];
    surface.rifeContentSize = CSize(0, 0);
    surface.cudaOutput.reset();
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

bool CDX11VideoProcessor::CanUseRifeCudaOutput()
{
    // The optional linear lease ABI is BGRA8; never quantize a precision frame here.
    if (GetRifeSurfaceFormat() != DXGI_FORMAT_B8G8R8A8_UNORM) return false;
#ifdef _WIN64
    CAutoLock rendererLock(&m_pFilter->m_RendererLock);
    CSize target;
    bool upscale = false;
    const CSize content = GetRifeContentSize();
    // Opt in after the existing source path has initialized the tested SDK.
    // Chained effects retain their established D3D ownership path. Odd widths
    // also stay there: native VSR repeatability needs separate investigation.
    return m_iMaxineOperation == MAXINE_OPERATION_Upscale
        && m_iMaxineAmount > 0
        && m_iMaxineDenoise == MAXINE_FILTER_Off && m_iMaxineDeblur == MAXINE_FILTER_Off
        && m_MaxineVSR.CanUseCudaInput() && content.cx % 2 == 0
        && GetMaxineVSRTargetSizeForInput(m_videoRect, content, true, target, upscale)
        && upscale && target.cx % 2 == 0;
#else
    return false;
#endif
}

bool CDX11VideoProcessor::ReserveRifeCudaPresentationSurface(
    const std::shared_ptr<RifeCudaOutputLease>& source, UINT& sourceSurface, const CSize contentSize)
{
    sourceSurface = UINT_MAX;
#ifdef _WIN64
    if (!source) return false;
    CAutoLock rendererLock(&m_pFilter->m_RendererLock);
    CComPtr<ID3D11Texture2D> fallback;
    const auto& image = source->GetImage();
    if (!AcquireRifePresentationSurface(image.width, image.height, &fallback, sourceSurface)) return false;
    m_FrameInterpolationPresentationSurfaces[sourceSurface].cudaOutput = source;
    m_FrameInterpolationPresentationSurfaces[sourceSurface].rifeContentSize = contentSize;
    return true;
#else
    UNREFERENCED_PARAMETER(source);
    UNREFERENCED_PARAMETER(contentSize);
    return false;
#endif
}
