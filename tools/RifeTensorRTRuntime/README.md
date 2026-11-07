# MPC Video Renderer RIFE TensorRT runtime

`MPCVRRifeRuntime64.dll` is the optional NVIDIA runtime used by the renderer-side RIFE bridge. The main MPC Video Renderer project does **not** link CUDA or TensorRT and continues to load/play normally when this DLL is absent.

## Runtime contract

The runtime accepts the original 11-channel Practical-RIFE representation used by the bundled RIFE 4.4, 4.6, 4.15 Lite, and 4.25 Lite ONNX models, with one NCHW input and one NCHW output:

- input: `[1, 11, H, W]`
- output: `[1, 3, H, W]`
- channels 0-2: first RGB frame, normalized 0..1
- channels 3-5: second RGB frame, normalized 0..1
- channel 6: requested interpolation timestep `t`
- channels 7-8: normalized X/Y coordinate grids
- channels 9-10: `2/(W-1)` and `2/(H-1)` planes
- H/W are padded to the model's required alignment; the current models use 32 pixels except RIFE 4.25 Lite, which requires 128. Output is cropped back to the source size.

The 4.25 archives also ship an experimental 7-channel `rife_v2` interface with internal padding. MPCVR intentionally uses the stable legacy 11-channel models for now because upstream still marks implementation 2 as experimental and potentially removable.

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

ABI-2 requests may include a nonzero `inputPairId` for one immutable source-pair
job. Each execution context packs that pair once and reuses its existing tensor
for subsequent outputs by updating only timestep channel 6. Source textures are
mapped/unmapped normally on the first output; later outputs use the CUDA tensor
without mapping the D3D inputs again. No additional tensor allocation is needed.
The renderer assigns a fresh ID to every pair job, including after seeking or
repeat playback; texture addresses and timestamps are not content identifiers.
Failures, context drains, and runtime/model/geometry replacement invalidate reuse.
Old ABI-2 callers omit the ID and keep full-pack behavior. Old runtimes ignore the
optional request tail, so renderer/runtime rollback remains compatible.

In deferred-release mode, a fresh pair maps both inputs and its output together,
then unmaps all three together after the existing kernel-completion wait. Later
outputs from a reused pair map only their output. Generic non-deferred callers
retain the separate input-release stream. Input claims, D3D locking, error cleanup
and drain completion remain required; batching does not permit graphics access
while resources are CUDA-mapped. Combined map/unmap host time is counted once in
`inputMapMs`/`outputUnmapMs`. Output acquisition now precedes the inference start
event, so compare total delivered throughput rather than historical inference
event timing alone.

The current ABI call is synchronous from the caller's point of view because it waits for the selected CUDA stream before returning the output surface. MPCVR's pipeline is responsible for dispatching these calls from worker threads so the renderer/reference-clock thread never blocks on inference.

## Optional CUDA output leases

`Source/RifeCudaOutputApi.h` defines an independently versioned optional API.
The original ABI-2 request, create parameters and D3D interpolation exports stay
compatible. The renderer enables CUDA outputs only when all five optional
exports are present and the CUDA output ABI matches.

Each runtime owns up to eight lazily allocated pitched BGRA8 buffers. Acquire
returns a generation-tagged lease, InterpolateCuda writes completed full padded
pixels using the same clamp/round/alpha kernel contract as the D3D path, and
Release returns the slot. Logical content dimensions are separate from padded
dimensions. A completed lease is immutable. Invalid or stale tokens are rejected;
pool exhaustion falls back to ordinary D3D interpolation. ExportCudaOutput copies
the complete padded image to a compatible texture and completes its CUDA read
before returning, including error cleanup.

The renderer lease retains the producing runtime and DLL through queueing,
presentation, cancellation, flush and model replacement. Consumers must complete
all reads before releasing the last lease. The initial Maxine consumer opts in
only after the tested native SDK 1.2 primary-context path initializes, for single
VSR passes with even input/output widths. Source frames, scene repeats/blends,
effect chains, odd widths and unsupported runtimes retain the D3D path. If settings
change after queueing, the renderer exports the lease and retries normal Maxine
processing. D3D graphics/video completion before Maxine mapping remains required.

The odd-width restriction is conservative: a portrait fixture showed small VSR
pixel variation on repeated original D3D calls as well as borrowed CUDA input.
This is not a claim that the underlying SDK behavior has been fixed.

## Engine cache

Serialized TensorRT engines are stored below the cache directory supplied by MPCVR. The file name includes:

- RIFE model SHA-256
- TensorRT major/minor version
- GPU compute capability and device name
- dynamic profile range
- normal dynamic plan or Performance Boost fixed-resolution plan

Normal mode uses one dynamic profile covering source sizes up to at least padded 4K dimensions, with a fixed padded 1920x1088 optimization point recorded in the cache name (`_opt1920x1088`). A model whose shape constraints reject that point falls back to 1920x1152 (`_opt1920x1152`): RIFE 4.25 Lite pads internally to 128 pixels and fails to build with 1088 rows. The runtime loads whichever candidate plan exists, otherwise builds them in order and names the plan after the point that succeeded. Earlier normal plans used whichever size first built them as the optimization point; those files keep their old names and are neither reused nor replaced. On an RTX 2070 SUPER with RIFE 4.6 at 1920x1088, a plan first built for 384x480 measured 13.55 ms, the fixed-point plan 12.18 ms, and a fixed-shape Boost plan 12.09 ms; the fixed point was also faster at 1280x736 and 3840x2176 and slower only at very small sizes (2.28 ms instead of 1.66 ms at 384x480). Performance Boost uses a fixed profile for the current padded resolution and therefore keeps a separate cached engine per resolution. The TensorRT level-5 builder experiment remains disabled because it benchmarked substantially slower (17.3 ms for the 1080p dynamic plan).

Normal inputs with a padded dimension below 128 use a separate profile whose
minimum includes the requested size. Its cache suffix records the reduced minima;
existing normal and Boost cache names are unchanged. Original and feature-stage
plans use private staging files and checked replacement without deleting the
previous plan on publication failure. A failed save reports its path to the debug
log; the already-built engine remains usable in memory.

Feature reuse (`FeatureReuseEngine.h`) builds two fixed-shape stages from the
hash-qualified sidecars: a six-channel image encoder and the remaining
synthesis graph. It is enabled only for model hashes from the rife-models
release named by `kValidatedModelsRelease`; `dev.ps1 test` fails when the
dependency lock pins another release, so new exports are revalidated before
reuse covers them. The synthesis sidecar still reshapes the two images to
2x3xHxW for the first warp. When the encoder is removed, TensorRT fuses that
reshape into the first warp kernel, and on an RTX 2070 SUPER (SM75, TensorRT
11.4) the fused kernel corrupted the top rows of RIFE 4.25 Lite output. The
runtime therefore replaces that reshape with a network input bound to the same
memory as the first six input channels; the warp then builds as in the original
engine. Stage plans use the `_pairfeatures_v2_` cache suffix, so plans from the
fused form are never reused. The encoder and synthesis contexts run in order on
one stream and share one user-managed TensorRT scratch allocation. TensorRT
11.4's 4.25 Lite plans (the original whole plan as well) read part of their
scratch before writing it; with their own previous-frame leftovers the effect
is negligible (6,198 instead of 6,140 pixels off by more than 0.05 from the CPU
reference on a 1920x1152 frame), but encoder data could decode as NaN, so the
scratch is cleared once per pair after the encoder runs.

Deleting the cache is safe; the runtime rebuilds the engine from ONNX.

## Adapter rule

For zero-copy D3D11/CUDA interoperability the CUDA device must correspond to the D3D11 adapter that owns the video textures. A requested GPU that does not match that adapter is rejected rather than silently performing cross-adapter or CPU copies.

## Not bundled

This directory intentionally does not contain TensorRT/CUDA redistributables or RIFE model weights. The separate installer is responsible for installing pinned, checksum-verified runtime/model files into the location probed by MPCVR.
