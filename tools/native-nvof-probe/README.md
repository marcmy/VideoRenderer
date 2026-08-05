# Native NVIDIA Optical Flow probes

These small 64-bit diagnostics verify the driver-only D3D11 Optical Flow path before it is integrated into MPC Video Renderer.

They do **not** require or load:

- `NvOFFRUC.dll`
- `cudart64_110.dll`
- an Optical Flow SDK installation
- an NVIDIA Developer Program account

All five tools load `nvofapi64.dll` from the Windows system directory.

## `NativeNvofProbe.exe`

Creates a D3D11 Optical Flow session on the first NVIDIA hardware adapter and reports:

- the driver-supported NVOF API version
- supported input and output formats
- supported output-vector grid sizes
- minimum and maximum frame dimensions
- region-of-interest capability

A successful Turing-class result should end with:

```text
D3D11 NVOF session: created
Output vector grids: 4x4
RESULT: PASS
```

## `NativeNvofFlowTest.exe`

Executes one forward optical-flow pass for a synthetic +16 pixel horizontal translation. It reads the S10.5 vectors back, validates their magnitude and direction consistency, and writes `NativeNvofFlow.bmp`.

A successful result ends with:

```text
nvOFExecute: submitted
FLOW RESULT: PASS
```

## `NativeNvofMidpointTest.exe`

Tests the first complete frame-synthesis stage:

1. Creates frame A and frame B, where B is A translated +16 pixels horizontally.
2. Requests forward and backward flow in one `nvOFExecute` call.
3. Verifies that B to A is approximately -16 pixels and A to B is approximately +16 pixels.
4. Upsamples the 4x4 flow fields and backward-warps both source images to `t = 0.5`.
5. Blends the two warped samples.
6. Compares the result against the exact +8 pixel midpoint ground truth.

The baseline warp is deliberately performed on the CPU after reading back the hardware vectors. This isolates and validates the flow direction, midpoint math, and error thresholds before moving the same operation to the GPU.

It writes:

- `NativeNvofMidpoint.bmp`
- `NativeNvofMidpointExpected.bmp`
- `NativeNvofMidpointDiff.bmp` (errors amplified 8x)

A successful result ends with:

```text
nvOFExecute: forward/backward submitted
MIDPOINT RESULT: PASS
```

## `NativeNvofGpuMidpointTest.exe`

Runs the same +16 to +8 midpoint test through a D3D11 compute shader. The NVOF forward and backward `R16G16_SINT` textures remain GPU-resident through flow upsampling, backward warping, and frame blending. They are read back only after synthesis for diagnostic statistics.

It writes:

- `NativeNvofGpuMidpoint.bmp`
- `NativeNvofGpuMidpointExpected.bmp`
- `NativeNvofGpuMidpointDiff.bmp` (errors amplified 8x)

A successful result ends with:

```text
D3D11 compute midpoint: dispatched with GPU-resident flow
GPU MIDPOINT RESULT: PASS
```

This is the closest simple standalone probe to the eventual MPCVR render path: decoded D3D11 textures, NVOF output textures, a compute-shader interpolation pass, and one synthesized D3D11 output texture.

## `NativeNvofOcclusionTest.exe`

Introduces the first layered-motion scene rather than translating the whole frame. A textured foreground rectangle moves +24 pixels horizontally and +12 pixels vertically over a different static textured background. The exact midpoint contains the object at +12/+6.

The test reports separate statistics for:

- foreground-object flow in both directions
- static-background flow in both directions
- stable-background synthesis error
- moving-object interior synthesis error
- occlusion and motion-boundary error
- forward/backward flow consistency

Boundary error is intentionally diagnostic. The current shader is still the naïve two-warp blend, so nonzero error where the object covers or reveals background is expected. The executable passes when NVOF separates the moving object from the stationary background and synthesis remains accurate away from those boundaries.

It writes:

- `NativeNvofOcclusionMidpoint.bmp`
- `NativeNvofOcclusionExpected.bmp`
- `NativeNvofOcclusionDiff.bmp` (errors amplified 8x)
- `NativeNvofOcclusionRegions.bmp`
- `NativeNvofOcclusionConsistency.bmp`

A successful diagnostic ends with:

```text
D3D11 compute midpoint: layered scene dispatched
OCCLUSION DIAGNOSTIC: PASS
```

The measured boundary and consistency behavior is the input for the next stage: confidence weighting, occlusion selection, disocclusion filling, and motion-edge-aware flow upsampling.

## Running

1. Download the `Native-NVOF-Probe` artifact from PR #25's **Native NVOF Probe** workflow.
2. Extract it.
3. Run the tests from PowerShell or Command Prompt:

```powershell
.\NativeNvofProbe.exe
.\NativeNvofFlowTest.exe
.\NativeNvofMidpointTest.exe
.\NativeNvofGpuMidpointTest.exe
.\NativeNvofOcclusionTest.exe
```

4. Copy the complete console output from `NativeNvofOcclusionTest.exe`. Keep its five generated bitmaps; the difference and consistency maps are especially useful even when the diagnostic passes.
