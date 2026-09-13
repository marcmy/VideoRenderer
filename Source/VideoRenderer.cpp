/*
 * RIFE migration wrapper around the existing renderer implementation.
 *
 * The legacy implementation remains byte-for-byte preserved in
 * VideoRendererLegacy.inl. Only the settings and playback entry points are
 * wrapped here while the RIFE path replaces NvOFFRUC end-to-end.
 */

#include "stdafx.h"
#include <mutex>
#include <unordered_set>
#include "VideoRenderer.h"
#include "RifePlaybackPipeline.h"

namespace {
void ForgetRifeSettings(const CMpcVideoRenderer* renderer);
}

#define GetSettings GetSettingsLegacy
#define SetSettings SetSettingsLegacy
#define SaveSettings SaveSettingsLegacy
#define Receive ReceiveLegacy
#include "VideoRendererLegacy.inl"
#undef Receive
#undef SaveSettings
#undef SetSettings
#undef GetSettings

namespace {

constexpr LPCWSTR OPT_RifeMode              = L"RifeMode";
constexpr LPCWSTR OPT_RifeCustomFps         = L"RifeCustomFps";
constexpr LPCWSTR OPT_RifeGpuThreads        = L"RifeGpuThreads";
constexpr LPCWSTR OPT_RifeModel             = L"RifeModel";
constexpr LPCWSTR OPT_RifeGPU               = L"RifeGPU";
constexpr LPCWSTR OPT_RifePerformanceBoost  = L"RifePerformanceBoost";
constexpr LPCWSTR OPT_RifeSceneDetection    = L"RifeSceneDetection";
constexpr LPCWSTR OPT_RifeSceneProcessing   = L"RifeSceneProcessing";
constexpr LPCWSTR OPT_RifeDuplicateRemoval  = L"RifeDuplicateRemoval";

std::mutex g_rifeSettingsMutex;
std::unordered_set<const CMpcVideoRenderer*> g_rifeSettingsLoaded;

void ForgetRifeSettings(const CMpcVideoRenderer* renderer)
{
	std::scoped_lock lock(g_rifeSettingsMutex);
	g_rifeSettingsLoaded.erase(renderer);
}

bool RifeSettingsChanged(const Settings_t& a, const Settings_t& b)
{
	return a.iRifeMode != b.iRifeMode
		|| a.iRifeCustomFps != b.iRifeCustomFps
		|| a.iRifeGpuThreads != b.iRifeGpuThreads
		|| a.iRifeModel != b.iRifeModel
		|| a.iRifeGPU != b.iRifeGPU
		|| a.bRifePerformanceBoost != b.bRifePerformanceBoost
		|| a.iRifeSceneDetection != b.iRifeSceneDetection
		|| a.iRifeSceneProcessing != b.iRifeSceneProcessing
		|| a.iRifeDuplicateRemoval != b.iRifeDuplicateRemoval;
}

void LoadRifeSettings(Settings_t& settings)
{
	CRegKey key;
	if (ERROR_SUCCESS != key.Open(HKEY_CURRENT_USER, OPT_REGKEY_VIDEORENDERER, KEY_READ)) {
		return;
	}

	DWORD dw = 0;
	const bool hasRifeMode = ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeMode, dw);
	if (hasRifeMode) {
		settings.iRifeMode = dw < RIFE_MODE_COUNT ? static_cast<int>(dw) : RIFE_MODE_Disabled;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeCustomFps, dw)) {
		settings.iRifeCustomFps = (dw >= RIFE_CUSTOM_FPS_MIN && dw <= RIFE_CUSTOM_FPS_MAX)
			? static_cast<int>(dw) : RIFE_CUSTOM_FPS_DEF;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeGpuThreads, dw)) {
		settings.iRifeGpuThreads = (dw >= RIFE_GPU_THREADS_MIN && dw <= RIFE_GPU_THREADS_MAX)
			? static_cast<int>(dw) : RIFE_GPU_THREADS_DEF;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeModel, dw)) {
		settings.iRifeModel = dw == RIFE_MODEL_46 ? RIFE_MODEL_46 : RIFE_MODEL_46;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeGPU, dw)) {
		if (dw == MAXDWORD) {
			settings.iRifeGPU = RIFE_GPU_Auto;
		} else if (dw <= 7) {
			settings.iRifeGPU = static_cast<int>(dw);
		}
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifePerformanceBoost, dw)) {
		settings.bRifePerformanceBoost = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeSceneDetection, dw)) {
		settings.iRifeSceneDetection = dw < RIFE_SCENE_COUNT ? static_cast<int>(dw) : RIFE_SCENE_NVOF;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeSceneProcessing, dw)) {
		settings.iRifeSceneProcessing = dw < RIFE_SCENE_PROCESS_COUNT ? static_cast<int>(dw) : RIFE_SCENE_PROCESS_Repeat;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeDuplicateRemoval, dw)) {
		settings.iRifeDuplicateRemoval = dw < RIFE_DUPLICATES_COUNT ? static_cast<int>(dw) : RIFE_DUPLICATES_Keep;
	}

	// One-way migration from the old midpoint-only NvOFFRUC setting. Merely
	// reading the legacy setting does not disable it yet; applying/saving an
	// enabled RIFE mode does that, so old builds retain a graceful fallback.
	if (!hasRifeMode && settings.iFrameInterpolationMode == FRUC_MODE_Double) {
		settings.iRifeMode = RIFE_MODE_Movie2x;
		if (settings.iFrameInterpolationGPU == FRUC_GPU_Auto
				|| (settings.iFrameInterpolationGPU >= 0 && settings.iFrameInterpolationGPU <= 7)) {
			settings.iRifeGPU = settings.iFrameInterpolationGPU;
		}
	}
}

void EnsureRifeSettingsLoaded(CMpcVideoRenderer* renderer, Settings_t& settings)
{
	std::scoped_lock lock(g_rifeSettingsMutex);
	if (g_rifeSettingsLoaded.insert(renderer).second) {
		LoadRifeSettings(settings);
	}
}

FrameRate ResolveDisplayRate(const DisplayConfig_t& display)
{
	if (display.refreshRate.Numerator && display.refreshRate.Denominator) {
		return {display.refreshRate.Numerator, display.refreshRate.Denominator};
	}
	// DisplayConfig is normally populated before playback. Keep a conservative
	// fallback so "To screen" still degrades predictably during a transient
	// display-mode transition instead of producing no presentation targets.
	return {60, 1};
}

} // namespace

STDMETHODIMP_(void) CMpcVideoRenderer::GetSettings(Settings_t& settings)
{
	EnsureRifeSettingsLoaded(this, m_Sets);
	settings = m_Sets;
}

STDMETHODIMP_(void) CMpcVideoRenderer::SetSettings(const Settings_t& settings)
{
	EnsureRifeSettingsLoaded(this, m_Sets);

	Settings_t adjusted = settings;
	if (adjusted.iRifeMode != RIFE_MODE_Disabled) {
		// RIFE owns production frame synthesis once enabled. Keep the old
		// NvOFFRUC values only as migration data; never let both paths run.
		adjusted.iFrameInterpolationMode = FRUC_MODE_Disabled;
	}

	if (RifeSettingsChanged(adjusted, m_Sets)) {
		if (m_RifePipeline) {
			m_RifePipeline->Reset();
		}
		ResetFrameInterpolationPresenterQueue();
	}

	SetSettingsLegacy(adjusted);
}

STDMETHODIMP CMpcVideoRenderer::SaveSettings()
{
	EnsureRifeSettingsLoaded(this, m_Sets);

	const HRESULT hr = SaveSettingsLegacy();
	if (FAILED(hr)) {
		return hr;
	}

	CRegKey key;
	if (ERROR_SUCCESS == key.Create(HKEY_CURRENT_USER, OPT_REGKEY_VIDEORENDERER)) {
		key.SetDWORDValue(OPT_RifeMode,             static_cast<DWORD>(m_Sets.iRifeMode));
		key.SetDWORDValue(OPT_RifeCustomFps,        static_cast<DWORD>(m_Sets.iRifeCustomFps));
		key.SetDWORDValue(OPT_RifeGpuThreads,       static_cast<DWORD>(m_Sets.iRifeGpuThreads));
		key.SetDWORDValue(OPT_RifeModel,            static_cast<DWORD>(m_Sets.iRifeModel));
		key.SetDWORDValue(OPT_RifeGPU,              static_cast<DWORD>(m_Sets.iRifeGPU));
		key.SetDWORDValue(OPT_RifePerformanceBoost, m_Sets.bRifePerformanceBoost ? 1u : 0u);
		key.SetDWORDValue(OPT_RifeSceneDetection,   static_cast<DWORD>(m_Sets.iRifeSceneDetection));
		key.SetDWORDValue(OPT_RifeSceneProcessing,  static_cast<DWORD>(m_Sets.iRifeSceneProcessing));
		key.SetDWORDValue(OPT_RifeDuplicateRemoval, static_cast<DWORD>(m_Sets.iRifeDuplicateRemoval));
	}

	return S_OK;
}

HRESULT CMpcVideoRenderer::Receive(IMediaSample* pSample)
{
	EnsureRifeSettingsLoaded(this, m_Sets);

#ifdef _WIN64
	const bool useRife = m_Sets.iRifeMode != RIFE_MODE_Disabled
		&& m_VideoProcessor && m_VideoProcessor->Type() == VP_DX11;
#else
	const bool useRife = false;
#endif
	if (!useRife) {
		return ReceiveLegacy(pSample);
	}

	if (m_bFlushing) {
		DLog(L"CMpcVideoRenderer::Receive(RIFE) - flushing, skip sample");
		return S_OK;
	}
	ASSERT(pSample);

	HRESULT hr = PrepareReceive(pSample);
	ASSERT(m_bInReceive == SUCCEEDED(hr));
	if (FAILED(hr)) {
		return hr == VFW_E_SAMPLE_REJECTED ? NOERROR : hr;
	}

	// Preserve the base renderer's paused-search behavior verbatim. RIFE only
	// owns continuously running playback; frame stepping and paused redraws stay
	// on MPCVR's mature synchronous path.
	if (m_State == State_Paused) {
		m_bInReceive = FALSE;
		{
			CAutoLock cVideoLock(&m_InterfaceLock);
			if (m_State == State_Stopped) {
				return NOERROR;
			}
			m_bInReceive = TRUE;
		}
		Ready();
	}
	if (m_State == State_Paused) {
		m_bInReceive = FALSE;
		CAutoLock cRendererLock(&m_RendererLock);
		DoRenderSample(m_pMediaSample);
	}

	bool rifeSubmitted = false;
	REFERENCE_TIME rifeFrameDuration = 0;
	if (m_State == State_Running && m_VideoProcessor->Type() == VP_DX11) {
		// BeginFlush enters with interface/renderer locks held and waits for
		// m_bInReceive to clear. Advertise Receive as inactive before entering
		// that lock order, then revalidate the graph state inside it.
		m_bInReceive = FALSE;
		{
			CAutoLock cVideoLock(&m_InterfaceLock);
			if (m_State != State_Running || m_bFlushing) {
				return NOERROR;
			}
			m_bInReceive = TRUE;

			CAutoLock cRendererLock(&m_RendererLock);
			if (!m_RifePipeline) {
				m_RifePipeline = std::make_unique<CRifePlaybackPipeline>(this);
			}

			auto* dx11 = static_cast<CDX11VideoProcessor*>(m_VideoProcessor.get());
			const uint64_t generation = m_FrameInterpolationPresenterGeneration.load(std::memory_order_acquire);
			const FrameRate displayRate = ResolveDisplayRate(m_DisplayConfig);
			rifeFrameDuration = m_FrameStats.GetAverageFrameDuration();
			rifeSubmitted = m_RifePipeline->SubmitSample(
				dx11,
				m_pMediaSample,
				m_Sets,
				generation,
				displayRate,
				rifeFrameDuration);
		}
	}

	if (rifeSubmitted) {
		// The RIFE presenter owns actual real/synthetic rendering, but Receive()
		// still bounds decoder delivery. Give the worker one source-frame of
		// additional lookahead so interpolation between A and B can finish before
		// its midpoint is due. Submission happens before this wait, so pacing at
		// B - duration keeps at most a small, fixed lead instead of allowing an
		// unbounded source backlog.
		REFERENCE_TIME rtSourceStart = INVALID_TIME;
		REFERENCE_TIME rtSourceEnd = INVALID_TIME;
		const bool haveSourceTime = m_pMediaSample
			&& SUCCEEDED(m_pMediaSample->GetTime(&rtSourceStart, &rtSourceEnd));
		if (haveSourceTime && rifeFrameDuration > 0) {
			hr = WaitForStreamTime(rtSourceStart - rifeFrameDuration);
		} else {
			hr = WaitForRenderTime();
		}
		if (FAILED(hr)) {
			m_bInReceive = FALSE;
			return NOERROR;
		}

		m_bInReceive = FALSE;

		CAutoLock cVideoLock(&m_InterfaceLock);
		if (m_State == State_Stopped) {
			return NOERROR;
		}
		CAutoLock cRendererLock(&m_RendererLock);
		ClearPendingSample();
		SendEndOfStream();
		CancelNotification();
		return NOERROR;
	}

	// A full source pool, device transition, or source-preparation failure must
	// never stall playback. Keep PrepareReceive's original timing notification,
	// render this frame normally, and invalidate any partially queued RIFE work.
	if (m_RifePipeline) {
		m_RifePipeline->Reset();
	}

	hr = WaitForRenderTime();
	if (FAILED(hr)) {
		m_bInReceive = FALSE;
		return NOERROR;
	}

	m_bInReceive = FALSE;
	ResetFrameInterpolationPresenterQueue();

	CAutoLock cVideoLock(&m_InterfaceLock);
	if (m_State == State_Stopped) {
		return NOERROR;
	}
	CAutoLock cRendererLock(&m_RendererLock);
	if (m_State == State_Running) {
		Render(m_pMediaSample);
	}
	ClearPendingSample();
	SendEndOfStream();
	CancelNotification();
	return NOERROR;
}
