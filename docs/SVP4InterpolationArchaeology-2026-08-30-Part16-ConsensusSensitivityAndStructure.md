# SVP4 interpolation archaeology — part 16: consensus sensitivity and structural safety

Date: 2026-08-30
Branch: `research/svp4-interpolation-archaeology`
Frozen live baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91` and must not be modified.

Part 15 introduced the dual-direction consensus policy for the existing golden-anchored Adaptive Splat hybrid. This part checks whether that policy is merely a corpus knife-edge and quantifies the visual distinction between the bounded candidate and the rejected direct algorithm-13 replay.

The result is encouraging enough to justify a separate live/test branch from the frozen baseline, while keeping all archaeology work isolated on this branch.

## 1. Occupancy-ramp sensitivity

The Part 15 default is:

```text
occupancyGain = smoothstep(20, 27, min(m1plus_BA, m1plus_AB))
```

A nearby sweep shows the Boromir result is not dependent on exactly those endpoints.

Representative variants:

| occupancy ramp | 022530 >4 LSB | 022539 >4 LSB | 022550 >4 LSB | 234826 >4 LSB |
|---|---:|---:|---:|---:|
| 19 -> 27 | ~2.60% | ~1.75% | 0 | 0 |
| **20 -> 27** | **~2.60%** | **~1.74%** | **0** | **0** |
| 21 -> 27 | ~2.60% | ~1.72% | 0 | 0 |
| 22 -> 26 | ~2.60% | ~1.87% | 0 | 0 |

The important decisions are stable:

```text
022530 -> high authority
022539 -> high authority
022550 -> zero authority
234826 -> zero authority
```

The 20 -> 27 default therefore sits inside a useful plateau rather than on a one-percent threshold accident.

## 2. MAD guard has a large natural separation

The independent high-frame-difference guard is:

```text
madGuard = 1 - smoothstep(0.075, 0.095, sourceMAD)
```

The restored corpus has a large gap between the quarantined non-cut case and ordinary motion cases.

Relevant source MAD values:

```text
001431  ~= 0.0978   non-cut; quarantined
022550  ~= 0.0540   next-highest ordinary non-cut capture
021001  ~= 0.0532
022539  ~= 0.0452
022530  ~= 0.0449
013416  ~= 0.0447
```

There are higher-MAD captures (`001406`, `000620`, `000853`), but they are already disabled by cut/class-3 safety handling.

Thus the current 0.075 -> 0.095 ramp does not sit close to any ordinary non-cut capture. In the present corpus it effectively quarantines `001431` without attenuating the Boromir or `013416` cases.

This remains our clean-room safety heuristic, not an SVP constant.

## 3. Why pixel percentage alone is insufficient

The failed direct algorithm-13 experiments made it visually obvious that broad geometry corruption forms large coherent warped islands, whereas the bounded golden-anchored candidate tends to produce sparse/localized changes.

To quantify that distinction, the `>4 LSB` change mask was analyzed by connected components.

Because these measurements were made from saved 8-bit diagnostic PNGs, their raw changed-pixel percentage differs slightly from the float-domain metrics in Parts 14–15. The structural comparison is the important result.

## 4. Bounded consensus candidate: connected-component structure

### `022530`

```text
>4-LSB changed area from PNG : ~2.13%
largest component            : 9,517 px (~0.62% frame)
components >= 1,000 px       : 8
total area in >=1,000 px comps: ~1.39% frame
```

### `022539`

```text
>4-LSB changed area from PNG : ~1.52%
largest component            : 7,047 px (~0.46% frame)
components >= 1,000 px       : 4
total area in >=1,000 px comps: ~1.17% frame
```

### `013416`

```text
>4-LSB changed area from PNG : ~0.43%
largest component            : 1,335 px (~0.087% frame)
components >= 1,000 px       : 2
total area in >=1,000 px comps: ~0.16% frame
```

This agrees with the exposure-enhanced inspection: the candidate changes stay bounded around a small number of local motion structures.

## 5. Rejected direct algorithm 13: structural explosion

Using the corresponding direct policy outputs from Part 14:

### `022530` direct C2 / phase 64 median

```text
>4-LSB changed area           : ~8.64%
components >= 1,000 px       : 30
components >= 5,000 px       : 7
total area in >=1,000 px comps: ~6.79% frame
```

### `022539` direct C1 midpoint median

```text
>4-LSB changed area           : ~17.72%
components >= 1,000 px       : 38
components >= 5,000 px       : 11
total area in >=1,000 px comps: ~14.54% frame
```

### `022550` direct C1 midpoint median

```text
>4-LSB changed area           : ~21.42%
components >= 1,000 px       : 56
components >= 5,000 px       : 9
total area in >=1,000 px comps: ~15.68% frame
```

This quantitatively captures the previously visual "liquid island" failure mode. The rejected direct synthesis does not merely change more pixels; it forms many more large connected regions of altered geometry.

## 6. Structural conclusion

For the stress cases where the bounded consensus candidate is active, its large-component changed area is dramatically lower than the rejected direct-algo13 path:

```text
022530: ~1.39% vs ~6.79%
022539: ~1.17% vs ~14.54%
013416 bounded: ~0.16% large-component area
```

The candidate therefore preserves the safety property we care about:

> repair remains local and golden-anchored instead of turning field-level badness into broad coherent alternate geometry.

This is a stronger criterion than raw PSNR/MAD or changed-pixel percentage alone and should be retained in future offline validation.

## 7. Available corpus boundary

A Library search for additional raw MPCVR NVOF capture archives found the existing archaeology handoffs and many historical build/test package hashes, but no clearly separate raw capture corpus beyond the restored captures already used in Parts 13–16.

The current validation set therefore remains the 18 restored capture directories from the August 24–25 corpus.

This is a limitation: the policy is still corpus-derived and must be exercised on live playback before promotion.

## 8. Decision boundary

The offline evidence is now sufficient to move from archaeology-only work to an isolated live/test implementation, subject to these constraints:

- create the test branch from the frozen live baseline, not from this research branch;
- do not enable NVIDIA hardware cost output;
- do not implement SVP adaptive phase;
- do not transplant direct algorithm 13;
- preserve the golden midpoint reconstruction as primary/final anchor;
- add modern software-SAD confidence and dual-direction field consensus only as bounded authority over the already-safe alternate reconstruction;
- preserve all existing cut/class-3 safety behavior;
- instrument the live implementation so its field classes, occupancy gain, MAD guard, and final global gain can be observed during playback.

The first live branch should be considered **Build #3 experimental**, not a release candidate.
