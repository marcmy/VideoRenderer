/*
 * (C) 2018-2026 see Authors.txt
 *
 * Settings dialog for the renderer hotkeys (Maxine, RIFE, SDR enhancement).
 */

#include "stdafx.h"
#include "resource.h"
#include "FeatureHotkeyDialog.h"
#include "SettingsDialogTheme.h"

#include <atomic>
#include <commctrl.h>
#include <format>

namespace {

std::atomic<int> g_captureDialogs;

// Indexed by FEATURE_HOTKEY_*.
constexpr int HotkeyControls[FEATURE_HOTKEY_COUNT] = {
	IDC_HOTKEY_MAXINE, IDC_HOTKEY_RIFE, IDC_HOTKEY_SDR_ENHANCE,
};
constexpr int ClearButtons[FEATURE_HOTKEY_COUNT] = {
	IDC_HOTKEY_MAXINE_CLEAR, IDC_HOTKEY_RIFE_CLEAR, IDC_HOTKEY_SDR_ENHANCE_CLEAR,
};
constexpr LPCWSTR FeatureNames[FEATURE_HOTKEY_COUNT] = {
	L"Maxine", L"RIFE frame interpolation", L"SDR enhancement",
};

void CopyFeatureHotkeys(Settings_t& dst, const Settings_t& src)
{
	for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
		dst.FeatureHotkey(feature) = src.FeatureHotkey(feature);
	}
}

bool FeatureHotkeysEqual(const Settings_t& a, const Settings_t& b)
{
	for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
		if (a.FeatureHotkey(feature) != b.FeatureHotkey(feature)) {
			return false;
		}
	}
	return true;
}

void SetControls(HWND hwnd, const Settings_t& settings)
{
	for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
		SendDlgItemMessageW(hwnd, HotkeyControls[feature], HKM_SETHOTKEY,
			static_cast<WPARAM>(NormalizeFeatureHotkey(settings.FeatureHotkey(feature))), 0);
	}
}

void InitializeDialog(HWND hwnd, const Settings_t& settings)
{
	for (const int id : HotkeyControls) {
		// A key without Ctrl or Alt (alone or with Shift) belongs to the
		// player; the control turns it into Ctrl+Alt+key.
		SendDlgItemMessageW(hwnd, id, HKM_SETRULES, HKCOMB_NONE | HKCOMB_S,
			MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
	}
	SetControls(hwnd, settings);
}

bool ReadControls(HWND hwnd, Settings_t& settings)
{
	Settings_t read = settings;
	for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
		const int hotkey = static_cast<int>(LOWORD(SendDlgItemMessageW(hwnd, HotkeyControls[feature], HKM_GETHOTKEY, 0, 0)));
		if (!IsValidFeatureHotkey(hotkey)) {
			MessageBoxW(hwnd, std::format(L"Choose a key with Ctrl or Alt for {}, or clear it.", FeatureNames[feature]).c_str(),
				L"Renderer hotkeys", MB_OK | MB_ICONERROR);
			SetFocus(GetDlgItem(hwnd, HotkeyControls[feature]));
			return false;
		}
		read.FeatureHotkey(feature) = NormalizeFeatureHotkey(hotkey);
	}
	for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
		for (int other = 0; other < feature; ++other) {
			if (read.FeatureHotkey(feature) && read.FeatureHotkey(feature) == read.FeatureHotkey(other)) {
				MessageBoxW(hwnd, std::format(L"{} and {} have the same hotkey.", FeatureNames[other], FeatureNames[feature]).c_str(),
					L"Renderer hotkeys", MB_OK | MB_ICONERROR);
				SetFocus(GetDlgItem(hwnd, HotkeyControls[feature]));
				return false;
			}
		}
	}
	CopyFeatureHotkeys(settings, read);
	return true;
}

INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* settings = reinterpret_cast<Settings_t*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	switch (message) {
	case WM_INITDIALOG:
		settings = reinterpret_cast<Settings_t*>(lParam);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(settings));
		InitializeSettingsDialogTheme(hwnd);
		InitializeDialog(hwnd, *settings);
		return TRUE;

	case WM_COMMAND:
		if (HIWORD(wParam) == BN_CLICKED) {
			for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
				if (LOWORD(wParam) == ClearButtons[feature]) {
					SendDlgItemMessageW(hwnd, HotkeyControls[feature], HKM_SETHOTKEY, 0, 0);
					return TRUE;
				}
			}
		}
		switch (LOWORD(wParam)) {
		case IDC_BUTTON_HOTKEYS_DEFAULTS:
			if (HIWORD(wParam) == BN_CLICKED) {
				Settings_t defaults;
				SetControls(hwnd, defaults);
				return TRUE;
			}
			break;
		case IDOK:
			if (ReadControls(hwnd, *settings)) {
				EndDialog(hwnd, IDOK);
			}
			return TRUE;
		case IDCANCEL:
			EndDialog(hwnd, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

} // namespace

bool ShowFeatureHotkeySettings(HWND parent, HINSTANCE instance, Settings_t& settings)
{
	const INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_HOTKEY_CLASS };
	InitCommonControlsEx(&controls);

	Settings_t candidate = settings;
	++g_captureDialogs;
	const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_FEATURE_HOTKEYS),
		parent, DialogProc, reinterpret_cast<LPARAM>(&candidate));
	--g_captureDialogs;
	if (result == -1) {
		MessageBoxW(parent, L"The hotkey settings window could not be opened.",
			L"MPC Video Renderer", MB_OK | MB_ICONERROR);
		return false;
	}
	if (result != IDOK || FeatureHotkeysEqual(candidate, settings)) {
		return false;
	}
	CopyFeatureHotkeys(settings, candidate);
	return true;
}

bool FeatureHotkeyCaptureActive()
{
	return g_captureDialogs.load() > 0;
}
