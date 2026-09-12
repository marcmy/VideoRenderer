/*
 * RIFE migration wrapper around the existing renderer implementation.
 *
 * The legacy implementation remains byte-for-byte preserved in
 * VideoRendererLegacy.inl.  Only the settings entry points are wrapped here
 * while the RIFE playback path is brought online.
 */

#include "stdafx.h"
#include <mutex>
#include <unordered_set>
#include "VideoRenderer.h"

#define GetSettings GetSettingsLegacy
#define SetSettings SetSettingsLegacy
#define SaveSettings SaveSettingsLegacy
#include "VideoRendererLegacy.inl"
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

	// One-way migration from the old midpoint-only NvOFFRUC setting.  Merely
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
		// RIFE owns production frame synthesis once enabled.  Keep the old
		// NvOFFRUC values only as migration data; never let both paths run.
		adjusted.iFrameInterpolationMode = FRUC_MODE_Disabled;
	}

	if (RifeSettingsChanged(adjusted, m_Sets)) {
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
