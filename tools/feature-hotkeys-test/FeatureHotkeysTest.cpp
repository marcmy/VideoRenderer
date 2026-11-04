// Tests for the renderer hotkeys (Source/FeatureHotkeys.h): key packing and
// validation, matching, the keyboard-hook decisions, mode toggling, and a
// Win32 check that a thread-specific WH_GETMESSAGE hook removes a bound hotkey
// before the player's accelerators see it while other keys still reach them.

#include "../../Source/FeatureHotkeys.h"

#include <cstdio>
#include <cstdlib>

namespace {

int g_checks = 0;

void Check(bool condition, const char* message)
{
	++g_checks;
	if (!condition) {
		printf("FAIL: %s\n", message);
		exit(1);
	}
}

constexpr BYTE CtrlAlt = HOTKEYF_CONTROL | HOTKEYF_ALT;

// lParam of a keyboard message.
LPARAM KeyDown(bool repeat = false) { return repeat ? (1 << 30) | 1 : 1; }
LPARAM KeyUp() { return static_cast<LPARAM>((1u << 31) | (1u << 30) | 1u); }

void TestDefaults()
{
	const int expected[FEATURE_HOTKEY_COUNT] = {
		MakeFeatureHotkey('M', CtrlAlt), MakeFeatureHotkey('F', CtrlAlt), MakeFeatureHotkey('E', CtrlAlt),
	};
	// Ctrl+Alt bindings in the default MPC-HC and MPC-BE key tables.
	const int playerKeys[] = {
		MakeFeatureHotkey('P', CtrlAlt), MakeFeatureHotkey('R', CtrlAlt), MakeFeatureHotkey('V', CtrlAlt),
		MakeFeatureHotkey(VK_UP, CtrlAlt), MakeFeatureHotkey(VK_DOWN, CtrlAlt), MakeFeatureHotkey('1', CtrlAlt),
	};
	for (int feature = 0; feature < FEATURE_HOTKEY_COUNT; ++feature) {
		const int hotkey = FeatureHotkeyDefault(feature);
		Check(hotkey == expected[feature], "defaults are Ctrl+Alt+M, Ctrl+Alt+F and Ctrl+Alt+E");
		Check(IsValidFeatureHotkey(hotkey) && NormalizeFeatureHotkey(hotkey) == hotkey, "defaults are valid");
		for (int other = 0; other < feature; ++other) {
			Check(FeatureHotkeyDefault(other) != hotkey, "defaults are distinct");
		}
		for (const int playerKey : playerKeys) {
			Check(hotkey != playerKey, "defaults avoid the players' Ctrl+Alt shortcuts");
		}
	}
	Check(FeatureHotkeyDefault(FEATURE_HOTKEY_COUNT) == 0, "unknown features have no default");
}

void TestValidation()
{
	Check(IsValidFeatureHotkey(0) && NormalizeFeatureHotkey(0) == 0, "0 means no hotkey");
	Check(IsValidFeatureHotkey(MakeFeatureHotkey('E', HOTKEYF_CONTROL)), "Ctrl+key is valid");
	Check(IsValidFeatureHotkey(MakeFeatureHotkey(VK_F5, HOTKEYF_ALT)), "Alt+key is valid");
	Check(IsValidFeatureHotkey(MakeFeatureHotkey('E', CtrlAlt | HOTKEYF_SHIFT)), "Ctrl+Alt+Shift+key is valid");
	Check(!IsValidFeatureHotkey(MakeFeatureHotkey('E', 0)), "a plain key belongs to the player");
	Check(!IsValidFeatureHotkey(MakeFeatureHotkey('E', HOTKEYF_SHIFT)), "Shift+key belongs to the player");
	for (const UINT vk : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RCONTROL, VK_LMENU}) {
		Check(!IsValidFeatureHotkey(MakeFeatureHotkey(vk, CtrlAlt)), "a modifier is not a hotkey key");
	}
	Check(!IsValidFeatureHotkey(MakeFeatureHotkey(0, CtrlAlt)), "a hotkey needs a key");
	Check(!IsValidFeatureHotkey(MakeFeatureHotkey('E', CtrlAlt | 0x10)), "unknown modifier bits are rejected");
	Check(!IsValidFeatureHotkey(0x10000 | 'E') && !IsValidFeatureHotkey(-1), "out-of-range values are rejected");
	Check(NormalizeFeatureHotkey(MakeFeatureHotkey('E', 0)) == 0, "invalid saved hotkeys become none");

	const int extended = MakeFeatureHotkey(VK_RIGHT, CtrlAlt | HOTKEYF_EXT);
	Check(IsValidFeatureHotkey(extended), "extended keys from the hotkey control are valid");
	Check(NormalizeFeatureHotkey(extended) == MakeFeatureHotkey(VK_RIGHT, CtrlAlt), "matching ignores the extended flag");
}

void TestMatching()
{
	const int hotkeys[FEATURE_HOTKEY_COUNT] = {
		FeatureHotkeyDefault(FEATURE_HOTKEY_Maxine), FeatureHotkeyDefault(FEATURE_HOTKEY_Rife), 0,
	};
	Check(FindFeatureHotkey(hotkeys, FeatureHotkeyFromKeyboard('M', false, true, true)) == FEATURE_HOTKEY_Maxine, "Ctrl+Alt+M is Maxine");
	Check(FindFeatureHotkey(hotkeys, FeatureHotkeyFromKeyboard('F', false, true, true)) == FEATURE_HOTKEY_Rife, "Ctrl+Alt+F is RIFE");
	Check(FindFeatureHotkey(hotkeys, FeatureHotkeyFromKeyboard('E', false, true, true)) == -1, "a cleared hotkey matches nothing");
	Check(FindFeatureHotkey(hotkeys, FeatureHotkeyFromKeyboard('M', true, true, true)) == -1, "extra Shift is a different key");
	Check(FindFeatureHotkey(hotkeys, FeatureHotkeyFromKeyboard('M', false, true, false)) == -1, "Ctrl+M stays the player's");
	Check(FindFeatureHotkey(hotkeys, FeatureHotkeyFromKeyboard('M', false, false, false)) == -1, "M stays the player's");
	Check(FindFeatureHotkey(hotkeys, FeatureHotkeyFromKeyboard(VK_MENU, false, true, true)) == -1, "pressing modifiers matches nothing");
	Check(FindFeatureHotkey(hotkeys, 0) == -1, "nothing pressed matches nothing");
}

void TestHookDecisions()
{
	UINT swallowed = 0;
	Check(ClassifyFeatureHotkey(true, 'E', KeyDown(), FEATURE_HOTKEY_SdrEnhance, swallowed) == FEATURE_HOTKEY_ACTION_Toggle,
		"the first key-down of a hotkey toggles");
	Check(swallowed == 'E', "the hotkey's key is remembered");
	Check(ClassifyFeatureHotkey(true, 'E', KeyDown(true), FEATURE_HOTKEY_SdrEnhance, swallowed) == FEATURE_HOTKEY_ACTION_Swallow,
		"auto-repeat is swallowed without toggling again");
	Check(ClassifyFeatureHotkey(true, VK_CONTROL, KeyUp(), -1, swallowed) == FEATURE_HOTKEY_ACTION_Pass,
		"releasing a modifier first reaches the player");
	Check(ClassifyFeatureHotkey(true, 'E', KeyUp(), -1, swallowed) == FEATURE_HOTKEY_ACTION_Swallow && swallowed == 0,
		"the hotkey's key-up is swallowed even after the modifiers are released");
	Check(ClassifyFeatureHotkey(true, 'E', KeyUp(), -1, swallowed) == FEATURE_HOTKEY_ACTION_Pass,
		"later key-ups reach the player");
	Check(ClassifyFeatureHotkey(true, 'Q', KeyDown(), -1, swallowed) == FEATURE_HOTKEY_ACTION_Pass && swallowed == 0,
		"unbound keys reach the player");
	Check(ClassifyFeatureHotkey(false, 'E', KeyDown(), FEATURE_HOTKEY_SdrEnhance, swallowed) == FEATURE_HOTKEY_ACTION_Pass && swallowed == 0,
		"a peeked key that stays queued is decided when it is removed");
	// A hotkey already held when playback starts only repeats: swallow, no toggle.
	Check(ClassifyFeatureHotkey(true, 'M', KeyDown(true), FEATURE_HOTKEY_Maxine, swallowed) == FEATURE_HOTKEY_ACTION_Swallow && swallowed == 'M',
		"a held hotkey does not toggle on repeat");
	Check(ClassifyFeatureHotkey(true, 'M', KeyUp(), -1, swallowed) == FEATURE_HOTKEY_ACTION_Swallow, "and its key-up is swallowed");
}

void TestToggle()
{
	constexpr int off = 0, on = 1, split = 2, count = 3;
	int last = on;
	int mode = ToggleFeatureMode(split, last, off, on, count);
	Check(mode == off && last == split, "switching off remembers the mode that was on");
	mode = ToggleFeatureMode(mode, last, off, on, count);
	Check(mode == split, "switching on restores the remembered mode");
	last = off;
	Check(ToggleFeatureMode(off, last, off, on, count) == on, "without a remembered mode, the fallback is used");
	last = 7;
	Check(ToggleFeatureMode(off, last, off, on, count) == on, "an invalid remembered mode uses the fallback");
	last = on;
	Check(ToggleFeatureMode(9, last, off, on, count) == on && last == on, "an invalid current mode counts as off");

	last = off;
	RememberFeatureMode(split, last, off, on, count);
	Check(last == split, "a mode chosen in the settings is remembered");
	RememberFeatureMode(off, last, off, on, count);
	Check(last == split, "switching off in the settings keeps the remembered mode");
	last = -3;
	RememberFeatureMode(off, last, off, on, count);
	Check(last == on, "an invalid remembered mode is repaired");
}

void TestFormatting()
{
	wchar_t text[64];
	FormatFeatureHotkey(FeatureHotkeyDefault(FEATURE_HOTKEY_SdrEnhance), text, std::size(text));
	Check(wcscmp(text, L"Ctrl+Alt+E") == 0, "Ctrl+Alt+E is formatted");
	FormatFeatureHotkey(MakeFeatureHotkey('7', HOTKEYF_ALT | HOTKEYF_SHIFT), text, std::size(text));
	Check(wcscmp(text, L"Alt+Shift+7") == 0, "Alt+Shift+7 is formatted");
	FormatFeatureHotkey(0, text, std::size(text));
	Check(wcscmp(text, L"None") == 0, "no hotkey is formatted as None");
}

// Win32: the renderer's hook arrangement on a player-like message loop.
UINT g_swallowed = 0;
int g_toggles = 0;
HHOOK g_hook = nullptr;
const int g_hotkeys[FEATURE_HOTKEY_COUNT] = {
	FeatureHotkeyDefault(FEATURE_HOTKEY_Maxine), FeatureHotkeyDefault(FEATURE_HOTKEY_Rife), FeatureHotkeyDefault(FEATURE_HOTKEY_SdrEnhance),
};

// Mirrors FeatureHotkeyProc in Source/RifeVideoRenderer.inl.
LRESULT CALLBACK TestHookProc(int code, WPARAM wParam, LPARAM lParam)
{
	auto* msg = reinterpret_cast<MSG*>(lParam);
	if (code != HC_ACTION || !msg || !IsKeyboardMessage(msg->message)) {
		return CallNextHookEx(g_hook, code, wParam, lParam);
	}
	const UINT vk = static_cast<UINT>(msg->wParam);
	const int feature = FindFeatureHotkey(g_hotkeys, FeatureHotkeyFromKeyboard(vk,
		GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0));
	const auto action = ClassifyFeatureHotkey(wParam == PM_REMOVE, vk, msg->lParam, feature, g_swallowed);
	if (action != FEATURE_HOTKEY_ACTION_Pass) {
		msg->message = WM_NULL;
	}
	if (action == FEATURE_HOTKEY_ACTION_Toggle) {
		++g_toggles;
	}
	return CallNextHookEx(g_hook, code, wParam, lParam);
}

int g_commands[3] = {};
int g_keyMessages = 0;

LRESULT CALLBACK PlayerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_COMMAND && LOWORD(wParam) >= 1 && LOWORD(wParam) <= 3) {
		++g_commands[LOWORD(wParam) - 1];
		return 0;
	}
	if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
		++g_keyMessages;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

void SetModifiers(bool control, bool alt)
{
	BYTE state[256] = {};
	GetKeyboardState(state);
	state[VK_CONTROL] = control ? 0x80 : 0;
	state[VK_LCONTROL] = control ? 0x80 : 0;
	state[VK_MENU] = alt ? 0x80 : 0;
	state[VK_LMENU] = alt ? 0x80 : 0;
	SetKeyboardState(state);
}

// Posts a key-down and key-up and runs a player's loop: hook, then
// accelerators, then dispatch.
void PressKey(HWND hwnd, HACCEL accelerators, UINT vk)
{
	PostMessageW(hwnd, WM_KEYDOWN, vk, KeyDown());
	PostMessageW(hwnd, WM_KEYUP, vk, KeyUp());
	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
		if (!TranslateAcceleratorW(hwnd, accelerators, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
}

void TestHookOnMessageLoop()
{
	const HINSTANCE instance = GetModuleHandleW(nullptr);
	WNDCLASSW wc = {};
	wc.lpfnWndProc = PlayerWndProc;
	wc.hInstance = instance;
	wc.lpszClassName = L"FeatureHotkeysTestPlayer";
	Check(RegisterClassW(&wc) != 0, "test window class");
	const HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, instance, nullptr);
	Check(hwnd != nullptr, "test window");

	// The player's shortcuts: Ctrl+Alt+R (reset statistics), Ctrl+M (mute),
	// and Ctrl+Alt+E as if a user had bound it in the player too.
	ACCEL table[] = {
		{FVIRTKEY | FCONTROL | FALT, 'R', 1},
		{FVIRTKEY | FCONTROL, 'M', 2},
		{FVIRTKEY | FCONTROL | FALT, 'E', 3},
	};
	const HACCEL accelerators = CreateAcceleratorTableW(table, static_cast<int>(std::size(table)));
	Check(accelerators != nullptr, "test accelerators");

	g_hook = SetWindowsHookExW(WH_GETMESSAGE, TestHookProc, nullptr, GetCurrentThreadId());
	Check(g_hook != nullptr, "a thread-specific message hook can be installed");

	SetModifiers(true, true);
	PressKey(hwnd, accelerators, 'E');
	Check(g_toggles == 1 && g_commands[2] == 0, "a bound hotkey toggles once and never reaches the player");
	PressKey(hwnd, accelerators, 'R');
	Check(g_toggles == 1 && g_commands[0] == 1, "the player's Ctrl+Alt+R still works");
	SetModifiers(true, false);
	PressKey(hwnd, accelerators, 'M');
	Check(g_toggles == 1 && g_commands[1] == 1, "the player's Ctrl+M still works");
	SetModifiers(false, false);
	const int keysBefore = g_keyMessages;
	PressKey(hwnd, accelerators, 'M');
	Check(g_toggles == 1 && g_keyMessages == keysBefore + 1, "plain keys reach the player's window");
	Check(g_swallowed == 0, "no key is left swallowed");

	UnhookWindowsHookEx(g_hook);
	SetModifiers(true, true);
	PressKey(hwnd, accelerators, 'E');
	Check(g_toggles == 1 && g_commands[2] == 1, "after unhooking, the key is the player's again");
	SetModifiers(false, false);

	DestroyAcceleratorTable(accelerators);
	DestroyWindow(hwnd);
}

} // namespace

int main()
{
	TestDefaults();
	TestValidation();
	TestMatching();
	TestHookDecisions();
	TestToggle();
	TestFormatting();
	TestHookOnMessageLoop();
	printf("PASS %d renderer hotkey checks, including a keyboard hook on a player message loop\n", g_checks);
	return 0;
}
