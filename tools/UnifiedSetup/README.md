# MPCVR Unified Setup

MPCVR Unified Setup installs and updates the complete enhanced playback stack:

- custom MPC Video Renderer
- NVIDIA Maxine video enhancement
- NVIDIA NvOFFRUC frame interpolation

## Default experience

Run the root-level launcher:

```text
MPCVR-Unified-Setup.cmd
```

The main window intentionally exposes one primary action:

```text
Install / Update
```

Setup then:

1. detects the NVIDIA GPU and supported K-Lite/MPC-HC installation
2. snapshots the current renderer and runtime state
3. installs or updates MPC Video Renderer
4. installs or verifies the embedded Maxine runtime
5. preserves an existing working NvOFFRUC runtime, or assists with NVIDIA's official SDK download when it is missing
6. verifies all installed components
7. automatically restores the previous state if the transaction fails
8. keeps a persistent rollback point after success

The first screen does not expose profiles, calibration videos, recommendation priorities, or separate Automatic/Guided/Advanced installation paths. Those systems remain available as optional tooling, but they are not part of normal installation.

## NVIDIA Optical Flow SDK acquisition

NVIDIA requires Developer Program authentication and license acceptance before downloading the Optical Flow SDK. The full SDK is therefore not mirrored in the GitHub package.

When NvOFFRUC is missing, setup:

1. reuses a compatible SDK ZIP from the local cache or Downloads folder when available
2. otherwise opens NVIDIA's official secured SDK page
3. waits while the user signs in and accepts NVIDIA's license
4. detects the completed ZIP in the Windows Downloads known folder
5. validates the archive structure
6. extracts only `NvOFFRUC.dll` and `cudart64_110.dll`
7. verifies pinned SHA-256 hashes
8. caches the validated SDK locally for repair or reinstallation
9. continues the installation automatically

Manual SDK selection is only a fallback for offline use, a nonstandard browser download location, or explicit advanced use.

## Advanced setup options

The secondary **Advanced...** dialog contains only installer-specific controls:

- use an existing Optical Flow SDK ZIP
- change the official-download wait time
- skip MPC Video Renderer
- skip Maxine
- skip frame interpolation
- open live renderer telemetry
- roll back the latest installation
- open the setup tools folder

Renderer quality, Maxine, interpolation, synchronization, HDR, and other playback settings remain customizable through MPC Video Renderer's native property pages. Calibration/profile command-line tools remain in the package for technical users and future automatic tuning work.

## Rollback

Successful setup retains the pre-install snapshot under:

```text
%LOCALAPPDATA%\MPCVR Unified Setup\backups
```

Use **Advanced... -> Rollback last install**, or run:

```text
tools\UnifiedSetup\Restore-MpcvrUnifiedBackup.cmd
```

## Optional technical tools

The package still contains the underlying tools for development and advanced use:

- `Get-MpcvrSystemProfile.ps1`
- `Get-MpcvrRendererTelemetry.ps1`
- `Calibrate-Mpcvr.cmd`
- `Recommend-Mpcvr.cmd`
- `AutoTune-Mpcvr.cmd`
- `Manage-MpcvrProfiles.cmd`
- `Apply-MpcvrProfile.cmd`

These are not required to install or update the renderer stack.

## Validation

CI validates:

- simplified GUI loading and status formatting under Windows PowerShell 5.1 and PowerShell 7
- renderer x86/x64 compilation
- K-Lite target detection and updater behavior
- Maxine runtime packaging and validation
- assisted NvOFFRUC SDK recognition, cache reuse, and runtime hash enforcement
- transactional filesystem backup and rollback
- unified ZIP assembly and embedded payload checksums
- telemetry, calibration, recommendation, profile, and renderer-settings tooling independently of the main install screen
