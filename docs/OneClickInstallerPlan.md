# One-click MPCVR + Maxine + NvOFFRUC installer plan

## Product goal

The normal user experience is deliberately simple:

```text
Install MPC Video Renderer + NVIDIA Maxine + Frame Interpolation
[ Install / Update ]
```

The installer detects the machine, acquires the permitted runtime dependencies, installs all three components transactionally, verifies the result, and keeps a rollback point. Users should not need to understand SDK ZIP layouts, profiles, calibration classes, renderer registry values, or separate setup modes.

## Default flow

1. Detect Windows architecture, NVIDIA GPU, driver, VRAM, display, and supported K-Lite/MPC-HC targets.
2. Show a short system/component status summary.
3. Present one primary **Install / Update** action.
4. Snapshot current renderer files, runtime directories, and environment variables.
5. Install/update the correct MPC Video Renderer binaries.
6. Install/verify the embedded slim Maxine runtime.
7. Preserve an existing working NvOFFRUC runtime or assist with NVIDIA's official SDK acquisition when missing.
8. Verify all installed components.
9. Restore the previous state automatically on failure.
10. Retain the successful pre-install snapshot for one-click rollback.

No profile name, calibration video, recommendation priority, or mode selection appears on the main screen.

## Runtime acquisition

### Maxine

Bundle the verified slim Maxine runtime in the release package and verify its files before activation.

### NvOFFRUC

Do not mirror the full Optical Flow SDK. NVIDIA requires Developer Program authentication and explicit license acceptance.

Assisted flow:

1. Preserve an installed working runtime.
2. Reuse a compatible cached or Downloads SDK ZIP when available.
3. Otherwise open NVIDIA's official secured SDK page.
4. Let the user sign in and accept NVIDIA's license.
5. Watch the Windows Downloads known folder for the completed ZIP.
6. Validate archive structure and pinned runtime hashes.
7. Extract only the required runtime DLLs.
8. Cache the validated SDK locally.
9. Continue setup automatically.

Manual ZIP selection is only an offline, timeout, nonstandard-download-location, or advanced fallback.

## Advanced access

Advanced controls remain available without cluttering normal setup.

The installer-specific **Advanced...** dialog may expose:

- existing SDK ZIP path
- download wait duration
- component skip switches
- telemetry
- rollback
- setup-tools folder

Renderer playback settings remain available through MPC Video Renderer's native property pages.

Calibration, recommendation, profile, and registry-application tools remain packaged for technical users and future automatic tuning, but are not presented as competing installation paths.

## Automatic tuning direction

Automatic calibration remains useful after the installation experience is stable, but it should be a background or post-install feature rather than a prerequisite for installation.

Principles:

- detected GPU provides only a starting estimate
- measured local performance is authoritative
- keep 30 -> 60 and 60 -> 120 measurements separate
- preserve manually locked advanced settings
- never silently reduce quality or disable a feature without a clear explanation
- avoid a dedicated FRUC flow-resolution boost when active Maxine VSR already provides equal-or-greater enlargement
- allow advanced users to override every recommendation

## Runtime gate

Before release, validate:

- fresh install over supported K-Lite/MPC-HC layouts
- update over an existing custom renderer
- Maxine-only playback
- NvOFFRUC-only 30 -> 60 and 60 -> 120 playback
- combined Maxine -> NvOFFRUC playback
- repeated seeking, pause/resume, file switching, stop, and shutdown
- fresh browser-assisted NVIDIA SDK acquisition
- cached SDK reuse
- timeout/manual-picker fallback
- transactional failure rollback
- manual rollback
- simple main GUI on Windows PowerShell 5.1 and PowerShell 7
- no profile/calibration/setup-mode decisions required for normal installation
