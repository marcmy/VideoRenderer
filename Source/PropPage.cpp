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

#include "stdafx.h"
#include "resource.h"
#include "Helper.h"
#include "DisplayConfig.h"
#include "PropPage.h"

void SetCursor(HWND hWnd, LPCWSTR lpCursorName)
{
	SetClassLongPtrW(hWnd, GCLP_HCURSOR, (LONG_PTR)::LoadCursorW(nullptr, lpCursorName));
}

void SetCursor(HWND hWnd, UINT nID, LPCWSTR lpCursorName)
{
	SetCursor(::GetDlgItem(hWnd, nID), lpCursorName);
}

inline void ComboBox_AddStringData(HWND hWnd, int nIDComboBox, LPCWSTR str, LONG_PTR data)
{
	LRESULT lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_ADDSTRING, 0, (LPARAM)str);
	if (lValue != CB_ERR) {
		SendDlgItemMessageW(hWnd, nIDComboBox, CB_SETITEMDATA, lValue, data);
	}
}

inline LONG_PTR ComboBox_GetCurItemData(HWND hWnd, int nIDComboBox)
{
	LRESULT lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETCURSEL, 0, 0);
	if (lValue != CB_ERR) {
		lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETITEMDATA, lValue, 0);
	}
	return lValue;
}

void ComboBox_SelectByItemData(HWND hWnd, int nIDComboBox, LONG_PTR data)
{
	LRESULT lCount = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETCOUNT, 0, 0);
	if (lCount != CB_ERR) {
		for (int idx = 0; idx < lCount; idx++) {
			const LRESULT lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETITEMDATA, idx, 0);
			if (data == lValue) {
				SendDlgItemMessageW(hWnd, nIDComboBox, CB_SETCURSEL, idx, 0);
				break;
			}
		}
	}
}


namespace {


void CopyFrameInterpolationSettings(Settings_t& dst, const Settings_t& src)
{
	dst.iFrameInterpolationMode = src.iFrameInterpolationMode;
	dst.iFrameInterpolationSourceLimit = src.iFrameInterpolationSourceLimit;
	dst.iFrameInterpolationMaxOutput = src.iFrameInterpolationMaxOutput;
	dst.iFrameInterpolationGPU = src.iFrameInterpolationGPU;
	dst.bFrameInterpolationFallback = src.bFrameInterpolationFallback;
}

bool FrameInterpolationSettingsEqual(const Settings_t& a, const Settings_t& b)
{
	return a.iFrameInterpolationMode == b.iFrameInterpolationMode
		&& a.iFrameInterpolationSourceLimit == b.iFrameInterpolationSourceLimit
		&& a.iFrameInterpolationMaxOutput == b.iFrameInterpolationMaxOutput
		&& a.iFrameInterpolationGPU == b.iFrameInterpolationGPU
		&& a.bFrameInterpolationFallback == b.bFrameInterpolationFallback;
}

void EnableFrameInterpolationDialogControls(HWND hwnd)
{
	const bool enabled = ComboBox_GetCurItemData(hwnd, IDC_FRUC_MODE) != FRUC_MODE_Disabled;
	for (const int id : {IDC_FRUC_SOURCE_LIMIT, IDC_FRUC_MAX_OUTPUT, IDC_FRUC_GPU, IDC_FRUC_FALLBACK}) {
		EnableWindow(GetDlgItem(hwnd, id), enabled);
	}
}

void SetFrameInterpolationDialogControls(HWND hwnd, const Settings_t& settings)
{
	ComboBox_SelectByItemData(hwnd, IDC_FRUC_MODE, settings.iFrameInterpolationMode);
	ComboBox_SelectByItemData(hwnd, IDC_FRUC_SOURCE_LIMIT, settings.iFrameInterpolationSourceLimit);
	ComboBox_SelectByItemData(hwnd, IDC_FRUC_MAX_OUTPUT, settings.iFrameInterpolationMaxOutput);
	ComboBox_SelectByItemData(hwnd, IDC_FRUC_GPU, settings.iFrameInterpolationGPU);
	CheckDlgButton(hwnd, IDC_FRUC_FALLBACK, settings.bFrameInterpolationFallback ? BST_CHECKED : BST_UNCHECKED);
	EnableFrameInterpolationDialogControls(hwnd);
}

void InitializeFrameInterpolationDialog(HWND hwnd, const Settings_t& settings)
{
	PopulateMaxineCombo(hwnd, IDC_FRUC_MODE, {{L"Disabled", FRUC_MODE_Disabled}, {L"Double source frame rate", FRUC_MODE_Double}});
	PopulateMaxineCombo(hwnd, IDC_FRUC_SOURCE_LIMIT, {{L"720p or lower", FRUC_SOURCE_LIMIT_720p}, {L"1080p or lower", FRUC_SOURCE_LIMIT_1080p}, {L"1440p or lower", FRUC_SOURCE_LIMIT_1440p}, {L"2160p or lower", FRUC_SOURCE_LIMIT_2160p}});
	PopulateMaxineCombo(hwnd, IDC_FRUC_MAX_OUTPUT, {{L"60 fps", FRUC_MAX_OUTPUT_60}, {L"120 fps", FRUC_MAX_OUTPUT_120}, {L"240 fps", FRUC_MAX_OUTPUT_240}});
	PopulateMaxineCombo(hwnd, IDC_FRUC_GPU, {{L"Auto", FRUC_GPU_Auto}, {L"GPU 0", 0}, {L"GPU 1", 1}, {L"GPU 2", 2}, {L"GPU 3", 3}, {L"GPU 4", 4}, {L"GPU 5", 5}, {L"GPU 6", 6}, {L"GPU 7", 7}});
	SetFrameInterpolationDialogControls(hwnd, settings);
}

void ReadFrameInterpolationDialog(HWND hwnd, Settings_t& settings)
{
	settings.iFrameInterpolationMode = static_cast<int>(ComboBox_GetCurItemData(hwnd, IDC_FRUC_MODE));
	settings.iFrameInterpolationSourceLimit = static_cast<int>(ComboBox_GetCurItemData(hwnd, IDC_FRUC_SOURCE_LIMIT));
	settings.iFrameInterpolationMaxOutput = static_cast<int>(ComboBox_GetCurItemData(hwnd, IDC_FRUC_MAX_OUTPUT));
	settings.iFrameInterpolationGPU = static_cast<int>(ComboBox_GetCurItemData(hwnd, IDC_FRUC_GPU));
	settings.bFrameInterpolationFallback = IsDlgButtonChecked(hwnd, IDC_FRUC_FALLBACK) == BST_CHECKED;
}

INT_PTR CALLBACK FrameInterpolationSettingsDlgProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* settings = reinterpret_cast<Settings_t*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	switch (message) {
	case WM_INITDIALOG:
		settings = reinterpret_cast<Settings_t*>(lParam);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(settings));
		InitializeFrameInterpolationDialog(hwnd, *settings);
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_FRUC_MODE:
			if (HIWORD(wParam) == CBN_SELCHANGE) { EnableFrameInterpolationDialogControls(hwnd); return TRUE; }
			break;
		case IDC_BUTTON_FRUC_DEFAULTS:
			if (HIWORD(wParam) == BN_CLICKED) { Settings_t defaults; CopyFrameInterpolationSettings(*settings, defaults); SetFrameInterpolationDialogControls(hwnd, *settings); return TRUE; }
			break;
		case IDOK:
			ReadFrameInterpolationDialog(hwnd, *settings); EndDialog(hwnd, IDOK); return TRUE;
		case IDCANCEL:
			EndDialog(hwnd, IDCANCEL); return TRUE;
		}
		break;
	}
	return FALSE;
}

void CopyMaxineSettings(Settings_t& dst, const Settings_t& src)
{
	dst.iMaxineOperation = src.iMaxineOperation;
	dst.iMaxineSourceMode = src.iMaxineSourceMode;
	dst.iMaxineQuality = src.iMaxineQuality;
	dst.iMaxineScale = src.iMaxineScale;
	dst.iMaxineOversample = src.iMaxineOversample;
	dst.iMaxineSourceLimit = src.iMaxineSourceLimit;
	dst.iMaxineDenoise = src.iMaxineDenoise;
	dst.iMaxineDeblur = src.iMaxineDeblur;
	dst.iMaxinePipeline = src.iMaxinePipeline;
	dst.iMaxineGPU = src.iMaxineGPU;
	dst.iMaxineAutoBitrate = src.iMaxineAutoBitrate;
}

bool MaxineSettingsEqual(const Settings_t& a, const Settings_t& b)
{
	return a.iMaxineOperation == b.iMaxineOperation
		&& a.iMaxineSourceMode == b.iMaxineSourceMode
		&& a.iMaxineQuality == b.iMaxineQuality
		&& a.iMaxineScale == b.iMaxineScale
		&& a.iMaxineOversample == b.iMaxineOversample
		&& a.iMaxineSourceLimit == b.iMaxineSourceLimit
		&& a.iMaxineDenoise == b.iMaxineDenoise
		&& a.iMaxineDeblur == b.iMaxineDeblur
		&& a.iMaxinePipeline == b.iMaxinePipeline
		&& a.iMaxineGPU == b.iMaxineGPU
		&& a.iMaxineAutoBitrate == b.iMaxineAutoBitrate;
}

void PopulateMaxineCombo(HWND hwnd, int id, std::initializer_list<std::pair<LPCWSTR, LONG_PTR>> items)
{
	SendDlgItemMessageW(hwnd, id, CB_RESETCONTENT, 0, 0);
	for (const auto& [label, value] : items) {
		ComboBox_AddStringData(hwnd, id, label, value);
	}
}

void EnableMaxineDialogControls(HWND hwnd)
{
	const LONG_PTR operation = ComboBox_GetCurItemData(hwnd, IDC_MAXINE_OPERATION);
	const LONG_PTR sourceMode = ComboBox_GetCurItemData(hwnd, IDC_MAXINE_SOURCE_MODE);
	const LONG_PTR scale = ComboBox_GetCurItemData(hwnd, IDC_MAXINE_SCALE);
	const bool enabled = operation != MAXINE_OPERATION_Disabled;
	const bool upscale = operation == MAXINE_OPERATION_Upscale;
	const bool denoiseOnly = operation == MAXINE_OPERATION_Denoise;
	const bool deblurOnly = operation == MAXINE_OPERATION_Deblur;

	for (const int id : {IDC_MAXINE_SOURCE_MODE, IDC_STATIC_MAXINE_QUALITY, IDC_MAXINE_QUALITY,
		IDC_STATIC_MAXINE_SCALE, IDC_MAXINE_SCALE, IDC_STATIC_MAXINE_PIPELINE, IDC_MAXINE_PIPELINE}) {
		EnableWindow(GetDlgItem(hwnd, id), enabled && upscale);
	}
	EnableWindow(GetDlgItem(hwnd, IDC_MAXINE_QUALITY), enabled && upscale && sourceMode != MAXINE_SOURCE_Bicubic);
	EnableWindow(GetDlgItem(hwnd, IDC_STATIC_MAXINE_QUALITY), enabled && upscale && sourceMode != MAXINE_SOURCE_Bicubic);
	EnableWindow(GetDlgItem(hwnd, IDC_STATIC_MAXINE_OVERSAMPLE), enabled && upscale && scale == MAXINE_SCALE_MatchOutput);
	EnableWindow(GetDlgItem(hwnd, IDC_MAXINE_OVERSAMPLE), enabled && upscale && scale == MAXINE_SCALE_MatchOutput);
	EnableWindow(GetDlgItem(hwnd, IDC_MAXINE_SOURCE_LIMIT), enabled);
	EnableWindow(GetDlgItem(hwnd, IDC_MAXINE_DENOISE), enabled && (upscale || denoiseOnly));
	EnableWindow(GetDlgItem(hwnd, IDC_MAXINE_DEBLUR), enabled && (upscale || deblurOnly));
	EnableWindow(GetDlgItem(hwnd, IDC_MAXINE_GPU), enabled);
	EnableWindow(GetDlgItem(hwnd, IDC_STATIC_MAXINE_AUTO_BITRATE), enabled && upscale && sourceMode == MAXINE_SOURCE_Auto);
	EnableWindow(GetDlgItem(hwnd, IDC_MAXINE_AUTO_BITRATE), enabled && upscale && sourceMode == MAXINE_SOURCE_Auto);
}

void SetMaxineDialogControls(HWND hwnd, const Settings_t& settings)
{
	ComboBox_SelectByItemData(hwnd, IDC_MAXINE_OPERATION, settings.iMaxineOperation);
	ComboBox_SelectByItemData(hwnd, IDC_MAXINE_SOURCE_MODE, settings.iMaxineSourceMode);
	ComboBox_SelectByItemData(hwnd, IDC_MAXINE_QUALITY, settings.iMaxineQuality);
	ComboBox_SelectByItemData(hwnd, IDC_MAXINE_SCALE, settings.iMaxineScale);
	ComboBox_SelectByItemData(hwnd, IDC_MAXINE_OVERSAMPLE, settings.iMaxineOversample);
	ComboBox_SelectByItemData(hwnd, IDC_MAXINE_SOURCE_LIMIT, settings.iMaxineSourceLimit);
	ComboBox_SelectByItemData(hwnd, IDC_MAXINE_DENOISE, settings.iMaxineDenoise);
	ComboBox_SelectByItemData(hwnd, IDC_MAXINE_DEBLUR, settings.iMaxineDeblur);
	ComboBox_SelectByItemData(hwnd, IDC_MAXINE_PIPELINE, settings.iMaxinePipeline);
	ComboBox_SelectByItemData(hwnd, IDC_MAXINE_GPU, settings.iMaxineGPU);
	SetDlgItemInt(hwnd, IDC_MAXINE_AUTO_BITRATE, settings.iMaxineAutoBitrate, FALSE);
	EnableMaxineDialogControls(hwnd);
}

void InitializeMaxineDialog(HWND hwnd, const Settings_t& settings)
{
	PopulateMaxineCombo(hwnd, IDC_MAXINE_OPERATION, {
		{L"Disabled", MAXINE_OPERATION_Disabled},
		{L"Upscale, with optional cleanup passes", MAXINE_OPERATION_Upscale},
		{L"Denoise only, keep source resolution", MAXINE_OPERATION_Denoise},
		{L"Deblur only, keep source resolution", MAXINE_OPERATION_Deblur},
	});
	PopulateMaxineCombo(hwnd, IDC_MAXINE_SOURCE_MODE, {
		{L"Automatic from reported source bitrate", MAXINE_SOURCE_Auto},
		{L"Standard, compressed video", MAXINE_SOURCE_Standard},
		{L"High bitrate / clean source", MAXINE_SOURCE_HighBitrate},
		{L"Bicubic baseline, no AI enhancement", MAXINE_SOURCE_Bicubic},
	});
	PopulateMaxineCombo(hwnd, IDC_MAXINE_QUALITY, {
		{L"Low", MAXINE_QUALITY_Low},
		{L"Medium", MAXINE_QUALITY_Medium},
		{L"High", MAXINE_QUALITY_High},
		{L"Ultra", MAXINE_QUALITY_Ultra},
	});
	PopulateMaxineCombo(hwnd, IDC_MAXINE_SCALE, {
		{L"Match player output", MAXINE_SCALE_MatchOutput},
		{L"1.33x (4/3)", MAXINE_SCALE_4_3X},
		{L"1.5x", MAXINE_SCALE_1_5X},
		{L"2x", MAXINE_SCALE_2X},
		{L"3x", MAXINE_SCALE_3X},
		{L"4x", MAXINE_SCALE_4X},
	});
	PopulateMaxineCombo(hwnd, IDC_MAXINE_OVERSAMPLE, {
		{L"Off", MAXINE_OVERSAMPLE_Off},
		{L"1.33x (4/3)", MAXINE_OVERSAMPLE_4_3X},
		{L"1.5x", MAXINE_OVERSAMPLE_1_5X},
		{L"2x", MAXINE_OVERSAMPLE_2X},
	});
	PopulateMaxineCombo(hwnd, IDC_MAXINE_SOURCE_LIMIT, {
		{L"Disabled", SUPERRES_Disable},
		{L"SD or lower", SUPERRES_SD},
		{L"720p or lower", SUPERRES_720p},
		{L"1080p or lower", SUPERRES_1080p},
		{L"1440p or lower", SUPERRES_1440p},
	});
	for (const int id : {IDC_MAXINE_DENOISE, IDC_MAXINE_DEBLUR}) {
		PopulateMaxineCombo(hwnd, id, {
			{L"Off", MAXINE_FILTER_Off},
			{L"Low", MAXINE_FILTER_Low},
			{L"Medium", MAXINE_FILTER_Medium},
			{L"High", MAXINE_FILTER_High},
			{L"Ultra", MAXINE_FILTER_Ultra},
		});
	}
	PopulateMaxineCombo(hwnd, IDC_MAXINE_PIPELINE, {
		{L"Upscale -> Denoise -> Deblur", MAXINE_PIPELINE_UpscaleDenoiseDeblur},
		{L"Upscale -> Deblur -> Denoise", MAXINE_PIPELINE_UpscaleDeblurDenoise},
		{L"Denoise -> Deblur -> Upscale", MAXINE_PIPELINE_DenoiseDeblurUpscale},
		{L"Deblur -> Denoise -> Upscale", MAXINE_PIPELINE_DeblurDenoiseUpscale},
		{L"Denoise -> Upscale -> Deblur", MAXINE_PIPELINE_DenoiseUpscaleDeblur},
		{L"Deblur -> Upscale -> Denoise", MAXINE_PIPELINE_DeblurUpscaleDenoise},
	});
	PopulateMaxineCombo(hwnd, IDC_MAXINE_GPU, {
		{L"Auto", MAXINE_GPU_Auto},
		{L"GPU 0", 0}, {L"GPU 1", 1}, {L"GPU 2", 2}, {L"GPU 3", 3},
		{L"GPU 4", 4}, {L"GPU 5", 5}, {L"GPU 6", 6}, {L"GPU 7", 7},
	});
	SetMaxineDialogControls(hwnd, settings);
}

bool ReadMaxineDialog(HWND hwnd, Settings_t& settings)
{
	auto ReadCombo = [hwnd](int id) -> int {
		return static_cast<int>(ComboBox_GetCurItemData(hwnd, id));
	};

	settings.iMaxineOperation = ReadCombo(IDC_MAXINE_OPERATION);
	settings.iMaxineSourceMode = ReadCombo(IDC_MAXINE_SOURCE_MODE);
	settings.iMaxineQuality = ReadCombo(IDC_MAXINE_QUALITY);
	settings.iMaxineScale = ReadCombo(IDC_MAXINE_SCALE);
	settings.iMaxineOversample = ReadCombo(IDC_MAXINE_OVERSAMPLE);
	settings.iMaxineSourceLimit = ReadCombo(IDC_MAXINE_SOURCE_LIMIT);
	settings.iMaxineDenoise = ReadCombo(IDC_MAXINE_DENOISE);
	settings.iMaxineDeblur = ReadCombo(IDC_MAXINE_DEBLUR);
	settings.iMaxinePipeline = ReadCombo(IDC_MAXINE_PIPELINE);
	settings.iMaxineGPU = ReadCombo(IDC_MAXINE_GPU);

	BOOL valid = FALSE;
	const UINT bitrate = GetDlgItemInt(hwnd, IDC_MAXINE_AUTO_BITRATE, &valid, FALSE);
	if (!valid || bitrate < MAXINE_AUTO_BITRATE_MIN || bitrate > MAXINE_AUTO_BITRATE_MAX) {
		MessageBoxW(hwnd, L"Enter an automatic high-bitrate threshold from 1 to 1000 Mbps.",
			L"NVIDIA Maxine settings", MB_OK | MB_ICONERROR);
		return false;
	}
	settings.iMaxineAutoBitrate = static_cast<int>(bitrate);

	if (settings.iMaxineOperation == MAXINE_OPERATION_Denoise
			&& settings.iMaxineDenoise == MAXINE_FILTER_Off) {
		MessageBoxW(hwnd, L"Choose a denoise strength for Denoise-only operation.",
			L"NVIDIA Maxine settings", MB_OK | MB_ICONERROR);
		return false;
	}
	if (settings.iMaxineOperation == MAXINE_OPERATION_Deblur
			&& settings.iMaxineDeblur == MAXINE_FILTER_Off) {
		MessageBoxW(hwnd, L"Choose a deblur strength for Deblur-only operation.",
			L"NVIDIA Maxine settings", MB_OK | MB_ICONERROR);
		return false;
	}
	return true;
}

INT_PTR CALLBACK MaxineSettingsDlgProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* settings = reinterpret_cast<Settings_t*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	switch (message) {
	case WM_INITDIALOG:
		settings = reinterpret_cast<Settings_t*>(lParam);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(settings));
		InitializeMaxineDialog(hwnd, *settings);
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_MAXINE_OPERATION:
		case IDC_MAXINE_SOURCE_MODE:
		case IDC_MAXINE_SCALE:
			if (HIWORD(wParam) == CBN_SELCHANGE) {
				EnableMaxineDialogControls(hwnd);
				return TRUE;
			}
			break;
		case IDC_BUTTON_MAXINE_DEFAULTS:
			if (HIWORD(wParam) == BN_CLICKED) {
				Settings_t defaults;
				CopyMaxineSettings(*settings, defaults);
				SetMaxineDialogControls(hwnd, *settings);
				return TRUE;
			}
			break;
		case IDOK:
			if (ReadMaxineDialog(hwnd, *settings)) {
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


// CVRMainPPage

// https://msdn.microsoft.com/ru-ru/library/windows/desktop/dd375010(v=vs.85).aspx

CVRMainPPage::CVRMainPPage(LPUNKNOWN lpunk, HRESULT* phr) :
	CBasePropertyPage(L"MainProp", lpunk, IDD_MAINPROPPAGE, IDS_MAINPROPPAGE_TITLE)
{
	DLog(L"CVRMainPPage()");
}

CVRMainPPage::~CVRMainPPage()
{
	DLog(L"~CVRMainPPage()");
}

void CVRMainPPage::SetControls()
{
	CheckDlgButton(IDC_CHECK1, m_SetsPP.bUseD3D11             ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK2, m_SetsPP.bShowStats            ? BST_CHECKED : BST_UNCHECKED);

	ComboBox_SelectByItemData(m_hWnd, IDC_COMBO1, m_SetsPP.iTexFormat);

	CheckDlgButton(IDC_CHECK7, m_SetsPP.VPFmts.bNV12          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK8, m_SetsPP.VPFmts.bP01x          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK9, m_SetsPP.VPFmts.bYUY2          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK4, m_SetsPP.VPFmts.bOther         ? BST_CHECKED : BST_UNCHECKED);
	SendDlgItemMessageW(IDC_COMBO9, CB_SETCURSEL, m_SetsPP.iVPDeinterlacing, 0);
	CheckDlgButton(IDC_CHECK3, m_SetsPP.bDeintDouble          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK5, m_SetsPP.bVPScaling            ? BST_CHECKED : BST_UNCHECKED);
	SendDlgItemMessageW(IDC_COMBO8, CB_SETCURSEL, m_SetsPP.iVPSuperRes, 0);
	CheckDlgButton(IDC_CHECK19, m_SetsPP.bVPRTXVideoHDR       ? BST_CHECKED : BST_UNCHECKED);

	if (m_SetsPP.bHdrPassthrough) {
		ComboBox_SelectByItemData(m_hWnd, IDC_COMBO10, 0);
	} else if (m_SetsPP.bHdrLocalToneMapping) {
		ComboBox_SelectByItemData(m_hWnd, IDC_COMBO10, m_SetsPP.iHdrLocalToneMappingType);
	} else {
		ComboBox_SelectByItemData(m_hWnd, IDC_COMBO10, -1);
	}

	CheckDlgButton(IDC_CHECK18, m_SetsPP.bHdrPreferDoVi       ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK14, m_SetsPP.bConvertToSdr        ? BST_CHECKED : BST_UNCHECKED);

	SendDlgItemMessageW(IDC_COMBO7, CB_SETCURSEL, m_SetsPP.iHdrToggleDisplay, 0);
	SendDlgItemMessageW(IDC_SLIDER1, TBM_SETPOS, 1, m_SetsPP.iHdrOsdBrightness);

	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETPOS, 1, m_SetsPP.iSDRDisplayNits / SDR_NITS_STEP);
	GetDlgItem(IDC_EDIT1).SetWindowTextW(std::to_wstring(m_SetsPP.iSDRDisplayNits).c_str());

	CheckDlgButton(IDC_CHECK6, m_SetsPP.bInterpolateAt50pct   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK10, m_SetsPP.bUseDither           ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK17, m_SetsPP.bDeintBlend          ? BST_CHECKED : BST_UNCHECKED);

	CheckDlgButton(IDC_CHECK11, m_SetsPP.bExclusiveFS         ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK15, m_SetsPP.bVBlankBeforePresent ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK13, m_SetsPP.bAdjustPresentTime   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK16, m_SetsPP.bReinitByDisplay     ? BST_CHECKED : BST_UNCHECKED);

	SendDlgItemMessageW(IDC_COMBO6, CB_SETCURSEL, m_SetsPP.iResizeStats, 0);

	SendDlgItemMessageW(IDC_COMBO5, CB_SETCURSEL, m_SetsPP.iChromaScaling, 0);
	SendDlgItemMessageW(IDC_COMBO2, CB_SETCURSEL, m_SetsPP.iUpscaling, 0);
	SendDlgItemMessageW(IDC_COMBO3, CB_SETCURSEL, m_SetsPP.iDownscaling, 0);
	SendDlgItemMessageW(IDC_COMBO4, CB_SETCURSEL, m_SetsPP.iSwapEffect, 0);

	m_SetsPP.iHdrDisplayMaxNits = discard<int>(m_SetsPP.iHdrDisplayMaxNits, HDR_NITS_DEF, HDR_NITS_MIN, HDR_NITS_MAX);
	SetDlgItemTextW(IDC_EDIT_DISPLAYMAX, std::to_wstring(m_SetsPP.iHdrDisplayMaxNits).c_str());
}

void CVRMainPPage::EnableControls()
{
	if (!IsWindows8OrGreater()) { // Windows 7
		const BOOL bEnable = !m_SetsPP.bUseD3D11;
		GetDlgItem(IDC_STATIC1).EnableWindow(bEnable); // not working for GROUPBOX
		GetDlgItem(IDC_STATIC2).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK7).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK8).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK9).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK4).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK3).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK5).EnableWindow(bEnable);
		GetDlgItem(IDC_STATIC3).EnableWindow(bEnable);
		GetDlgItem(IDC_COMBO4).EnableWindow(bEnable);
	}
	else if (IsWindows10OrGreater()) {
		const BOOL bEnable = m_SetsPP.bUseD3D11;
		GetDlgItem(IDC_COMBO10).EnableWindow(bEnable);
		GetDlgItem(IDC_STATIC5).EnableWindow(bEnable);
		GetDlgItem(IDC_COMBO7).EnableWindow(bEnable);
		GetDlgItem(IDC_STATIC6).EnableWindow(bEnable);
		GetDlgItem(IDC_SLIDER1).EnableWindow(bEnable);
		const BOOL bEnableSuperRes = bEnable && m_SetsPP.bVPScaling;
		GetDlgItem(IDC_STATIC7).EnableWindow(bEnableSuperRes);
		GetDlgItem(IDC_COMBO8).EnableWindow(bEnableSuperRes);
#ifdef _WIN64
		GetDlgItem(IDC_BUTTON_MAXINE).EnableWindow(bEnable);
		GetDlgItem(IDC_BUTTON_FRAMEINTERPOLATION).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK19).EnableWindow(bEnable && m_SetsPP.bHdrPassthrough);
#endif
	}

	GetDlgItem(IDC_STATIC8).EnableWindow(m_SetsPP.bConvertToSdr);
	GetDlgItem(IDC_EDIT1).EnableWindow(m_SetsPP.bConvertToSdr);
	GetDlgItem(IDC_SLIDER2).EnableWindow(m_SetsPP.bConvertToSdr);
	GetDlgItem(IDC_EDIT_DISPLAYMAX).EnableWindow(m_SetsPP.bHdrLocalToneMapping);
}

bool CVRMainPPage::ShowMaxineSettings()
{
	Settings_t candidate = m_SetsPP;
	const INT_PTR result = DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_MAXINESETTINGS),
		m_hWnd, MaxineSettingsDlgProc, reinterpret_cast<LPARAM>(&candidate));
	if (result == -1) {
		MessageBoxW(L"The NVIDIA Maxine settings window could not be opened.", L"MPC Video Renderer",
			MB_OK | MB_ICONERROR);
		return false;
	}
	if (result != IDOK || MaxineSettingsEqual(candidate, m_SetsPP)) {
		return false;
	}
	CopyMaxineSettings(m_SetsPP, candidate);
	return true;
}


bool CVRMainPPage::ShowFrameInterpolationSettings()
{
	Settings_t candidate = m_SetsPP;
	const INT_PTR result = DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_FRAMEINTERPOLATION), m_hWnd, FrameInterpolationSettingsDlgProc, reinterpret_cast<LPARAM>(&candidate));
	if (result == -1) {
		MessageBoxW(L"The NVIDIA frame interpolation settings window could not be opened.", L"MPC Video Renderer", MB_OK | MB_ICONERROR);
		return false;
	}
	if (result != IDOK || FrameInterpolationSettingsEqual(candidate, m_SetsPP)) { return false; }
	CopyFrameInterpolationSettings(m_SetsPP, candidate);
	return true;
}

HRESULT CVRMainPPage::OnConnect(IUnknown *pUnk)
{
	if (pUnk == nullptr) return E_POINTER;

	m_pVideoRenderer = pUnk;
	if (!m_pVideoRenderer) {
		return E_NOINTERFACE;
	}

	return S_OK;
}

HRESULT CVRMainPPage::OnDisconnect()
{
	if (m_pVideoRenderer == nullptr) {
		return E_UNEXPECTED;
	}

	if (m_SetsPP.iSDRDisplayNits != m_oldSDRDisplayNits) {
		// OK or Apply buttons were not pressed. cancel the settings.
		m_pVideoRenderer->GetSettings(m_SetsPP);
		m_SetsPP.iSDRDisplayNits = m_oldSDRDisplayNits;
		m_pVideoRenderer->SetSettings(m_SetsPP);
	}

	m_pVideoRenderer.Release();

	return S_OK;
}

HRESULT CVRMainPPage::OnActivate()
{
	// set m_hWnd for CWindow
	m_hWnd = m_hwnd;

	m_pVideoRenderer->GetSettings(m_SetsPP);
	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;

	if (!IsWindows7SP1OrGreater()) {
		GetDlgItem(IDC_CHECK1).EnableWindow(FALSE);
		m_SetsPP.bUseD3D11 = false;
	}
	if (!IsWindows10OrGreater()) {
		GetDlgItem(IDC_COMBO10).EnableWindow(FALSE);
		GetDlgItem(IDC_STATIC5).EnableWindow(FALSE);
		GetDlgItem(IDC_COMBO7).EnableWindow(FALSE);
		GetDlgItem(IDC_STATIC6).EnableWindow(FALSE);
		GetDlgItem(IDC_SLIDER1).EnableWindow(FALSE);
		GetDlgItem(IDC_STATIC7).EnableWindow(FALSE);
		GetDlgItem(IDC_COMBO8).EnableWindow(FALSE);
		GetDlgItem(IDC_BUTTON_MAXINE).EnableWindow(FALSE);
		GetDlgItem(IDC_BUTTON_FRAMEINTERPOLATION).EnableWindow(FALSE);
		GetDlgItem(IDC_CHECK19).EnableWindow(FALSE);
	}

#ifndef _WIN64
	GetDlgItem(IDC_STATIC7).EnableWindow(FALSE);
	GetDlgItem(IDC_COMBO8).EnableWindow(FALSE);
	GetDlgItem(IDC_BUTTON_MAXINE).EnableWindow(FALSE);
	GetDlgItem(IDC_BUTTON_FRAMEINTERPOLATION).EnableWindow(FALSE);
	GetDlgItem(IDC_CHECK19).EnableWindow(FALSE);
#endif

	EnableControls();

	SendDlgItemMessageW(IDC_COMBO6, CB_ADDSTRING, 0, (LPARAM)L"Fixed font size");
	SendDlgItemMessageW(IDC_COMBO6, CB_ADDSTRING, 0, (LPARAM)L"Increase font by window");

	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"Auto 8/10-bit Integer",  0);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"8-bit Integer",          8);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"10-bit Integer",        10);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"16-bit Floating Point", 16);

	SendDlgItemMessageW(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"Disable");
	SendDlgItemMessageW(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"Enable");
	SendDlgItemMessageW(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"HACK future frames");

	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"Disable");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for SD");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 720p");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 1080p");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 1440p");

	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Do not change");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on (fullscreen)");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on/off (fullscreen)");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on/off");

	SendDlgItemMessageW(IDC_COMBO5, CB_ADDSTRING, 0, (LPARAM)L"Nearest-neighbor");
	SendDlgItemMessageW(IDC_COMBO5, CB_ADDSTRING, 0, (LPARAM)L"Bilinear");
	SendDlgItemMessageW(IDC_COMBO5, CB_ADDSTRING, 0, (LPARAM)L"Catmull-Rom");

	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Nearest-neighbor");
	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Mitchell-Netravali");
	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Catmull-Rom");
	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Lanczos2");
	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Lanczos3");
	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Jinc2m");

	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Box");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bilinear");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Hamming");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bicubic");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bicubic sharp");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Lanczos");

	SendDlgItemMessageW(IDC_COMBO4, CB_ADDSTRING, 0, (LPARAM)L"Discard");
	SendDlgItemMessageW(IDC_COMBO4, CB_ADDSTRING, 0, (LPARAM)L"Flip");

	SendDlgItemMessageW(IDC_SLIDER1, TBM_SETRANGE, 0, MAKELONG(0, 2));
	SendDlgItemMessageW(IDC_SLIDER1, TBM_SETTIC, 0, 1);

	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETRANGE, 0, MAKELONG(SDR_NITS_MIN / SDR_NITS_STEP, SDR_NITS_MAX / SDR_NITS_STEP));
	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETTIC, 0, SDR_NITS_DEF / SDR_NITS_STEP);
	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETLINESIZE, 0, 1); // arrow keys
	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETPAGESIZE, 0, 5); // clicks on trackbar's channel

	SetDlgItemTextW(IDC_EDIT2, GetNameAndVersion());

	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Ignore", -1);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Passthrough", 0);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"ACES", 1);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Reinhard", 2);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Hable", 3);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Mobius", 4);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"BT2390/ST 2094-10", 5);

	SetControls();

	SetCursor(m_hWnd, IDC_ARROW);
	SetCursor(m_hWnd, IDC_COMBO1, IDC_HAND);

	AddHint(IDC_CHECK5,
		L"It works fast, but it's not always good.\n"
		"Disable it if you want to use shaders for resizing.");
	AddHint(IDC_COMBO8,
		L"Sets the maximum source resolution for hardware video-processor\n"
		"Super Resolution. Maxine has its own source limit.");
	AddHint(IDC_BUTTON_MAXINE,
		L"Opens the dedicated NVIDIA Maxine settings window.\n"
		"Maxine is available in the 64-bit Direct3D 11 renderer.");
	AddHint(IDC_CHECK19,
		L"Available for Direct3D 11.\n"
		"Requires hardware and driver support:\n"
		"- Nvidia RTX (x64 only)");
	AddHint(IDC_COMBO5,
		L"Used for YUV 4:2:0/4:2:2 input formats\n"
		"when the DVXA2/D3D11 Video Processor is not active.");
	AddHint(IDC_COMBO2,
		L"Used to increase image size when the\n"
		"DVXA2/D3D11 Video Processor is not used for resizing.");
	AddHint(IDC_COMBO3,
		L"Used to reduce image size when the\n"
		"DVXA2/D3D11 Video Processor is not used for resizing.");
	AddHint(IDC_COMBO4,
		L"'Flip' is more efficient, but 'Discard' may work\n"
		"more correctly in some rare situations.");

	return S_OK;
}

INT_PTR CVRMainPPage::OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_COMMAND) {
		LRESULT lValue;
		const int nID = LOWORD(wParam);
		int action = HIWORD(wParam);

		if (action == BN_CLICKED) {
			if (nID == IDC_CHECK1) {
				m_SetsPP.bUseD3D11 = IsDlgButtonChecked(IDC_CHECK1) == BST_CHECKED;
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK2) {
				m_SetsPP.bShowStats = IsDlgButtonChecked(IDC_CHECK2) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK3) {
				m_SetsPP.bDeintDouble = IsDlgButtonChecked(IDC_CHECK3) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK5) {
				m_SetsPP.bVPScaling = IsDlgButtonChecked(IDC_CHECK5) == BST_CHECKED;
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK6) {
				m_SetsPP.bInterpolateAt50pct = IsDlgButtonChecked(IDC_CHECK6) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK7) {
				m_SetsPP.VPFmts.bNV12 = IsDlgButtonChecked(IDC_CHECK7) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK8) {
				m_SetsPP.VPFmts.bP01x = IsDlgButtonChecked(IDC_CHECK8) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK9) {
				m_SetsPP.VPFmts.bYUY2 = IsDlgButtonChecked(IDC_CHECK9) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK4) {
				m_SetsPP.VPFmts.bOther = IsDlgButtonChecked(IDC_CHECK4) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK10) {
				m_SetsPP.bUseDither = IsDlgButtonChecked(IDC_CHECK10) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK17) {
				m_SetsPP.bDeintBlend = IsDlgButtonChecked(IDC_CHECK17) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK11) {
				m_SetsPP.bExclusiveFS = IsDlgButtonChecked(IDC_CHECK11) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK15) {
				m_SetsPP.bVBlankBeforePresent = IsDlgButtonChecked(IDC_CHECK15) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK13) {
				m_SetsPP.bAdjustPresentTime = IsDlgButtonChecked(IDC_CHECK13) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK16) {
				m_SetsPP.bReinitByDisplay = IsDlgButtonChecked(IDC_CHECK16) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK18) {
				m_SetsPP.bHdrPreferDoVi = IsDlgButtonChecked(IDC_CHECK18) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK14) {
				m_SetsPP.bConvertToSdr = IsDlgButtonChecked(IDC_CHECK14) == BST_CHECKED;
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK19) {
				m_SetsPP.bVPRTXVideoHDR = IsDlgButtonChecked(IDC_CHECK19) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}

			if (nID == IDC_BUTTON_MAXINE) {
				if (ShowMaxineSettings()) { SetDirty(); }
				return (LRESULT)1;
			}

			if (nID == IDC_BUTTON_FRAMEINTERPOLATION) {
				if (ShowFrameInterpolationSettings()) { SetDirty(); }
				return (LRESULT)1;
			}

			if (nID == IDC_BUTTON1) {
				m_SetsPP.SetDefault();
				SetControls();
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
		}

		if (action == CBN_SELCHANGE) {
			if (nID == IDC_COMBO6) {
				lValue = SendDlgItemMessageW(IDC_COMBO6, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iResizeStats) {
					m_SetsPP.iResizeStats = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO1) {
				lValue = ComboBox_GetCurItemData(m_hWnd, IDC_COMBO1);
				if (lValue != m_SetsPP.iTexFormat) {
					m_SetsPP.iTexFormat = lValue;
					SetDirty();
#ifdef _WIN64
					GetDlgItem(IDC_CHECK19).EnableWindow(m_SetsPP.bUseD3D11 && m_SetsPP.bHdrPassthrough && m_SetsPP.iTexFormat != TEXFMT_8INT);
#endif
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO9) {
				lValue = SendDlgItemMessageW(IDC_COMBO9, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iVPDeinterlacing) {
					m_SetsPP.iVPDeinterlacing = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO8) {
				lValue = SendDlgItemMessageW(IDC_COMBO8, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iVPSuperRes) {
					m_SetsPP.iVPSuperRes = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO7) {
				lValue = SendDlgItemMessageW(IDC_COMBO7, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iHdrToggleDisplay) {
					m_SetsPP.iHdrToggleDisplay = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO5) {
				lValue = SendDlgItemMessageW(IDC_COMBO5, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iChromaScaling) {
					m_SetsPP.iChromaScaling = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO2) {
				lValue = SendDlgItemMessageW(IDC_COMBO2, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iUpscaling) {
					m_SetsPP.iUpscaling = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO3) {
				lValue = SendDlgItemMessageW(IDC_COMBO3, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iDownscaling) {
					m_SetsPP.iDownscaling = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO4) {
				lValue = SendDlgItemMessageW(IDC_COMBO4, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iSwapEffect) {
					m_SetsPP.iSwapEffect = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO10) {
				lValue = SendDlgItemMessageW(IDC_COMBO10, CB_GETCURSEL, 0, 0);
				switch (lValue) {
					case 0:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = false;
						break;
					case 1:
						m_SetsPP.bHdrPassthrough = true;
						m_SetsPP.bHdrLocalToneMapping = false;
						break;
					case 2:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = true;
						m_SetsPP.iHdrLocalToneMappingType = 1;
						break;
					case 3:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = true;
						m_SetsPP.iHdrLocalToneMappingType = 2;
						break;
					case 4:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = true;
						m_SetsPP.iHdrLocalToneMappingType = 3;
						break;
					case 5:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = true;
						m_SetsPP.iHdrLocalToneMappingType = 4;
						break;
					case 6:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = true;
						m_SetsPP.iHdrLocalToneMappingType = 5;
						break;
					default:
						break;
				}
				SetDirty();
				EnableControls();
				return (LRESULT)1;
			}
		}
	}
	else if (uMsg == WM_HSCROLL) {
		if ((HWND)lParam == GetDlgItem(IDC_SLIDER1)) {
			LRESULT lValue = SendDlgItemMessageW(IDC_SLIDER1, TBM_GETPOS, 0, 0);
			if (lValue != m_SetsPP.iHdrOsdBrightness) {
				m_SetsPP.iHdrOsdBrightness = lValue;
				SetDirty();
			}
			return (LRESULT)1;
		}
		if ((HWND)lParam == GetDlgItem(IDC_SLIDER2)) {
			LRESULT lValue = SendDlgItemMessageW(IDC_SLIDER2, TBM_GETPOS, 0, 0);
			lValue *= SDR_NITS_STEP;
			if (lValue != m_SetsPP.iSDRDisplayNits) {
				m_SetsPP.iSDRDisplayNits = lValue;
				GetDlgItem(IDC_EDIT1).SetWindowTextW(std::to_wstring(m_SetsPP.iSDRDisplayNits).c_str());
				SetDirty();
				{
					// apply only SDRDisplayNits
					Settings_t sets;
					m_pVideoRenderer->GetSettings(sets);
					sets.iSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
					m_pVideoRenderer->SetSettings(sets);
				}
			}
			return (LRESULT)1;
		}
	}

	// Let the parent class handle the message.
	return CBasePropertyPage::OnReceiveMessage(hwnd, uMsg, wParam, lParam);
}

HRESULT CVRMainPPage::OnApplyChanges()
{
	wchar_t data[32] = {};
	GetDlgItemTextW(IDC_EDIT_DISPLAYMAX, data, 32);
	int displayMaxNits;
	try {
		displayMaxNits = std::stoi(data);
	} catch (const std::exception&) {
		MessageBoxW(L"Invalid HDR Brightness. Please enter a valid number from 100 to 10000.", L"Error", MB_OK | MB_ICONERROR);
		return S_FALSE;
	}

	if (displayMaxNits <= HDR_NITS_MIN || displayMaxNits > HDR_NITS_MAX) {
		MessageBoxW(L"Invalid HDR Brightness. Please enter a valid number from 100 to 10000.", L"Error", MB_OK | MB_ICONERROR);
		return S_FALSE;
	}
	// if not error then set to m_setsPP
	m_SetsPP.iHdrDisplayMaxNits = displayMaxNits;

	m_pVideoRenderer->SetSettings(m_SetsPP);
	m_pVideoRenderer->SaveSettings();

	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;

	return S_OK;
}

HWND CVRMainPPage::CreateHintWindow(HWND parent, int timePop, int timeInit, int timeReshow)
{
	HWND hhint = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
		WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr, nullptr, nullptr);

	SetWindowPos(hhint, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAKELONG(timePop, 0));
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_INITIAL, MAKELONG(timeInit, 0));
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_RESHOW, MAKELONG(timeReshow, 0));
	SendMessageW(hhint, TTM_SETMAXTIPWIDTH, 0, 470);
	return hhint;
}

void CVRMainPPage::AddHint(int id, const LPCWSTR text)
{
	if (!m_hHint) {
		m_hHint = CreateHintWindow(m_Dlg, 15000);
	}
	TOOLINFOW ti;
	ti.cbSize = sizeof(TOOLINFOW);
	ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
	ti.hwnd = m_Dlg;
	ti.uId = (LPARAM)GetDlgItem(id).m_hWnd;
	ti.lpszText = const_cast<LPWSTR>(text);
	SendMessageW(m_hHint, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// CVRInfoPPage

CVRInfoPPage::CVRInfoPPage(LPUNKNOWN lpunk, HRESULT* phr) :
	CBasePropertyPage(L"InfoProp", lpunk, IDD_INFOPROPPAGE, IDS_INFOPROPPAGE_TITLE)
{
	DLog(L"CVRInfoPPage()");
}

CVRInfoPPage::~CVRInfoPPage()
{
	DLog(L"~CVRInfoPPage()");

	if (m_hMonoFont) {
		DeleteObject(m_hMonoFont);
		m_hMonoFont = 0;
	}
}

HRESULT CVRInfoPPage::OnConnect(IUnknown *pUnk)
{
	if (pUnk == nullptr) return E_POINTER;

	m_pVideoRenderer = pUnk;
	if (!m_pVideoRenderer) {
		return E_NOINTERFACE;
	}

	return S_OK;
}

HRESULT CVRInfoPPage::OnDisconnect()
{
	if (m_pVideoRenderer == nullptr) {
		return E_UNEXPECTED;
	}

	m_pVideoRenderer.Release();

	return S_OK;
}

HWND GetParentOwner(HWND hwnd)
{
	HWND hWndParent = hwnd;
	HWND hWndT;
	while ((::GetWindowLongPtrW(hWndParent, GWL_STYLE) & WS_CHILD) &&
		(hWndT = ::GetParent(hWndParent)) != NULL) {
		hWndParent = hWndT;
	}

	return hWndParent;
}

static WNDPROC OldControlProc;
static LRESULT CALLBACK ControlProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_KEYDOWN && LOWORD(wParam) == VK_ESCAPE) {
		// fixed Esc handling when EDITTEXT control has ES_MULTILINE property and is in focus
		HWND parentOwner = GetParentOwner(control);
		if (parentOwner) {
			::PostMessageW(parentOwner, WM_COMMAND, IDCANCEL, 0);
		}
		return TRUE;
	}

	return CallWindowProcW(OldControlProc, control, message, wParam, lParam); // call edit control's own windowproc
}

HRESULT CVRInfoPPage::OnActivate()
{
	// set m_hWnd for CWindow
	m_hWnd = m_hwnd;

	SetDlgItemTextW(IDC_EDIT2, GetNameAndVersion());

	// init monospace font
	LOGFONTW lf = {};
	HDC hdc = GetWindowDC();
	lf.lfHeight = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
	ReleaseDC(hdc);
	lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
	wcscpy_s(lf.lfFaceName, L"Consolas");
	m_hMonoFont = CreateFontIndirectW(&lf);

	GetDlgItem(IDC_EDIT1).SetFont(m_hMonoFont);
	ASSERT(m_pVideoRenderer);

	if (!m_pVideoRenderer->GetActive()) {
		SetDlgItemTextW(IDC_EDIT1, L"filter is not active");
		return S_OK;
	}

	std::wstring strInfo(L"Windows ");
	strInfo.append(GetWindowsVersion());
	strInfo.append(L"\r\n");

	std::wstring strVP;
	if (S_OK == m_pVideoRenderer->GetVideoProcessorInfo(strVP)) {
		str_replace(strVP, L"\n", L"\r\n");
		strInfo.append(strVP);
	}

#ifdef _DEBUG
	{
		std::vector<DisplayConfig_t> displayConfigs;

		bool ret = GetDisplayConfigs(displayConfigs);

		strInfo.append(L"\r\n");

		for (const auto& dc : displayConfigs) {
			double freq = (double)dc.refreshRate.Numerator / (double)dc.refreshRate.Denominator;
			strInfo += std::format(L"\r\n{} - {:.3f} Hz", dc.displayName, freq);

			if (dc.bitsPerChannel) { // if bitsPerChannel is not set then colorEncoding and other values are invalid
				const wchar_t* colenc = ColorEncodingToString(dc.colorEncoding);
				if (colenc) {
					strInfo += std::format(L" {}", colenc);
				}
				strInfo += std::format(L" {}-bit", dc.bitsPerChannel);
			}

			const wchar_t* output = OutputTechnologyToString(dc.outputTechnology);
			if (output) {
				strInfo += std::format(L" {}", output);
			}
		}
	}
#endif

	SetDlgItemTextW(IDC_EDIT1, strInfo.c_str());

	OldControlProc = (WNDPROC)::SetWindowLongPtrW(::GetDlgItem(m_hWnd, IDC_EDIT1), GWLP_WNDPROC, (LONG_PTR)ControlProc);

	return S_OK;
}
