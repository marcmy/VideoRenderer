/*
 * (C) 2018-2026 see Authors.txt
 *
 * RIFE frame interpolation property dialog.
 */

#include "stdafx.h"
#include "resource.h"
#include "RifeSettingsDialog.h"

namespace {

void AddComboItem(HWND hwnd, int id, LPCWSTR label, LONG_PTR data)
{
	const LRESULT index = SendDlgItemMessageW(hwnd, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
	if (index != CB_ERR) {
		SendDlgItemMessageW(hwnd, id, CB_SETITEMDATA, index, data);
	}
}

void PopulateCombo(HWND hwnd, int id, std::initializer_list<std::pair<LPCWSTR, LONG_PTR>> items)
{
	SendDlgItemMessageW(hwnd, id, CB_RESETCONTENT, 0, 0);
	for (const auto& [label, data] : items) {
		AddComboItem(hwnd, id, label, data);
	}
}

LONG_PTR ComboValue(HWND hwnd, int id)
{
	const LRESULT index = SendDlgItemMessageW(hwnd, id, CB_GETCURSEL, 0, 0);
	if (index == CB_ERR) {
		return CB_ERR;
	}
	return SendDlgItemMessageW(hwnd, id, CB_GETITEMDATA, index, 0);
}

void SelectComboValue(HWND hwnd, int id, LONG_PTR value)
{
	const LRESULT count = SendDlgItemMessageW(hwnd, id, CB_GETCOUNT, 0, 0);
	if (count == CB_ERR) {
		return;
	}
	for (LRESULT i = 0; i < count; ++i) {
		if (SendDlgItemMessageW(hwnd, id, CB_GETITEMDATA, i, 0) == value) {
			SendDlgItemMessageW(hwnd, id, CB_SETCURSEL, i, 0);
			return;
		}
	}
}

void CopyRifeSettings(Settings_t& dst, const Settings_t& src)
{
	dst.iRifeMode = src.iRifeMode;
	dst.iRifeCustomFps = src.iRifeCustomFps;
	dst.iRifeGpuThreads = src.iRifeGpuThreads;
	dst.iRifeModel = src.iRifeModel;
	dst.iRifeGPU = src.iRifeGPU;
	dst.bRifePerformanceBoost = src.bRifePerformanceBoost;
	dst.iRifeSceneDetection = src.iRifeSceneDetection;
	dst.iRifeSceneProcessing = src.iRifeSceneProcessing;
	dst.iRifeDuplicateRemoval = src.iRifeDuplicateRemoval;
}

bool RifeSettingsEqual(const Settings_t& a, const Settings_t& b)
{
	return a.iRifeMode == b.iRifeMode
		&& a.iRifeCustomFps == b.iRifeCustomFps
		&& a.iRifeGpuThreads == b.iRifeGpuThreads
		&& a.iRifeModel == b.iRifeModel
		&& a.iRifeGPU == b.iRifeGPU
		&& a.bRifePerformanceBoost == b.bRifePerformanceBoost
		&& a.iRifeSceneDetection == b.iRifeSceneDetection
		&& a.iRifeSceneProcessing == b.iRifeSceneProcessing
		&& a.iRifeDuplicateRemoval == b.iRifeDuplicateRemoval;
}

void EnableControls(HWND hwnd)
{
	const LONG_PTR mode = ComboValue(hwnd, IDC_RIFE_MODE);
	const BOOL enabled = mode != RIFE_MODE_Disabled;
	const BOOL custom = enabled && mode == RIFE_MODE_Custom;

	EnableWindow(GetDlgItem(hwnd, IDC_STATIC_RIFE_CUSTOM_FPS), custom);
	EnableWindow(GetDlgItem(hwnd, IDC_RIFE_CUSTOM_FPS), custom);
	for (const int id : {
		IDC_RIFE_GPU_THREADS,
		IDC_RIFE_MODEL,
		IDC_RIFE_GPU,
		IDC_RIFE_PERFORMANCE_BOOST,
		IDC_RIFE_SCENE_DETECTION,
		IDC_RIFE_SCENE_PROCESSING,
		IDC_RIFE_DUPLICATE_REMOVAL,
	}) {
		EnableWindow(GetDlgItem(hwnd, id), enabled);
	}
}

void SetControls(HWND hwnd, const Settings_t& settings)
{
	SelectComboValue(hwnd, IDC_RIFE_MODE, settings.iRifeMode);
	SetDlgItemInt(hwnd, IDC_RIFE_CUSTOM_FPS, settings.iRifeCustomFps, FALSE);
	SelectComboValue(hwnd, IDC_RIFE_GPU_THREADS, settings.iRifeGpuThreads);
	SelectComboValue(hwnd, IDC_RIFE_MODEL, settings.iRifeModel);
	SelectComboValue(hwnd, IDC_RIFE_GPU, settings.iRifeGPU);
	CheckDlgButton(hwnd, IDC_RIFE_PERFORMANCE_BOOST,
		settings.bRifePerformanceBoost ? BST_CHECKED : BST_UNCHECKED);
	SelectComboValue(hwnd, IDC_RIFE_SCENE_DETECTION, settings.iRifeSceneDetection);
	SelectComboValue(hwnd, IDC_RIFE_SCENE_PROCESSING, settings.iRifeSceneProcessing);
	SelectComboValue(hwnd, IDC_RIFE_DUPLICATE_REMOVAL, settings.iRifeDuplicateRemoval);
	EnableControls(hwnd);
}

void InitializeDialog(HWND hwnd, const Settings_t& settings)
{
	PopulateCombo(hwnd, IDC_RIFE_MODE, {
		{L"Disabled", RIFE_MODE_Disabled},
		{L"To screen", RIFE_MODE_ToScreen},
		{L"Movie x2", RIFE_MODE_Movie2x},
		{L"Movie x2½", RIFE_MODE_Movie2_5x},
		{L"Movie x3", RIFE_MODE_Movie3x},
		{L"Movie x4", RIFE_MODE_Movie4x},
		{L"Movie x5", RIFE_MODE_Movie5x},
		{L"60 fps", RIFE_MODE_Fixed60},
		{L"72 fps", RIFE_MODE_Fixed72},
		{L"90 fps", RIFE_MODE_Fixed90},
		{L"120 fps", RIFE_MODE_Fixed120},
		{L"Custom", RIFE_MODE_Custom},
	});
	PopulateCombo(hwnd, IDC_RIFE_GPU_THREADS, {
		{L"1", 1}, {L"2", 2}, {L"3", 3},
	});
	PopulateCombo(hwnd, IDC_RIFE_MODEL, {
		{L"4.6", RIFE_MODEL_46},
	});
	PopulateCombo(hwnd, IDC_RIFE_GPU, {
		{L"Auto", RIFE_GPU_Auto},
		{L"GPU 0", 0}, {L"GPU 1", 1}, {L"GPU 2", 2}, {L"GPU 3", 3},
		{L"GPU 4", 4}, {L"GPU 5", 5}, {L"GPU 6", 6}, {L"GPU 7", 7},
	});
	PopulateCombo(hwnd, IDC_RIFE_SCENE_DETECTION, {
		{L"NVOF motion vectors", RIFE_SCENE_NVOF},
		{L"Image comparison", RIFE_SCENE_Image},
		{L"Disabled", RIFE_SCENE_Disabled},
	});
	PopulateCombo(hwnd, IDC_RIFE_SCENE_PROCESSING, {
		{L"Blend adjacent frames", RIFE_SCENE_PROCESS_Blend},
		{L"Repeat frame", RIFE_SCENE_PROCESS_Repeat},
	});
	PopulateCombo(hwnd, IDC_RIFE_DUPLICATE_REMOVAL, {
		{L"Do not remove", RIFE_DUPLICATES_Keep},
		{L"Remove every other frame", RIFE_DUPLICATES_RemoveEveryOther},
	});
	SetControls(hwnd, settings);
}

bool ReadControls(HWND hwnd, Settings_t& settings)
{
	settings.iRifeMode = static_cast<int>(ComboValue(hwnd, IDC_RIFE_MODE));
	settings.iRifeGpuThreads = static_cast<int>(ComboValue(hwnd, IDC_RIFE_GPU_THREADS));
	settings.iRifeModel = static_cast<int>(ComboValue(hwnd, IDC_RIFE_MODEL));
	settings.iRifeGPU = static_cast<int>(ComboValue(hwnd, IDC_RIFE_GPU));
	settings.bRifePerformanceBoost = IsDlgButtonChecked(hwnd, IDC_RIFE_PERFORMANCE_BOOST) == BST_CHECKED;
	settings.iRifeSceneDetection = static_cast<int>(ComboValue(hwnd, IDC_RIFE_SCENE_DETECTION));
	settings.iRifeSceneProcessing = static_cast<int>(ComboValue(hwnd, IDC_RIFE_SCENE_PROCESSING));
	settings.iRifeDuplicateRemoval = static_cast<int>(ComboValue(hwnd, IDC_RIFE_DUPLICATE_REMOVAL));

	BOOL valid = FALSE;
	const UINT customFps = GetDlgItemInt(hwnd, IDC_RIFE_CUSTOM_FPS, &valid, FALSE);
	if (settings.iRifeMode == RIFE_MODE_Custom
			&& (!valid || customFps < RIFE_CUSTOM_FPS_MIN || customFps > RIFE_CUSTOM_FPS_MAX)) {
		MessageBoxW(hwnd, L"Enter a custom RIFE output rate from 24 to 240 fps.",
			L"RIFE frame interpolation settings", MB_OK | MB_ICONERROR);
		return false;
	}
	if (valid) {
		settings.iRifeCustomFps = std::clamp(static_cast<int>(customFps),
			RIFE_CUSTOM_FPS_MIN, RIFE_CUSTOM_FPS_MAX);
	}
	return true;
}

INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* settings = reinterpret_cast<Settings_t*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	switch (message) {
	case WM_INITDIALOG:
		settings = reinterpret_cast<Settings_t*>(lParam);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(settings));
		InitializeDialog(hwnd, *settings);
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_RIFE_MODE:
			if (HIWORD(wParam) == CBN_SELCHANGE) {
				EnableControls(hwnd);
				return TRUE;
			}
			break;
		case IDC_BUTTON_RIFE_DEFAULTS:
			if (HIWORD(wParam) == BN_CLICKED) {
				Settings_t defaults;
				CopyRifeSettings(*settings, defaults);
				SetControls(hwnd, *settings);
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

bool ShowRifeFrameInterpolationSettings(HWND parent, HINSTANCE instance, Settings_t& settings)
{
	Settings_t candidate = settings;
	const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_FRAMEINTERPOLATION),
		parent, DialogProc, reinterpret_cast<LPARAM>(&candidate));
	if (result == -1) {
		MessageBoxW(parent, L"The RIFE frame interpolation settings window could not be opened.",
			L"MPC Video Renderer", MB_OK | MB_ICONERROR);
		return false;
	}
	if (result != IDOK || RifeSettingsEqual(candidate, settings)) {
		return false;
	}
	CopyRifeSettings(settings, candidate);
	return true;
}
