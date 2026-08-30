# SVP4 interpolation archaeology — part 13: classifier-direction audit and Build #3 supersession

Date: 2026-08-30
Branch: `research/svp4-interpolation-archaeology`
Frozen live baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91` and must not be modified.

This note records a reproducibility audit performed after Part 12. It supersedes the interpretation that the Part 12 **B->A class-1 gate** was a validated SVP-like Build #3 candidate.

The modern 4x4 software-SAD confidence mechanism remains useful. The corrections are about **which directional vector array SVP's global classifier scores and how the resulting class participates in policy**.

## 1. Why the audit was necessary

Part 12 reported a particularly attractive separation:

```text
022518 -> class 0
022530 -> class 1
022539 -> class 1
022550 -> class 0
```

and used that class-1 state to gate the Adaptive Splat / robust-median reconstruction.

When the archived capture corpus was restored and the newly committed replay was rerun, most modern-SAD occupancies reproduced closely, but several did not:

```text
Part 12             committed clean replay
013339  31.73% C1   25.03% C1  on B->A
013416  36.02% C1   36.53% C2  on B->A
001431  39.09% C1   43.82% C2  on B->A
```

This was not caused by using a different corpus. As a checksum, the older MPCVR-side `q` measurement from the same Part 12 table was independently rerun and reproduced the recorded values across the restored corpus, including `013339=9.43`, `013416=18.53`, and `001431=27.74`.

The drift is therefore isolated to the transient modern-SAD study/replay, not the archived captures.

Several implementation possibilities were tested without explaining the discrepant rows: RGB/BGR ordering, BT.601 versus BT.709, limited versus full range, rounded versus truncated luma, opposite flow sign, integer block-origin offsets, fractional versus integer displacement, and out-of-bounds suppression.

The Part 12 table should consequently be treated as a useful exploratory result, not a canonical numeric oracle.

## 2. Proprietary classifier direction

The proprietary `svpflow2.dll` classifier around VA `0x180005fe0` selects between two vector-array pointers in the bidirectional vector structure.

The relevant selection is conceptually:

```text
selected = vector_array_at_0x48
if direction_flags does not contain bit 1:
    selected = vector_array_at_0x60
```

Normal bidirectional NVOF uses direction flags value `3`, so the classifier scores the array at `+0x48`.

The luma helper still combines the luma bytes from both `+0x48` and `+0x60`, which is why the normal pair-luma denominator remains 510.

Independent open-svpflow behavior corroborates the array semantics. Its NVOF generator constructs:

```text
current  = run_direction(A, B)
previous = run_direction(B, A)
```

and its pair classifier does:

```text
classify(current, luma(previous + current))
```

The combined binary and independent-source evidence therefore identifies the normal field-classification direction as **current A->B**, while both A->B and B->A luma contribute to normalization.

This matters because the MPCVR diagnostic filenames use the opposite naming convention:

```text
flow-forward-B-to-A-s10.5.bin   = B -> A
flow-backward-A-to-B-s10.5.bin  = A -> B
```

Part 12 / the first Build #3 script had deliberately used the B->A classifier result to reproduce its transient study. That is a historical replay choice, not proprietary classifier fidelity.

## 3. Restored-corpus directional audit

Using the committed modern software-SAD definition, pair-luma denominator 510, and the exact proprietary border / two-thirds zero-skip classifier:

| Capture | B->A class / m1+ | SVP-current A->B class / m1+ |
|---|---:|---:|
| 022518 | C0 / 9.04% | C0 / 8.46% |
| 022530 | C1 / 29.56% | **C2 / 40.88%** |
| 022539 | C1 / 28.44% | C1 / 26.23% |
| 022550 | C0 / 19.98% | **C1 / 21.39%** |
| 234826 | C0 / 17.73% | **C1 / 21.14%** |
| 234854 | C0 / 16.43% | C0 / 17.10% |
| 234925 | C1 / 20.07% | C1 / 21.29% |
| 234955 | C0 / 14.62% | C0 / 15.55% |
| 235102 | C1 / 23.80% | C1 / 31.71% |
| 013339 | C1 / 25.03% | C1 / 26.45% |
| 013416 | **C2 / 36.53%** | **C2 / 55.84%** |
| 001431 | C2 / 43.82% | **C1 / 40.30%** |
| 000820 | C1 / 22.61% | C1 / 28.14% |
| 020950 | C1 / 20.85% | C1 / 21.24% |
| 021001 | C1 / 22.30% | C1 / 26.05% |

The important consequence is immediate: the proprietary-current direction does **not** retain the simple Part 12 Boromir split. `022530` moves upward to class 2, while `022550` becomes class 1. `234826` also becomes class 1.

So there is no defensible basis for saying `class == 1` is the uniquely useful robust-reconstruction regime.

## 4. Corrected `force13` interpretation

A deeper policy trace corrected an over-narrow initial reading of the classifier's special `-1` return.

There are **two separate mechanisms**:

### 4.1 `blocks13` special result

The exact classifier can return:

```text
-1
```

through the separate `blocks13` occupancy threshold. The classifier wrapper exposes that condition through a separate boolean output and clips the ordinary returned class to nonnegative before later policy uses it.

Stock:

```text
blocks13 = 0
```

so this extra early threshold is dormant by default.

### 4.2 `scene.force13` configuration

`scene.force13` is a distinct option and defaults to:

```text
true
```

The supplied `override_list.txt` confirms the stock-facing value, and the proprietary binary stores/uses it independently of the `class == -1` signal.

Open-svpflow's independently reconstructed policy makes the intended ordinary-class behavior explicit:

```text
if mode is SmoothFps/NVOF
and scene_class is 1 or 2
and requested algorithm >= 11
and scene.force13 is true:
    effective algorithm = 13
```

Therefore **ordinary classes 1 and 2 are both robust-algorithm territory when `scene.force13=true`**. The special `blocks13 -> -1` signal is an additional mechanism, not the sole meaning of `force13`.

This changes the correct clean-room question from:

```text
Should class 1 enable robust median?
```

to:

```text
Can classes 1 and 2 safely use the robust algorithm-13 family when combined
with the rest of SVP's scene-phase, ownership, and confidence policy?
```

## 5. `scene.adaptive=210` is a separate phase policy

Another important correction is that `scene.adaptive` is not the algorithm selector.

Open-svpflow decodes decimal digits independently:

```text
210 -> [-1, 0, 1]
```

for classes 0, 1, and 2 respectively. `-1` means no adaptive override. For scene mode 3 and interpolation ratios of at least 2x:

```text
class 0 -> ordinary phase unchanged
class 1 -> scene-phase mode 0
class 2 -> scene-phase mode 1
class 3 -> handled separately as the worst/scene state
```

The scene-phase modes alter where synthetic output samples land relative to the real endpoints. Mode 1 biases the synthesized phase toward a real endpoint more aggressively than mode 0. This is a cadence/temporal-safety policy layered alongside `force13`, not a replacement for it.

That separation explains why copying only `class == 1 -> median` was incomplete even before the direction audit.

## 6. Gate-matrix replay at qscale 1600

A new research tool is committed as:

```text
tools/research/splat_candidate_v2_gate_matrix.py
```

It keeps the Part 12 historical B->A class-1 gate for reproducibility and compares it against explicit current-direction interpretations:

```text
historical-ba-class1
svp-current-class1
svp-current-class12
both-class1
either-class1
local-only
```

Selected `>4 LSB` modification rates at `qscale=1600`:

| Capture | historical B->A C1 | SVP-current C1 | SVP-current C1/C2 |
|---|---:|---:|---:|
| 022518 | 0 | 0 | 0 |
| 022530 | 2.58% | 0 | 2.58% |
| 022539 | 1.87% | 1.87% | 1.87% |
| 022550 | 0 | **0.48%** | **0.48%** |
| 234826 | 0 | **0.22%** | **0.22%** |
| 234925 | 0.68% | 0.68% | 0.68% |
| 235102 | 0.82% | 0.82% | 0.82% |
| 013339 | 0.10% | 0.10% | 0.10% |
| 013416 | 0 | 0 | **0.50%** |
| 001431 | 0 | **1.18%** | **1.18%** |
| 000820 | 0.79% | 0.79% | 0.79% |
| 020950 | 1.13% | 1.13% | 1.13% |
| 021001 | 1.51% | 1.51% | 1.51% |

The current `class in {1,2}` column is now the more relevant **force13-family** research condition, but it is still not a complete SVP policy replay because it lacks class-dependent scene-phase behavior and other synthesis/mask interactions.

It broadens support exactly where the corrected classifier says it should:

- restores `022530` despite it being class 2;
- activates `013416` at ~0.50% >4 LSB;
- also activates `022550`, `234826`, and `001431`.

So the direction correction does not kill robust median; it shows why **class-aware policy** is required before promotion.

## 7. `013416` exposure-enhanced safety check

The previously outstanding qscale-1600 exposure/difference inspection was completed for the `svp-current-class12` variant.

Metrics:

```text
>1 LSB : 3.69%
>4 LSB : 0.503%
>8 LSB : 0.0093%
alpha mean : 0.0441
alpha > .05: 11.66%
```

The x32 difference map is sparse and concentrated around existing motion/warp structures. Side-by-side gamma-lifted GOLD and OUT crops did **not** reveal a newly coherent stretched/liquid geometry structure comparable to the earlier failed broad raw-flow experiments.

That is encouraging for the **local** modern-SAD + robust-median construction. It is not yet enough to validate the **global policy**. `001431`, for example, receives ~1.18% >4-LSB changes under the class1/2 family and needs equal scrutiny.

## 8. What survives from Part 12

The following findings remain useful:

- modern proprietary SVP can derive image-domain 4x4 luma SAD confidence without live NVIDIA cost output;
- the normal bidirectional pair-luma denominator is 510;
- qscale around 1600 remains a sensible local confidence-attenuation scale to investigate;
- Adaptive Splat + channelwise median remains far safer than directly promoting raw vectors;
- hardware cost should remain disabled in the live Turing renderer;
- coverage/ownership, vector confidence, global field quality, robust algorithm selection, and scene-phase policy must remain separate signals.

What is superseded:

- the Part 12 modern-SAD table as an exact numeric oracle for all captures;
- the claim that its B->A class-1 pattern is the proprietary field classifier's natural Build #3 gate;
- treating ordinary class 1 alone as the full `force13` policy.

## 9. Revised next direction

Do **not** create a live Build #3 from the old class-1 gate.

The next offline target is a fuller policy replay:

1. use the actual current A->B field classifier;
2. treat classes 1 and 2 as eligible for algorithm-13-style robust reconstruction when `scene.force13=true`;
3. reproduce the `scene.adaptive=210` class-dependent phase mapping separately;
4. keep modern SAD as local vector-confidence attenuation rather than a global motion-trust switch;
5. preserve coverage/ownership as an independent cue;
6. compare the resulting policy on `022530`, `022539`, `022550`, `013416`, `001431`, ordinary-motion captures, and true cuts;
7. only return to a live candidate if that complete policy preserves the Boromir gains without recreating cadence collapse or liquid geometry.

The frozen baseline remains the live reference throughout this work.
