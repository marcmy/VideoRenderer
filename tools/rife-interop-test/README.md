# CUDA / D3D11 context concurrency regression

This hardware test exercises the runtime's `D3D11InteropLock` with CUDA's real
map/unmap API and a second thread issuing D3D11 draw commands. The threads use
separate textures: only the immediate device context is shared. No TensorRT
engine, RIFE model, scene detector, or player is involved.

Requires an NVIDIA CUDA-capable GPU, a CUDA 12 runtime DLL, Visual Studio C++
tools, and the Windows D3D11 debug layer. Run `build-test.cmd`, then:

```powershell
.\RifeInteropConcurrencyTest.exe 'C:\path\to\cudart64_12.dll'
```

The guarded test runs for 15 seconds and returns failure on a CUDA error, D3D11
debug error, or device removal. Success requires actual drawing and mapping;
inspect the final operation counts as well as the exit code.

`--unguarded` deliberately removes the context lock for reproducing the original
failure. This can trigger a GPU driver reset; it is not part of normal validation.
On an RTX 2070 SUPER with driver 616.92 and CUDA 12.9, the unguarded reproduction
failed after 3 draws / 42 maps with device reason `0x887A0006` (DEVICE_HUNG).
The guarded counterpart completed 60,300 draws / 85,324 maps without errors.

Reference: [the original independent reproduction and explicit lock fix](https://forums.developer.nvidia.com/t/d3d11-device-context-in-a-separate-thread-gets-corrupted-when-cuda-graphics-resource-mapping-is-used/232326/2).
This test verifies the interop boundary; player cadence and fullscreen transitions
still need playback validation.
