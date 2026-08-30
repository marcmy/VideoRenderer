# SVP4 interpolation archaeology — part 11: exact classifier, cost packing, and NVOF source geometry

Date: 2026-08-29/30
Branch: `research/svp4-interpolation-archaeology`
Frozen baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`.

This note records the exact proprietary field-classifier behavior, open-svpflow's corroborating NVOF score packing, and the now-confirmed proprietary NVOF source-ratio constraints. It supersedes the earlier temporary assumption that effective grid 12 / source ratio 1/3 might be valid.

## 1. Exact proprietary field classifier

The classifier in the supplied `plugins64/svpflow2.dll` is around VA `0x180005fe0`.

Stock values passed by its caller are:

```text
blocks   = 20
blocks13 = 0
zero     = 200
m1       = 1600
m2       = 2800
scene    = 4000
ignore   = 0.04
scene.luma = 1.5
```

The outer border is:

```text
borderX = trunc(width  * ignore)
borderY = trunc(height * ignore)
```

When `ignore > 0.01`, each is clamped to at least one cell.

For every retained cell, the DLL computes integer:

```text
normalized = score24 * 255 / max(lumaMap[cell], 1)
```

The low-score denominator exclusion is now exact:

```text
zeroSkipLimit = floor(2 * fullGridCellCount / 3)
```

A cell with `normalized < zero` is excluded from the denominator only while `zeroSkipped < zeroSkipLimit`. After that cap, additional low-score cells count in `considered` but not in any severity band. This prevents the denominator from collapsing to a tiny set of bad vectors.

Severity bands are exclusive:

```text
sceneCount : normalized >= scene
m2Count    : m2 <= normalized < scene
m1Count    : m1 <= normalized < m2
```

Then:

```text
required = floor(blocks * considered / 100)

sceneCount                         >= required -> class 3
sceneCount + m2Count               >= required -> class 2
sceneCount + m2Count + m1Count     >= required -> class 1
```

If ordinary classification is zero and `blocks13 > 0`, SVP computes `floor(blocks13 * considered / 100)` and can return the special class `-1`. Stock `blocks13=0` leaves that path dormant.

## 2. Exact classifier luma map

The helper immediately before the classifier combines the directional stored luma bytes:

```text
denom = marker == 3 ? 510.0 : 255.0
v = (previousLuma + currentLuma) / denom
scaled = trunc(pow(v, scene.luma) * 255)
if scaled < 21:
    scaled = 20
lumaMapByte = scaled & 0xff
```

The proprietary binary constants decode exactly to `255.0` and `510.0`; default `scene.luma` is `1.5`.

Open-svpflow independently reconstructs the same normalization order and the same low-byte luma behavior, so it is useful corroboration for the score/luma encoding. The proprietary DLL remains the authority for the more unusual denominator-floor and border behavior.

## 3. Open-svpflow legacy R32 NVOF cost packing

Open-svpflow explicitly requests NVIDIA's legacy 32-bit cost format (`NV_FORMAT_UINT`) and downloads one `u32` cost per NVOF cell.

For each 4x4 NVOF input block it packs:

```text
costShift = floor(2 * log2(max(scale,1)))
raw1 = (cost << costShift) + ((sum4x4Luma & 0xFFF0) << 20)  // u32 arithmetic
score = raw1 & 0x00ffffff
luma  = raw1 >> 24
```

In the normal no-overflow case, `luma` is approximately the 4x4 source-luma average and `score` carries the scale-adjusted legacy NVIDIA cost.

The exact classifier oracle preserving the proprietary integer behavior is committed as:

```text
tools/research/svp_exact_field_classifier.py
```

## 4. Proprietary NVOF source ratios are now proven

A string embedded in the supplied proprietary `svpflow2.dll` states:

```text
SVSmoothFps/NVOF: 'vec_src' must be in [1/1,1/2,1/4,1/6,1/8] of the source size
```

A neighboring diagnostic states:

```text
SVSmoothFps/NVOF: minimal 4*4 blocks amount is 40*32
```

Therefore valid source scale factors are exactly:

```text
1, 2, 4, 6, 8
```

and the corresponding effective full-resolution motion grids are:

```text
4, 8, 16, 24, 32
```

**Effective grid 12 / source ratio 1/3 is not a valid proprietary SVP NVOF mode.** Earlier exploratory notes that included grid 12 are superseded by this binary evidence.

## 5. Exact `nvof_grid` geometry

The supplied `script/generate.js` crops the right/bottom source extent to a multiple of the requested effective grid and then creates the NVOF vector source at `4 / nvof_grid` of that cropped size.

Conceptually:

```text
cropW  = sourceW - sourceW % nvof_grid
cropH  = sourceH - sourceH % nvof_grid
vecW   = cropW / nvof_grid * 4
vecH   = cropH / nvof_grid * 4
```

NVIDIA Optical Flow itself still runs with native output grid 4 over that resized source.

Correct scale/cost-shift mapping:

| effective `nvof_grid` | vector source | scale | `costShift` | score-cost multiplier |
|---:|---:|---:|---:|---:|
| 4  | 1/1 | 1 | 0 | 1 |
| 8  | 1/2 | 2 | 2 | 4 |
| 16 | 1/4 | 4 | 4 | 16 |
| 24 | 1/6 | 6 | 5 | 32 |
| 32 | 1/8 | 8 | 6 | 64 |

The multiplier is SVP's packed-score compensation; it does not make a full-resolution NVOF run equivalent to actually running NVOF on the reduced source.

Manager dynamically lowers an oversized grid when the resulting NVOF source would violate its minimum dimensions. On the current ~1918x803 capture corpus, effective grid 32 would produce only about 100 source rows and therefore fails the `40x32` four-pixel-cell minimum; the natural maximum is effective grid 24.

The installed archive does not contain a fixed active `profile.nvof_grid` value. Grid selection is profile/runtime state, not a fixed engine constant.

## 6. NVOF input-format difference: MPCVR versus SVP

Current native MPCVR feeds NVOF:

```text
BGRA8
```

The supplied SVP script explicitly converts its vector source to:

```text
YUV420P8
```

and open-svpflow uploads that source to NVIDIA as:

```text
NV12
```

This is a real implementation difference. It does not by itself prove NV12 is better, but it means identical NVOF quality/grid settings do not necessarily receive identical image representations.

Potential effects to measure independently include RGB-to-luma conversion differences, chroma subsampling/noise suppression, downscale itself, and interactions between format and downscale.

## 7. Dual R8/R32 hardware-cost replay

NVIDIA documents R8 as the newer bandwidth-efficient cost format and R32 as legacy, but does not publish a numeric conversion. We therefore do not assume that R8 is simply the low byte or a fixed shift of R32.

The standalone utility:

```text
tools/native-nvof-probe/NativeNvofCostReplayDual.cpp
```

runs R8 and R32 in separate one-pair sessions and records:

- format support;
- cost mean/p50/p90/p99;
- replay flow versus original MPCVR capture flow;
- R8-versus-R32 flow difference;
- Pearson cost correlation;
- affine `R32 ~= a*R8+b` fit;
- median nonzero `R32/R8` ratio;
- best simple `min(R32 >> shift,255)` mapping.

This compiles cleanly under MSVC C++20 `/W4 /WX`. Runtime support and numeric relationship still require the actual Turing driver.

## 8. Standalone input-format/effective-grid sweep

A second diagnostic deliberately keeps **NVOF output cost disabled** so it can study input geometry/format without repeating the historical Turing cost stalls:

```text
tools/native-nvof-probe/NativeNvofInputSweep.cpp
```

It includes:

```text
bgra-native
bgra/nv12 grid 4
bgra/nv12 grid 8
bgra/nv12 grid 16
bgra/nv12 grid 24
bgra/nv12 grid 32 when SVP minimum dimensions allow it
```

Each variant gets a fresh PerfSlow, grid4, bidirectional, temporal-hints-disabled session with cost disabled.

The NV12 approximation converts capture BGRA to BT.709 limited-range YUV420 **before** separately resizing the Y/U/V planes with Catmull-Rom bicubic, matching the ordering of the supplied VapourSynth script more closely than resizing RGB first. It remains an approximation because the diagnostic BMP has already passed through a video-to-RGB conversion and cannot recover the decoder's original YUV samples bit-for-bit.

The tightened tool and recursive batch packaging compile successfully under MSVC `/W4 /WX`.

## 9. Current interpretation

The remaining SVP advantage increasingly looks cumulative rather than mysterious:

```text
SVP-like NVOF input representation (YUV420/NV12)
+ optional valid source downscale (1/1,1/2,1/4,1/6,1/8)
+ NVIDIA / image-domain vector-quality information
+ exact field-health classifier
+ cover/uncover ownership
+ robust algo13 median
+ nearest-endpoint artifact fallback
+ optional neighboring-frame-pair hypotheses
```

MPCVR's frozen golden reconstruction is already strong around its full-resolution BGRA NVOF field. Adaptive Splat V1.2 targets the reconstruction/ownership side, while the dual-cost and input-format/grid probes isolate the remaining motion-field/confidence differences without destabilizing live playback.
