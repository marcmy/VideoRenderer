#pragma once

// Match the hosting property page; leave native light/high-contrast dialogs alone.
void InitializeSettingsDialogTheme(HWND dialog);

struct SettingsTooltip {
	int id;
	const wchar_t* text;
};

// Shows each text while hovering its control, even when the control is disabled, or a combo
// box's or edit box's label, which must come just before the control in the dialog template.
void AddSettingsTooltips(HWND dialog, const SettingsTooltip* tips, size_t count);

template <size_t N>
void AddSettingsTooltips(HWND dialog, const SettingsTooltip (&tips)[N])
{
	AddSettingsTooltips(dialog, tips, N);
}
