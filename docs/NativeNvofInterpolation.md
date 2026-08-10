# Native NVIDIA Optical Flow frame interpolation

## Status

The native D3D11 NVOF backend is now the working frame-interpolation implementation on `feature/native-nvof-interpolation`.

The validated runtime path requires only:

- an NVIDIA GPU/driver that exposes the required NVOF D3D11 capabilities
- MPC Video Renderer itself

It does **not** require the Optical Flow SDK ZIP, `NvOFFRUC.dll`, CUDA, or `cudart64_110.dll` for frame interpolation.

The proprietary NvOFFRUC path remains useful as a development quality reference, especially for same-frame-pair replay diagnostics, but is not part of the native runtime path.

## Production pipeline

For each pair of processed source frames:

1. NVOF generates forward and backward optical flow in one D3D11 execute operation.
2. Production deliberately uses the **4x4 NVOF output grid** on every supported GPU generation. The dense reconstruction and quality thresholds are validated for this grid and must not silently switch to a different grid on newer GPUs.
3. Forward/backward consistency validation marks unreliable coarse flow cells.
4. A GPU jump-flood pass fills rejected coarse cells from the nearest valid motion seeds.
5. A joint edge-aware upsampler reconstructs a full-resolution `R32G32_FLOAT` motion field, guided by the next real frame so separate motion layers are less likely to be averaged together.
6. The midpoint is synthesized primarily as a one-sided warp of the next real frame.
7. A whole-frame GPU quality gate rejects catastrophic pairs. If at least 25% of the coarse NVOF cells simultaneously exceed 20 pixels of motion and 20 pixels of forward/backward inconsistency, the inserted midpoint repeats the previous real frame instead of presenting badly distorted geometry.

The quality-gate decision stays GPU-resident; there is no CPU flow readback in the normal render path.

## Why the whole-frame quality gate exists

A same-pair replay through the legacy NVIDIA NvOFFRUC runtime provided the key reference result for a difficult Apex frame pair:

```text
Prime process: 1.437 ms, repeated=no
Midpoint process: 16.6276 ms, repeated=yes
FRUC REPLAY RESULT: PASS
```

In other words, the proprietary reference implementation also declined to present an interpolated midpoint for that pair. The native backend now follows the same general policy: interpolate usable pairs, but prefer a real frame over severe synthetic distortion when the motion field is globally unreliable.

## Playback/reset behavior

Playback discontinuities use a **soft reset**:

- frame/timestamp history is cleared
- temporal NVOF hints are disabled for the first new pair
- the initialized NVOF session, registered D3D11 resources, and precompiled compute shaders remain alive

This is important for latency. Earlier development builds tore down the backend and synchronously recompiled HLSL after play/seek operations, causing multi-second video stalls followed by catch-up playback and A/V desynchronization. Production shaders are now compiled at build time and embedded as SM5 bytecode; the renderer does not call `D3DCompile` for the native interpolation path.

A full teardown still occurs when the interpolation object is destroyed or the D3D11 device/output dimensions require reinitialization.

## Runtime capability policy

The renderer must query capabilities from the active NVOF session rather than infer support from a GPU model name.

Production currently requires:

- NVOF D3D11 session creation
- `B8G8R8A8_UNORM` input support
- `R16G16_SINT` optical-flow output support
- a supported 4x4 output-vector grid

If the active GPU/driver does not advertise 4x4 output vectors, native interpolation fails cleanly and reports the advertised grid sizes.

Even on hardware that advertises denser 2x2 or 1x1 output grids, the first shipping version intentionally stays on 4x4. A denser-grid path would change the reconstruction and quality-gate behavior and therefore requires separate validation before it can become an optimization.

MPCVR runtime status reports the driver NVOF API version and the grid sizes advertised by the active GPU/driver.

## Hardware validation matrix

Real NVOFA execution cannot be certified by ordinary GitHub-hosted Windows runners, so cross-generation playback testing is a release requirement.

| GPU generation | Release status | Required checks |
| --- | --- | --- |
| Turing / RTX 20 | Validated on RTX 2070 SUPER | capability probe, 20->40, 30->60, 60->120, slow-motion artifact inspection, seek/pause/resume, A/V sync |
| Ampere / RTX 30 | Hardware test required | capability probe, 30->60, 60->120 if display permits, seek/pause/resume, quality-gate behavior |
| Ada / RTX 40 | Hardware test required | same as Ampere |
| Newer NVIDIA generations | Hardware test required; do not assume from generation name | capability probe first, then full playback validation only when the required 4x4/D3D11 formats are advertised |

`NativeNvofProbe.exe` is the first-stage compatibility test. It reports the adapter, driver NVOF API, input/output formats, output grid sizes, supported dimensions, and ROI support without installing MPCVR into K-Lite.

## Current visual validation

The RTX 2070 SUPER reference system has been tested successfully with:

- 20 -> 40 fps
- 30 -> 60 fps
- 60 -> 120 fps
- slow-motion inspection of scenes that previously exposed severe hand/finger/weapon distortion
- startup and repeated seeks after the soft-reset fix

The breakthrough dense-flow + frame-quality-gate build did not show the previous fast-motion distortion in those tests.

## CI safeguards

The dense production shaders are stored as HLSL plus committed precompiled bytecode:

- `NvidiaOpticalFlowDenseSeed.hlsl`
- `NvidiaOpticalFlowDenseJump.hlsl`
- `NvidiaOpticalFlowDenseUpsample.hlsl`
- `NvidiaOpticalFlowDenseWarp.hlsl`

`Validate dense NVOF shaders` compiles every production shader with `fxc`, warnings-as-errors, then compares regenerated bytecode against the committed headers. This prevents source/bytecode drift while preserving the no-runtime-compile design.

The standalone Native NVOF probe workflow additionally builds the capability and historical diagnostic probes.

## Official API references

- NVOFA Programming Guide: https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html
- NVOFA Application Note: https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/index.html
- Public NVIDIA Optical Flow API header: https://github.com/NVIDIA/NVIDIAOpticalFlowSDK/blob/master/nvOpticalFlowCommon.h

The public NVIDIA header carries a permissive redistribution notice. No proprietary NVIDIA SDK DLL is added to this repository.
