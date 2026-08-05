# Native NVIDIA Optical Flow probes

These small 64-bit diagnostics verify the driver-only D3D11 Optical Flow path before it is integrated into MPC Video Renderer.

They do **not** require or load:

- `NvOFFRUC.dll`
- `cudart64_110.dll`
- an Optical Flow SDK installation
- an NVIDIA Developer Program account

Both tools load `nvofapi64.dll` from the Windows system directory.

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

Executes the next milestone rather than only querying capabilities. It:

1. Creates a textured 640x360 D3D11 frame.
2. Creates a second frame translated 16 pixels horizontally.
3. Initializes NVOF for a 4x4 output-vector grid.
4. Registers both input textures and an `R16G16_SINT` output texture.
5. Calls `nvOFExecute`.
6. Reads the S10.5 motion vectors back and checks that the central field is predominantly horizontal with approximately the expected magnitude.
7. Writes `NativeNvofFlow.bmp`, a color-coded visualization of the returned flow field.

A successful result ends with:

```text
nvOFExecute: submitted
FLOW RESULT: PASS
```

The signed X direction may be positive or negative depending on the API's frame ordering. The validation therefore checks magnitude and direction consistency rather than requiring one sign.

## Running

1. Download the `Native-NVOF-Probe` artifact from PR #25's **Native NVOF Probe** workflow.
2. Extract it.
3. Run both executables from PowerShell or Command Prompt:

```powershell
.\NativeNvofProbe.exe
.\NativeNvofFlowTest.exe
```

4. Copy the complete console output from `NativeNvofFlowTest.exe`. Keep `NativeNvofFlow.bmp` if the test fails or the vector field looks inconsistent.
