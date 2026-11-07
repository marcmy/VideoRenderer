# RIFE 4.25 Lite fused warp experiment

## Scope

The feature-reuse synthesis engine can combine coordinate generation, bilinear
border sampling, reshaping and concatenation into one CUDA plugin per refinement
stage. A simple GridSample-only replacement was rejected: its extra copies erased
the kernel's gain (18.7 ms versus 18.3 ms for the complete model).

The fused path runs on every compute capability the runtime accepts (SM75, SM86,
SM89 and SM120), for the already validated 4.25 Lite model hash in
`FeatureReuseEngine.h`. The measurements below are all from SM75; the others share
the source and were enabled without a measurement of their own. It requires Feature
reuse. Other models and the ordinary dynamic engine keep their existing paths. A failed
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

## Keeping the frame consecutive pairs share, 2026-11-06

Each pair encoded both images, although its first image was the previous pair's second.
Request tail field `previousPairId` promises that relationship: the renderer sets it
only when both jobs really used that one source frame, so a seek, a repeat, a skipped
pair or a changed runtime promises nothing and both images are encoded as before.

On the SM75 and 4.25 Lite path, a one-image encoder plan (`_encoder1`) now replaces the
two-image plan, reading past the graph's reshape into two. A pair that shares a frame
copies those features and encodes only its second image. Others run the plan twice,
which is slower than the two-image plan, but only happens after a discontinuity.

Consecutive pairs normally run on different contexts, so the copy crosses CUDA streams.
All of this happens under one lock: a reader waits on an event for the encoder it copies
from, and that context waits for the reader's copy before it next overwrites the buffer.
`RifePrecisionGpuTest` runs both contexts unsynchronized and compares every image with
its expected direction. Of 240 racing requests 239 kept a frame, and none used a wrong one.

The one-image plan's scratch is half the size, so the shared allocation falls to
synthesis's 168 MiB and runtime VRAM falls about 176 MiB for two contexts. Each pair now
clears only the range both plans can touch, 135 MiB.

Distributed runtimes, built by the pinned toolchain. Median of per-run minimums across
four alternating runs on the two clips, with contexts alternating as in playback:

| Runtime with feature reuse | First target, ms | Reused target, ms | Runtime VRAM, MiB |
| --- | ---: | ---: | ---: |
| Finished in one kernel | 7.82 | 4.66 | 930 |
| Keeping the shared frame | 6.48 | 4.66 | 754 |

Plans differ in how they round. Against ONNX Runtime on one real input, the two-image plan's
features differed by 0.00071 on average and the one-image plan's by 0.00078, on values
averaging 0.234. Final images against the whole-model engine, values differing by more
than 32/255, moved in both directions: 0.0182% against 0.0230% before on the mandel clip,
and 0.0583% against 0.0474% on the pan clip. The same previous runtime measured 0.0220% and
0.0534% in an earlier run, so these differences are within what the engines vary between runs.

The saving is small at high rates, because a pair with more targets encodes once.

## Loading each texel at once, and keeping only the fine block short, 2026-11-06

At this point the plugin stages were 39% of a reused target. Block 4 and stages 3 and 4 move
about 141 MB per frame and were the largest single costs, so this round targets memory traffic.

- The warp source is now one 16-byte texel per pixel per image, r g b 0 f0 f1 f2 f3, built
  once per image by `InterleaveImageKernel`. Seven planar loads per sample became one.
- Stage 3 has a second output with only the accumulated flow, [1,H,W,4]. Stage 4 had read that
  flow back from the 64-byte-per-pixel kHWC8 tensor, touching every cache line of it again.
  This alone took stage 4 from 0.57 to 0.29 ms.
- Stages 0-2 read and write kHWC8 directly with 16-byte stores, leaving fewer reformats.
- Arithmetic is FP32, rounded only where a value is stored. It saves no time, but the graph's
  FP16 `grid + flow * scale` snapped sampling positions to about half a pixel near the
  picture's sides, and `x + flow` is what that expression means.
- Fewer rows. At 1080p, 64 of the 1152 rows are padding only the coarse blocks ask for.

Dropping those rows everywhere was fast but wrong. The coarse blocks reach hundreds of pixels,
so their flow depends on where the picture ends, and a shorter picture moved flow everywhere:
on the pan clip, values more than 32/255 from the true model rose from 0.021% to 0.249%, and
vertical motion lost 1 dB in the bottom 120 rows against true in-between frames. So the coarse
blocks keep the padded height, which costs little at their resolution, and only block 4 and
stages 3 and 4 run at the content height rounded up to 64. Rows of the coarse levels past that
height read the last source row, which is black at 1080p. A height already a multiple of 128
is unaffected, so 4K's 2176 gains nothing, and 720p's 768 is already its content rounded up.

Distributed runtimes, built by the pinned toolchain. Median of per-run minimums across
four alternating runs on the two clips:

| Runtime with feature reuse | First target, ms | Reused target, ms | Runtime VRAM, MiB |
| --- | ---: | ---: | ---: |
| Keeping the shared frame | 6.35 | 4.53 | 754 |
| This change | 5.79 | 3.78 | 822 |

The extra VRAM is the warp source. Builder optimization level 5 and all three tiling levels
were also tried on this graph and gained nothing, so the defaults stay.

Accuracy was measured two ways. Against the model evaluated in FP32 on the CPU, with the same
padding, on the harness's first target:

| Engine | mandel: mean/255, >8, >32 | pan: mean/255, >8, >32 |
| --- | --- | --- |
| Whole model, FP16 | 0.228, 0.597%, 0.094% | 0.314, 0.844%, 0.121% |
| Keeping the shared frame | 0.229, 0.594%, 0.094% | 0.326, 0.864%, 0.132% |
| FP32 kernels at the padded height | 0.023, 0.038%, 0.009% | 0.071, 0.120%, 0.021% |
| FP32 kernels at the content height everywhere | 0.170, 0.298%, 0.129% | 0.403, 0.720%, 0.249% |
| FP32 kernels, only the fine block short | 0.075, 0.143%, 0.051% | 0.185, 0.344%, 0.094% |

Against true in-between frames, PSNR in dB on six four-frame clips where the runtime is given
frames 0 and 3: four 1/f-textured moving scenes, `testsrc2` and a fractal zoom.

| Engine | All rows | Bottom 120 rows | Bottom 24 rows |
| --- | ---: | ---: | ---: |
| Whole model, FP16 | 21.589 | 23.858 | 20.981 |
| Keeping the shared frame | 21.585 | 23.857 | 20.986 |
| FP32 kernels at the padded height | 21.820 | 26.179 | 24.420 |
| FP32 kernels at the content height everywhere | 21.695 | 27.402 | 24.763 |
| FP32 kernels, only the fine block short | 21.901 | 28.644 | 26.340 |

`testsrc2`'s nearly static bottom rows dominate the means of those two bottom columns. Per clip
against the previous runtime, over all rows this change gained 0.16 to 1.00 dB on the four
moving scenes and lost 0.02 to 0.04 dB on the other two. In the bottom 24 rows it gained on
the slowest and fastest scenes and lost up to 0.22 dB on the diagonal, vertical and zoom clips.

Two separately built plans that should compute the same thing are not identical: they differed
by a mean of 0.24 to 0.42/255 on the textured clips, while repeating a run of one build differed
by exactly zero. Treat smaller differences between plans as noise.

## Sharing flow within a pair, 2026-11-06

Everything above computes what the model would. This is the first change that doesn't, so it
is a setting, off by default, and a different kind of result to report.

At 8x a pair has seven targets, and most of a target's time goes to estimating flow that
barely depends on which target it is. **Flow reuse** runs the flow estimate once per pair, at
t = 0.5, and moves it to each target's time. Motion within a pair becomes a straight line.

For a target at t, each pixel reads the anchor's field where its contents were at the anchor,
x - (t - 0.5)(F1 - F0), and flow scales with the time to each image: F0 by 2t, F1 by 2(1 - t).

- **Coarse** runs blocks 0-3 once per pair, then stage 6 and block 4 per target from one level
  holding the accumulated flow, and `MpcvrRifeFinishFrame`. `MpcvrRifeRetimeLevel` moves that level.
- **Retime** runs every block once per pair and keeps final flow and mask.
  `MpcvrRifeRetimeFrame` then warps, blends and writes the surface, with no block and no
  TensorRT. Since the anchor's mask knows only equal distances, each image's weight is
  multiplied by the fourth power of the distance to the other image. At t = 0.5 this is
  the model's own blend, and towards either end it becomes that source image.

Each mode has its own plans, built on first use. Neither plan contains what the mode skips.

Other exact savings in this change: the fused pack writes only the images and the first
pixel's timestep, a reused target stores only that pixel, and the whole-model plan, which
named the tensors and served as the fallback, is released once the fused contexts exist.

Distributed runtimes through the ABI, 1920x1080 in 1920x1152, 8x with one first and six later
targets per pair on a textured moving clip. Median of three alternating runs, each the minimum
per run. Wall is `totalRuntimeMs`, median. VRAM is one mode alone in a fresh process.

| Runtime | First, ms | Later, ms | Mean of 7, ms | Wall mean, ms | VRAM alone, MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| r2923 | 6.04 | 3.85 | 4.17 | 4.33 | 906 |
| Off | 5.77 | 3.70 | 3.99 | 4.18 | 890 |
| Coarse | 5.64 | 2.56 | 3.00 | 3.15 | 914 |
| Retime | 5.92 | 0.32 | 1.12 | 1.26 | 930 |

At 3840x2160 in 3840x2176, with the same clip doubled, on a local build, one run each:

| Mode | First, ms | Later, ms | Mean of 7, ms | Wall mean, ms | VRAM, MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Off | 21.0 | 13.1 | 14.2 | 16.0 | 3120 |
| Coarse | 22.0 | 10.3 | 12.0 | 12.8 | 3186 |
| Retime | 22.0 | 1.09 | 4.08 | 4.53 | 3292 |

Coarse gains little at 4K, where block 4 dominates even more.

### What it does to the picture

Seven nine-frame clips with true in-betweens at every eighth of a pair, frames 0 and 8 given:
motion of 8 to 110 pixels per pair, a tracking shot, 60 crossing objects, accelerating motion
and rotation. All synthetic. PSNR in dB, SSIM, and sharpness as a percentage of the truth's,
averaged over the seven targets and the clips, from a local build of this runtime:

| Mode | PSNR | SSIM | Sharpness |
| --- | ---: | ---: | ---: |
| Off | 22.23 | 0.655 | 86.8% |
| Coarse | 23.60 | 0.722 | 86.7% |
| Retime | 23.90 | 0.739 | 86.5% |

Those numbers favour reuse, and they should not be read alone:

- Mostly this measures how badly the model places things away from t = 0.5 on these clips,
  more than how good reuse is. On the tracking shot the model's t = 7/8 had warped arcs across
  the background that retiming didn't. Its own output at t = 0.5 is what reuse inherits.
- Crops show the opposite on moving edges. The model keeps a solid object, slightly misplaced;
  reuse places it better with a more translucent or streaked edge. On the accelerating clip its
  disc went visibly translucent, and SSIM didn't notice, because a straight line is wrong there.
- The clips' motion is mostly linear, which is what this assumes. Real acceleration within one
  source frame is usually far smaller than that clip's, but this has not been measured on real
  footage, which is why the mode is the user's to choose.

Tried and dropped: splatting flow forward to each target, choosing the winner of a collision by
how far the two warped images disagree, or by speed. It was meant to fix leading edges and
changed PSNR by 0.03 dB and the crops by nothing visible. Anchors at 0.25 and 0.75 scored
worse than one at 0.5 (20.7 against 22.4 dB on an earlier set), consistent with the model's
off-centre results being its weaker ones; that wasn't investigated further.

## Not waiting for Present, 2026-11-06

Measurements in isolation missed the largest cost left. Mapping a D3D11 surface for CUDA needs
the device lock, and `IDXGISwapChain::Present` holds that lock while it blocks. Beside a real
blit-model swap chain presenting with vsync, one Coarse request took:

| Output surface | Map, ms | Unmap, ms | GPU, ms | Whole request, ms |
| --- | ---: | ---: | ---: | ---: |
| Alone | 0.06 | 0.06 | 2.54 | 2.63 |
| Beside Present | 0.07 | 1.41 | 2.54 | 4.21 |

Nearly all of the gap between isolated and playback timing was that wait, and it grows with the
time Present blocks. Nothing was computing.

So the surfaces CUDA uses every frame are now created shared, and the runtime imports each once
with `cudaImportExternalMemory`. No request takes the device lock, and the registration lookup that
took it even when there was nothing to register now doesn't. `PersistentSurfaces` falls back to
mapping for a texture that isn't shared or can't be imported, so nothing else about a caller has to
change. Ordering that the map used to provide is explicit:

- CUDA to D3D11: a request has finished writing when it returns, as before.
- D3D11 to CUDA: each staged input copy and each presentation copy signals an `ID3D11Fence` from
  the immediate context. A worker reads the fence, which takes no lock, before a job starts. Input
  copies are flushed as they are made and a finished job's copies when it is finalized, so the
  flush the fence's slow path falls back to is rare. The readiness probe reads the same fence where
  it had taken the lock twice per pair.

Output through the map and through the import is byte-identical on all three flow reuse modes when
the frame kernel below isn't involved. `RifePrecisionGpuTest` has D3D11 rewrite inputs between
pairs and read every output, ordered only by a fence, in both formats.

### The frame outside TensorRT

With surfaces imported, nothing needs the finished frame as a tensor. Having a TensorRT stage
write three FP16 planes for another kernel to convert was the last of that pattern. The whole and
tail plans now end at what the frame is made from, block 4's deconvolution and the accumulated
flow, and `MpcvrRifeFinishFrame` warps, blends and writes the surface in one kernel.

It and `MpcvrRifeRetimeFrame` also read the images differently. Measured alone on 1920x1152:

| Final warp and blend, BGRA8 written | ms |
| --- | ---: |
| Software bilinear from the warp source | 0.22 |
| GPU filter, pitch-linear texture | 0.14 |
| GPU filter, block-linear array | 0.14 |

So they sample the imported input surfaces through the GPU's filter, with no copy. The earlier
warps can't gain from this: the same test with 7 channels read and 32 stored took 0.55 ms either
way, since that stage is limited by storing 64 bytes per pixel. Hardware weights have eight
fractional bits, the one place the path that was exact is now not: against the warp source's result,
at most 0.0054 on random noise, and at most one 8-bit step on the moving test images. Scores against
true in-betweens didn't change in any digit shown above.

Distributed runtimes through the ABI, 1920x1080 in 1920x1152, 8x, the new one on shared surfaces.
Median of three alternating runs, GPU times each the minimum per run. Alone:

| Mode | Runtime | First, ms | Later, ms | Mean of 7, ms | Wall mean, ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Off | r2926 | 5.76 | 3.69 | 3.99 | 4.17 |
| Off | This | 5.53 | 3.50 | 3.79 | 3.86 |
| Coarse | r2926 | 5.63 | 2.52 | 2.96 | 3.13 |
| Coarse | This | 5.37 | 2.31 | 2.75 | 2.83 |
| Retime | r2926 | 5.93 | 0.31 | 1.11 | 1.33 |
| Retime | This | 5.70 | 0.15 | 0.95 | 0.99 |

Beside a 480x270 swap chain presenting with vsync, each Present blocking about 3 ms, GPU now the
median, since the point is what overlap does:

| Mode | Runtime | Unmap, ms | GPU mean of 7, ms | Wall mean, ms |
| --- | --- | ---: | ---: | ---: |
| Off | r2926 | 0.50 | 4.18 | 4.84 |
| Off | This | 0 | 4.44 | 4.50 |
| Coarse | r2926 | 1.36 | 3.11 | 4.76 |
| Coarse | This | 0 | 3.43 | 3.48 |
| Retime | r2926 | 0.83 | 1.25 | 1.73 |
| Retime | This | 0 | 1.14 | 1.17 |

Now wall and GPU nearly agree. GPU time rose in Off and Coarse because CUDA no longer stops during
Present and shares the GPU with whatever D3D11 is doing, so it is no longer a floor to compare
across this change. This swap chain only presents. It has no presenter's copies or shaders, so
playback's own figures will be higher, and this hasn't been measured in the player.

The per-pair copy of the shared frame was left alone. It costs about 0.03 ms a generated frame at
8x, and removing it means a ring both contexts bind, which every warp would have to index.

## Scheduler

The adaptive scheduler's delivery checks and recovery headroom were preserved.
Changing its estimates would not reduce GPU work. Recovery that accounts for
different fresh/reused costs needs separate, fresh timing evidence and playback
validation before changing policy.

## Correctness and limits

`RifeWarpGpuTest` tests every stage of the production plugin against an independent
CPU reference at 128x64, 128x128, 640x1152, 1792x1024, 1920x1088 and 1920x1152. It
checks dynamic profiles, serialization/deserialization, finite results and rejected
stages and sizes. TensorRT's own reformats sit between the plugin and the test's linear
buffers, so the interleaved layouts are checked as declared. The reference evaluates in FP32 as
the plugin does; contraction is disabled for this CUDA translation unit.

Stages 5, 6 and 7, which split the graph for flow reuse, have the same CPU reference, and
`MpcvrRifeRetimeLevel`, `MpcvrRifeRetimeFrame` and `MpcvrRifeFinishFrame` are checked against it
on linear, BGRA8 and RGBA16F targets, the frame kernels reading the warp source and imported
surfaces. At 1920x1088 the finished frame was within 0.00031 of the reference reading the warp
source and within 0.0054 through the filter, on noise.

`RifePrecisionGpuTest` compares each flow reuse mode with the whole model. At t = 0.5 they
agreed within 0.0039 on BGRA8 and 0.00049 on RGBA16F, a repeated target matched exactly, and
t = 0.25 and 0.75 stayed within 0.0078 of the truth on its gentle test motion. That is a check
of the mechanics. What reuse does to real motion is the section above.

It also has D3D11 rewrite both inputs between pairs of imported surfaces, ordered only by a fence,
and reads every output. Against mapping the same textures, all three modes stayed within 0.0039 on
BGRA8 and 0.0034 on RGBA16F, which is the frame kernel's filter.

`RifePrecisionGpuTest` passed with all three feature models, BGRA8/16F output,
pair invalidation, repeated reuse and independent contexts. Whole-model versus
split-model comparisons at 128x128 stayed below 0.01 normalized maximum error, and content
shorter than its surface stayed below 0.016 in its visible rows on gentle motion.

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
