# MPCVR Unified Setup

This directory is the shared foundation for the eventual one-click MPC Video Renderer + NVIDIA Maxine + NvOFFRUC installer.

## Design rules

- Automatic mode chooses conservative starting settings and refines them with local calibration.
- Guided mode exposes simple Quality, Balanced, and Performance choices with warnings and user overrides.
- Advanced mode exposes the complete renderer configuration and never silently overwrites a manually locked profile.
- GPU-model detection is only an initial estimate. Measured performance on the current machine is authoritative.
- Recommendations remain separate for 30 -> 60 and 60 -> 120 workloads.
- Existing Maxine, NvOFFRUC, K-Lite updater, rollback, and checksum logic should be reused rather than duplicated.

## Current first slice

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

`profiles/profile.schema.json` defines the portable profile format for Automatic, Guided, and Advanced modes. It includes:

- machine fingerprinting
- source/output conditions
- Maxine and interpolation settings
- fallback behavior
- calibration measurements
- a `locked` flag that protects manually managed profiles

## Validation

Run:

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\UnifiedSetup\Test-MpcvrUnifiedSetup.ps1
```

The validation imports the module, checks the exported commands, parses the profile schema, captures a real Windows inventory, and verifies JSON round-tripping.

## Next slices

1. Unified setup orchestrator that reuses the existing Maxine runtime installer, NvOFFRUC runtime installer, renderer updater, backup, and rollback logic.
2. Profile storage, import/export, manual locking, and restore-defaults support.
3. A renderer-readable calibration/status channel so timing data does not have to be scraped from Ctrl+J.
4. Local 30 -> 60 and 60 -> 120 calibration runs with configurable timing headroom.
5. Automatic, Guided, and fully unlocked Advanced user interfaces.
6. Release packaging, checksum verification, diagnostics export, update, rollback, and uninstall.
