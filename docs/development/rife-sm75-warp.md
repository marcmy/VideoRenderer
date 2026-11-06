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

The `_synth_w2` cache suffix distinguishes plugin plans from native plans and from
the first plugin version's `_synth_w1`, without lengthening their filenames. Registration precedes deserialization and is removed
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

## Folding the next block's downscale into the warp, 2026-11-06

Three refinement stages built a full-resolution 28-channel image only to shrink it
at once to 1/16, 1/8 and 1/4 size. A `half_pixel` linear Resize at exactly 1/scale
makes each output the mean of the four input pixels around its centre, so the
plugin now warps just those: 1/64, 1/16 and 1/4 of the image. The last stage has no
downscale and keeps the full-resolution kernel. ONNX Runtime confirmed the formula
on real model tensors to within one FP16 rounding step at all three scales.

Plugin version 2 serializes its scale, and plans use the `_synth_w2` suffix. Sizes
that are not a multiple of the scale are rejected; feature-engine sizes are always
multiples of 32. Rounding happens once per output instead of once per warp plus
once per resize, so downscaled warps can differ from the graph by one FP16 step.

The desktop GPU carried a steady unrelated load of about 51% during this comparison,
so averages were unusable: the unchanged reuse-off baseline read 22-41 ms. Per-call
minimums separate cleanly, because outside work only ever slows a call. This table
gives the median across eight alternating runs on two synthetic 1920x1152 clips,
each run reporting its minimum over 59 repetitions, built with the same compiler.
The two-target mean is the workload of 30-to-90 FPS playback.

| Runtime with feature reuse | First target, ms | Reused target, ms | Mean of two targets, ms | Runtime VRAM, MiB |
| --- | ---: | ---: | ---: | ---: |
| Fused warp | 17.17 | 13.37 | 15.27 | 1122 |
| Fused warp and downscale | 14.75 | 10.87 | 12.81 | 1090 |

That is about 16% less TensorRT time, or 19% more inference capacity. These are
best-case isolated figures, not playback measurements or sustained rates.

Comparing visible RGB output with the whole-model engine, after the change versus
before: mandel clip mean 0.047 versus 0.043 of 255, pan clip 0.118 for both. Pixels
differing by more than 32/255 were 0.057% versus 0.051% and 0.120% versus 0.117%.
They sit on hard detail, with a local gradient near 30 against 1.5 overall, and not
along picture edges. Any two plans differ this much, including the previous fused
engine against the new one, so these differences are not specific to this change.

The flow, state and tail inputs of the three downscaled stages are still upsampled
to full resolution by TensorRT and then mostly discarded. Those stages' `Resize_1`,
`Split` and flow arithmetic are the next candidates. They feed each other, so
keeping them at low resolution needs its own numerical check.

## Scheduler

The adaptive scheduler's delivery checks and recovery headroom were preserved.
Changing its estimates would not reduce GPU work. Recovery that accounts for
different fresh/reused costs needs separate, fresh timing evidence and playback
validation before changing policy.

## Correctness and limits

`RifeWarpGpuTest` tests the production plugin against an independent CPU sampler
at 1x1, 128x128, 257x129, 640x1152 and 1920x1152, and for each downscale at one
output pixel, 128x128, 640x1152, 1920x1152 and 1792x1024. It checks dynamic
profiles, serialization/deserialization, finite results, rejected scales and sizes,
and all unwarped channels. At scale 1 the maximum difference was zero. Downscaled
warps stayed within one FP16 step (0.000488) and their tail channels matched
exactly. The graph's two explicit FP16 coordinate rounding steps are retained;
contraction is disabled for this CUDA translation unit.

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
