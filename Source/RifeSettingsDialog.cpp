/*
 * (C) 2018-2026 see Authors.txt
 *
 * RIFE frame interpolation property dialog.
 */

#include "stdafx.h"
#include "resource.h"
#include "RifeSettingsDialog.h"

#include <algorithm>
#include <format>

namespace {

std::wstring FormatMilli(uint32_t value)
{
	if (!value) {
		return {};
	}
	const uint32_t whole = value / 1000;
	const uint32_t fraction = value % 1000;
	if (!fraction) {
		return std::to_wstring(whole);
	}
	std::wstring result = std::format(L"{}.{:03}", whole, fraction);
	while (result.back() == L'0') {
		result.pop_back();
	}
	return result;
}

bool ParseUnsignedText(HWND hwnd, int id, uint32_t limit, uint32_t& value)
{
	wchar_t text[32] = {};
	GetDlgItemTextW(hwnd, id, text, static_cast<int>(std::size(text)));
	if (!*text) {
		value = 0;
		return true;
	}
	uint64_t parsed = 0;
	for (const wchar_t* p = text; *p; ++p) {
		if (*p < L'0' || *p > L'9') {
			return false;
		}
		parsed = parsed * 10 + static_cast<uint32_t>(*p - L'0');
		if (parsed > limit) {
			return false;
		}
	}
	value = static_cast<uint32_t>(parsed);
	return true;
}

bool ParseMilliText(HWND hwnd, int id, uint32_t limit, uint32_t& value)
{
	wchar_t text[32] = {};
	GetDlgItemTextW(hwnd, id, text, static_cast<int>(std::size(text)));
	if (!*text) {
		value = 0;
		return true;
	}

	uint64_t whole = 0;
	uint32_t fraction = 0;
	uint32_t fractionDigits = 0;
	bool decimal = false;
	for (const wchar_t* p = text; *p; ++p) {
		if (*p == L'.' && !decimal) {
			decimal = true;
			continue;
		}
		if (*p < L'0' || *p > L'9') {
			return false;
		}
		const uint32_t digit = static_cast<uint32_t>(*p - L'0');
		if (!decimal) {
			whole = whole * 10 + digit;
			if (whole > limit / 1000 + 1) {
				return false;
			}
		}
		else {
			if (fractionDigits >= 3) {
				return false;
			}
			fraction = fraction * 10 + digit;
			++fractionDigits;
		}
	}
	if (decimal && fractionDigits == 0) {
		return false;
	}
	while (fractionDigits < 3) {
		fraction *= 10;
		++fractionDigits;
	}
	const uint64_t milli = whole * 1000 + fraction;
	if (milli > limit) {
		return false;
	}
	value = static_cast<uint32_t>(milli);
	return true;
}

void SetOptionalUInt(HWND hwnd, int id, uint32_t value)
{
	const std::wstring text = value ? std::to_wstring(value) : std::wstring();
	SetDlgItemTextW(hwnd, id, text.c_str());
}

void SetMilli(HWND hwnd, int id, uint32_t value)
{
	const std::wstring text = FormatMilli(value);
	SetDlgItemTextW(hwnd, id, text.c_str());
}

std::wstring FormatRange(uint32_t minimum, uint32_t maximum, bool milli)
{
	auto Format = [milli](uint32_t value) {
		return milli ? FormatMilli(value) : std::to_wstring(value);
	};
	if (!minimum && !maximum) {
		return L"any";
	}
	if (!minimum) {
		return std::format(L"≤{}", Format(maximum));
	}
	if (!maximum) {
		return std::format(L"≥{}", Format(minimum));
	}
	if (minimum == maximum) {
		return Format(minimum);
	}
	return std::format(L"{}-{}", Format(minimum), Format(maximum));
}

std::wstring FormatRule(const RifeRateRule& rule, uint32_t index)
{
	std::wstring action;
	if (rule.off) {
		action = L"off";
	}
	else if (rule.maxMultiplierMilli || rule.maxOutputFpsMilli) {
		action = L"max ";
		if (rule.maxMultiplierMilli) {
			action += FormatMilli(rule.maxMultiplierMilli) + L"x";
		}
		if (rule.maxOutputFpsMilli) {
			if (rule.maxMultiplierMilli) {
				action += L", ";
			}
			action += FormatMilli(rule.maxOutputFpsMilli) + L" fps";
		}
	}
	else {
		action = L"uncapped";
	}
	return std::format(L"{}{}. L {} / S {} / {} fps -> {}",
		rule.enabled ? L"" : L"[disabled] ", index + 1,
		FormatRange(rule.minLongEdge, rule.maxLongEdge, false),
		FormatRange(rule.minShortEdge, rule.maxShortEdge, false),
		FormatRange(rule.minFpsMilli, rule.maxFpsMilli, true), action);
}

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

void UpdateRuleButtons(HWND hwnd)
{
	const LRESULT selected = SendDlgItemMessageW(hwnd, IDC_RIFE_RULES_LIST, LB_GETCURSEL, 0, 0);
	const BOOL hasSelection = selected != LB_ERR;
	EnableWindow(GetDlgItem(hwnd, IDC_RIFE_RULE_EDIT), hasSelection);
	EnableWindow(GetDlgItem(hwnd, IDC_RIFE_RULE_REMOVE), hasSelection);
}

void RefreshRuleList(HWND hwnd, const RifeRateRules& rules, int preferredSelection = -1)
{
	SendDlgItemMessageW(hwnd, IDC_RIFE_RULES_LIST, LB_RESETCONTENT, 0, 0);
	for (uint32_t i = 0; i < rules.count; ++i) {
		const std::wstring label = FormatRule(rules.rules[i], i);
		SendDlgItemMessageW(hwnd, IDC_RIFE_RULES_LIST, LB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(label.c_str()));
	}
	if (rules.count) {
		const int selection = preferredSelection >= 0
			? std::min(preferredSelection, static_cast<int>(rules.count) - 1)
			: 0;
		SendDlgItemMessageW(hwnd, IDC_RIFE_RULES_LIST, LB_SETCURSEL, selection, 0);
	}
	UpdateRuleButtons(hwnd);
}

void EnableRuleCapControls(HWND hwnd)
{
	const BOOL enabled = IsDlgButtonChecked(hwnd, IDC_RIFE_RULE_OFF) != BST_CHECKED;
	EnableWindow(GetDlgItem(hwnd, IDC_RIFE_RULE_MAX_MULTIPLIER), enabled);
	EnableWindow(GetDlgItem(hwnd, IDC_RIFE_RULE_MAX_OUTPUT_FPS), enabled);
}

void SetRuleControls(HWND hwnd, const RifeRateRule& rule)
{
	CheckDlgButton(hwnd, IDC_RIFE_RULE_ENABLED, rule.enabled ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hwnd, IDC_RIFE_RULE_OFF, rule.off ? BST_CHECKED : BST_UNCHECKED);
	SetOptionalUInt(hwnd, IDC_RIFE_RULE_MIN_LONG, rule.minLongEdge);
	SetOptionalUInt(hwnd, IDC_RIFE_RULE_MAX_LONG, rule.maxLongEdge);
	SetOptionalUInt(hwnd, IDC_RIFE_RULE_MIN_SHORT, rule.minShortEdge);
	SetOptionalUInt(hwnd, IDC_RIFE_RULE_MAX_SHORT, rule.maxShortEdge);
	SetMilli(hwnd, IDC_RIFE_RULE_MIN_FPS, rule.minFpsMilli);
	SetMilli(hwnd, IDC_RIFE_RULE_MAX_FPS, rule.maxFpsMilli);
	SetMilli(hwnd, IDC_RIFE_RULE_MAX_MULTIPLIER, rule.maxMultiplierMilli);
	SetMilli(hwnd, IDC_RIFE_RULE_MAX_OUTPUT_FPS, rule.maxOutputFpsMilli);
	EnableRuleCapControls(hwnd);
}

bool ReadRuleControls(HWND hwnd, RifeRateRule& rule)
{
	RifeRateRule candidate = rule;
	candidate.enabled = IsDlgButtonChecked(hwnd, IDC_RIFE_RULE_ENABLED) == BST_CHECKED;
	candidate.off = IsDlgButtonChecked(hwnd, IDC_RIFE_RULE_OFF) == BST_CHECKED;
	if (!ParseUnsignedText(hwnd, IDC_RIFE_RULE_MIN_LONG, RifeRateRulesMaxDimension, candidate.minLongEdge)
			|| !ParseUnsignedText(hwnd, IDC_RIFE_RULE_MAX_LONG, RifeRateRulesMaxDimension, candidate.maxLongEdge)
			|| !ParseUnsignedText(hwnd, IDC_RIFE_RULE_MIN_SHORT, RifeRateRulesMaxDimension, candidate.minShortEdge)
			|| !ParseUnsignedText(hwnd, IDC_RIFE_RULE_MAX_SHORT, RifeRateRulesMaxDimension, candidate.maxShortEdge)
			|| !ParseMilliText(hwnd, IDC_RIFE_RULE_MIN_FPS, RifeRateRulesMaxFpsMilli, candidate.minFpsMilli)
			|| !ParseMilliText(hwnd, IDC_RIFE_RULE_MAX_FPS, RifeRateRulesMaxFpsMilli, candidate.maxFpsMilli)
			|| !ParseMilliText(hwnd, IDC_RIFE_RULE_MAX_MULTIPLIER, RifeRateRulesMaxMultiplierMilli, candidate.maxMultiplierMilli)
			|| !ParseMilliText(hwnd, IDC_RIFE_RULE_MAX_OUTPUT_FPS, RifeRateRulesMaxFpsMilli, candidate.maxOutputFpsMilli)
			|| !ValidateRifeRateRule(candidate)) {
		MessageBoxW(hwnd,
			L"Enter valid inclusive source ranges. FPS and multiplier values may use up to three decimals; multiplier caps must be at least 1x.",
			L"RIFE playback rule", MB_OK | MB_ICONERROR);
		return false;
	}
	rule = candidate;
	return true;
}

INT_PTR CALLBACK RuleDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* rule = reinterpret_cast<RifeRateRule*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	switch (message) {
	case WM_INITDIALOG:
		rule = reinterpret_cast<RifeRateRule*>(lParam);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(rule));
		SetRuleControls(hwnd, *rule);
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_RIFE_RULE_OFF:
			if (HIWORD(wParam) == BN_CLICKED) {
				EnableRuleCapControls(hwnd);
				return TRUE;
			}
			break;
		case IDOK:
			if (ReadRuleControls(hwnd, *rule)) {
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

bool ShowRuleEditor(HWND parent, RifeRateRule& rule)
{
	RifeRateRule candidate = rule;
	const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_RIFE_RATE_RULE),
		parent, RuleDialogProc, reinterpret_cast<LPARAM>(&candidate));
	if (result != IDOK) {
		return false;
	}
	rule = candidate;
	return true;
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
	dst.rifeRules = src.rifeRules;
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
		&& a.iRifeDuplicateRemoval == b.iRifeDuplicateRemoval
		&& a.rifeRules == b.rifeRules;
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
	CheckDlgButton(hwnd, IDC_RIFE_RULES_ENABLED,
		settings.rifeRules.enabled ? BST_CHECKED : BST_UNCHECKED);
	RefreshRuleList(hwnd, settings.rifeRules);
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
	settings.rifeRules.enabled = IsDlgButtonChecked(hwnd, IDC_RIFE_RULES_ENABLED) == BST_CHECKED;

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
	if (!ValidateRifeRateRules(settings.rifeRules)) {
		MessageBoxW(hwnd, L"One or more RIFE playback rules are invalid.",
			L"RIFE frame interpolation settings", MB_OK | MB_ICONERROR);
		return false;
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
		case IDC_RIFE_RULES_LIST:
			if (HIWORD(wParam) == LBN_SELCHANGE) {
				UpdateRuleButtons(hwnd);
				return TRUE;
			}
			if (HIWORD(wParam) == LBN_DBLCLK) {
				const LRESULT selected = SendDlgItemMessageW(hwnd, IDC_RIFE_RULES_LIST, LB_GETCURSEL, 0, 0);
				if (selected != LB_ERR && static_cast<uint32_t>(selected) < settings->rifeRules.count
						&& ShowRuleEditor(hwnd, settings->rifeRules.rules[static_cast<uint32_t>(selected)])) {
					RefreshRuleList(hwnd, settings->rifeRules, static_cast<int>(selected));
				}
				return TRUE;
			}
			break;
		case IDC_RIFE_RULE_ADD:
			if (HIWORD(wParam) == BN_CLICKED) {
				if (settings->rifeRules.count >= settings->rifeRules.rules.size()) {
					MessageBoxW(hwnd, L"A maximum of 32 RIFE playback rules is supported.",
						L"RIFE frame interpolation settings", MB_OK | MB_ICONINFORMATION);
					return TRUE;
				}
				RifeRateRule rule;
				if (ShowRuleEditor(hwnd, rule)) {
					const uint32_t index = settings->rifeRules.count++;
					settings->rifeRules.rules[index] = rule;
					RefreshRuleList(hwnd, settings->rifeRules, static_cast<int>(index));
				}
				return TRUE;
			}
			break;
		case IDC_RIFE_RULE_EDIT:
			if (HIWORD(wParam) == BN_CLICKED) {
				const LRESULT selected = SendDlgItemMessageW(hwnd, IDC_RIFE_RULES_LIST, LB_GETCURSEL, 0, 0);
				if (selected != LB_ERR && static_cast<uint32_t>(selected) < settings->rifeRules.count
						&& ShowRuleEditor(hwnd, settings->rifeRules.rules[static_cast<uint32_t>(selected)])) {
					RefreshRuleList(hwnd, settings->rifeRules, static_cast<int>(selected));
				}
				return TRUE;
			}
			break;
		case IDC_RIFE_RULE_REMOVE:
			if (HIWORD(wParam) == BN_CLICKED) {
				const LRESULT selected = SendDlgItemMessageW(hwnd, IDC_RIFE_RULES_LIST, LB_GETCURSEL, 0, 0);
				if (selected != LB_ERR && static_cast<uint32_t>(selected) < settings->rifeRules.count) {
					const uint32_t index = static_cast<uint32_t>(selected);
					for (uint32_t i = index + 1; i < settings->rifeRules.count; ++i) {
						settings->rifeRules.rules[i - 1] = settings->rifeRules.rules[i];
					}
					--settings->rifeRules.count;
					settings->rifeRules.rules[settings->rifeRules.count] = {};
					RefreshRuleList(hwnd, settings->rifeRules, static_cast<int>(index));
				}
				return TRUE;
			}
			break;
		case IDC_RIFE_RULE_EXAMPLE:
			if (HIWORD(wParam) == BN_CLICKED) {
				settings->rifeRules = MakeExampleRifeRateRules();
				CheckDlgButton(hwnd, IDC_RIFE_RULES_ENABLED, BST_CHECKED);
				RefreshRuleList(hwnd, settings->rifeRules);
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
