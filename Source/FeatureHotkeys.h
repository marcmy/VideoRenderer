/*
 * (C) 2018-2026 see Authors.txt
 *
 * Renderer hotkeys that switch Maxine, RIFE and SDR enhancement on or off.
 * Pure logic, shared by the renderer, its settings dialog and the tests.
 */

#pragma once

#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cwchar>
#include <iterator>

enum FEATURE_HOTKEY {
	FEATURE_HOTKEY_Maxine = 0,
	FEATURE_HOTKEY_Rife,
	FEATURE_HOTKEY_SdrEnhance,
	FEATURE_HOTKEY_COUNT
};

// A hotkey is packed like a hotkey control value: the low byte is the virtual
// key and the high byte holds HOTKEYF_* modifiers. 0 means no hotkey.
constexpr int MakeFeatureHotkey(UINT vk, BYTE modifiers)
{
	return static_cast<int>((static_cast<UINT>(modifiers) << 8) | (vk & 0xFF));
}

constexpr UINT FeatureHotkeyKey(int hotkey) { return static_cast<UINT>(hotkey) & 0xFF; }
constexpr BYTE FeatureHotkeyModifiers(int hotkey) { return static_cast<BYTE>((static_cast<UINT>(hotkey) >> 8) & 0xFF); }

// Ctrl+Alt+M, Ctrl+Alt+F and Ctrl+Alt+E are unused by MPC-HC and MPC-BE.
// Ctrl+Alt+R is not: both players use it to reset renderer statistics.
constexpr int FeatureHotkeyDefault(int feature)
{
	constexpr BYTE ctrlAlt = HOTKEYF_CONTROL | HOTKEYF_ALT;
	switch (feature) {
	case FEATURE_HOTKEY_Maxine:     return MakeFeatureHotkey('M', ctrlAlt);
	case FEATURE_HOTKEY_Rife:       return MakeFeatureHotkey('F', ctrlAlt);
	case FEATURE_HOTKEY_SdrEnhance: return MakeFeatureHotkey('E', ctrlAlt);
	}
	return 0;
}

inline bool IsModifierKey(UINT vk)
{
	switch (vk) {
	case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
	case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
	case VK_MENU: case VK_LMENU: case VK_RMENU:
	case VK_LWIN: case VK_RWIN:
		return true;
	}
	return false;
}

// The renderer sees the player's keyboard, so a hotkey needs Ctrl or Alt;
// plain and Shift-only keys belong to the player.
inline bool IsValidFeatureHotkey(int hotkey)
{
	if (hotkey == 0) {
		return true;
	}
	const UINT vk = FeatureHotkeyKey(hotkey);
	const BYTE modifiers = FeatureHotkeyModifiers(hotkey);
	return hotkey > 0 && hotkey <= 0xFFFF && vk && !IsModifierKey(vk)
		&& !(modifiers & ~(HOTKEYF_SHIFT | HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_EXT))
		&& (modifiers & (HOTKEYF_CONTROL | HOTKEYF_ALT));
}

// The extended-key flag only distinguishes keys such as the numeric keypad's
// Enter; matching ignores it.
inline int NormalizeFeatureHotkey(int hotkey)
{
	return IsValidFeatureHotkey(hotkey) ? hotkey & ~(HOTKEYF_EXT << 8) : 0;
}

inline int FeatureHotkeyFromKeyboard(UINT vk, bool shift, bool control, bool alt)
{
	const BYTE modifiers = static_cast<BYTE>((shift ? HOTKEYF_SHIFT : 0)
		| (control ? HOTKEYF_CONTROL : 0) | (alt ? HOTKEYF_ALT : 0));
	return NormalizeFeatureHotkey(MakeFeatureHotkey(vk, modifiers));
}

// Index of the feature bound to a pressed key, or -1.
inline int FindFeatureHotkey(const int (&hotkeys)[FEATURE_HOTKEY_COUNT], int pressed)
{
	if (pressed == 0) {
		return -1;
	}
	for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
		if (NormalizeFeatureHotkey(hotkeys[feature]) == pressed) {
			return feature;
		}
	}
	return -1;
}

// What the message hook does with one keyboard message.
enum FEATURE_HOTKEY_ACTION {
	FEATURE_HOTKEY_ACTION_Pass,    // not ours: the player handles it
	FEATURE_HOTKEY_ACTION_Toggle,  // first key-down of a bound hotkey: swallow and toggle
	FEATURE_HOTKEY_ACTION_Swallow, // auto-repeat or key-up of a hotkey already handled
};

constexpr bool IsKeyboardMessage(UINT message)
{
	return message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP;
}

// Decides one keyboard message. removed is false while the message is only
// peeked and stays queued; it is decided when it is removed. lParam is the
// message's key-state word: bit 30 is the previous key state (auto-repeat)
// and bit 31 the transition (key up). swallowedKey holds the key whose
// key-down was swallowed, so its repeats and key-up are swallowed too.
inline FEATURE_HOTKEY_ACTION ClassifyFeatureHotkey(bool removed, UINT vk, LPARAM lParam, int feature, UINT& swallowedKey)
{
	if (!removed) {
		return FEATURE_HOTKEY_ACTION_Pass;
	}
	const bool keyUp = (static_cast<ULONG_PTR>(lParam) & (1u << 31)) != 0;
	const bool repeat = (static_cast<ULONG_PTR>(lParam) & (1u << 30)) != 0;
	if (keyUp) {
		if (swallowedKey && vk == swallowedKey) {
			swallowedKey = 0;
			return FEATURE_HOTKEY_ACTION_Swallow;
		}
		return FEATURE_HOTKEY_ACTION_Pass;
	}
	if (feature < 0) {
		return FEATURE_HOTKEY_ACTION_Pass;
	}
	if (repeat && vk == swallowedKey) {
		return FEATURE_HOTKEY_ACTION_Swallow;
	}
	swallowedKey = vk;
	return repeat ? FEATURE_HOTKEY_ACTION_Swallow : FEATURE_HOTKEY_ACTION_Toggle;
}

// Switches a mode between off and the last mode that was on. When the last
// mode is unknown or off, the fallback is used.
inline int ToggleFeatureMode(int current, int& last, int off, int fallback, int count)
{
	if (current != off && current >= 0 && current < count) {
		last = current;
		return off;
	}
	return last != off && last >= 0 && last < count ? last : fallback;
}

// Remembers a mode that is on, so the hotkey can restore it later.
inline void RememberFeatureMode(int current, int& last, int off, int fallback, int count)
{
	if (current != off && current >= 0 && current < count) {
		last = current;
	} else if (last == off || last < 0 || last >= count) {
		last = fallback;
	}
}

// Readable name of a hotkey ("Ctrl+Alt+E"), or "None".
inline void FormatFeatureHotkey(int hotkey, wchar_t* text, size_t size)
{
	if (!size) {
		return;
	}
	text[0] = L'\0';
	if (!NormalizeFeatureHotkey(hotkey)) {
		wcsncpy_s(text, size, L"None", _TRUNCATE);
		return;
	}
	const BYTE modifiers = FeatureHotkeyModifiers(hotkey);
	if (modifiers & HOTKEYF_CONTROL) wcsncat_s(text, size, L"Ctrl+", _TRUNCATE);
	if (modifiers & HOTKEYF_ALT)     wcsncat_s(text, size, L"Alt+", _TRUNCATE);
	if (modifiers & HOTKEYF_SHIFT)   wcsncat_s(text, size, L"Shift+", _TRUNCATE);
	const UINT vk = FeatureHotkeyKey(hotkey);
	wchar_t key[32] = {};
	if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
		key[0] = static_cast<wchar_t>(vk);
	} else {
		const LONG scan = static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
		if (!GetKeyNameTextW(scan, key, static_cast<int>(std::size(key)))) {
			swprintf_s(key, L"Key %u", vk);
		}
	}
	wcsncat_s(text, size, key, _TRUNCATE);
}
