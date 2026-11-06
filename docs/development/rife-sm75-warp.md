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

A `_synth_wN` cache suffix distinguishes plugin plans from native plans and from
each other's plugin versions, without lengthening their filenames. Registration precedes deserialization and is removed
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

## Reading flow and state from the blocks' low-resolution outputs, 2026-11-06

TensorRT still upsampled all 13 channels of each of four blocks to full resolution,
split them and accumulated flow, although the first three warps read only a fraction
of the result. Plugin version 3 takes the blocks' outputs directly and evaluates the
same operations only at the pixels each stage reads, keeping the graph's FP16
rounding after each upsample and addition. This is not an approximation of the flow.
Only the last stage works at full resolution, and it also returns the accumulated
flow that the final warp needs. A fifth plugin stage replaces block 0's full-size
input concat and its 1/32 downscale. Plans use the `_synth_w3` suffix.

Full-resolution output matched the independent reference exactly at every size in
`RifeWarpGpuTest`, and block 0's input and the downscaled state and flow were within
one FP16 step of it. With real block outputs, ONNX Runtime agreed on block 0's input
exactly and on state and flow within about two steps of their magnitude. Its warped
channels differ more, with mean 0.00276 and maximum 0.54 on images, but the previous
kernel given ONNX Runtime's exact flow shows the same 0.00276 and 0.54. That
difference is between the kernels' and ONNX Runtime's sampling, not from this change.

The GPU read 3% when sampled after this comparison, and timings were far steadier
than the previous section's. Median across eight alternating runs on the two
synthetic 1920x1152 clips, each run reporting its minimum over 59 repetitions, built
with the same compiler:

| Runtime with feature reuse | First target, ms | Reused target, ms | Mean of two targets, ms | Runtime VRAM, MiB |
| --- | ---: | ---: | ---: | ---: |
| Fused warp and downscale | 13.80 | 10.16 | 11.98 | 1058 |
| Low-resolution flow | 9.97 | 6.31 | 8.14 | 930 |

That is about 32% less TensorRT time, or 47% more inference capacity. Again these
are best-case isolated figures, not playback measurements or sustained rates.

Compared with the whole-model engine, values differing by more than 32/255 were
0.0220% against 0.0227% before on the mandel clip, and 0.0372% against 0.0440% on the
pan clip. They again sit on hard detail, not along picture edges. Against ONNX
Runtime at timesteps 0.25, 0.5 and 0.75 on a real input, the new engine's rate was
0.0142%, 0.0240% and 0.0178%, against native TensorRT's 0.0131%, 0.0265% and 0.0161%.

## Finishing the frame in one kernel, 2026-11-06

A layer profile of the 6.2 ms synthesis graph still showed work nothing needed. Block
4's DepthToSpace took 0.64 ms to build 13 full-resolution channels, of which the frame
uses five, and copying them for the final warp took 0.27 ms. Copying stage 3's planar
output into `kHWC8`, the format block 4's first convolution reads, took 0.60 ms.

Plugin version 4 changes four things, and plans use the `_synth_w4` suffix:

- Stage 3 writes `kHWC8` directly, and that copy disappears. It uses one 16-byte store
  per eight channels: writing channels one at a time in this format was about four
  times slower, 2.77 ms against 0.65 ms for the stage.
- A new stage 4 replaces DepthToSpace, the final flow addition, warp, sigmoid and blend.
  It reads only the channels used, and takes the accumulated flow from stage 3's output
  instead of a separate tensor.
- Block 4's last ConvTranspose outputs only the 20 of its 52 maps that those five
  channels come from, using the original weights. It fell from 0.40 to 0.25 ms.
- Each pair now clears only the scratch that synthesis can read, 168 rather than
  271 MiB of the shared allocation.

Three other changes were measured and rejected. Interleaving stages 0-2 made stage 2
slower, 0.37 against 0.24 ms, and gained 0.03 ms between stages 0 and 1. Writing the
final image as `kCHW4` made no difference, and neither did block widths of 32 or 256 in
place of 128. Stage 4 took about 0.51 ms in all of them, and without its exponential.

Both distributed runtimes, built by the pinned toolchain, in the same harness: median
across eight alternating runs on the two clips, each run reporting its minimum over 59
repetitions. The minimums did not overlap.

| Runtime with feature reuse | First target, ms | Reused target, ms | One first and two reused, mean ms |
| --- | ---: | ---: | ---: |
| Low-resolution flow | 9.58 | 6.17 | 7.31 |
| Finished in one kernel | 7.67 | 4.41 | 5.49 |

The last column is the workload of 30-to-120 FPS playback: about 25% less TensorRT time,
or 33% more inference capacity. These are best-case isolated figures, not playback
measurements or sustained rates.

Synthesis scratch fell from 271 to 168 MiB, but the encoder's 270 MiB scratch shares that
allocation and sets its size, so runtime VRAM reads about the same.

Against the whole-model engine, the distributed builds' values differing by more than
32/255 were 0.0250% against 0.0193% before on the mandel clip, and 0.0426% against
0.0474% on the pan clip. Locally compiled, the same comparison read 0.0209% against
0.0200% and 0.0507% against 0.0547%, so these move in both directions between clips and
builds. They sit on hard detail, with a local gradient near 30 against 1.5 overall.
Against ONNX Runtime at timesteps 0.25, 0.5 and 0.75 on a real input, a local build's rate
was 0.0135%, 0.0263% and 0.0187%, against native TensorRT's 0.0131%, 0.0265% and 0.0161%.

The largest cost left outside block 4 is the encoder, about 2.8 ms per pair. Each pair
encodes both images, although the first was the previous pair's second. Reusing those
features would save about half of that, but needs the renderer to identify consecutive
pairs through the runtime ABI.

## Scheduler

The adaptive scheduler's delivery checks and recovery headroom were preserved.
Changing its estimates would not reduce GPU work. Recovery that accounts for
different fresh/reused costs needs separate, fresh timing evidence and playback
validation before changing policy.

## Correctness and limits

`RifeWarpGpuTest` tests every stage of the production plugin against an independent
CPU reference at 32x32, 128x64, 640x1152, 1792x1024 and 1920x1152. It checks dynamic
profiles, serialization/deserialization, finite results and rejected stages and
sizes. TensorRT's own reformats sit between the plugin and the test's linear buffers,
so the interleaved layouts are checked as declared. The full-resolution stage and block
0's input matched exactly, and downscaled warps and state stayed within one FP16 step
(0.000488). The final stage matched within two steps, from the last bit of the
exponential, averaging about 2e-9. The graph's two explicit FP16
coordinate rounding steps are retained; contraction is disabled for this CUDA
translation unit.

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
