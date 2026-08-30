# SVP4 interpolation archaeology — part 13: classifier-direction audit and Build #3 supersession

Date: 2026-08-30
Branch: `research/svp4-interpolation-archaeology`
Frozen live baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91` and must not be modified.

This note records a reproducibility audit performed after Part 12. It supersedes the interpretation that the Part 12 **B->A class-1 gate** was a validated SVP-like Build #3 candidate.

The modern 4x4 software-SAD confidence mechanism remains useful. The correction is about **which directional vector array SVP's global classifier scores, and what a class value is allowed to mean**.

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

## 2. Exact proprietary classifier direction

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

and its pair classifier explicitly does:

```text
classify(current, luma(previous + current))
```

The combined binary and independent-source evidence therefore identifies the normal proprietary field-classification direction as **current A->B**, while both A->B and B->A luma contribute to normalization.

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

## 4. `force13` is not ordinary class 1

The classifier call site also resolves an earlier conceptual ambiguity.

After the classifier returns, the proprietary path tests specifically for:

```text
class == -1
```

and sets the `force13` boolean from that comparison.

The exact classifier returns `-1` only through the separate `blocks13` occupancy threshold. Stock configuration has:

```text
blocks13 = 0
```

so the special force13 state is dormant under stock defaults.

Therefore this clean-room heuristic:

```text
ordinary class 1 -> enable robust median
```

was never an exact reproduction of SVP's `force13` mechanism. Ordinary classes 0/1/2 are instead inputs to the separate adaptive scene/algorithm policy.

That distinction is now important enough that future research must not label an ordinary class-1 gate as `force13`.

## 5. Gate-matrix replay at qscale 1600

A new research tool is committed as:

```text
tools/research/splat_candidate_v2_gate_matrix.py
```

It keeps the Part 12 historical B->A class-1 gate for reproducibility and compares it against the now-correct directional interpretations:

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

No simple replacement gate wins this audit:

- current `class == 1` loses `022530` while adding `022550`, `234826`, and `001431`;
- current `class in {1,2}` restores `022530` and the ~0.50% `013416` experiment, but also broadens support further;
- requiring both directions to be class 1 rejects important class-2 cases by construction and still has no proprietary justification.

The global class is therefore better treated as a **policy input**, not a one-bit permission gate.

## 6. `013416` exposure-enhanced safety check

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

That is a useful local safety result, but it does not validate the global gate. `001431`, for example, receives ~1.18% >4-LSB changes from the actual current-direction class-1 gate and must be evaluated as part of any future policy.

## 7. What survives from Part 12

The following findings remain useful:

- modern proprietary SVP can derive image-domain 4x4 luma SAD confidence without live NVIDIA cost output;
- the normal bidirectional pair-luma denominator is 510;
- qscale around 1600 remains a sensible local confidence-attenuation scale to investigate;
- Adaptive Splat + channelwise median remains far safer than directly promoting raw vectors;
- hardware cost should remain disabled in the live Turing renderer;
- coverage/ownership, vector confidence, global field quality, and robust synthesis must remain separate signals.

What is superseded:

- the Part 12 modern-SAD table as an exact numeric oracle for all captures;
- the claim that its B->A class-1 pattern is the proprietary field classifier's natural Build #3 gate;
- treating ordinary class 1 as equivalent to `force13`.

## 8. Revised next direction

Do **not** create a live Build #3 from the current class gate.

The next useful archaeology target is now the policy layer after classification:

1. finish the exact `scene.adaptive = 210` mapping from classes 0/1/2 into synthesis behavior;
2. distinguish that adaptive mapping from the special `blocks13 -> -1 -> force13` path;
3. determine whether class 2 is primarily an algorithm-selection change, an artifact-mask-strength change, or both;
4. keep modern SAD as a local vector-confidence attenuation term regardless of the global policy;
5. only return to a live candidate after a policy replay preserves the Boromir improvements without unnecessarily activating the cleaner corpus.

The frozen baseline remains the live reference throughout this work.
