# MPCVR Maxine + RIFE Setup

`MPCVR-Maxine-RIFE-Setup.zip` is the recommended download for K-Lite Codec Pack users.

After extracting the ZIP, close MPC-HC and run:

`Install-MPCVR-Maxine-RIFE.cmd`

The one-click setup uses built-in Windows PowerShell 5.1 and performs four steps:

1. Installs the verified five-DLL NVIDIA Maxine VideoSuperRes runtime under `%LOCALAPPDATA%\MPCVR Maxine Runtime\nvvfx\libs` and sets the current user's `NV_VIDEO_EFFECTS_PATH`.
2. Detects the installed NVIDIA GPU compute capability and transactionally installs the matching RIFE 4.6 TensorRT runtime, model, and builder resources under `%LOCALAPPDATA%\MPCVideoRenderer\RIFE`.
3. Installs the `Restore MPC-VR Maxine + RIFE` desktop shortcut for use after future K-Lite updates.
4. Requests administrator permission and transactionally installs the custom 32-bit and 64-bit MPC Video Renderer files into K-Lite.

Every declared file in `payload\PAYLOAD-SHA256SUMS.txt` is verified before any installer is allowed to change the system. Setup then runs the RIFE preflight, verifies the five Maxine runtime DLLs, and checks the user `NV_VIDEO_EFFECTS_PATH` value before reporting success.

On first playback, TensorRT may build a GPU-specific engine under `%LOCALAPPDATA%\MPCVideoRenderer\RIFE\cache`. Press `Ctrl+J` in MPC-HC to confirm the loaded Maxine runtime, `RIFE runtime: ready`, and `RIFE model: 4.6`.

The older `Install-MPCVR-Maxine.cmd` and `.ps1` filenames remain compatibility wrappers and forward to the Maxine + RIFE setup when the unified payload is present.

## Release assets

Normal releases expose:

- `MPCVR-Maxine-RIFE-Setup.zip` for K-Lite users
- `MpcVideoRenderer-Maxine-RIFE.zip` for manual or portable installation
- `SHA256SUMS.txt` covering the public release ZIPs

For one migration release, byte-identical `MPCVR-Maxine-Setup.zip` and `MpcVideoRenderer-Maxine.zip` aliases remain available so old documentation and installed restore shortcuts continue to work. The legacy renderer `.sha256` asset also remains temporarily for older updater scripts.
