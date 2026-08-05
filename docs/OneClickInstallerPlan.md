# One-click MPCVR + Maxine + NvOFFRUC installer plan

## Goal

Ship the completed renderer stack in a form that non-technical MPC-HC users can install, verify, update, roll back, and remove without manually registering filters, extracting NVIDIA DLLs, editing environment variables, or locating codec-pack folders.

The default experience should be automatic and safe, while advanced users retain full control over every renderer option.

## Required components

- MPC Video Renderer custom x64/x86 binaries
- NVIDIA Maxine runtime setup
- NVIDIA NvOFFRUC runtime setup
- GPU-aware, locally calibrated presets for Maxine-only, interpolation-only, and combined operation
- automatic MPC-HC and K-Lite path detection
- filter registration and previous-version backup
- rollback and full uninstall
- profile export/import and restore-defaults support

## Installer behavior

1. Detect Windows architecture, NVIDIA GPU model, GPU architecture, VRAM, driver, supported RTX/NVOFA capabilities, display resolution, and refresh rate.
2. Detect installed MPC-HC/K-Lite locations and existing MPCVR registration.
3. Install the correct renderer binaries and preserve the previous version.
4. Detect existing Maxine and NvOFFRUC runtimes.
5. Where NVIDIA licensing prevents bundling runtime files, accept the official installer/SDK archive from the user and perform extraction and configuration automatically.
6. Configure runtime locations without requiring manual environment-variable editing.
7. Choose a conservative starting preset from detected hardware capabilities.
8. Run a short local calibration for representative 30 -> 60 and 60 -> 120 workloads rather than relying only on a hardcoded GPU model table.
9. Measure Maxine time, NvOFFRUC time, total processing time, dropped/failed frames, pacing stability, and available timing headroom.
10. Store separate recommendations by source resolution, source frame rate, output resolution, and display refresh rate.
11. Offer one-click verification that reports Maxine, FRUC, runtime, target frame rate, measured frame rate, and timing-headroom status.
12. Provide one-click rollback, uninstall, and diagnostic export.
13. Publish checksums for every release artifact.
14. If combined Maxine + interpolation is unsupported or too slow on a system, show a clear confirmation dialog offering to reduce quality, reduce scale, disable Maxine, or disable interpolation instead of silently degrading playback.
15. Re-run calibration after a GPU, driver, display, renderer, Maxine-runtime, or NvOFFRUC-runtime change.

## Configuration modes

### Automatic

- Detect hardware and display capabilities.
- Run local calibration.
- Apply safe recommendations automatically.
- Adjust recommendations separately for 30 fps and 60 fps sources.
- Prefer stable pacing and timing headroom over the highest possible quality setting.

### Guided

- Expose simple Quality, Performance, and Balanced choices.
- Explain the expected effect of each choice.
- Warn when the selected combination is unlikely to meet its target output frame rate.
- Allow the user to continue despite the warning.

### Advanced

Expose the complete renderer configuration without artificial restrictions, including:

- Maxine operation, quality, mode, scale, oversampling, denoise, deblur, pipeline order, and GPU selection
- NvOFFRUC enable state, source-resolution limit, output-frame-rate limit, runtime path, and failure behavior
- combined Maxine + NvOFFRUC operation
- renderer scaling, presentation, synchronization, color, HDR, and processing options
- diagnostic overlays and detailed timing information
- benchmark duration, required timing headroom, and recommendation thresholds
- automatic fallback behavior and whether warnings should be shown

Advanced users must be able to:

- ignore automatic recommendations
- force any supported combination
- disable automatic adjustment
- save named per-machine and per-content profiles
- export and import profiles
- duplicate and edit generated presets
- restore automatic recommendations or factory defaults at any time

Automatic calibration must never overwrite a manually locked advanced profile without explicit confirmation.

## Calibration principles

- GPU detection provides only an initial estimate; measured local performance is authoritative.
- Do not assume that every GPU with the same model performs identically.
- Account for driver version, clock behavior, thermals, power limits, background GPU load, output resolution, refresh rate, and source format.
- Require configurable timing headroom instead of accepting a preset that merely reaches the target in an ideal short test.
- Keep 30 -> 60 and 60 -> 120 recommendations separate.
- Retain a hardware capability table for sensible first-run defaults, but refine it with real measurements from the current machine.

## Runtime gate

Do not finalize distribution until all of these pass:

- Maxine-only playback
- NvOFFRUC-only 30 -> 60 and 60 -> 120 playback
- combined Maxine -> NvOFFRUC playback
- repeated seeks and seek-bar dragging
- pause/resume and pause -> seek -> resume
- file switching, stop, and player shutdown
- fallback behavior on unsupported sources and hardware
- automatic calibration and recommendation generation
- guided-mode warnings and user overrides
- advanced-mode persistence, profile export/import, and restore-defaults behavior
