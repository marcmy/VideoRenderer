# MPC-VR Maxine runtime installer

This helper installs a known-good NVIDIA Video Effects VideoSuperRes runtime for the custom MPC Video Renderer build without requiring PowerShell 7 or administrator access.

## End-user setup

1. Download `MaxineRuntimeInstaller-Setup.zip` from the latest Maxine release.
2. Extract the ZIP.
3. Close MPC-HC.
4. Run `Install-MPCVRMaxineRuntime.cmd`.
5. Reopen MPC-HC and press **Ctrl+J** while a video is playing. The statistics should report the loaded Maxine runtime path.

The launcher uses the built-in Windows PowerShell 5.1 executable at:

`%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe`

The installer downloads `MPCVR-Maxine-Runtime.zip` and its SHA-256 file from the repository's latest release, verifies the archive, installs it under:

`%LOCALAPPDATA%\MPCVR Maxine Runtime\nvvfx\libs`

It then creates or updates the current user's environment variable:

`NV_VIDEO_EFFECTS_PATH=%LOCALAPPDATA%\MPCVR Maxine Runtime\nvvfx\libs`

No system-wide PATH modification is required.

## Core runtime files

The tested runtime bundle intentionally contains only the five files MPC-VR requires before attempting to load VideoSuperRes:

- `NVCVImage.dll`
- `NVVideoEffects.dll`
- `nvngxruntime.dll`
- `nvngx_vsr.dll`
- `nvVFXVideoSuperRes.dll`

This keeps the unpacked runtime around 46 MiB instead of carrying hundreds of MiB of optional TensorRT and NPP libraries. The five-file bundle was runtime-tested in MPC-HC with Maxine active, including the new output-oversampling path, and worked without the excluded libraries.

The installer still copies every file present in the verified bundle. If testing proves that a specific optional dependency is required, it can be added without redesigning the installer.

## Publishing a known-good runtime bundle

`Export-MPCVRMaxineRuntime.ps1` reads the runtime selected by `NV_VIDEO_EFFECTS_PATH` and creates:

- `MPCVR-Maxine-Runtime.zip`
- `MPCVR-Maxine-Runtime.zip.sha256`

By default, the exporter copies only the five core DLLs plus obvious license, notice, EULA, and README files. It writes `runtime-manifest.json` with packaged and excluded file sizes so the candidate remains auditable.

For diagnostic comparison only, `-IncludeAllFiles` recreates the old full-directory bundle:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Export-MPCVRMaxineRuntime.ps1 -IncludeAllFiles
```

Review the applicable NVIDIA software and model licenses and include all required license and notice files before publishing any runtime binaries.

The release workflow carries an existing verified runtime bundle forward from the previous latest release, so the runtime does not need to be rebuilt for every renderer release.

## Security behavior

- The downloaded archive is rejected unless its SHA-256 matches the separately published checksum.
- The five required DLLs are checked before and after installation.
- An existing runtime is moved aside and restored if installation fails.
- MPC-HC must be closed so loaded DLLs cannot interfere with replacement.
- Only the current user's environment variable is changed.
