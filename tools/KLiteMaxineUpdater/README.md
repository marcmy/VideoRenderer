# K-Lite MPC-VR Maxine updater

K-Lite Codec Pack updates can overwrite the custom MPC Video Renderer build. This helper creates a desktop shortcut that restores the latest successful custom Maxine build from this repository.

## Setup

1. Download this folder or `KLiteMaxineUpdater-Setup.zip` from the latest Maxine release.
2. Run `Install-KLiteMPCVRUpdater.cmd` once.
3. Use the **Restore MPC-VR Maxine** desktop shortcut after a K-Lite update.

The installer prefers PowerShell 7 (`pwsh.exe`) when it is available and automatically falls back to the built-in Windows PowerShell 5.1 (`powershell.exe`). The desktop shortcut uses whichever supported host is available during setup.

The updater downloads the assets from the repository's latest immutable Maxine release, verifies the published SHA-256 checksum, requests administrator access, and replaces:

- `C:\Program Files (x86)\K-Lite Codec Pack\Filters\MPCVR\MpcVideoRenderer.ax`
- `C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax`

MPC-HC must be closed while the files are replaced. Existing registration remains valid because the files are replaced at the same paths.
