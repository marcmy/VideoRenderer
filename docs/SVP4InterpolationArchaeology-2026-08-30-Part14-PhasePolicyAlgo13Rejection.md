# SVP4 interpolation archaeology — part 14: adaptive phase policy and direct algo13 rejection

Date: 2026-08-30
Branch: `research/svp4-interpolation-archaeology`
Frozen live baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91` and must not be modified.

Part 13 corrected the direction used by the proprietary/global NVOF classifier and established that ordinary classes 1 and 2 must be treated as policy inputs rather than a single Build #3 permission bit.

This part traces that policy one step farther and tests the most literal translation: apply SVP-style adaptive phase selection and algorithm-13 median synthesis directly to our repaired NVOF hypotheses.

That direct translation is **rejected**.

The useful result is architectural: the policy semantics are now substantially clearer, but SVP's synthesis operator cannot simply be transplanted onto our present reconstructed motion hypotheses.

## 1. `scene.adaptive=210` decodes to class-specific phase policy

The independently reconstructed option parser decodes decimal adaptive digits as:

```text
scene.adaptive = 210
               -> [-1, 0, 1]
```

for ordinary classes 0, 1, and 2.

`-1` means no adaptive phase override. Nonnegative values are passed into the scene-phase timing function.

At an exact 2x interpolation rate, for the nominal midpoint output whose ordinary phase is 128:

```text
class 0 -> no adaptive override
class 1 -> adaptive mode 0 -> phase 128 -> t = 0.50
class 2 -> adaptive mode 1 -> phase  64 -> t = 0.25
class 3 -> worst/scene handling is separate
```

The class-2 policy is therefore materially different from class 1. It does not merely mean "use more of the same correction." It deliberately moves the synthesized output temporally closer to a real endpoint when the field is more questionable.

## 2. `scene.force13` applies to ordinary classes 1 and 2

Part 13 was amended after tracing the independent implementation's policy call site.

With `scene.force13=true` (the reconstructed default), the effective algorithm becomes 13 when all of the following hold:

```text
mode is SmoothFps or NVOF
scene class is 1 or 2
requested algorithm >= 11
```

The special classifier `-1` state from `blocks13` remains a distinct classifier behavior, but it is not the only route relevant to robust algorithm selection.

Therefore the ordinary stock-like policy is conceptually:

```text
C1 -> algorithm 13 at normal midpoint phase
C2 -> algorithm 13 plus more conservative adaptive phase
```

## 3. Exact algorithm-13 combination

The reconstructed GPU kernel gives algorithm 13 a particularly simple combination rule.

For phase `t = phase / 256` it creates:

```text
refA = A sampled with phase-scaled A->B motion
refB = B sampled with complementary phase-scaled B->A motion
safe = (1-t)*A + t*B
```

and returns, independently per channel:

```text
median(refA, refB, safe)
```

The two motion-compensated hypotheses are **not** blended against one another with time weights before the median. Time affects their sampling displacement and the unwarped temporal hypothesis.

This matters for C2. A faithful direct replay must rebuild the hypotheses at phase 64 / `t=0.25`; it is not valid to take a midpoint median and merely reduce its alpha.

## 4. Reproducible direct-policy replay

The diagnostic is committed as:

```text
tools/research/splat_candidate_v3_phase_policy.py
```

It uses:

- the restored MPCVR diagnostic motion fields;
- modern 4x4 luma-SAD confidence at `qscale=1600`;
- the corrected current A->B field class;
- C1 -> `t=0.5`;
- C2 -> `t=0.25`;
- phase-scaled repaired splat hypotheses;
- direct channelwise algorithm-13 median.

This is deliberately a diagnostic reproduction of the policy idea, not a candidate for the live renderer.

## 5. Direct replay fails on the important motion cases

### `022530` — current class 2, phase 64

The direct C2 replay uses `t=0.25`.

Relative to the unwarped phase-64 temporal reference:

```text
median >1 LSB : 13.645%
median >4 LSB :  8.812%
median >8 LSB :  5.143%
```

Visual inspection shows large coherent warped/chunky islands across the moving figure and background — the same broad failure family that earlier raw-flow/ownership experiments resurrected.

This is a hard rejection. The phase-64 temporal image itself is smooth; the geometry corruption comes from admitting the direct motion-compensated hypotheses through the median.

### `022539` — current class 1, midpoint phase 128

This rules out the idea that the problem is specific to C2's quarter-phase shift.

Direct algorithm-13 median versus the frozen midpoint golden output:

```text
>1 LSB : 41.584%
>4 LSB : 18.252%
>8 LSB :  9.512%
```

The direct median visibly reintroduces broad distorted geometry.

### `022550` — current class 1, midpoint phase 128

Likewise:

```text
>1 LSB : 51.545%
>4 LSB : 22.046%
>8 LSB : 11.962%
```

Again, this is much broader and more dangerous than the bounded Adaptive Splat hybrid.

### `013416` — current class 2, phase 64

Relative to the phase-64 temporal reference:

```text
>1 LSB : 9.306%
>4 LSB : 5.858%
>8 LSB : 3.429%
```

This is dramatically broader than the previously safe bounded midpoint hybrid, which changed only about 0.50% of pixels by more than 4 LSB.

### `001431` — current class 1, midpoint phase 128

Direct median versus golden:

```text
>1 LSB : 67.167%
>4 LSB : 31.617%
>8 LSB : 15.826%
```

This frame was already a difficult/high-change case and reinforces that direct algorithm substitution is not a safe clean-room translation.

## 6. Why this does not invalidate SVP algorithm 13

The result is narrower than "algorithm 13 is bad."

SVP's own synthesis operator is coupled to a different motion/reconstruction pipeline, including some combination of:

- its vector-source representation and optional source downscale;
- its own vector packing/sampling geometry;
- its coverage and artifact masks;
- super-frame/source sampling behavior;
- endpoint-biased fallback;
- potentially neighboring-pair hypotheses in other algorithms.

Our experiment asks a different question:

> Can we take the algorithm-13 policy and feed it our present repaired Adaptive Splat motion hypotheses directly?

The answer is **no**.

That is exactly why the frozen golden renderer must remain one of the hypotheses in our clean-room design.

## 7. What remains valid

The following findings survive the rejection:

- modern 4x4 luma-SAD is still useful as local vector confidence;
- field classes contain useful global information;
- C1 and C2 should not be collapsed into one boolean severity state;
- direct raw/repaired motion promotion remains unsafe;
- the channelwise median is useful only when at least one of its hypotheses is already strongly anchored;
- our safer construction remains:

```text
golden midpoint
+ bounded repaired alternate
+ safe temporal midpoint
-> channelwise median
-> limited local alpha back into golden
```

The critical difference is that **golden remains inside the robust hypothesis set and remains the final anchor**.

## 8. Revised Build #3 direction

Do not implement SVP's adaptive phase or direct algorithm-13 operator in the live renderer.

Instead, translate the field classifier into **continuous authority over the already-safe hybrid**.

The next experiment should therefore:

1. retain midpoint timing;
2. retain the frozen golden output as primary;
3. keep modern SAD as local confidence;
4. require stronger evidence than one directional class before granting broad authority;
5. use field severity to scale the safe hybrid rather than replace the synthesis operator;
6. quarantine high whole-frame-change cases until separately validated.

That constrained policy is evaluated in Part 15.
