# Native NVIDIA Optical Flow probes

These small 64-bit diagnostics verify the driver-only D3D11 Optical Flow path before it is integrated into MPC Video Renderer.

They do **not** require or load:

- `NvOFFRUC.dll`
- `cudart64_110.dll`
- an Optical Flow SDK installation
- an NVIDIA Developer Program account

All three tools load `nvofapi64.dll` from the Windows system directory.

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

The baseline warp is deliberately performed on the CPU after reading back the hardware vectors. This isolates and validates the flow direction, midpoint math, and error thresholds before the same operation is moved into an MPCVR D3D11 shader.

It writes:

- `NativeNvofMidpoint.bmp`
- `NativeNvofMidpointExpected.bmp`
- `NativeNvofMidpointDiff.bmp` (errors amplified 8x)

A successful result ends with:

```text
nvOFExecute: forward/backward submitted
MIDPOINT RESULT: PASS
```

## Running

1. Download the `Native-NVOF-Probe` artifact from PR #25's **Native NVOF Probe** workflow.
2. Extract it.
3. Run the tests from PowerShell or Command Prompt:

```powershell
.\NativeNvofProbe.exe
.\NativeNvofFlowTest.exe
.\NativeNvofMidpointTest.exe
```

4. Copy the complete console output from `NativeNvofMidpointTest.exe`. Keep the three midpoint bitmaps if it fails or the difference image contains visible structure.
