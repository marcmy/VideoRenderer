# K-Lite MPC-VR Maxine + RIFE updater

K-Lite Codec Pack updates can overwrite the custom MPC Video Renderer build. This helper creates a desktop shortcut that restores the latest successful custom Maxine + RIFE build from this repository.

## Setup

1. Download this folder or the updater payload from the latest Maxine + RIFE release.
2. Run `Install-KLiteMPCVRUpdater.cmd` once.
3. Use the **Restore MPC-VR Maxine + RIFE** desktop shortcut after a K-Lite update.

The installer stores `Update-KLiteMPCVR.ps1` and `KLiteRendererInstall.psm1` under `%LOCALAPPDATA%\MPCVR Custom Updater`. It prefers PowerShell 7 (`pwsh.exe`) when it is available and automatically falls back to the built-in Windows PowerShell 5.1 (`powershell.exe`). The desktop shortcut uses whichever supported host is available during setup. The old **Restore MPC-VR Maxine** shortcut is removed only after the replacement shortcut is successfully created.

The updater first looks for `MpcVideoRenderer-Maxine-RIFE.zip`. For compatibility with older releases and existing shortcuts it falls back to `MpcVideoRenderer-Maxine.zip` and retains support for the legacy per-asset `.sha256` checksum convention when `SHA256SUMS.txt` is unavailable.

After verifying the selected renderer ZIP, the updater requests administrator access and transactionally replaces:

- `C:\Program Files (x86)\K-Lite Codec Pack\Filters\MPCVR\MpcVideoRenderer.ax`
- `C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax`

Before each replacement, the current renderer file is copied to a unique temporary backup. Source and destination SHA-256 hashes are compared after every copy. If either replacement or verification fails, every touched renderer is restored in reverse order and its original hash is verified before the updater reports failure.

MPC-HC must be closed while the files are replaced. Existing registration remains valid because the files are replaced at the same paths.
