# MPCVR Maxine + RIFE Setup

`MPCVR-Maxine-RIFE-Setup.zip` is the recommended download for K-Lite Codec Pack users.

After extracting the ZIP, close MPC-HC and run:

`Install-MPCVR-Maxine-RIFE.cmd`

The one-click setup uses built-in Windows PowerShell 5.1 and performs four steps:

1. Installs the verified five-DLL NVIDIA Maxine VideoSuperRes runtime under `%LOCALAPPDATA%\MPCVR Maxine Runtime\nvvfx\libs` and sets the current user's `NV_VIDEO_EFFECTS_PATH`.
2. Detects the installed NVIDIA GPU compute capability, downloads only the matching RIFE TensorRT architecture pack from the exact immutable GitHub release, verifies it, and transactionally installs the RIFE 4.6 runtime/model under `%LOCALAPPDATA%\MPCVideoRenderer\RIFE`.
3. Installs the `Restore MPC-VR Maxine + RIFE` desktop shortcut for use after future K-Lite updates.
4. Requests administrator permission and transactionally installs the custom 32-bit and 64-bit MPC Video Renderer files into K-Lite.

Every declared file in `payload\PAYLOAD-SHA256SUMS.txt` is verified before any installer is allowed to change the system. Setup then runs the RIFE preflight, verifies the five Maxine runtime DLLs, and checks the user `NV_VIDEO_EFFECTS_PATH` value before reporting success.

On first playback, TensorRT may build a GPU-specific engine under `%LOCALAPPDATA%\MPCVideoRenderer\RIFE\cache`. Press `Ctrl+J` in MPC-HC to confirm the loaded Maxine runtime, `RIFE runtime: ready`, and `RIFE model: 4.6`.

The older `Install-MPCVR-Maxine.cmd` and `.ps1` filenames remain compatibility wrappers and forward to the Maxine + RIFE setup when the unified payload is present.

The TensorRT runtime supports NVIDIA Turing (compute capability 7.5), Ampere (8.6), Ada (8.9), and Blackwell (12.0). Architecture packs are no longer embedded in the setup ZIP. The installer detects all NVIDIA adapters and downloads only the union of packs required by the machine, so installation requires Internet access for the architecture-specific fetch.

`payload\THIRD-PARTY-NOTICES.txt` indexes the Maxine, CUDA, TensorRT, and RIFE model notice/license files bundled with the setup. Release assembly fails if any required notice class is missing.

## Release assets

Normal releases expose:

- `MPCVR-Maxine-RIFE-Setup.zip` for K-Lite users
- `MpcVideoRenderer-Maxine-RIFE.zip` for manual or portable installation
- `MPCVR-RIFE-sm75.zip`, `MPCVR-RIFE-sm86.zip`, `MPCVR-RIFE-sm89.zip`, and `MPCVR-RIFE-sm120.zip` as installer-consumed architecture packs
- `SHA256SUMS.txt` covering the public release ZIPs and architecture packs

`MpcVideoRenderer-Maxine.zip` remains temporarily as a byte-identical renderer alias so older installed restore shortcuts continue to work. Its legacy `.sha256` asset also remains temporarily for older updater scripts. The redundant `MPCVR-Maxine-Setup.zip` setup alias is no longer published.
