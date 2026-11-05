# RIFE 4.25 Lite fused warp experiment

## Scope

The feature-reuse synthesis engine can combine coordinate generation, bilinear
border sampling, reshaping and concatenation into one CUDA plugin per refinement
stage. A simple GridSample-only replacement was rejected: its extra copies erased
the kernel's gain (18.7 ms versus 18.3 ms for the complete model).

The fused path is restricted to SM75 and the already validated 4.25 Lite model
hash in `FeatureReuseEngine.h`. It requires Feature reuse. Other models, GPU
architectures and the ordinary dynamic engine keep their existing paths. A failed
fused build falls back to the native feature engine. ABI 2 is unchanged.

The `_synth_w1` cache suffix distinguishes plugin plans from native plans, without
lengthening their filenames. Registration precedes deserialization and is removed
when the runtime DLL unloads. Existing caches are retained. The image encoder's
plan is reused; the new synthesis plan needs one initial build at each feature
engine size, as the existing feature-reuse path already does.

## Isolated measurements, 2026-11-05

Hardware: RTX 2070 SUPER, SM75. TensorRT 11.4.0.106; local CUDA compiler 13.5.
These are isolated tests, not MPC-HC playback measurements. The distributable
runtime keeps the pinned CUDA 13.4.1 toolchain.

At 1920x1080 content / 1920x1152 padded inference, Performance boost off, two
allocated contexts, a synthetic Mandelbrot pair and its reverse were submitted
through the runtime ABI at timesteps 1/3 and 2/3. Seven measured repetitions
followed one warmup repetition. Calls in this comparison used one context.

| Runtime with feature reuse | First target, ms | Reused target, ms | Mean of two targets, ms | Runtime VRAM, MiB |
| --- | ---: | ---: | ---: | ---: |
| Previous | 19.67 | 15.12 | 17.40 | 1232 |
| Fused warp | 17.07 | 13.03 | 15.05 | 1094 |

This is approximately 13% less TensorRT time, or 16% more inference capacity for
that workload. It leaves little spare time for 30-to-90 FPS playback: two
synthesized frames must fit each 33.3 ms source interval, alongside the rest of
the pipeline. It does not establish sustained 90 FPS, let alone 120 FPS.

Alternating cached-engine synthesis-only runs measured native 14.67-15.21 ms
versus fused 12.66-12.74 ms. A continuously queued two-context test measured
15.25 and 12.39 ms per output respectively. More contexts added little throughput.
During 2x playback there is only one synthesized output per source pair; inference
shorter than the source interval need not overlap another call. An overlap of
1.00 alone is not evidence of a synchronization defect.

The adaptive scheduler's delivery checks and recovery headroom were preserved.
Changing its estimates would not reduce GPU work. Recovery that accounts for
different fresh/reused costs needs separate, fresh timing evidence and playback
validation before changing policy.

## Correctness and limits

`RifeWarpGpuTest` tests the production plugin against an independent CPU sampler
at 1x1, 128x128, 257x129, 640x1152 and 1920x1152. It checks dynamic profiles,
serialization/deserialization, finite results and exact copying of all unwarped
channels. The measured maximum difference was zero in the native test. The graph's
two explicit FP16 coordinate rounding steps are retained; contraction is disabled
for this CUDA translation unit.

`RifePrecisionGpuTest` passed with all three feature models, BGRA8/16F output,
pair invalidation, repeated reuse and independent contexts. Whole-model versus
split-model comparisons at 128x128 stayed below 0.01 normalized maximum error.

At full resolution, different TensorRT plans already have sparse large output
differences. The runtime comparison above measured mean/max differences against
the whole engine of 0.000094/0.820 before and 0.000101/0.847 after. These maxima
must not be hidden by reporting averages alone. Direct kernel parity does not
prove equivalent visual quality across arbitrary videos; same-clip playback
acceptance remains necessary before a renderer release.

The offline experiments and logs are preserved under
`_artifacts/rife-warp-20261105`. No installed renderer, player or user engine
cache was changed by these tests.

## Running the native tests

Configure `tools/RifeTensorRTRuntime` with `MPCVR_RIFE_BUILD_TESTS=ON`, an installed
CUDA toolkit and `TENSORRT_ROOT`. Build and explicitly run `RifeWarpGpuTest.exe`.
It writes no engine cache and requires no model files.

`RifePrecisionGpuTest.exe <runtime.dll> <model-directory> <private-cache>` also
needs the matching TensorRT/CUDA libraries and SM-specific builder resource beside
the runtime. Use a new private cache, never the player's cache. GPU tests are
explicitly invoked rather than added to the normal CPU/WARP test command.

Plugin lifetime and serialization follow NVIDIA's
[TensorRT V3 plugin documentation](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/inference-library/plugins-cpp.html).
