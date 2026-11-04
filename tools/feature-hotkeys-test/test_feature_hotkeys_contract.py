from pathlib import Path
import re


repo = Path(__file__).resolve().parents[2]

renderer = (repo / "Source" / "VideoRenderer.cpp").read_text(encoding="utf-8")
rife = (repo / "Source" / "RifeVideoRenderer.inl").read_text(encoding="utf-8")
processor = (repo / "Source" / "DX11VideoProcessor.cpp").read_text(encoding="utf-8")
settings = (repo / "Source" / "IVideoRenderer.h").read_text(encoding="utf-8")
proppage = (repo / "Source" / "PropPage.cpp").read_text(encoding="utf-8")
resources = (repo / "Source" / "MpcVideoRenderer.rc").read_text(encoding="utf-8")


def body(source, start, end):
    first = source.find(start)
    assert first != -1, f"{start!r} was not found"
    last = source.find(end, first)
    assert last != -1, f"end of {start!r} was not found"
    return source[first:last]


# The hook watches only the player's UI thread, never the whole desktop.
hooks = re.findall(r"= SetWindowsHookExW\(([^;]*)\);", rife + renderer)
assert hooks == ["WH_GETMESSAGE, FeatureHotkeyProc, nullptr, threadId"], (
    f"the renderer must install exactly one thread-specific message hook, found {hooks}"
)
assert "WH_KEYBOARD_LL" not in rife and "RegisterHotKey" not in rife, "no global keyboard hooks or system hotkeys"
register = body(rife, "void CMpcVideoRenderer::RegisterFeatureHotkeys()", "\nvoid CMpcVideoRenderer::OnFeatureHotkey")
assert "GetWindowThreadProcessId(m_hWndParentMain, nullptr)" in register, "the hook must target the player's window thread"

init = body(renderer, "HRESULT CMpcVideoRenderer::Init(", "\nSTDMETHODIMP CMpcVideoRenderer::put_Owner")
assert "RegisterFeatureHotkeys();" in init, "Init() must register the renderer's hotkeys once the player window is known"

# Destruction: nothing may call into a renderer being destroyed.
destructor = body(renderer, "CMpcVideoRenderer::~CMpcVideoRenderer()", "\n}\n")
statements = [line.strip() for line in destructor.splitlines()[2:] if line.strip() and not line.strip().startswith("//")]
assert statements[1] == "UnregisterFeatureHotkeys(this);", "unregistering must come right after the destructor's log line"

# The hook and timer call into the renderer only outside the lock, through
# the helper that makes unregistering wait for them.
proc = body(rife, "LRESULT CALLBACK FeatureHotkeyProc(", "\nvoid CALLBACK FeatureHotkeyMessageTimerProc")
timer = body(rife, "void CALLBACK FeatureHotkeyMessageTimerProc(", "\nvoid UnregisterFeatureHotkeys")
for name, source in (("hook", proc), ("timer", timer)):
    calls = re.findall(r"renderer->\w+\(", source)
    assert calls and all(re.search(r"CallFeatureHotkeyRenderer\(lock, [^\n]*" + re.escape(c), source) for c in calls), (
        f"the {name} procedure must call the renderer through CallFeatureHotkeyRenderer"
    )
assert "IsKeyboardMessage(msg->message)" in proc and "msg->message = WM_NULL;" in proc, (
    "the hook must look at key messages only and blank the ones it handles"
)
assert "wParam == PM_REMOVE" in proc, "peeked messages that stay queued must be decided when removed"
helper = body(rife, "void CallFeatureHotkeyRenderer(", "\nLRESULT CALLBACK FeatureHotkeyProc")
assert helper.index("lock.unlock();") < helper.index("call();") < helper.index("lock.lock();"), (
    "the renderer must be called with the hotkey lock released"
)
unregister = body(rife, "void UnregisterFeatureHotkeys(", "\nvoid UpdateFeatureHotkeyBindings")
remove = body(rife, "void RemoveFeatureHotkeyRenderer(", "\n// For the destructor")
assert "g_featureHotkeyIdle.wait(" in unregister and "RemoveFeatureHotkeyRenderer(renderer);" in unregister, (
    "unregistering must wait for running calls before removing the renderer"
)
assert "UnhookWindowsHookEx" in remove, "the hook must be removed with a thread's last renderer"
# Init() holds the renderer lock: registering must never wait for a toggle.
assert "g_featureHotkeyIdle" not in register and "UnregisterFeatureHotkeys" not in register, (
    "registering must not wait for running hotkey calls"
)
assert "FeatureHotkeyCaptureActive()" in proc, "hotkeys must pause while their settings capture keys"

# Settings: appended after the SDR enhancement settings, loaded, saved, clamped.
layout = body(settings, "int iSdrEnhanceSkinProtection;", "Settings_t() {")
fields = ["iHotkeyMaxine", "iHotkeyRife", "iHotkeySdrEnhance", "iMaxineLastOperation", "iRifeLastMode", "iSdrEnhanceLastMode"]
assert re.findall(r"int (\w+);", layout)[1:] == fields, "hotkey settings must be appended in order"
for option in ("HotkeyMaxine", "HotkeyRife", "HotkeySdrEnhance", "MaxineLastOperation", "RifeLastMode", "SdrEnhanceLastMode"):
    assert f"QueryDWORDValue(OPT_{option}," in rife and f"SetDWORDValue(OPT_{option}," in rife, f"{option} must be loaded and saved"
set_settings = body(rife, "STDMETHODIMP_(void) CMpcVideoRenderer::SetSettings(", "\nSTDMETHODIMP CMpcVideoRenderer::SaveSettings")
assert "ClampFeatureHotkeySettings(adjusted);" in set_settings and "UpdateFeatureHotkeyBindings(this, m_Sets);" in set_settings, (
    "applied settings must be clamped and reach the hook's bindings"
)

# The toggle goes through the normal settings path and persists.
toggle = body(rife, "void CMpcVideoRenderer::OnFeatureHotkey(", "\nvoid CMpcVideoRenderer::OnFeatureHotkeyMessageExpired")
assert toggle.index("SetSettings(settings);") < toggle.index("SaveSettings();"), "a toggle must apply and save the settings"
for mode in ("iMaxineOperation", "iRifeMode", "iSdrEnhanceMode"):
    assert f"settings.{mode} = ToggleFeatureMode(" in toggle, f"{mode} must toggle between off and the last mode"

# The message is drawn with the statistics in both presentation paths.
assert processor.count("\tDrawToggleMessage(pBackBuffer);\n") == 2, "Render() and FillBlack() must draw the toggle message"

# Settings UI.
assert "IDC_BUTTON_HOTKEYS" in resources and "IDD_FEATURE_HOTKEYS DIALOGEX" in resources, "the hotkey dialog must exist"
assert re.search(r"nID == IDC_BUTTON_HOTKEYS\) \{\s*if \(ShowFeatureHotkeySettings\(\)\) \{ SetDirty\(\); \}", proppage), (
    "the main page button must open the hotkey dialog"
)

print("Renderer hotkey contract test passed")
