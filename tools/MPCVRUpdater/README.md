# MPC-VR Maxine + RIFE updater

Player and K-Lite Codec Pack updates can overwrite the custom MPC Video Renderer build. This helper creates a desktop shortcut that installs the latest successful custom Maxine + RIFE build for every supported player it finds. Run it again after installing another player.

## Setup

1. Download `MPCVR-Setup.zip` from the latest release and run its setup, which installs this updater. To install only the updater, get this folder and run `Install-MPCVRUpdater.cmd` once.
2. Use the **Restore MPC-VR Maxine + RIFE** desktop shortcut after a player or K-Lite update.

The installer stores `Update-MPCVR.ps1` and `MPCVRPlayerInstall.psm1` under `%LOCALAPPDATA%\MPCVR Custom Updater`. It prefers PowerShell 7 (`pwsh.exe`) when it is available and automatically falls back to the built-in Windows PowerShell 5.1 (`powershell.exe`). The desktop shortcut uses whichever supported host is available during setup. The old **Restore MPC-VR Maxine** shortcut is removed only after the replacement shortcut is successfully created.

The updater first looks for the small `MPCVR-Setup.zip`, which updates pinned dependencies as needed, then `MPCVR-Portable.zip` and `MpcVideoRenderer-Maxine-RIFE.zip`. For compatibility with older releases and existing shortcuts it falls back to `MpcVideoRenderer-Maxine.zip` and retains support for the legacy per-asset `.sha256` checksum convention when `SHA256SUMS.txt` is unavailable.

## What is installed where

| Player | How it finds the renderer | File the updater installs |
| --- | --- | --- |
| MPC-HC (standalone) | Loads `<player folder>\MPCVR\<renderer>` by path if present, whatever is registered | `<player folder>\MPCVR\MpcVideoRenderer64.ax`, or `MpcVideoRenderer.ax` for 32-bit |
| K-Lite Codec Pack (MPC-HC) | The same | `<pack>\MPC-HC64\MPCVR\MpcVideoRenderer64.ax`, `<pack>\MPC-HC\MPCVR\MpcVideoRenderer.ax` for its 32-bit MPC-HC, and any shared `<pack>\Filters\MPCVR` or `Filters64\MPCVR` renderer it has |
| MPC-BE | Only the registered DirectShow filter | `<player folder>\Filters\MpcVideoRenderer64.ax`, or `MpcVideoRenderer.ax` for 32-bit |

Players are found from their installers' records, the folder the player itself recorded, and the default folders under Program Files. K-Lite in a custom folder is recognized and its MPC-HC isn't treated as standalone. A portable copy that left no record isn't found: copy the renderer from `MPCVR-Portable.zip` using the table above.

## Registration

Only one file per architecture can be registered on a machine, and it is shared by every program using the filter. So the updater touches registration only when MPC-BE needs it:

- MPC-HC does not need registration, so with no MPC-BE it is left exactly as it was.
- If what is registered is already this exact build, for example K-Lite's copy that was just replaced, it stays registered there and MPC-BE loads that file.
- Otherwise, when a stock renderer elsewhere is registered or nothing is, MPC-BE's own copy is registered with `regsvr32`, for that architecture only. The result is read back.

The updater does not change the renderer a player is set to use. In MPC-BE choose **MPC Video Renderer** under **Options > Video**, in MPC-HC under **Options > Playback > Output**.

Settings, the Maxine runtime and the RIFE runtime are per user and are not per player, so they are shared by all players. Removing the player that owns the registration, such as K-Lite, can leave MPC-BE without a renderer; run the shortcut again.

## Safety

After verifying the selected renderer ZIP, the updater requests administrator access. The whole install is one transaction across all players. Before each replacement, the current renderer file is copied to a unique temporary backup, and source and destination SHA-256 hashes are compared after every copy. Finally it checks that each player's file, the registered one for MPC-BE, is this build. If anything fails, the previous registration is restored, every touched renderer is restored in reverse order with its original hash verified, and files and folders it added are removed, before the updater reports failure.

MPC-HC and MPC-BE must be closed while the files are replaced; the updater waits for them.

## Compatibility

Existing K-Lite installations keep the same files at the same paths, and their registration remains valid. The shortcut keeps its name and the updater keeps its folder. Shortcuts pinned elsewhere that still run `Update-KLiteMPCVR.ps1` keep working, since that name now forwards to `Update-MPCVR.ps1`.

Shortcuts installed before the lightweight release still run their old local updater script. Run the current `MPCVR-Setup.zip` once to replace that script; this cannot be done remotely just by changing release filenames. Shortcuts from a lightweight release already download the current setup, which replaces their script.

## Tests

`Test-PlayerInstall.ps1` runs in `dev.ps1 test` and in CI under both PowerShell hosts, on fixture folders with registration replaced by a fake. `Test-RealPlayers.ps1` installs the real MPC-HC, MPC-BE and K-Lite on a CI runner, runs the updater with real registration, and plays a clip in each to see which file the process loaded. It refuses to run outside CI.
