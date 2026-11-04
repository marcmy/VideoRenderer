# Renderer hotkeys

Keyboard shortcuts that switch Maxine, RIFE frame interpolation and SDR
enhancement on and off during playback.

| Feature | Default | Switches between |
|---|---|---|
| Maxine | Ctrl+Alt+M | Off and the last Maxine operation (upscaling by default) |
| RIFE frame interpolation | Ctrl+Alt+F | Disabled and the last RIFE mode (Movie 2× by default) |
| SDR enhancement | Ctrl+Alt+E | Off and the last mode (On or A/B split) |

Each toggle is applied through the normal settings path, saved, and shown as
a two-second message at the top of the video (Direct3D 11). The keys are set
in the main settings page → **Hotkeys...**; each can be cleared. Hotkeys need
Ctrl or Alt, because plain keys belong to the player.

The defaults avoid the Ctrl+Alt shortcuts in MPC-HC's and MPC-BE's default key
tables. Ctrl+Alt+R is not used: both players reset renderer statistics with it.
A key that the user has also bound in the player is taken by the renderer
while a video is open. A system-wide hotkey, such as the shortcut key of a
Start menu or desktop shortcut, goes to its program instead and never reaches
the player. On keyboard layouts with AltGr, AltGr+key is reported as
Ctrl+Alt+key.

## How keys reach the renderer

The player owns the keyboard. Its message loop translates its accelerators
before any window sees a key, and the renderer's window rarely has focus. The
renderer therefore installs a thread-specific `WH_GETMESSAGE` hook on the
player's UI thread (the thread of its top-level window) while a renderer
exists. The hook ignores everything except keyboard messages, and changes only
the bound hotkeys' messages (key-down, auto-repeat and key-up) to `WM_NULL`,
so the player's accelerators never see them. `WH_KEYBOARD` was rejected
because it misses key messages that were posted rather than typed.

- The hook never calls into the renderer while holding its own lock, because
  the player may initialise the renderer on another thread while holding the
  renderer's lock. The destructor first unregisters and waits for any running
  hotkey call.
- With several renderers on one thread (briefly, during a file switch), the
  most recently registered one receives the hotkeys.
- While the Hotkeys settings window is open, the hotkeys are paused so that
  its key boxes can capture any combination.

## Tests

`tools/feature-hotkeys-test/build-test.cmd` covers key packing and validation,
matching, the hook's decisions (first key-down toggles, repeats and the
key-up are swallowed, other keys pass), mode toggling, and a message loop with
accelerators: a bound key never reaches them, while the player's own keys
still do. `test_feature_hotkeys_contract.py` checks the thread-specific hook,
lock discipline, destruction order, settings persistence and UI wiring.
