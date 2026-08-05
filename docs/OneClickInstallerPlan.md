# One-click MPCVR + Maxine + NvOFFRUC installer plan

## Goal

Ship the completed renderer stack in a form that non-technical MPC-HC users can install, verify, update, roll back, and remove without manually registering filters, extracting NVIDIA DLLs, editing environment variables, locating codec-pack folders, or hunting through download directories.

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
5. Prefer an existing verified runtime, then an embedded runtime where redistribution is permitted.
6. Bundle the verified slim Maxine runtime with the application package.
7. When NvOFFRUC is missing, open NVIDIA's official secured Optical Flow SDK download URL. The user signs in and accepts NVIDIA's license in the browser; setup watches the configured Windows Downloads folder, detects the completed compatible ZIP, validates its structure and pinned runtime hashes, caches it locally, extracts only the required runtime DLLs, and continues automatically.
8. Require manual SDK/archive selection only as a final fallback when the browser saves outside the configured Downloads folder, automatic detection times out, or the user explicitly disables assisted acquisition.
9. Never bypass NVIDIA authentication, license acceptance, or technical download restrictions.
10. Configure runtime locations without requiring manual environment-variable editing.
11. Choose a conservative starting preset from detected hardware capabilities.
12. Run a short local calibration for representative 30 -> 60 and 60 -> 120 workloads rather than relying only on a hardcoded GPU model table.
13. Measure Maxine time, NvOFFRUC time, total processing time, dropped/failed frames, pacing stability, and available timing headroom.
14. Store separate recommendations by source resolution, source frame rate, output resolution, and display refresh rate.
15. Offer one-click verification that reports Maxine, FRUC, runtime, target frame rate, measured frame rate, and timing-headroom status.
16. Provide one-click rollback, uninstall, and diagnostic export.
17. Publish checksums for every release artifact.
18. If combined Maxine + interpolation is unsupported or too slow on a system, show a clear confirmation dialog offering to reduce quality, reduce scale, disable Maxine, or disable interpolation instead of silently degrading playback.
19. Re-run calibration after a GPU, driver, display, renderer, Maxine-runtime, or NvOFFRUC-runtime change.
20. On GPUs that may benefit from a higher-resolution FRUC working surface, support an optional flow-resolution boost. Automatic mode must suppress this dedicated boost whenever active Maxine VSR already produces an equal-or-larger working resolution. Merely enabling Maxine is not sufficient: denoise/deblur-only operation or inactive VSR must not suppress the boost.

## Runtime acquisition policy

### Maxine

The release package includes the verified slim five-DLL Maxine runtime. Setup verifies the embedded archive and installed files before activation.

### NvOFFRUC

The full NVIDIA Optical Flow SDK is not mirrored in the GitHub release. NVIDIA requires Developer Program authentication and explicit SDK-license acceptance for the official download.

Default assisted flow:

1. Reuse a complete installed NvOFFRUC runtime when present.
2. Reuse a previously validated SDK ZIP from the local setup cache or Windows Downloads folder.
3. Otherwise open NVIDIA's official secured SDK 5.0.7 URL in the default browser.
4. Watch the Windows Downloads known folder for a newly completed compatible ZIP.
5. Validate the archive structure.
6. Extract the x64 `NvOFFRUC.dll` and `cudart64_110.dll` into staging.
7. Verify both runtime files against pinned SHA-256 hashes.
8. Cache the validated SDK ZIP under `%LOCALAPPDATA%\MPCVR Unified Setup\downloads` for repair/reinstallation.
9. Install transactionally and restore the prior runtime automatically on failure.

Advanced controls:

- provide an SDK ZIP or extracted SDK directory explicitly
- disable browser-assisted acquisition
- change the automatic wait duration
- deliberately allow unverified runtime files for experimentation, with a prominent warning

## Configuration modes

### Automatic

- Detect hardware and display capabilities.
- Run local calibration.
- Apply safe recommendations automatically.
- Adjust recommendations separately for 30 fps and 60 fps sources.
- Prefer stable pacing and timing headroom over the highest possible quality setting.
- Treat active Maxine VSR enlargement as the FRUC working-resolution boost when it already meets or exceeds the requested scale, avoiding duplicate upscale work.

### Guided

- Expose simple Quality, Performance, and Balanced choices.
- Explain the expected effect of each choice.
- Warn when the selected combination is unlikely to meet its target output frame rate.
- Allow the user to continue despite the warning.
- Explain when the dedicated FRUC flow-resolution boost is redundant because Maxine VSR is already enlarging the frame.

### Advanced

Expose the complete renderer configuration without artificial restrictions, including:

- Maxine operation, quality, mode, scale, oversampling, denoise, deblur, pipeline order, and GPU selection
- NvOFFRUC enable state, source-resolution limit, output-frame-rate limit, runtime path, and failure behavior
- FRUC flow-resolution boost: Off, Automatic, 1.33x, 1.5x, or 2x
- whether to permit a dedicated FRUC boost even when active Maxine VSR already provides an equal-or-larger working resolution
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

## Flow-resolution boost policy

The dedicated FRUC boost exists to enlarge the optical-flow working surface before interpolation. It is not automatically combined with Maxine VSR when Maxine already supplies the same or greater enlargement.

Automatic decision rule:

1. Determine whether Maxine VSR is active for the current frame and calculate its actual output scale.
2. Determine the calibrated FRUC working-scale request.
3. If active Maxine VSR scale is greater than or equal to the requested FRUC scale, use the Maxine output directly and disable the dedicated boost.
4. If Maxine is inactive, denoise/deblur-only, or upscales less than the requested FRUC scale, the dedicated boost may supply only the missing working resolution when calibration shows adequate headroom.
5. Advanced mode may force redundant operation for experimentation, but it must display the expected extra pixel cost and require explicit confirmation.

The decision must be based on the active processing path, not simply on whether the Maxine setting is enabled.

## Calibration principles

- GPU detection provides only an initial estimate; measured local performance is authoritative.
- Do not assume that every GPU with the same model performs identically.
- Account for driver version, clock behavior, thermals, power limits, background GPU load, output resolution, refresh rate, and source format.
- Require configurable timing headroom instead of accepting a preset that merely reaches the target in an ideal short test.
- Keep 30 -> 60 and 60 -> 120 recommendations separate.
- Retain a hardware capability table for sensible first-run defaults, but refine it with real measurements from the current machine.
- Calibrate flow-resolution boost settings independently for 30 -> 60 and 60 -> 120 workloads.
- Never assume a dedicated FRUC boost is useful when active Maxine VSR already provides the requested working resolution.

## Validation status

The assisted acquisition implementation is validated under Windows PowerShell 5.1 and PowerShell 7 for:

- compatible SDK ZIP recognition
- rejection of incompatible ZIPs
- automatic cache/Downloads reuse without opening the browser
- cache creation and revalidation
- rejection of tampered runtime DLLs
- explicit Advanced override for unverified runtime experimentation
- official NVIDIA URL format
- installer syntax and parameter contracts
- timezone-safe download timestamp filtering

Unified package workflow run `30995920905` built the renderer and assembled the setup successfully. Artifact `8926172380` has GitHub digest `sha256:c50b4dfc6cc646585c7b5568eea5b7e9053cdba0269208e8fa768b61d655d363`.

The downloaded public ZIP was independently extracted and verified:

- 51 files
- renderer payload checksum passed
- Maxine runtime payload checksum passed
- assisted acquisition module present
- public ZIP SHA-256: `df22878844f12bb60278e6bb5a4810746d899ca8f15c71702bf187af1ba72c99`

## Remaining runtime gate

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
- fresh assisted NVIDIA SDK acquisition from the official browser flow
- cached SDK reuse without another browser download
- timeout/manual-picker fallback
- rejection of incompatible or tampered SDK/runtime files
