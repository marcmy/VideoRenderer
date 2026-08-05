# MPCVR Unified Setup

This directory contains the shared foundation for the one-click MPC Video Renderer + NVIDIA Maxine + NvOFFRUC installer.

## Design rules

- Automatic mode chooses conservative starting settings and refines them with local calibration.
- Guided mode exposes simple Quality, Balanced, and Performance choices with warnings and user overrides.
- Advanced mode exposes the complete renderer configuration and never silently overwrites a manually locked profile.
- GPU-model detection is only an initial estimate. Measured performance on the current machine is authoritative.
- Recommendations remain separate for 30 -> 60 and 60 -> 120 workloads.
- Existing Maxine, NvOFFRUC, K-Lite updater, rollback, and checksum logic is reused rather than copied into unrelated installers.

## System inventory and profiles

`MpcvrSetup.Common.psm1` captures a versioned system inventory containing:

- NVIDIA GPU name, driver, VRAM, PCI/PNP identity, active output size, and refresh information when available
- connected display information
- known K-Lite MPC-HC renderer targets and installed renderer versions
- Maxine and NvOFFRUC runtime paths, required-file status, and missing files
- Windows and PowerShell environment information

`Get-MpcvrSystemProfile.ps1` writes the inventory to:

```text
%LOCALAPPDATA%\MPCVR Unified Setup\system-profile.json
```

An alternate path can be supplied with `-OutputPath`.

`profiles/profile.schema.json` defines the portable profile format for Automatic, Guided, and Advanced modes. It includes machine fingerprinting, source/output conditions, renderer settings, fallback behavior, calibration measurements, and a `locked` flag that protects manually managed profiles.

`Manage-MpcvrProfiles.cmd` supports:

```text
List, Create, Show, Validate, Import, Export, Duplicate,
Lock, Unlock, Delete, RestoreDefaults
```

Locked profiles cannot be overwritten or deleted without an explicit override. Arbitrary Advanced-mode properties survive import, export, duplication, and JSON round trips.

## Transactional installer

`Install-MpcvrUnified.cmd` launches `Install-MpcvrUnified.ps1`, which:

1. validates the renderer, Maxine, and NvOFFRUC installer entry points
2. detects current runtimes and supported K-Lite renderer targets
3. elevates once when administrative access is required
4. closes safely if MPC-HC is still running
5. creates a persistent pre-install snapshot of:
   - active and default Maxine runtime locations
   - active and default NvOFFRUC runtime locations
   - each detected or potential K-Lite renderer file
   - `NV_VIDEO_EFFECTS_PATH` and `NV_OFFRUC_PATH`
6. runs the existing component installers in sequence
7. verifies the resulting runtime files and renderer targets
8. retains the snapshot as a one-click rollback point
9. automatically restores the old state if any component fails midway

The renderer updater installs only the x86 and/or x64 K-Lite targets that actually exist. It no longer requires both layouts.

### Current install command

Close MPC-HC, then run:

```powershell
.\tools\UnifiedSetup\Install-MpcvrUnified.cmd
```

Renderer and the verified slim Maxine runtime are embedded in the unified release package. When NvOFFRUC is not already installed and no SDK path is supplied, setup opens NVIDIA's official secured Optical Flow SDK download page. The user signs in and accepts NVIDIA's license in the browser; setup watches the Windows Downloads folder, recognizes the completed compatible ZIP, validates and caches it, extracts only the required runtime files, and continues automatically.

Manual `-NvOffrucSdkPath` selection remains available as an offline or Advanced fallback. `-DisableOfficialDownload` skips the browser-assisted path. `-OfficialDownloadWaitMinutes` changes the default 15-minute detection window. `-AllowUnverifiedRuntimeFiles` is an Advanced-only testing escape hatch and emits warnings.

Optional component switches are available for controlled testing:

```text
-SkipRenderer
-SkipMaxine
-SkipNvOffruc
```

`-Mode Automatic`, `-Mode Guided`, and `-Mode Advanced` are recorded in the transaction and used by the calibration/profile tools.

## First-run GUI hardening

The first packaged run exposed assumptions that the original form-only validation did not execute. The GUI now:

- reads GPU VRAM from the inventory's `MemoryMiB` property
- reads display size from `ScreenWidth` and `ScreenHeight`
- treats optional inventory properties as optional under strict mode
- shows safe fallback text instead of aborting status refresh
- resolves an omitted profile root to `%LOCALAPPDATA%\MPCVR Unified Setup\profiles`
- catches profile-list refresh errors inside the WinForms callback
- uses ASCII `Browse...`, `30 -> 60`, and `60 -> 120` labels to avoid PowerShell/WinForms encoding corruption

`Start-MpcvrUnifiedSetup.ps1 -ValidateOnly` now creates and formats a realistic synthetic inventory and resolves/lists the default profile root under both Windows PowerShell 5.1 and PowerShell 7.

## Rollback

The latest successful pre-install snapshot is recorded under:

```text
%LOCALAPPDATA%\MPCVR Unified Setup\backups
```

Restore it with:

```powershell
.\tools\UnifiedSetup\Restore-MpcvrUnifiedBackup.cmd
```

A specific snapshot can be selected with `-BackupPath`.

## Renderer telemetry

The custom renderer publishes a versioned, process-local shared-memory block named:

```text
Local\MPCVR.UnifiedSetup.Telemetry.<process-id>
```

It contains renderer state and timing counters only. It intentionally excludes filenames, media paths, titles, and other user content.

Read the current block with:

```powershell
.\tools\UnifiedSetup\Get-MpcvrRendererTelemetry.ps1 -WaitSeconds 10
```

Published data includes source/output dimensions, source/target/measured FPS, Maxine pass timings, NvOFFRUC timing, combined processing time, source-frame budget, timing headroom, and dropped/skipped/failed-frame counters.

## Local calibration

Start playback with the unified renderer build, then run:

```powershell
.\tools\UnifiedSetup\Calibrate-Mpcvr.cmd `
  -WarmupSeconds 3 `
  -DurationSeconds 12 `
  -ProfileName "My calibrated 60 to 120 profile"
```

Calibration records median and percentile timings, pacing stability, frame-counter deltas, and timing headroom. Reports are stored under:

```text
%LOCALAPPDATA%\MPCVR Unified Setup\calibrations
```

The verdict is one of:

- **Stable**: pacing, frame counters, and configured timing headroom passed.
- **Marginal**: playback is close to the target but lacks comfortable headroom.
- **Unstable**: pacing, errors, or timing headroom failed.

## Recommendations

Generate ranked choices from a calibration report:

```powershell
.\tools\UnifiedSetup\Recommend-Mpcvr.cmd `
  -CalibrationPath "C:\Path\To\calibration.json" `
  -Priority Balanced `
  -SaveRecommendedProfile
```

Priorities are:

- **Balanced**: starts with low-impact reductions and, when a whole stage must be disabled, considers the measured Maxine and NvOFFRUC costs.
- **Smoothness**: preserves frame interpolation as long as possible.
- **Quality**: preserves Maxine enhancement as long as possible.

A changed recommendation is always marked **requires recalibration**. The tool never claims that lowering one setting is guaranteed to solve the workload without measuring it again.

Candidate reductions can include:

1. disable Maxine oversampling
2. disable extra denoise/deblur passes
3. lower Maxine quality by one level
4. lower the Maxine scale cap
5. disable Maxine while preserving interpolation
6. disable interpolation while preserving Maxine

Stable measured settings can be retained without another calibration pass.

## Safe renderer-settings application

Profiles map directly to MPCVR's existing values under:

```text
HKCU\Software\MPCVideoRenderer
```

No parallel renderer configuration system is introduced.

Preview a profile without changing anything:

```powershell
.\tools\UnifiedSetup\Apply-MpcvrProfile.cmd `
  -Action Preview `
  -ProfileName "My calibrated 60 to 120 profile"
```

Apply it after closing MPC-HC:

```powershell
.\tools\UnifiedSetup\Apply-MpcvrProfile.cmd `
  -Action Apply `
  -ProfileName "My calibrated 60 to 120 profile"
```

Every apply operation:

- computes an exact before/after diff
- writes a timestamped JSON backup
- changes only recognized Maxine and frame-interpolation values
- preserves unrelated and unknown registry values
- verifies every requested DWORD after writing
- automatically restores the backup if verification fails

Restore a settings backup with:

```powershell
.\tools\UnifiedSetup\Apply-MpcvrProfile.cmd `
  -Action Restore `
  -BackupPath "C:\Path\To\renderer-settings-backup.json"
```

Applying a locked profile is allowed; the lock protects the profile itself from silent modification. Automatic calibration and recommendation generation still cannot overwrite that locked profile without explicit permission.

## Validation

The Windows CI suites cover:

- PowerShell 5.1 and PowerShell 7 parsing and execution
- system inventory and JSON round trips
- transactional filesystem snapshots and rollback
- profile import/export/duplicate/lock/delete behavior
- renderer telemetry shared-memory self-tests
- calibration percentile and report helpers
- renderer settings dry-run/apply/verify/backup/restore against a disposable registry key
- preservation of unknown registry values
- synthetic Stable, Marginal, and Unstable recommendations for Balanced, Smoothness, and Quality priorities
- assisted NvOFFRUC SDK ZIP recognition, cache reuse, tamper rejection, and installer contracts
- first-run inventory formatting and default profile-root resolution
- x86/x64 renderer compilation and security scanning

## Remaining major slices

1. Complete real-machine Automatic and Guided calibration runs on representative 30 fps and 60 fps sources.
2. Implement and calibrate the optional FRUC flow-resolution boost.
3. Add per-content/profile selection rules rather than one manually selected global renderer profile.
4. Finish diagnostics export, uninstall, and retention/cleanup policy for rollback snapshots.
