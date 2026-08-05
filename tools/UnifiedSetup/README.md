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
.\tools\UnifiedSetup\Install-MpcvrUnified.cmd `
  -NvOffrucSdkPath "C:\Path\To\Optical_Flow_SDK_5.0.7.zip"
```

When renderer and Maxine payloads are not embedded, their existing installers can obtain the latest published files. NvOFFRUC still requires the official NVIDIA Optical Flow SDK archive unless a complete runtime is already installed.

Optional component switches are available for controlled testing:

```text
-SkipRenderer
-SkipMaxine
-SkipNvOffruc
```

`-Mode Automatic`, `-Mode Guided`, and `-Mode Advanced` are recorded in the transaction now. Applying calibrated recommendations for those modes is a later slice; the installer does not yet rewrite renderer settings based on the selected mode.

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

## Validation

Run:

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\UnifiedSetup\Test-MpcvrUnifiedSetup.ps1
```

The Windows validation:

- imports both setup modules
- checks exported commands and PowerShell syntax
- parses the profile schema
- captures a real system inventory and verifies JSON round-tripping
- creates a disposable renderer/runtime snapshot
- mutates existing files and creates a new runtime path
- restores the original content and verifies file hashes
- confirms that paths absent before the transaction are removed
- validates the unified installer, rollback entry point, Maxine installer, renderer updater, and NvOFFRUC installer

## Remaining slices

1. Profile storage, import/export, manual locking, and restore-defaults commands.
2. A renderer-readable calibration/status channel so timing data does not have to be scraped from Ctrl+J.
3. Local 30 -> 60 and 60 -> 120 calibration runs with configurable timing headroom.
4. Recommendation generation using GPU capability as a starting estimate and local measurements as authority.
5. Automatic, Guided, and fully unlocked Advanced user interfaces.
6. Final release packaging, diagnostics export, update, uninstall, and retention/cleanup policy for rollback snapshots.
