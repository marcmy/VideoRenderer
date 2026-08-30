# SVP4 interpolation archaeology — part 11: exact classifier denominator, dual cost, and NVOF source format

Date: 2026-08-29/30
Branch: `research/svp4-interpolation-archaeology`
Frozen baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`.

This continuation closes several remaining ambiguities around SVP's field-quality classifier and identifies another concrete difference between SVP's NVOF input path and MPCVR's native path.

## 1. Exact proprietary classifier loop

The classifier entry in the supplied `plugins64/svpflow2.dll` is around VA:

```text
0x180005fe0
```

The call site around `0x180011a98` supplies:

```text
blocks%       <- config +0x1fc
blocks13%     <- config +0x200
zero          <- config +0x204
m1            <- config +0x208
m2            <- config +0x20c
scene         <- config +0x210
luma gamma    <- config +0x218 (used before classifier)
ignore        <- config +0x220
```

Stock defaults remain:

```text
blocks   = 20
blocks13 = 0
zero     = 200
m1       = 1600
m2       = 2800
scene    = 4000
ignore   = 0.04
luma     = 1.5
```

## 2. Exact border exclusion

For grid width/height, the classifier computes:

```text
borderX = trunc(width  * ignore)
borderY = trunc(height * ignore)
```

The binary constant involved in the minimum-border test is exactly:

```text
0.01
```

When `ignore > 0.01`, each border is clamped to at least one cell:

```text
borderX = max(borderX, 1)
borderY = max(borderY, 1)
```

Cells are considered only when:

```text
borderX <= x <= width  - borderX - 1
borderY <= y <= height - borderY - 1
```

Thus default `ignore=.04` excludes the outer ~4% of the motion grid and always excludes at least one cell on each edge.

## 3. Exact normalized vector score

For each interior cell the DLL loads the 24-bit vector score and computes:

```text
normalized = score * 255 / max(lumaMap[cell], 1)
```

The multiplication and division are integer operations in the proprietary implementation.

This exactly corroborates the independently reconstructed open-svpflow scene classifier's normalization order.

## 4. Exact zero-score denominator floor

This was previously understood only approximately. The assembly now makes the denominator behavior exact.

Before the loop the classifier computes:

```text
zeroSkipLimit = floor(2 * fullGridCellCount / 3)
```

`fullGridCellCount` is `width * height`, before border exclusion.

For an interior cell with:

```text
normalized < zero
```

SVP excludes that cell from the classifier denominator only while:

```text
zeroSkipped < zeroSkipLimit
```

After `zeroSkipLimit` low-score cells have been omitted, additional low-score interior cells increment the considered-cell denominator normally (but do not enter m1/m2/scene severity counts).

Conceptually:

```text
considered = 0
zeroSkipped = 0

for each interior cell:
    q = score*255/max(luma,1)

    if q < zero and zeroSkipped < floor(2*fullGrid/3):
        zeroSkipped += 1
        continue

    considered += 1
    classify q into m1/m2/scene bands
```

This is the exact reason the denominator cannot collapse to only a tiny set of bad vectors: at most about two thirds of the full grid can be removed as near-zero-error vectors, leaving an effective denominator floor around one third of the full field (modulo the small ignored border).

## 5. Exact severity/occupancy return logic

Counts are exclusive severity bands:

```text
sceneCount : q >= scene
m2Count    : m2 <= q < scene
m1Count    : m1 <= q < m2
```

Ordinary required occupancy is integer:

```text
required = floor(blocks * considered / 100)
```

Return:

```text
if sceneCount >= required:
    return 3

if sceneCount + m2Count >= required:
    return 2

if sceneCount + m2Count + m1Count >= required:
    return 1
```

If ordinary classification would be zero and `blocks13 > 0`, SVP computes:

```text
required13 = floor(blocks13 * considered / 100)
```

and returns the special class:

```text
-1
```

when total m1+ occupancy reaches that lower threshold.

Stock `blocks13=0` leaves this special lower threshold dormant.

## 6. Exact luma-map construction before classification

The helper immediately before the classifier is around:

```text
0x180005e60
```

For each cell it loads the luma byte stored with the previous-direction vector and the luma byte stored with the current-direction vector.

The normalization denominator is:

```text
marker == 3 ? 510.0 : 255.0
```

The binary constants at `0x180056910` and `0x180056918` decode exactly to `255.0` and `510.0`.

Then:

```text
v = (previousLuma + currentLuma) / denom
lumaMap = trunc(pow(v, scene.luma) * 255)
```

with default:

```text
scene.luma = 1.5
```

The result has a floor of 20 before storing its low byte:

```text
if result < 21:
    result = 20
lumaMapByte = result & 0xff
```

This behavior matches open-svpflow's independent `luma_byte()` reconstruction, including its byte wrapping rather than an ordinary saturating clamp above 255.

## 7. How open-svpflow builds NVOF vector score/luma

Open-svpflow explicitly requests NVIDIA's **legacy 32-bit cost** (`NV_FORMAT_UINT`) and downloads one `u32` cost value per NVOF cell.

For each 4x4 source block:

```text
costShift = floor(2 * log2(max(scale,1)))
raw1 = (cost << costShift) + ((sum4x4Luma & 0xFFF0) << 20)   // u32 arithmetic
score = raw1 & 0x00ffffff
luma  = raw1 >> 24
```

Thus, in the usual no-overflow case, the vector's luma byte is approximately the average 8-bit luma of its 4x4 NVOF input block while `score` carries the scale-adjusted legacy NVIDIA cost.

The two directions use their respective input/reference frame luma when packing their vectors.

## 8. Meaning of `scale` / `nvof_grid`

Open-svpflow obtains:

```text
scale = originalVideoWidth / vectorSourceWidth
```

The supplied SVP4 `script/generate.js` shows exactly how Manager creates that vector source:

Avisynth conceptually:

```text
input_m8 = input_m8.BicubicResize(
    input_m.width  / nvof_blk * 4,
    input_m.height / nvof_blk * 4,
    ...)
```

VapourSynth equivalently uses integer division and crops the source extent to a multiple of `nvof_blk`.

Native NVOF still runs with a 4x4 output grid. `nvof_grid` therefore means the **effective full-resolution spacing** achieved by resizing the NVOF source first.

Approximate mapping:

| nvof_grid | NVOF input size | integer scale | costShift | score cost multiplier |
|---:|---:|---:|---:|---:|
| 4 | full | 1 | 0 | 1 |
| 8 | 1/2 | 2 | 2 | 4 |
| 12 | 1/3 | 3 | 3 | 8 |
| 16 | 1/4 | 4 | 4 | 16 |
| 24 | 1/6 | 6 | 5 | 32 |
| 32 | 1/8 | 8 | 6 | 64 |

The multiplier uses the power-of-two `costShift`; it is not simply `scale^2` for non-power-of-two scale values.

Manager dynamically lowers `nvof_grid` when the reduced vector source would become too small:

```text
while grid > 4 and (dst_w/grid < 40 or dst_h/grid < 32):
    32 -> 24
    24 -> 16
    otherwise grid /= 2
```

The installed snapshot does not contain a fixed `profile.nvof_grid` default. The generator consumes the active profile value, so exact grid selection must be obtained from runtime/profile state or studied as a parameter sweep.

## 9. Another concrete SVP-vs-MPCVR difference: NVOF input format

The native MPCVR baseline feeds NVIDIA Optical Flow:

```text
BGRA8
```

The supplied SVP4 script explicitly constructs its NVOF vector source as:

```text
YUV420P8
```

Open-svpflow's NVOF implementation then uploads that source as:

```text
NV12
```

Therefore even at the same SLOW quality preset and nominal 4x4 NVOF engine grid, the two implementations are not necessarily feeding identical image representations into the NVOF engine.

This does **not** prove NV12 is better than BGRA8. It establishes a concrete variable that should be measured.

Potential mechanisms worth testing independently:

- direct luma input avoids driver RGB-to-luma conversion differences;
- 4:2:0 chroma removes some high-frequency color detail/noise from the motion-estimation source;
- the larger SVP effect may actually come from source downscale rather than the format itself;
- format and downscale may interact.

Do not change MPCVR's production NVOF input format without a controlled standalone replay comparison.

## 10. Dual R8/R32 replay utility

Because NVIDIA documentation identifies `UINT` as legacy 32-bit cost and `UINT8` as the newer bandwidth-efficient format but does not publish a numeric conversion, the standalone replay utility has been extended with a dual-format experiment.

New source:

```text
tools/native-nvof-probe/NativeNvofCostReplayDual.cpp
```

The tool creates **separate NVOF sessions** for R8 and R32 to avoid cross-format session state contaminating the comparison.

For each supported format it records:

- forward/backward cost binaries;
- forward/backward replay flow;
- execution + blocking readback time;
- cost mean/p50/p90/p99;
- replay-flow difference from the original MPCVR capture.

When both formats succeed it additionally records:

- R8-vs-R32 flow difference;
- Pearson correlation between cost maps;
- affine least-squares `R32 ~= a*R8 + b`;
- median nonzero `R32/R8` ratio;
- best simple `min(R32 >> shift,255)` mapping over shifts 0..24;
- exact-match rate and MAE of that best shift.

Unsupported R32 is recorded as a result rather than making an otherwise-valid R8 replay fail.

The dual utility compiles successfully under MSVC C++20 with `/W4 /WX` in the dedicated Windows CI workflow.

## 11. Why a full-resolution R32 replay is not enough to emulate coarse SVP grids

A critical caution:

```text
full-resolution R32 cost * scale multiplier
```

is **not** equivalent to:

```text
run NVOF on SVP's downscaled input, then scale that resulting cost
```

Downscaling changes the motion-estimation problem itself and can change both vectors and raw NVIDIA costs.

Therefore, if cost/grid research proves useful, the correct next experiment is a standalone **input-grid sweep** that actually resizes the NVOF source and executes NVOF at those sizes. Do not fake coarse-grid SVP behavior by merely multiplying the full-resolution cost map.

## 12. Current interpretation

The remaining SVP advantage increasingly looks cumulative rather than mysterious:

```text
NVOF source representation (YUV420/NV12)
+ optional source downscale / effective grid
+ NVIDIA or image-domain vector quality
+ exact field-health classifier
+ cover/uncover ownership
+ robust algo13 median
+ nearest-endpoint artifact fallback
+ optional neighboring-pair hypotheses
```

MPCVR already has a strong golden reconstruction around its native full-resolution BGRA8 NVOF field. Build #2 Adaptive Splat V1.2 addresses the robust reconstruction/ownership side. The dual-cost and future input-grid/format probes now target the **motion-field / confidence side** without destabilizing live playback.
