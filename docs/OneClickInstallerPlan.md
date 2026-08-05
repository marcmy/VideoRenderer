# One-click MPCVR + Maxine + NvOFFRUC installer plan

## Goal

Ship the completed renderer stack in a form that non-technical MPC-HC users can install, verify, update, roll back, and remove without manually registering filters, extracting NVIDIA DLLs, editing environment variables, or locating codec-pack folders.

## Required components

- MPC Video Renderer custom x64/x86 binaries
- NVIDIA Maxine runtime setup
- NVIDIA NvOFFRUC runtime setup
- conservative presets for Maxine-only, interpolation-only, and combined operation
- automatic MPC-HC and K-Lite path detection
- filter registration and previous-version backup
- rollback and full uninstall

## Installer behavior

1. Detect Windows architecture, NVIDIA GPU, driver, and supported RTX/NVOFA capabilities.
2. Detect installed MPC-HC/K-Lite locations and existing MPCVR registration.
3. Install the correct renderer binaries and preserve the previous version.
4. Detect existing Maxine and NvOFFRUC runtimes.
5. Where NVIDIA licensing prevents bundling runtime files, accept the official installer/SDK archive from the user and perform extraction and configuration automatically.
6. Configure runtime locations without requiring manual environment-variable editing.
7. Apply a safe default preset based on GPU capability and display refresh rate.
8. Offer one-click verification that reports Maxine, FRUC, runtime, and rendered-frame-rate status.
9. Provide one-click rollback, uninstall, and diagnostic export.
10. Publish checksums for every release artifact.

## Runtime gate

Do not finalize distribution until all of these pass:

- Maxine-only playback
- NvOFFRUC-only 30→60 and 60→120 playback
- combined Maxine→NvOFFRUC playback
- repeated seeks and seek-bar dragging
- pause/resume and pause→seek→resume
- file switching, stop, and player shutdown
- fallback behavior on unsupported sources and hardware
