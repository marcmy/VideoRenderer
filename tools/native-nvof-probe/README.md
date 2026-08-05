# Native NVIDIA Optical Flow probe

This small 64-bit diagnostic verifies the driver-only D3D11 Optical Flow path before it is integrated into MPC Video Renderer.

It does **not** require or load:

- `NvOFFRUC.dll`
- `cudart64_110.dll`
- an Optical Flow SDK installation
- an NVIDIA Developer Program account

It loads `nvofapi64.dll` from the Windows system directory, creates a D3D11 Optical Flow session on the first NVIDIA hardware adapter, and reports:

- the driver-supported NVOF API version
- supported input and output formats
- supported output-vector grid sizes
- minimum and maximum frame dimensions
- region-of-interest capability

## Running

1. Download the `Native-NVOF-Probe` artifact from the PR's **Native NVOF Probe** workflow.
2. Extract it.
3. Run `NativeNvofProbe.exe` from PowerShell or Command Prompt.
4. Copy the complete console output into the PR or development conversation.

A successful Turing-class result should end with:

```text
D3D11 NVOF session: created
Output vector grids: 4x4
RESULT: PASS
```

The exact formats and dimension limits are driver/GPU dependent. Ampere and newer hardware may expose smaller output grids.
