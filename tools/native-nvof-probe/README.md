# Native NVIDIA Optical Flow probes

These 64-bit diagnostics exercise the driver-only D3D11 Optical Flow path used by MPC Video Renderer.

The primary shipping use is now **cross-GPU compatibility validation**. The older synthetic probes are retained because they document and regression-test the stages that led to the production dense-flow implementation.

Except for the separate NvOFFRUC replay reference probe, the native NVOF tools do **not** require or load:

- `NvOFFRUC.dll`
- CUDA or `cudart64_110.dll`
- an Optical Flow SDK runtime installation
- an NVIDIA Developer Program account

They load the driver-provided `nvofapi64.dll` from the Windows system directory.

## First test on any new GPU: `NativeNvofProbe.exe`

Run this before installing a native-NVOF MPCVR test build on an unvalidated GPU.

It creates a D3D11 NVOF session on the first NVIDIA hardware adapter and reports:

- adapter name
- driver-supported NVOF API version
- supported input and output formats
- supported output-vector grid sizes
- minimum and maximum frame dimensions
- region-of-interest capability

The first shipping native interpolation pipeline is deliberately fixed to a **4x4 output grid** even if the GPU exposes denser grids. Production MPCVR performs the same capability check at runtime and refuses native interpolation cleanly if 4x4 output is unavailable.

A compatible result must include `4x4` in `Output vector grids` and end with:

```text
D3D11 NVOF session: created
Output vector grids: ... 4x4 ...
RESULT: PASS
```

When testing an RTX 30-, RTX 40-, or newer GPU, preserve the **complete probe output**. The advertised grid list and formats are part of the release-validation record; do not infer compatibility from the GPU model name alone.

## Historical/regression probes

### `NativeNvofFlowTest.exe`

Executes one forward optical-flow pass for a synthetic +16 pixel horizontal translation. It validates S10.5 vector magnitude/direction and writes `NativeNvofFlow.bmp`.

Expected ending:

```text
nvOFExecute: submitted
FLOW RESULT: PASS
```

### `NativeNvofMidpointTest.exe`

Validates bidirectional flow direction and midpoint math using a synthetic +16 pixel source translation with an exact +8 pixel midpoint. The synthesis reference runs on the CPU after NVOF readback so flow semantics can be verified independently of the GPU interpolation shader.

Expected ending:

```text
nvOFExecute: forward/backward submitted
MIDPOINT RESULT: PASS
```

### `NativeNvofGpuMidpointTest.exe`

Runs the same +16 -> +8 test through D3D11 compute while keeping NVOF flow textures GPU-resident through synthesis.

Expected ending:

```text
D3D11 compute midpoint: dispatched with GPU-resident flow
GPU MIDPOINT RESULT: PASS
```

### `NativeNvofOcclusionTest.exe`

Uses layered foreground/background motion to expose occlusion/disocclusion behavior. It verifies accurate flow and synthesis away from motion boundaries while reporting boundary error diagnostically.

Expected ending:

```text
D3D11 compute midpoint: layered scene dispatched
OCCLUSION DIAGNOSTIC: PASS
```

### `NativeNvofOcclusionAwareTest.exe`

Compares the naïve layered blend against the visibility-aware inverse-warp experiment. This was an important quality milestone but is **not the current production synthesizer**; production now uses validated coarse flow, jump-flood infill, edge-aware dense reconstruction, and a whole-frame quality gate.

Expected ending:

```text
D3D11 compute synthesis: baseline + occlusion-aware dispatched
OCCLUSION-AWARE RESULT: PASS
```

### `NativeNvofBoundaryRefineTest.exe`

Retains the rejected boundary-refinement experiment for regression/history. It is not a shipping algorithm and may report a failed quality comparison on the synthetic scene by design.

## Separate NvOFFRUC same-pair reference probe

`NativeFrucReplayTest.exe` is built by the **Native FRUC Replay Probe** workflow rather than the normal driver-only probe artifact. It exists only to compare the proprietary reference backend against captured real frame pairs.

A critical real-world capture produced:

```text
Prime process: 1.437 ms, repeated=no
Midpoint process: 16.6276 ms, repeated=yes
FRUC REPLAY RESULT: PASS
```

That result motivated the production native whole-frame quality gate: severely unreliable motion pairs are allowed to repeat a real frame rather than force a visibly broken synthetic midpoint.

## Production implementation

The shipping candidate no longer uses the historical inverse-warp or forward-splat experiments. Its GPU path is:

```text
4x4 bidirectional NVOF
        -> forward/backward validation
        -> jump-flood coarse-flow infill
        -> edge-aware full-resolution dense flow
        -> next-frame-dominant midpoint warp
        -> whole-frame catastrophic-motion quality gate
```

All production dense shaders are precompiled at build time. The separate `Validate dense NVOF shaders` workflow recompiles them with warnings-as-errors and verifies the committed bytecode byte-for-byte.

## Running the compatibility test

1. Download the `Native-NVOF-Probe` artifact from PR #25's **Native NVOF Probe** workflow.
2. Extract it.
3. On a new GPU, run this first:

```powershell
.\NativeNvofProbe.exe
```

4. Send/save the complete output.
5. Only after the capability result is compatible, install the current MPCVR shipping candidate and test playback.

For deeper regression work, the other executables can be run individually:

```powershell
.\NativeNvofFlowTest.exe
.\NativeNvofMidpointTest.exe
.\NativeNvofGpuMidpointTest.exe
.\NativeNvofOcclusionTest.exe
.\NativeNvofOcclusionAwareTest.exe
.\NativeNvofBoundaryRefineTest.exe
```

## Cross-generation playback checklist

For each new GPU generation, record:

- full `NativeNvofProbe.exe` output
- NVIDIA driver version
- MPCVR runtime status line including advertised NVOF grids
- 30 -> 60 playback
- 60 -> 120 where the display and source allow it
- repeated seeking and pause/resume
- A/V sync
- fast hands/fingers/thin geometry and camera pans
- whether difficult sequences show distortion or only occasional quality-gate frame repetition

Do not enable 2x2/1x1 production flow merely because a newer GPU advertises it. A denser-grid implementation requires its own reconstruction and quality validation.
