# SVP4 interpolation archaeology — part 18: coverage masks and algorithm-21/22 replay

Date: 2026-08-30
Branch: `research/svp4-interpolation-archaeology`

The live Build 3 branch is intentionally untouched by this work.

Part 17 established the proprietary runtime control flow for `scene.adaptive`, `scene.force13`, and `blocks13`, then identified algorithms 21/22 as a separate coverage-aware synthesis family. This part reconstructs that coverage path against the restored MPCVR NVOF capture corpus and asks a narrower engineering question: does SVP21-style directional hole repair add a useful mechanism to the current golden-anchored Build 3 candidate?

## 1. Proprietary coverage-mask arithmetic

The proprietary mask generator consumes the opposite-direction vector field and a phase value. For each vector cell it computes a phase-scaled displacement using the vector marker:

```text
dx_phase = trunc(vector_dx * phase / (marker * 256))
dy_phase = trunc(vector_dy * phase / (marker * 256))
```

It then bilinearly splats the displaced 4x4 block footprint into a coverage accumulation grid, applies a 3x3 coverage window, and converts missing coverage to an 8-bit mask:

```text
covered   = accumulated_3x3 >> 3
remaining = max(block_area - covered, 0)

mask = clamp_0_255(
    remaining
    * (mask.cover / 100)
    * 256
    / block_area
)
```

Stock `mask.cover=100` therefore makes the mask approximately normalized uncovered block area.

Direction usage at interpolation time is:

```text
maskA = coverage(opposite A->B field, 256-phase)
maskB = coverage(opposite B->A field, phase)
```

The independent open-svpflow renderer has the same high-level `coverage_mask -> splat_coverage -> 3x3 window -> finish_coverage` construction.

## 2. NVOF vector-unit caveat

The proprietary mask-generator arithmetic itself is binary-confirmed, but the supplied MPCVR capture files contain raw NVIDIA S10.5 vectors rather than the proprietary in-memory vector payload.

For full-resolution NVOF, the independent reconstruction packs raw S10.5 vectors as:

```text
stored vector = trunc(raw / 8)
marker = 4
```

The normal renderer then divides the stored vector by marker, recovering approximately `raw/32` pixels.

Using those pre-marker stored units in the proprietary coverage formula gives the expected midpoint displacement:

```text
(raw/8) * 128 / (4*256) = raw/64
```

which is half of the full `raw/32` optical-flow displacement.

The replay therefore uses `raw/8` as its primary vector-unit interpretation. It also exposes `--vector-divisor=32` as a sensitivity check for the alternative of feeding already marker-divided pixel vectors into the same mask routine.

This distinction is large. Representative mean directional mask values at phase 128 are:

| capture | pre-marker `/8` mask A/B | post-marker `/32` mask A/B |
|---|---:|---:|
| `022530` | 50.1 / 65.3 | 23.1 / 25.5 |
| `022539` | 58.4 / 63.8 | 23.1 / 33.2 |
| `013416` | 89.5 / 47.6 | 31.4 / 19.5 |
| `001431` | 62.3 / 76.4 | 21.1 / 23.5 |

Until a proprietary NVOF vector payload or coverage surface is captured directly, the `/8` input mapping should be described as the best-supported reconstruction rather than a captured oracle.

## 3. Algorithm 21 and 22 pixel formulas

With directional warped samples `warpA` and `warpB` and the two coverage masks:

```text
A' = blend255(warpA, warpB, maskA)
B' = blend255(warpB, warpA, maskB)
```

Algorithm 21 performs:

```text
out21 = blend256(A', B', phase)
```

Algorithm 22 instead bounds the ordinary unwarped temporal sample between those corrected hypotheses:

```text
base  = blend256(A, B, phase)
out22 = clamp(base between A' and B')
```

This replay uses the reconstructed integer NVOF vector scaling and the same 4x4 / origin-2 spatial interpolation model. It is an archaeology diagnostic, not a canonical byte-exact SVP render oracle.

## 4. Direct replay on the restored corpus

`>4 LSB` changed-pixel percentages versus the frozen MPCVR midpoint:

| capture | temporal | mode11 | mode13 | mode21 | mode22 |
|---|---:|---:|---:|---:|---:|
| `001431` | 41.90% | 39.28% | 33.43% | **40.32%** | 36.20% |
| `013416` | 31.47% | 47.92% | 33.69% | **48.44%** | 39.07% |
| `022518` | 24.11% | 14.97% | 11.85% | **15.54%** | 13.74% |
| `022530` | 12.78% | 42.17% | 29.67% | **44.08%** | 37.70% |
| `022539` | 7.88% | 38.23% | 25.70% | **40.91%** | 33.37% |
| `022550` | 18.16% | 33.61% | 24.62% | **34.37%** | 28.83% |

Across the full 18-capture corpus, the pattern is stable:

- direct algorithm 21 is generally farther from the frozen midpoint than reconstructed algorithm 13;
- algorithm 22 is safer than 21 but still usually farther than 13;
- the coverage correction does not rescue the current raw NVOF warp geometry.

This does **not** prove SVP21 is intrinsically inferior. SVP21 was designed around SVP's own vector-generation and synthesis pipeline. It shows that directly transplanting its pixel formula onto the current MPCVR NVOF fields is not a safe move.

## 5. Structural failure is stronger than the scalar metric suggests

Connected components of the `>4 LSB` difference mask show that algorithm 21 tends to form large coherent islands rather than small local corrections.

Representative largest-component area as a percentage of the entire frame:

| capture | mode13 | mode21 | mode22 | Build 3 |
|---|---:|---:|---:|---:|
| `013416` | 9.59% | **38.02%** | 22.98% | **0.087%** |
| `022530` | 3.93% | **6.88%** | 4.19% | **0.618%** |
| `022539` | 2.03% | **10.17%** | 5.41% | **0.458%** |
| `022550` | 4.09% | **19.10%** | 13.11% | **0%** |
| `001431` | 5.80% | **15.06%** | 7.15% | quarantined |

Large-component total area (`components >= 1000 px`) tells the same story. For example:

```text
022530:
  mode13  26.24%
  mode21  40.64%
  mode22  33.88%
  Build3   1.39%

022539:
  mode13  21.60%
  mode21  37.38%
  mode22  28.94%
  Build3   1.17%
```

Thus direct SVP21-style synthesis is rejected for the same broad reason as direct algorithm 13: the current NVOF motion geometry is not trustworthy enough to let the raw synthesis family own large portions of the image.

## 6. Does the coverage-hole signal improve Build 3 as a guard?

Build 3 already has a separate splat-support gate. To test whether SVP's coverage mask supplies independent safety information, the saved Build 3 alpha surface was compared against the reconstructed bilateral-hole mask.

Only a small fraction of Build 3's authority mass falls in regions where **both** directional SVP masks report meaningful holes (`mask > 32`):

```text
022530  ~1.80% of Build3 alpha mass
022539  ~1.13%
013416  ~2.27%
013339  ~4.43%
235102  ~2.22%
```

At a stronger bilateral-hole threshold (`mask > 128`), the overlap is below 1% of authority mass in every tested active capture and about 0.12-0.85% in the representative cases.

This is a useful independent validation of the current design: Build 3's support mechanism is already avoiding almost exactly the regions where SVP21's coverage system detects bilateral holes.

## 7. Explicit Build 3 attenuation test

As a final check, Build 3's existing correction was attenuated by the reconstructed bilateral-hole mask without changing its hypothesis:

```text
out' = golden + (Build3 - golden) * hole_guard
```

Several guards were tested, including a direct linear attenuation and smooth ramps over 32->128 and 64->160.

The effect is negligible:

```text
022530: 0.000% of pixels move >4 LSB relative to Build3
022539: 0.000% of pixels move >4 LSB relative to Build3
013416: <=0.003% move >4 LSB relative to Build3
```

Therefore adding the SVP21 bilateral-hole mask purely as another Build 3 suppression term would add complexity without materially changing the current candidate.

## 8. Decision

The algorithm-21 investigation produces a useful negative/validation result:

1. **Do not transplant raw algorithm 21 or 22 synthesis onto the current NVOF fields.**
2. **Do not add the bilateral SVP coverage mask merely as another Build 3 confidence gate.** The existing support gate already rejects almost all of those regions.
3. The coverage reconstruction remains valuable as an independent diagnostic and as groundwork for algorithm 23.
4. Algorithm 23 is still interesting because it does something Build 3 does not: it attempts to *recover* a directional hole using motion from the adjacent frame pair rather than merely suppressing authority.
5. Before an algorithm-23 experiment, the remaining stock-adjacent safety mechanism worth reconstructing is `mask.area`, because it can explicitly pull motion synthesis back toward the temporal base.

The live Build 3 branch remains unchanged.

## 9. Reproducibility

Tool:

```text
tools/research/svp21_coverage_replay.py
```

Primary corpus run:

```text
python tools/research/svp21_coverage_replay.py <capture dirs...> \
  --phase 128 \
  --vector-divisor 8 \
  --mask-cover 100 \
  --json summary-div8.json
```

Vector-scale sensitivity:

```text
python tools/research/svp21_coverage_replay.py <capture dirs...> \
  --phase 128 \
  --vector-divisor 32 \
  --mask-cover 100 \
  --json summary-div32.json
```

The golden midpoint is used only as the frozen MPCVR reference for measuring how broadly each reconstruction departs from the known-safe baseline. Distance from that baseline is not itself a ground-truth interpolation-quality score.
