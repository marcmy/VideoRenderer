/*
 * (C) 2018-2026 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// RIFE settings and playback integration, included by VideoRenderer.cpp.

#include "FeatureHotkeyDialog.h"

#include <condition_variable>
#include <unordered_map>
#include <vector>

namespace {

constexpr LPCWSTR OPT_RifeMode              = L"RifeMode";
constexpr LPCWSTR OPT_RifeCustomFps         = L"RifeCustomFps";
constexpr LPCWSTR OPT_RifeGpuThreads        = L"RifeGpuThreads";
constexpr LPCWSTR OPT_RifeModel             = L"RifeModel";
constexpr LPCWSTR OPT_RifeGPU               = L"RifeGPU";
constexpr LPCWSTR OPT_RifePreservePrecision = L"RifePreservePrecision";
constexpr LPCWSTR OPT_RifeFeatureReuse = L"RifeFeatureReuse";
constexpr LPCWSTR OPT_RifeFlowReuse = L"RifeFlowReuse";
constexpr LPCWSTR OPT_RifePerformanceBoost  = L"RifePerformanceBoost";
constexpr LPCWSTR OPT_RifeProcessingResolution = L"RifeProcessingResolution";
constexpr LPCWSTR OPT_RifeProcessingLimit      = L"RifeProcessingLimit";
constexpr LPCWSTR OPT_RifeSceneDetection    = L"RifeSceneDetection";
constexpr LPCWSTR OPT_RifeSceneThreshold    = L"RifeSceneThreshold";
constexpr LPCWSTR OPT_RifeSceneProcessing   = L"RifeSceneProcessing";
constexpr LPCWSTR OPT_RifeDuplicateRemoval  = L"RifeDuplicateRemoval";
constexpr LPCWSTR OPT_RifeRateRules          = L"RifeRateRules";
constexpr LPCWSTR OPT_DetailedStatistics     = L"DetailedStatistics";
constexpr LPCWSTR OPT_SdrEnhanceMode         = L"SdrEnhanceMode";
constexpr LPCWSTR OPT_SdrEnhanceContrast     = L"SdrEnhanceContrast";
constexpr LPCWSTR OPT_SdrEnhancePivot        = L"SdrEnhancePivot";
constexpr LPCWSTR OPT_SdrEnhanceVibrance     = L"SdrEnhanceVibrance";
constexpr LPCWSTR OPT_SdrEnhanceSkin         = L"SdrEnhanceSkinProtection";
constexpr LPCWSTR OPT_HotkeyMaxine           = L"HotkeyMaxine";
constexpr LPCWSTR OPT_HotkeyRife             = L"HotkeyRife";
constexpr LPCWSTR OPT_HotkeySdrEnhance       = L"HotkeySdrEnhance";
constexpr LPCWSTR OPT_MaxineLastOperation    = L"MaxineLastOperation";
constexpr LPCWSTR OPT_RifeLastMode           = L"RifeLastMode";
constexpr LPCWSTR OPT_SdrEnhanceLastMode     = L"SdrEnhanceLastMode";

constexpr UINT FeatureHotkeyMessageMs = 2000;

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
		|| a.bRifePreservePrecision != b.bRifePreservePrecision
		|| a.bRifeFeatureReuse != b.bRifeFeatureReuse
		|| a.iRifeFlowReuse != b.iRifeFlowReuse
		|| a.bRifePerformanceBoost != b.bRifePerformanceBoost
		|| a.iRifeProcessingResolution != b.iRifeProcessingResolution
		|| a.iRifeProcessingLimit != b.iRifeProcessingLimit
		|| a.iRifeSceneDetection != b.iRifeSceneDetection
		|| a.iRifeSceneThreshold != b.iRifeSceneThreshold
		|| a.iRifeSceneProcessing != b.iRifeSceneProcessing
		|| a.iRifeDuplicateRemoval != b.iRifeDuplicateRemoval
		|| a.rifeRules != b.rifeRules;
}

void ClampSdrEnhanceSettings(Settings_t& settings)
{
	if (settings.iSdrEnhanceMode < 0 || settings.iSdrEnhanceMode >= SDR_ENHANCE_COUNT) {
		settings.iSdrEnhanceMode = SDR_ENHANCE_Off;
	}
	settings.iSdrEnhanceContrast = std::clamp(settings.iSdrEnhanceContrast,
		SDR_ENHANCE_CONTRAST_MIN, SDR_ENHANCE_CONTRAST_MAX);
	settings.iSdrEnhancePivot = std::clamp(settings.iSdrEnhancePivot,
		SDR_ENHANCE_PIVOT_MIN, SDR_ENHANCE_PIVOT_MAX);
	settings.iSdrEnhanceVibrance = std::clamp(settings.iSdrEnhanceVibrance,
		SDR_ENHANCE_VIBRANCE_MIN, SDR_ENHANCE_VIBRANCE_MAX);
	settings.iSdrEnhanceSkinProtection = std::clamp(settings.iSdrEnhanceSkinProtection, 0, 100);
}

void ClampFeatureHotkeySettings(Settings_t& settings)
{
	for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
		int& hotkey = settings.FeatureHotkey(feature);
		hotkey = NormalizeFeatureHotkey(hotkey);
	}
	// The mode a hotkey restores is the last one that was on.
	RememberFeatureMode(settings.iMaxineOperation, settings.iMaxineLastOperation,
		MAXINE_OPERATION_Disabled, MAXINE_OPERATION_Upscale, MAXINE_OPERATION_COUNT);
	RememberFeatureMode(settings.iRifeMode, settings.iRifeLastMode,
		RIFE_MODE_Disabled, RIFE_MODE_Movie2x, RIFE_MODE_COUNT);
	RememberFeatureMode(settings.iSdrEnhanceMode, settings.iSdrEnhanceLastMode,
		SDR_ENHANCE_Off, SDR_ENHANCE_On, SDR_ENHANCE_COUNT);
}

void LoadFeatureHotkeySettings(CRegKey& key, Settings_t& settings)
{
	DWORD dw = 0;
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HotkeyMaxine, dw)) settings.iHotkeyMaxine = static_cast<int>(dw);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HotkeyRife, dw)) settings.iHotkeyRife = static_cast<int>(dw);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HotkeySdrEnhance, dw)) settings.iHotkeySdrEnhance = static_cast<int>(dw);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_MaxineLastOperation, dw)) settings.iMaxineLastOperation = static_cast<int>(dw);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeLastMode, dw)) settings.iRifeLastMode = static_cast<int>(dw);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SdrEnhanceLastMode, dw)) settings.iSdrEnhanceLastMode = static_cast<int>(dw);
	ClampFeatureHotkeySettings(settings);
}

// Renderer hotkeys. The player owns the keyboard: its accelerators are
// translated in its message loop before any window sees the key. A
// thread-specific WH_GETMESSAGE hook on the player's UI thread sees each key
// message as the loop retrieves it; it blanks only the three bound hotkeys
// and leaves everything else alone. (WH_KEYBOARD would miss key messages that
// were posted rather than typed.) Hook procedures, the toggle and the expiry
// timer all run on that thread.
struct FeatureHotkeyRenderer {
	CMpcVideoRenderer* renderer = nullptr;
	int hotkeys[FEATURE_HOTKEY_COUNT] = {};
	UINT_PTR messageTimer = 0;
	// A hook or timer callback is running in this renderer (outside the lock).
	int busy = 0;
	DWORD busyThread = 0;
};

struct FeatureHotkeyThread {
	HHOOK hook = nullptr;
	UINT swallowedKey = 0;
	// The most recently registered renderer is the one playing: during a file
	// switch the old one may still exist briefly.
	std::vector<FeatureHotkeyRenderer> renderers;
};

std::mutex g_featureHotkeyMutex;
std::condition_variable g_featureHotkeyIdle;
std::unordered_map<DWORD, FeatureHotkeyThread> g_featureHotkeyThreads;

FeatureHotkeyRenderer* FindFeatureHotkeyRenderer(const CMpcVideoRenderer* renderer)
{
	for (auto& [threadId, thread] : g_featureHotkeyThreads) {
		for (auto& entry : thread.renderers) {
			if (entry.renderer == renderer) {
				return &entry;
			}
		}
	}
	return nullptr;
}

// Calls into a registered renderer without holding the lock, so the
// renderer's own locks never nest inside it (the player may initialise the
// renderer on another thread while holding them). Unregistering waits until
// the call has returned, so the renderer outlives it.
template <class Call>
void CallFeatureHotkeyRenderer(std::unique_lock<std::mutex>& lock, FeatureHotkeyRenderer& entry, Call&& call)
{
	const CMpcVideoRenderer* renderer = entry.renderer;
	++entry.busy;
	entry.busyThread = GetCurrentThreadId();
	lock.unlock();
	call();
	lock.lock();
	// The renderer may have re-registered meanwhile; never count below zero.
	if (auto* current = FindFeatureHotkeyRenderer(renderer); current && current->busy > 0) {
		--current->busy;
	}
	g_featureHotkeyIdle.notify_all();
}

LRESULT CALLBACK FeatureHotkeyProc(int code, WPARAM wParam, LPARAM lParam)
{
	// Every message the thread retrieves passes here; only key messages matter.
	auto* msg = reinterpret_cast<MSG*>(lParam);
	if (code != HC_ACTION || !msg || !IsKeyboardMessage(msg->message) || FeatureHotkeyCaptureActive()) {
		// While the hotkey settings are open, their key boxes get every key.
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}

	std::unique_lock lock(g_featureHotkeyMutex);
	const auto it = g_featureHotkeyThreads.find(GetCurrentThreadId());
	if (it == g_featureHotkeyThreads.end()) {
		lock.unlock();
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}
	FeatureHotkeyThread& thread = it->second;
	const HHOOK hook = thread.hook;
	const UINT vk = static_cast<UINT>(msg->wParam);

	int feature = -1;
	if (!thread.renderers.empty()) {
		const int pressed = FeatureHotkeyFromKeyboard(vk,
			GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0);
		feature = FindFeatureHotkey(thread.renderers.back().hotkeys, pressed);
	}

	const auto action = ClassifyFeatureHotkey(wParam == PM_REMOVE, vk, msg->lParam, feature, thread.swallowedKey);
	if (action != FEATURE_HOTKEY_ACTION_Pass) {
		// The player's message loop, accelerators included, sees WM_NULL.
		msg->message = WM_NULL;
	}
	if (action == FEATURE_HOTKEY_ACTION_Toggle) {
		CMpcVideoRenderer* renderer = thread.renderers.back().renderer;
		CallFeatureHotkeyRenderer(lock, thread.renderers.back(), [=] { renderer->OnFeatureHotkey(feature); });
	}
	lock.unlock();
	return CallNextHookEx(hook, code, wParam, lParam);
}

void CALLBACK FeatureHotkeyMessageTimerProc(HWND, UINT, UINT_PTR timer, DWORD)
{
	KillTimer(nullptr, timer);
	std::unique_lock lock(g_featureHotkeyMutex);
	for (auto& [threadId, thread] : g_featureHotkeyThreads) {
		for (auto& entry : thread.renderers) {
			if (entry.messageTimer == timer) {
				entry.messageTimer = 0;
				CMpcVideoRenderer* renderer = entry.renderer;
				CallFeatureHotkeyRenderer(lock, entry, [=] { renderer->OnFeatureHotkeyMessageExpired(); });
				return;
			}
		}
	}
}

// Removes a renderer's registration, and the hook with a thread's last
// renderer. The caller holds the lock.
void RemoveFeatureHotkeyRenderer(const CMpcVideoRenderer* renderer)
{
	for (auto it = g_featureHotkeyThreads.begin(); it != g_featureHotkeyThreads.end();) {
		auto& renderers = it->second.renderers;
		std::erase_if(renderers, [renderer](const FeatureHotkeyRenderer& entry) { return entry.renderer == renderer; });
		if (renderers.empty()) {
			if (it->second.hook) {
				UnhookWindowsHookEx(it->second.hook);
			}
			it = g_featureHotkeyThreads.erase(it);
		} else {
			++it;
		}
	}
}

// For the destructor: waits until no hook or timer call is running in the
// renderer. A call running on this thread is the caller itself.
void UnregisterFeatureHotkeys(const CMpcVideoRenderer* renderer)
{
	std::unique_lock lock(g_featureHotkeyMutex);
	g_featureHotkeyIdle.wait(lock, [renderer] {
		const auto* entry = FindFeatureHotkeyRenderer(renderer);
		return !entry || !entry->busy || entry->busyThread == GetCurrentThreadId();
	});
	RemoveFeatureHotkeyRenderer(renderer);
}

void UpdateFeatureHotkeyBindings(const CMpcVideoRenderer* renderer, const Settings_t& settings)
{
	std::scoped_lock lock(g_featureHotkeyMutex);
	if (auto* entry = FindFeatureHotkeyRenderer(renderer)) {
		for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
			entry->hotkeys[feature] = settings.FeatureHotkey(feature);
		}
	}
}

void StartFeatureHotkeyMessageTimer(const CMpcVideoRenderer* renderer)
{
	std::scoped_lock lock(g_featureHotkeyMutex);
	if (auto* entry = FindFeatureHotkeyRenderer(renderer)) {
		// A thread timer: no window procedure has to know about it.
		entry->messageTimer = SetTimer(nullptr, entry->messageTimer,
			FeatureHotkeyMessageMs + 100, FeatureHotkeyMessageTimerProc);
	}
}

void LoadSdrEnhanceSettings(CRegKey& key, Settings_t& settings)
{
	// Signed percentages are stored as two's-complement DWORDs.
	DWORD dw = 0;
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SdrEnhanceMode, dw)) settings.iSdrEnhanceMode = static_cast<int>(dw);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SdrEnhanceContrast, dw)) settings.iSdrEnhanceContrast = static_cast<int>(dw);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SdrEnhancePivot, dw)) settings.iSdrEnhancePivot = static_cast<int>(dw);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SdrEnhanceVibrance, dw)) settings.iSdrEnhanceVibrance = static_cast<int>(dw);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SdrEnhanceSkin, dw)) settings.iSdrEnhanceSkinProtection = static_cast<int>(dw);
	ClampSdrEnhanceSettings(settings);
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
		switch (dw) {
		case RIFE_MODEL_44:
		case RIFE_MODEL_46:
		case RIFE_MODEL_415_LITE:
		case RIFE_MODEL_425_LITE:
			settings.iRifeModel = static_cast<int>(dw);
			break;
		default:
			settings.iRifeModel = RIFE_MODEL_425_LITE;
			break;
		}
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeGPU, dw)) {
		if (dw == MAXDWORD) {
			settings.iRifeGPU = RIFE_GPU_Auto;
		} else if (dw <= 7) {
			settings.iRifeGPU = static_cast<int>(dw);
		}
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifePreservePrecision, dw)) settings.bRifePreservePrecision = !!dw;
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeFeatureReuse, dw)) settings.bRifeFeatureReuse = !!dw;
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeFlowReuse, dw) && dw <= RIFE_FLOW_REUSE_Retime) settings.iRifeFlowReuse = static_cast<int>(dw);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifePerformanceBoost, dw)) {
		settings.bRifePerformanceBoost = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeProcessingResolution, dw)) {
		settings.iRifeProcessingResolution = dw < RIFE_RESOLUTION_COUNT
			? static_cast<int>(dw) : RIFE_RESOLUTION_Source;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeProcessingLimit, dw)) {
		settings.iRifeProcessingLimit = dw >= RifeProcessingLimitMin && dw <= RifeProcessingLimitMax
			? static_cast<int>(dw) : RifeProcessingLimitDefault;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DetailedStatistics, dw)) {
		settings.bDetailedStats = !!dw;
	}
	LoadSdrEnhanceSettings(key, settings);
	LoadFeatureHotkeySettings(key, settings);
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeSceneDetection, dw)) {
		settings.iRifeSceneDetection = dw < RIFE_SCENE_COUNT ? static_cast<int>(dw) : RIFE_SCENE_SVPflow1;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeSceneThreshold, dw)) {
		settings.iRifeSceneThreshold = dw <= 100 ? static_cast<int>(dw) : 15;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeSceneProcessing, dw)) {
		settings.iRifeSceneProcessing = dw < RIFE_SCENE_PROCESS_COUNT ? static_cast<int>(dw) : RIFE_SCENE_PROCESS_Repeat;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_RifeDuplicateRemoval, dw)) {
		settings.iRifeDuplicateRemoval = dw < RIFE_DUPLICATES_COUNT ? static_cast<int>(dw) : RIFE_DUPLICATES_Keep;
	}
	std::array<wchar_t, RifeRateRulesMaxTextLength + 1> rulesText = {};
	ULONG rulesChars = static_cast<ULONG>(rulesText.size());
	if (ERROR_SUCCESS == key.QueryStringValue(OPT_RifeRateRules, rulesText.data(), &rulesChars)) {
		RifeRateRules parsed;
		if (ParseRifeRateRules(rulesText.data(), parsed)) {
			settings.rifeRules = parsed;
		}
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
	adjusted.iRifeSceneThreshold = std::clamp(settings.iRifeSceneThreshold, 0, 100);
	adjusted.iRifeProcessingResolution = settings.iRifeProcessingResolution >= 0
		&& settings.iRifeProcessingResolution < RIFE_RESOLUTION_COUNT
		? settings.iRifeProcessingResolution : RIFE_RESOLUTION_Source;
	adjusted.iRifeProcessingLimit = std::clamp(settings.iRifeProcessingLimit,
		RifeProcessingLimitMin, RifeProcessingLimitMax);
	ClampSdrEnhanceSettings(adjusted);
	ClampFeatureHotkeySettings(adjusted);
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
	UpdateFeatureHotkeyBindings(this, m_Sets);
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
		key.SetDWORDValue(OPT_RifePreservePrecision, m_Sets.bRifePreservePrecision ? 1u : 0u);
		key.SetDWORDValue(OPT_RifeFeatureReuse, m_Sets.bRifeFeatureReuse ? 1u : 0u);
		key.SetDWORDValue(OPT_RifeFlowReuse, static_cast<DWORD>(m_Sets.iRifeFlowReuse));
		key.SetDWORDValue(OPT_RifePerformanceBoost, m_Sets.bRifePerformanceBoost ? 1u : 0u);
		key.SetDWORDValue(OPT_DetailedStatistics,   m_Sets.bDetailedStats ? 1u : 0u);
		key.SetDWORDValue(OPT_SdrEnhanceMode,       static_cast<DWORD>(m_Sets.iSdrEnhanceMode));
		key.SetDWORDValue(OPT_SdrEnhanceContrast,   static_cast<DWORD>(m_Sets.iSdrEnhanceContrast));
		key.SetDWORDValue(OPT_SdrEnhancePivot,      static_cast<DWORD>(m_Sets.iSdrEnhancePivot));
		key.SetDWORDValue(OPT_SdrEnhanceVibrance,   static_cast<DWORD>(m_Sets.iSdrEnhanceVibrance));
		key.SetDWORDValue(OPT_SdrEnhanceSkin,       static_cast<DWORD>(m_Sets.iSdrEnhanceSkinProtection));
		key.SetDWORDValue(OPT_HotkeyMaxine,         static_cast<DWORD>(m_Sets.iHotkeyMaxine));
		key.SetDWORDValue(OPT_HotkeyRife,           static_cast<DWORD>(m_Sets.iHotkeyRife));
		key.SetDWORDValue(OPT_HotkeySdrEnhance,     static_cast<DWORD>(m_Sets.iHotkeySdrEnhance));
		key.SetDWORDValue(OPT_MaxineLastOperation,  static_cast<DWORD>(m_Sets.iMaxineLastOperation));
		key.SetDWORDValue(OPT_RifeLastMode,         static_cast<DWORD>(m_Sets.iRifeLastMode));
		key.SetDWORDValue(OPT_SdrEnhanceLastMode,   static_cast<DWORD>(m_Sets.iSdrEnhanceLastMode));
		key.SetDWORDValue(OPT_RifeSceneDetection,   static_cast<DWORD>(m_Sets.iRifeSceneDetection));
		key.SetDWORDValue(OPT_RifeProcessingResolution, static_cast<DWORD>(m_Sets.iRifeProcessingResolution));
		key.SetDWORDValue(OPT_RifeProcessingLimit, static_cast<DWORD>(m_Sets.iRifeProcessingLimit));
		key.SetDWORDValue(OPT_RifeSceneThreshold,    static_cast<DWORD>(m_Sets.iRifeSceneThreshold));
		key.SetDWORDValue(OPT_RifeSceneProcessing,  static_cast<DWORD>(m_Sets.iRifeSceneProcessing));
		key.SetDWORDValue(OPT_RifeDuplicateRemoval, static_cast<DWORD>(m_Sets.iRifeDuplicateRemoval));
		const std::wstring serializedRules = SerializeRifeRateRules(m_Sets.rifeRules);
		if (!serializedRules.empty()) {
			key.SetStringValue(OPT_RifeRateRules, serializedRules.c_str());
		}
	}

	return S_OK;
}

void CMpcVideoRenderer::RegisterFeatureHotkeys()
{
	if (!m_hWndParentMain) {
		return;
	}
	const DWORD threadId = GetWindowThreadProcessId(m_hWndParentMain, nullptr);
	if (!threadId) {
		return;
	}

	EnsureRifeSettingsLoaded(this, m_Sets);
	// Init() holds the renderer lock, so this never waits for a running toggle.
	std::scoped_lock lock(g_featureHotkeyMutex);
	if (const auto it = g_featureHotkeyThreads.find(threadId); it != g_featureHotkeyThreads.end()) {
		for (const auto& entry : it->second.renderers) {
			if (entry.renderer == this) {
				return;
			}
		}
	}
	// First Init(), or the player window moved to another thread.
	RemoveFeatureHotkeyRenderer(this);

	FeatureHotkeyThread& thread = g_featureHotkeyThreads[threadId];
	if (!thread.hook) {
		// Thread-specific, never global: only the player's own keyboard input.
		thread.hook = SetWindowsHookExW(WH_GETMESSAGE, FeatureHotkeyProc, nullptr, threadId);
		DLogIf(!thread.hook, L"CMpcVideoRenderer::RegisterFeatureHotkeys() : SetWindowsHookExW() failed with error {}",
			HR2Str(HRESULT_FROM_WIN32(GetLastError())));
	}
	FeatureHotkeyRenderer entry;
	entry.renderer = this;
	for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
		entry.hotkeys[feature] = m_Sets.FeatureHotkey(feature);
	}
	thread.renderers.push_back(entry);
}

void CMpcVideoRenderer::OnFeatureHotkey(int feature)
{
	Settings_t settings;
	GetSettings(settings);

	std::wstring message;
	switch (feature) {
	case FEATURE_HOTKEY_Maxine:
		settings.iMaxineOperation = ToggleFeatureMode(settings.iMaxineOperation, settings.iMaxineLastOperation,
			MAXINE_OPERATION_Disabled, MAXINE_OPERATION_Upscale, MAXINE_OPERATION_COUNT);
		switch (settings.iMaxineOperation) {
		case MAXINE_OPERATION_Upscale: message = L"Maxine: upscaling"; break;
		case MAXINE_OPERATION_Denoise: message = L"Maxine: denoising"; break;
		case MAXINE_OPERATION_Deblur:  message = L"Maxine: deblurring"; break;
		default:                       message = L"Maxine: off"; break;
		}
		break;
	case FEATURE_HOTKEY_Rife:
		settings.iRifeMode = ToggleFeatureMode(settings.iRifeMode, settings.iRifeLastMode,
			RIFE_MODE_Disabled, RIFE_MODE_Movie2x, RIFE_MODE_COUNT);
		message = settings.iRifeMode != RIFE_MODE_Disabled
			? L"RIFE frame interpolation: on" : L"RIFE frame interpolation: off";
		break;
	case FEATURE_HOTKEY_SdrEnhance:
		settings.iSdrEnhanceMode = ToggleFeatureMode(settings.iSdrEnhanceMode, settings.iSdrEnhanceLastMode,
			SDR_ENHANCE_Off, SDR_ENHANCE_On, SDR_ENHANCE_COUNT);
		message = settings.iSdrEnhanceMode == SDR_ENHANCE_Split ? L"SDR enhancement: A/B split"
			: settings.iSdrEnhanceMode == SDR_ENHANCE_On ? L"SDR enhancement: on" : L"SDR enhancement: off";
		break;
	default:
		return;
	}
#ifndef _WIN64
	if (feature != FEATURE_HOTKEY_SdrEnhance) {
		message += L" (needs the 64-bit renderer)";
	}
#endif
	if (m_VideoProcessor->Type() != VP_DX11) {
		message += L" (needs Direct3D 11)";
	}

	SetSettings(settings);
	SaveSettings();

	if (m_VideoProcessor->Type() == VP_DX11) {
		static_cast<CDX11VideoProcessor*>(m_VideoProcessor.get())->ShowToggleMessage(message, FeatureHotkeyMessageMs);
		if (m_filterState != State_Running) {
			// Paused or stopped: show it now and clear it when it expires.
			Redraw();
			StartFeatureHotkeyMessageTimer(this);
		}
	}
}

void CMpcVideoRenderer::OnFeatureHotkeyMessageExpired()
{
	if (m_filterState != State_Running) {
		Redraw();
	}
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

	// RIFE only owns running playback. Preserve early paused-sample readiness;
	// Maxine model loading now runs independently of the first draw.
	if (m_State == State_Paused) {
		m_bInReceive = FALSE;
		{
			CAutoLock cVideoLock(&m_InterfaceLock);
			if (m_State == State_Stopped) {
				return NOERROR;
			}
			m_bInReceive = TRUE;
		}
		m_bInReceive = FALSE;
		CAutoLock cRendererLock(&m_RendererLock);
		if (m_State == State_Paused && !m_bFlushing && m_pMediaSample) {
			Ready();
			DoRenderSample(m_pMediaSample);
		}
	}

	bool rifeSubmitted = false;
	REFERENCE_TIME rifeFrameDuration = 0;
	// A pending frame step (MPC-HC steps one frame after every seek while
	// paused) is completed only by DoRenderSample(). The RIFE presenter never
	// calls it, so a stepped sample must take the synchronous path below or the
	// graph keeps running instead of returning to pause.
	if (m_State == State_Running && m_VideoProcessor->Type() == VP_DX11 && !m_Stepping) {
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
			// A step requested while waiting for the locks also needs the
			// synchronous renderer.
			if (!m_Stepping) {
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

	// Paused seek/step samples and running source fallbacks use the synchronous
	// renderer. Invalidate queued RIFE work without discarding the learned cap:
	// BeginFlush/NewSegment already preserve it, and a paused preroll must not
	// turn that suspension into a hard reset. Stop/settings changes own resets.
	if (m_RifePipeline) {
		m_RifePipeline->Suspend();
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
