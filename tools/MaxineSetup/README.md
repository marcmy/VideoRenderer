# MPCVR Maxine Setup

`MPCVR-Maxine-Setup.zip` is the recommended download for K-Lite Codec Pack users.

After extracting the ZIP, close MPC-HC and run:

`Install-MPCVR-Maxine.cmd`

The one-click setup uses built-in Windows PowerShell 5.1 and performs three steps:

1. Installs the verified five-DLL NVIDIA Maxine VideoSuperRes runtime under `%LOCALAPPDATA%\MPCVR Maxine Runtime\nvvfx\libs` and sets the current user's `NV_VIDEO_EFFECTS_PATH`.
2. Installs the `Restore MPC-VR Maxine` desktop shortcut for use after future K-Lite updates.
3. Requests administrator permission and installs the custom 32-bit and 64-bit MPC Video Renderer files into K-Lite.

The release package contains both the renderer and runtime payloads, so setup does not need to download additional components after the ZIP is downloaded.

## Release assets

Normal releases expose only two ZIP downloads:

- `MPCVR-Maxine-Setup.zip` for K-Lite users
- `MpcVideoRenderer-Maxine.zip` for manual or portable installation

`SHA256SUMS.txt` contains the hashes for both public ZIPs. A small legacy renderer checksum asset may remain temporarily so updater scripts installed by older releases continue to work.

The Maxine runtime remains embedded inside the setup ZIP. Release automation extracts and verifies it from the previous latest setup when producing the next immutable release. The first unified release can migrate from the older standalone runtime assets.
