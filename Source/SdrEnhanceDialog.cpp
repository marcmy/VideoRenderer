/*
 * (C) 2018-2026 see Authors.txt
 *
 * SDR-to-SDR perceptual enhancement property dialog.
 */

#include "stdafx.h"
#include "resource.h"
#include "SdrEnhanceDialog.h"
#include "SettingsDialogTheme.h"

#include <format>

namespace {

void CopySdrEnhanceSettings(Settings_t& dst, const Settings_t& src)
{
	dst.iSdrEnhanceMode = src.iSdrEnhanceMode;
	dst.iSdrEnhanceContrast = src.iSdrEnhanceContrast;
	dst.iSdrEnhancePivot = src.iSdrEnhancePivot;
	dst.iSdrEnhanceVibrance = src.iSdrEnhanceVibrance;
	dst.iSdrEnhanceSkinProtection = src.iSdrEnhanceSkinProtection;
}

bool SdrEnhanceSettingsEqual(const Settings_t& a, const Settings_t& b)
{
	return a.iSdrEnhanceMode == b.iSdrEnhanceMode
		&& a.iSdrEnhanceContrast == b.iSdrEnhanceContrast
		&& a.iSdrEnhancePivot == b.iSdrEnhancePivot
		&& a.iSdrEnhanceVibrance == b.iSdrEnhanceVibrance
		&& a.iSdrEnhanceSkinProtection == b.iSdrEnhanceSkinProtection;
}

void EnableControls(HWND hwnd)
{
	const LRESULT mode = SendDlgItemMessageW(hwnd, IDC_SDR_ENHANCE_MODE, CB_GETCURSEL, 0, 0);
	const bool enabled = mode != CB_ERR && mode != SDR_ENHANCE_Off;
	for (const int id : {IDC_SDR_ENHANCE_CONTRAST, IDC_SDR_ENHANCE_PIVOT,
			IDC_SDR_ENHANCE_VIBRANCE, IDC_SDR_ENHANCE_SKIN}) {
		EnableWindow(GetDlgItem(hwnd, id), enabled);
	}
}

void SetControls(HWND hwnd, const Settings_t& settings)
{
	SendDlgItemMessageW(hwnd, IDC_SDR_ENHANCE_MODE, CB_SETCURSEL, settings.iSdrEnhanceMode, 0);
	SetDlgItemInt(hwnd, IDC_SDR_ENHANCE_CONTRAST, settings.iSdrEnhanceContrast, TRUE);
	SetDlgItemInt(hwnd, IDC_SDR_ENHANCE_PIVOT, settings.iSdrEnhancePivot, FALSE);
	SetDlgItemInt(hwnd, IDC_SDR_ENHANCE_VIBRANCE, settings.iSdrEnhanceVibrance, TRUE);
	SetDlgItemInt(hwnd, IDC_SDR_ENHANCE_SKIN, settings.iSdrEnhanceSkinProtection, FALSE);
	EnableControls(hwnd);
}

void InitializeDialog(HWND hwnd, const Settings_t& settings)
{
	// Item order matches SDR_ENHANCE_*.
	for (const LPCWSTR label : {L"Off (renderer output unchanged)", L"On",
			L"A/B split: original left, enhanced right"}) {
		SendDlgItemMessageW(hwnd, IDC_SDR_ENHANCE_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
	}
	SetControls(hwnd, settings);
}

bool ReadPercent(HWND hwnd, int id, int minimum, int maximum, LPCWSTR name, int& value)
{
	BOOL valid = FALSE;
	const int read = static_cast<int>(GetDlgItemInt(hwnd, id, &valid, minimum < 0));
	if (!valid || read < minimum || read > maximum) {
		MessageBoxW(hwnd, std::format(L"Enter a {} from {} to {} percent.", name, minimum, maximum).c_str(),
			L"SDR enhancement", MB_OK | MB_ICONERROR);
		SetFocus(GetDlgItem(hwnd, id));
		return false;
	}
	value = read;
	return true;
}

bool ReadControls(HWND hwnd, Settings_t& settings)
{
	const LRESULT mode = SendDlgItemMessageW(hwnd, IDC_SDR_ENHANCE_MODE, CB_GETCURSEL, 0, 0);
	Settings_t read = settings;
	read.iSdrEnhanceMode = mode >= 0 && mode < SDR_ENHANCE_COUNT ? static_cast<int>(mode) : SDR_ENHANCE_Off;
	if (!ReadPercent(hwnd, IDC_SDR_ENHANCE_CONTRAST, SDR_ENHANCE_CONTRAST_MIN, SDR_ENHANCE_CONTRAST_MAX,
			L"contrast", read.iSdrEnhanceContrast)
			|| !ReadPercent(hwnd, IDC_SDR_ENHANCE_PIVOT, SDR_ENHANCE_PIVOT_MIN, SDR_ENHANCE_PIVOT_MAX,
				L"contrast pivot", read.iSdrEnhancePivot)
			|| !ReadPercent(hwnd, IDC_SDR_ENHANCE_VIBRANCE, SDR_ENHANCE_VIBRANCE_MIN, SDR_ENHANCE_VIBRANCE_MAX,
				L"vibrance", read.iSdrEnhanceVibrance)
			|| !ReadPercent(hwnd, IDC_SDR_ENHANCE_SKIN, 0, 100, L"skin-tone protection",
				read.iSdrEnhanceSkinProtection)) {
		return false;
	}
	CopySdrEnhanceSettings(settings, read);
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
		switch (LOWORD(wParam)) {
		case IDC_SDR_ENHANCE_MODE:
			if (HIWORD(wParam) == CBN_SELCHANGE) {
				EnableControls(hwnd);
				return TRUE;
			}
			break;
		case IDC_BUTTON_SDR_ENHANCE_DEFAULTS:
			if (HIWORD(wParam) == BN_CLICKED) {
				// Restore the strengths but keep the selected mode, so the
				// defaults can be compared without switching the effect off.
				Settings_t defaults;
				defaults.iSdrEnhanceMode = static_cast<int>(
					SendDlgItemMessageW(hwnd, IDC_SDR_ENHANCE_MODE, CB_GETCURSEL, 0, 0));
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

bool ShowSdrEnhanceSettings(HWND parent, HINSTANCE instance, Settings_t& settings)
{
	Settings_t candidate = settings;
	const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SDRENHANCE),
		parent, DialogProc, reinterpret_cast<LPARAM>(&candidate));
	if (result == -1) {
		MessageBoxW(parent, L"The SDR enhancement settings window could not be opened.",
			L"MPC Video Renderer", MB_OK | MB_ICONERROR);
		return false;
	}
	if (result != IDOK || SdrEnhanceSettingsEqual(candidate, settings)) {
		return false;
	}
	CopySdrEnhanceSettings(settings, candidate);
	return true;
}
