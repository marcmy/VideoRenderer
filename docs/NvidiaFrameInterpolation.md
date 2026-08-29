# NVIDIA frame interpolation

This feature uses NVIDIA Optical Flow SDK 5.0.7 NvOFFRUC to generate one midpoint frame between consecutive progressive SDR frames, producing exact 2x presentation when the configured source-resolution and output-rate limits allow it.

## Runtime installation

The renderer dynamically loads NVIDIA's proprietary runtime; the DLLs are not committed or published with this GPL repository.

1. Download `Optical_Flow_SDK_5.0.7.zip` from NVIDIA.
2. Run `tools\nvoffruc\Install-NvOFFRUCRuntime.cmd` and select the SDK ZIP when prompted.
3. Restart MPC-HC.
4. Enable **Frame interpolation → Double source frame rate** in the renderer settings.
5. Press **Ctrl+J** during playback and verify that `Frame interp` reports `Active` and that `FRUC runtime` shows the installed directory.

The installer copies only `NvOFFRUC.dll` and `cudart64_110.dll` into `%LOCALAPPDATA%\MPCVR NvOFFRUC Runtime` and sets the current-user `NV_OFFRUC_PATH` environment variable. Administrator access is not required.

## First implementation limits

- 64-bit Direct3D 11 renderer
- NVIDIA GPU supported by NvOFFRUC
- progressive SDR input
- exact 2x interpolation only
- output frame rate must not exceed the selected limit
- source dimensions must not exceed the selected limit

Interlaced and HDR video automatically use the normal renderer path. The engine resets on seeks, discontinuities, flushes, device resets, and output-size changes.
