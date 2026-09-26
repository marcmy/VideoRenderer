# MPC Video Renderer RIFE TensorRT runtime

`MPCVRRifeRuntime64.dll` is the optional NVIDIA runtime used by the renderer-side RIFE bridge. The main MPC Video Renderer project does **not** link CUDA or TensorRT and continues to load/play normally when this DLL is absent.

## Runtime contract

The runtime accepts the original 11-channel Practical-RIFE representation used by the bundled RIFE 4.4, 4.6, and 4.15 Lite ONNX models, with one NCHW input and one NCHW output:

- input: `[1, 11, H, W]`
- output: `[1, 3, H, W]`
- channels 0-2: first RGB frame, normalized 0..1
- channels 3-5: second RGB frame, normalized 0..1
- channel 6: requested interpolation timestep `t`
- channels 7-8: normalized X/Y coordinate grids
- channels 9-10: `2/(W-1)` and `2/(H-1)` planes
- H/W are padded to multiples of 32; output is cropped back to the source size.

For TensorRT 11, precision is model-defined because TensorRT 11 networks are strongly typed. The bundled models are converted to mixed FP16/FP32 with FP16 public I/O for the intended real-time path. TensorRT 10.14 is also supported by the source layout, but TensorRT 11 is the primary target.

## Build

Requirements:

- Windows x64
- Visual Studio 2022
- CMake 3.25+
- CUDA Toolkit with D3D11 interop
- TensorRT 10.14+ or 11.x

Set the TensorRT root and configure normally:

```powershell
$env:TENSORRT_ROOT = 'C:\TensorRT'
cmake -S . -B build -A x64
cmake --build build --config Release
```

On Windows, TensorRT 10/11 uses versioned import-library names such as `nvinfer_11.lib` and `nvonnxparser_11.lib`. The CMake project accepts both those names and the unversioned Linux-style names so the same source remains portable across supported TensorRT package layouts.

The resulting file is `MPCVRRifeRuntime64.dll`.

For the shipping runtime, CUDA code generation is limited to the supported GPU architecture set:

- `sm_75`, `sm_86`, and `sm_89` machine code
- `sm_120` machine code plus `compute_120` PTX for Blackwell

At runtime, compute capabilities 7.5, 8.6, 8.9, and 12.0 are accepted. Other compute capabilities are rejected before TensorRT engine creation. Each accepted architecture also requires the matching TensorRT 11 builder resource beside the runtime DLL (`nvinfer_builder_resource_sm75_11.dll`, `nvinfer_builder_resource_sm86_11.dll`, `nvinfer_builder_resource_sm89_11.dll`, or `nvinfer_builder_resource_sm120_11.dll`). The common `nvinfer_builder_resource_ptx_11.dll` remains part of the runtime payload and is discovered through TensorRT's existing internal-library path.

## GPU path

Steady-state interpolation is GPU-only:

1. D3D11 source/output textures are registered once with CUDA and cached.
2. CUDA maps the resources and packs BGRA8 source textures directly into the RIFE input tensor.
3. TensorRT executes `Frame(A, B, t)` on one of the configured execution contexts/CUDA streams.
4. A CUDA postprocess kernel writes RGB output directly into the mapped D3D11 output texture.

There is no normal-playback CPU pixel readback.

The current ABI call is synchronous from the caller's point of view because it waits for the selected CUDA stream before returning the output surface. MPCVR's pipeline is responsible for dispatching these calls from worker threads so the renderer/reference-clock thread never blocks on inference.

## Engine cache

Serialized TensorRT engines are stored below the cache directory supplied by MPCVR. The file name includes:

- RIFE model SHA-256
- TensorRT major/minor version
- GPU compute capability and device name
- dynamic profile range
- normal dynamic plan or Performance Boost fixed-resolution plan

Normal mode uses one dynamic profile covering source sizes up to at least padded 4K dimensions, with the current size as the optimization point. Performance Boost uses a fixed profile for the current padded resolution and therefore keeps a separate cached engine per resolution. Cached throughput testing showed this original fixed-shape path is consistently faster than the shared dynamic plan. The later TensorRT level-5 Boost experiment remains disabled because it benchmarked substantially slower.

Deleting the cache is safe; the runtime rebuilds the engine from ONNX.

## Adapter rule

For zero-copy D3D11/CUDA interoperability the CUDA device must correspond to the D3D11 adapter that owns the video textures. A requested GPU that does not match that adapter is rejected rather than silently performing cross-adapter or CPU copies.

## Not bundled

This directory intentionally does not contain TensorRT/CUDA redistributables or RIFE model weights. The separate installer is responsible for installing pinned, checksum-verified runtime/model files into the location probed by MPCVR.
