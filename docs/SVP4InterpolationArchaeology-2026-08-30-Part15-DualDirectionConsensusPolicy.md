# SVP4 interpolation archaeology — part 15: dual-direction consensus authority

Date: 2026-08-30
Branch: `research/svp4-interpolation-archaeology`
Frozen live baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91` and must not be modified.

Part 14 rejected a literal transplant of SVP adaptive phase + algorithm 13 onto our present repaired motion hypotheses. The useful lesson is that the reconstructed field classifier should control **how much authority a safe alternate hypothesis receives**, not replace our synthesis operator.

This part tests an independent clean-room policy built around that principle.

The result is the first post-audit policy that restores essentially the desired Boromir authority while leaving the borderline/easier pair untouched.

It remains an offline candidate, not yet a live constant set.

## 1. Preserve the safe reconstruction shape

The local reconstruction is unchanged from the modern-SAD Adaptive Splat candidate:

```text
modern directional 4x4 luma-SAD q
        -> confidence-weighted repaired A/B splats
        -> confidence-cubed directional ownership
        -> alternate midpoint splat S

golden midpoint G
safe temporal midpoint T
alternate S
        -> channelwise median R = median(G, S, T)

final = G*(1-alpha) + R*alpha
```

The important safety properties remain:

- `G` is inside the robust hypothesis set;
- `G` is also the final anchor;
- raw vector geometry is never promoted directly;
- local alpha remains bounded to 0.60;
- modern SAD only attenuates/supports the alternate hypothesis.

The only new question is how much **global field authority** multiplies that already-bounded local alpha.

## 2. Require directional consensus, not one class number

Part 13 showed that a single proprietary-current A->B class does not preserve the useful Boromir split:

```text
022530 -> A->B class 2
022539 -> A->B class 1
022550 -> A->B class 1
234826 -> A->B class 1
```

The opposite B->A direction contains complementary information:

```text
022530 -> B->A class 1
022539 -> B->A class 1
022550 -> B->A class 0
234826 -> B->A class 0
```

That suggests an independent clean-room interpretation:

> Grant meaningful alternate authority only when **both directions independently say the motion field is unhealthy**.

The candidate therefore requires:

```text
B->A class in {1,2}
AND
A->B class in {1,2}
```

Class 0 on either side prevents global promotion. Class 3 / detected cuts are also disabled.

This is not claimed to be proprietary SVP policy. It is our safety-oriented use of the two reconstructed confidence fields.

## 3. Use continuous occupancy authority

A binary class boundary is unnecessarily brittle, especially because capture-BMP luma is only an approximation of SVP's decoder-domain YUV.

Define:

```text
dualM1 = min(B->A m1+ occupancy, A->B m1+ occupancy)
```

Then the current candidate uses:

```text
occupancyGain = smoothstep(20, 27, dualM1)
```

This has several useful properties:

- one bad direction cannot create full-frame authority by itself;
- a field barely crossing the 20% classifier threshold receives almost no authority;
- the two severe Boromir fling pairs receive high authority;
- the borderline `022550` side remains effectively off;
- small reconstruction/quantization drift around 20% produces a small continuous change rather than a hard jump.

The upper knee at 27% is empirical and corpus-derived. It is not a proprietary constant.

## 4. Independent high-frame-difference safety guard

`001431` is a special high-change case:

```text
source MAD ~= 0.098
```

The ordinary scene-cut heuristic does not classify it as a cut because its coarse-frame correlation remains above the cut threshold, but it is far outside the source-MAD range of the ordinary difficult motion pairs.

For the first conservative candidate, global authority is therefore multiplied by:

```text
madGuard = 1 - smoothstep(0.075, 0.095, sourceMAD)
```

This is explicitly an **independent safety heuristic**, not reconstructed SVP behavior.

Its purpose is to quarantine unusually large whole-frame changes until those cases are evaluated separately instead of allowing a new policy to broaden them accidentally.

## 5. Complete candidate policy

Conceptually:

```text
ordinaryConsensus =
    B->A class in {1,2}
    AND A->B class in {1,2}
    AND not cut

occupancyGain = smoothstep(20, 27, min(m1BA, m1AB))
madGuard      = 1 - smoothstep(0.075, 0.095, sourceMAD)

globalGain = ordinaryConsensus ? occupancyGain * madGuard : 0
alpha      = localModernSadAlpha * globalGain
output     = golden*(1-alpha) + robustMedian*alpha
```

The replay is committed as:

```text
tools/research/splat_candidate_v3_consensus_policy.py
```

Defaults:

```text
qscale          = 1600
occupancy start = 20%
full authority  = 27%
MAD guard start = 0.075
MAD guard stop  = 0.095
local alpha cap = 0.60
```

Every policy number is exposed as a command-line parameter for sensitivity sweeps.

## 6. Restored-corpus result

The table reports B->A/A->B class, minimum directional m1+ occupancy, source MAD, final global gain, and final output difference from the frozen golden midpoint.

| Capture | Classes BA/AB | dual m1+ | source MAD | gain | >4 LSB | >8 LSB |
|---|---:|---:|---:|---:|---:|---:|
| 000620 | 2/2 | 40.89% | 0.084 | 0.000 | 0.000% | 0.000% |
| 000820 | 1/1 | 22.61% | 0.043 | 0.314 | 0.004% | 0.002% |
| 000853 | 2/2 | 45.13% | 0.068 | 0.000 | 0.000% | 0.000% |
| 001406 | 3/3 | 52.10% | 0.120 | 0.000 | 0.000% | 0.000% |
| 001431 | 2/1 | 40.30% | 0.098 | 0.000 | 0.000% | 0.000% |
| 022518 | 0/0 | 8.46% | 0.035 | 0.000 | 0.000% | 0.000% |
| 022530 | 1/2 | 29.56% | 0.045 | **1.000** | **2.578%** | 0.000% |
| 022539 | 1/1 | 26.23% | 0.045 | **0.966** | **1.735%** | 0.233% |
| 022550 | 0/1 | 19.98% | 0.054 | **0.000** | **0.000%** | 0.000% |
| 234826 | 0/1 | 17.73% | 0.039 | 0.000 | 0.000% | 0.000% |
| 234854 | 0/0 | 16.43% | 0.035 | 0.000 | 0.000% | 0.000% |
| 234925 | 1/1 | 20.07% | 0.032 | 0.000 | 0.000% | 0.000% |
| 234955 | 0/0 | 14.62% | 0.041 | 0.000 | 0.000% | 0.000% |
| 235102 | 1/1 | 23.80% | 0.023 | 0.563 | 0.000% | 0.000% |
| 013339 | 1/1 | 25.03% | 0.030 | 0.806 | 0.065% | 0.015% |
| 013416 | 2/2 | 36.53% | 0.045 | 1.000 | 0.503% | 0.009% |
| 020950 | 1/1 | 20.85% | 0.042 | 0.041 | 0.000% | 0.000% |
| 021001 | 1/1 | 22.30% | 0.053 | 0.254 | 0.027% | 0.010% |

`000620`, `000853`, and `001406` are suppressed by the existing cut/class-3 safety logic despite high confidence-field occupancy.

## 7. Boromir result is notably clean

This policy recovers the useful selectivity that motivated Part 12 without relying on the incorrect single-direction class gate:

```text
022518 -> 0.000% >4 LSB
022530 -> 2.578% >4 LSB
022539 -> 1.735% >4 LSB
022550 -> 0.000% >4 LSB
```

The two same-shot extreme-motion captures retain approximately the desired qscale-1600 correction authority.

At the same time:

- the earlier/easier `022518` stays untouched;
- borderline `022550` stays untouched;
- `234826`, which became a false positive under the corrected A->B class-1 gate, stays untouched.

This is substantially better policy behavior than `currentClass == 1` or `currentClass in {1,2}`.

## 8. `013416` remains locally safe under the bounded hybrid

The consensus policy gives `013416` full global gain because both directions strongly classify the field unhealthy.

The final bounded hybrid still changes only:

```text
>4 LSB : 0.503%
>8 LSB : 0.009%
```

The exposure-enhanced inspection completed in Part 13 found these changes sparse/localized and did not reveal the old coherent liquid/stretched geometry.

This is an important contrast with the direct C2 algorithm-13 experiment in Part 14, where the phase-64 direct median changed 5.858% by more than 4 LSB relative to its temporal reference and visibly admitted broad geometry corruption.

The safety comes from the bounded golden-anchored hypothesis structure, not from the class itself.

## 9. `001431` is deliberately quarantined, not solved

Without the high-frame-difference guard, the ordinary consensus rule would grant full authority to `001431` and the bounded hybrid changes about 1.18% of pixels by more than 4 LSB.

The current candidate instead sets its global gain to zero because:

```text
source MAD ~= 0.098 > MAD guard stop
```

This should not be interpreted as proof that leaving the golden frame unchanged is optimal. It means only that `001431` is sufficiently different from the ordinary motion cases that it should not determine the first live policy without dedicated evaluation.

## 10. Why this candidate is stronger than the earlier Build #3 gate

The earlier proposal was effectively:

```text
one directional class == 1 -> permit robust reconstruction
```

The new policy instead separates four decisions:

```text
local vector quality       -> modern 4x4 SAD
local geometric support    -> repaired splat coverage
field consensus/severity   -> two directional classifiers + min occupancy
large-frame-change safety  -> source-MAD guard
```

No single signal gets to authorize raw motion.

This aligns with the strongest architectural lesson from the SVP archaeology: visibility, vector trust, global field health, and robust synthesis are related but distinct problems.

## 11. Remaining validation before a live Build #3

This is the first post-audit policy worth treating as a serious candidate, but it is still corpus-tuned.

Before creating a live branch:

1. sweep the `20 -> 27%` occupancy ramp around nearby values and verify the Boromir split is not a knife-edge;
2. sweep the MAD guard and confirm it changes only deliberately quarantined high-difference cases;
3. re-inspect the amplified `022530`, `022539`, `013416`, and `013339` outputs;
4. run any additional captures available outside the current archaeology corpus;
5. preserve all cut/class-3 behavior exactly;
6. only then port the policy onto a branch created from the frozen baseline.

The key point is that Build #3 is no longer "copy SVP algorithm 13." It is now a much narrower and safer proposition:

> use SVP-inspired modern confidence and field-health evidence to decide how much of our independently repaired, golden-anchored alternate reconstruction is allowed.
