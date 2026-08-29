# SVP4 interpolation archaeology — part 4: V1.2 simplification / live design

Date: 2026-08-29
Parent notebooks:
- `docs/SVP4InterpolationArchaeology-2026-08-29.md`
- `docs/SVP4InterpolationArchaeology-2026-08-29-Part2.md`
- `docs/SVP4InterpolationArchaeology-2026-08-29-Part3-V1.md`

Branch: `research/svp4-interpolation-archaeology`
Frozen baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`.

This file records the simplifications needed to make the consolidated adaptive midpoint-splat candidate practical as a Shader Model 5 / D3D11 live experiment.

## 1. V1 -> V1.1: tighter local q-risk ramp

V1 used:

```text
local_q_authority = smoothstep(1000, 2200, Q)
```

A narrow perturbation sweep tested:

- 800 -> 2000
- 900 -> 2100
- 1000 -> 2200
- 1100 -> 2300
- **1200 -> 2400**
- 1000 -> 2400
- 1200 -> 2600

The 1200->2400 ramp consistently removed low-amplitude (>1 LSB) churn while preserving the meaningful >4/>8 LSB correction footprint.

Representative float-replay measurements:

| Capture | 1000->2200 >1 | 1000->2200 >4 | 1200->2400 >1 | 1200->2400 >4 |
| --- | ---: | ---: | ---: | ---: |
| 022530 | ~10.65% | ~2.193% | ~9.99% | ~2.191% |
| 022539 | ~4.43% | ~1.177% | ~3.96% | ~1.176% |
| 020950 | ~7.04% | ~0.549% | ~6.88% | ~0.548% |
| 021001 | ~5.79% | ~0.799% | ~5.36% | ~0.759% |
| 022550 | ~1.14% | ~0.070% | ~1.07% | ~0.069% |
| 234925 | ~9.32% | ~0.234% | ~8.15% | ~0.226% |
| 235102 | ~8.97% | ~0.568% | ~8.08% | ~0.567% |
| 000820 | ~2.66% | ~0.082% | ~2.46% | ~0.071% |
| 001431 | ~4.96% | ~0.137% | ~4.62% | ~0.129% |

Conclusion: **V1.1 uses local q ramp 1200->2400.**

## 2. Directional qscale = 1200 is a knee, not a magic constant

A narrow qscale sweep around the directional splat photometric confidence showed a distinct transition near 1200.

For 022530:

- qscale 1000: >1 ~8.43%, >4 ~0.62%, A/B disagreement ~0.01268
- **qscale 1200: >1 ~10.65%, >4 ~2.19%, disagreement ~0.01375**
- qscale 1400: >1 ~10.93%, >4 ~4.25%, disagreement ~0.01471

For 020950:

- qscale 1000: essentially no >4-LSB authority
- qscale 1200: >4 ~0.55%
- qscale 1400: >4 ~0.85%

A/B hypothesis disagreement worsens monotonically as qscale increases. 1200 is therefore the first setting that gives the severe Boromir field material correction authority without rapidly admitting much noisier alternate support.

Keep `qscale = 1200` for the first live experiment.

## 3. Classifier sample-phase stability

The cheap live field classifier is intended to use one representative image sample per 4x4 NVOF cell. All 16 integer sample phases inside the cell (x/y offset 0..3) were replayed over the full corpus.

The occupancy decision is highly stable.

Examples:

- Bilbo: 0.74..0.79%
- 022518: 7.59..7.94%
- 234955: 16.56..17.28%
- 013416: 18.53..18.89%
- 022550: 20.28..20.84%
- 022539: 23.22..23.66%
- 022530: 33.60..34.03%

Under the 15->25% smooth field-authority ramp, borderline 022550 therefore moves only about 0.54..0.62 authority rather than toggling 0/1. This is strong evidence that explicit temporal hysteresis is unnecessary for the **first** live V1.x build. True playback remains the final temporal test because the corpus does not contain full consecutive sequences.

Raw results: `research-results/q-phase-scan-2026-08-29.json`.

## 4. V1.1 splat footprint radius: 3x3 wins

The offline prototype originally used a radius-3 / 7x7 Gaussian footprint. This would be unnecessarily expensive for a Shader Model 5 scatter implementation.

V1.1 was replayed with:

- radius 1, sigma 0.75 -> **3x3**
- radius 2, sigma 1.00 -> 5x5
- radius 3, sigma 1.25 -> 7x7
- radius 4, sigma 1.50 -> 9x9

The result is remarkably insensitive to footprint size. Radius 1 is generally equal or slightly better in A/B warped-hypothesis agreement.

Representative results:

### 022530

- r1: >1 9.88%, >4 2.18%, A/B disagreement 0.01344
- r2: >1 9.93%, >4 2.18%, disagreement 0.01358
- r3: >1 9.99%, >4 2.19%, disagreement 0.01375
- r4: >1 10.05%, >4 2.20%, disagreement 0.01393

### 022539

- r1: >1 3.93%, >4 1.13%, disagreement 0.01337
- r3: >1 3.96%, >4 1.18%, disagreement 0.01351

### 020950

- r1: >1 6.68%, >4 0.58%, disagreement 0.00819
- r3: >1 6.88%, >4 0.55%, disagreement 0.00859

The same result held on the remaining corpus:

- 022550
- 234925
- 235102
- 000820
- 001431
- 021001
- gray-zone 013416

There is no observed quality reason to pay for 7x7.

**V1.2 chooses radius=1 / 3x3, sigma=0.75.**

Live cost implication: each valid vector projects to 9 coarse target cells instead of 49. With separate A/B directions and fixed-point x/y/weight accumulators, this reduces worst-case scatter atomic traffic by roughly 5.4x.

## 5. Full-resolution local-authority Gaussian blur is unnecessary

V1/V1.1 blurred the finished full-resolution local authority map with Gaussian sigma=0.8 before applying the 0.60 cap. That would either require another full-frame pass or several extra samples in Warp.

V1.2 compared the Gaussian reference with:

- no blur;
- 5-tap cross;
- 3x3 [1,2,1] separable approximation.

The no-blur output is already effectively identical to the Gaussian reference.

Representative **output difference versus the Gaussian-reference V1.1**, not versus golden:

- 022530: mean ~1.25e-5 normalized RGB; only ~0.001% pixels differ by >1 LSB
- 022539: mean ~8.1e-6; ~0.002% >1 LSB
- 020950: mean ~9e-6; effectively 0% >1 LSB
- 021001: mean ~7.3e-6; ~0.001% >1 LSB
- 234925: mean ~1.78e-5; ~0.003% >1 LSB
- 000820: mean ~8.4e-6; ~0.005% >1 LSB
- 013416: mean ~1.8e-6; 0% >1 LSB

Meaningful >4-LSB correction footprint relative to golden is unchanged.

Conclusion: **remove the Gaussian authority blur entirely for V1.2.** It was not buying enough to justify any GPU work.

## 6. V1.2 consolidated offline candidate

```text
scene cut?
  yes -> existing cut handling, robust authority = 0
  no
   |
   v
one-sample/cell clean-room field q
m1 occupancy = fraction(q >= 1600), outer ~4% ignored
fieldAuthority = smoothstep(15%, 25%, occupancy)
   |
   v
per-direction valid native vectors (round-trip <=20 px)
   |
   +-- consistency confidence exp(-min(error,40)/8)
   +-- directional q confidence exp(-min(q_dir,8000)/1200)
   |
3x3 midpoint inverse-flow splat, sigma 0.75
   |
normalize A/B inverse flow + support
   |
backward-warp A and B
   |
soft ownership gamma=3
   |
S = ownership-weighted alternate midpoint hypothesis
T = 0.5*(A+B)
M = channelwise_median(golden, S, T)
   |
local = smoothstep(1200,2400,Q)
      * smoothstep(0.03,0.22,max(CA,CB))
local = min(local,0.60)
alpha = local * fieldAuthority
out = lerp(golden,M,alpha)
```

Differences versus V1:

1. local q ramp 1200->2400 instead of 1000->2200;
2. splat footprint 3x3/sigma0.75 instead of 7x7/sigma1.25;
3. no full-resolution authority blur.

These simplifications make the candidate materially cheaper while preserving the offline behavior that survived the visual falsification pass.

## 7. `area_blend=0.4` convergence — strong hypothesis, not proof

SVP binary parsing confirms exact default `area_blend = 0.4`.

Independent historical testing around SVP 4.3 reported that `area_blend=1.0` largely restores the older motion-compensated behavior, while lower values allow the newer artifact replacement behavior to act even with `mask.area=0` on algorithms such as 13/23.

A plausible interpretation is therefore:

```text
area_blend = residual MC/interpolated-image weight inside artifact/robust regions
1-area_blend = maximum artifact-fallback/replacement authority
```

If true, stock 0.4 implies maximum replacement authority ~0.6 — an interesting independent convergence with the clean-room V1.x `alpha <= 0.60` cap.

Do **not** treat the exact formula as proven yet. Continue the binary dataflow trace before adopting this as a literal SVP formula.

## 8. Live Shader Model 5 design sketch

The next isolated live build should preserve the golden output path and add V1.2 as a post-golden robust hypothesis.

Likely resources:

1. 1x1 `R32_UINT` m1-bad-cell counter;
2. coarse combined-q texture for local authority;
3. per-direction coarse midpoint splat accumulators:
   - signed fixed-point sumX;
   - signed fixed-point sumY;
   - unsigned fixed-point weight;
4. normalized per-direction inverse-flow/support textures.

Important SM5 constraint from COV-FB55 work: component atomics on `RWTexture2D<uint2>` failed under `fxc`. Prefer separate scalar `R32_SINT/R32_UINT` UAVs for first implementation rather than relying on vector-component atomics.

Scatter pass cost with 3x3:

- 9 target cells / valid vector / direction;
- 3 atomic adds per target (x, y, weight);
- 27 atomics / direction, 54 worst-case per cell for two valid directions;
- at 1080p 4x4 flow grid (~129,600 cells), worst-case ~7.0 million atomic adds rather than ~38 million for 7x7.

No CPU readback or synchronization should be introduced.

## 9. Immediate next steps

1. Choose fixed-point scales and prove accumulator overflow bounds for expected/max NVOF displacement and 3x3 kernel weight.
2. Test integer quantization of the splat offline so the replay matches what SM5 can actually implement.
3. Determine whether q can be stored compactly (for example normalized/quantized R16 or R32_UINT) without changing field/local decisions materially.
4. Translate V1.2 into an isolated experimental branch from the frozen baseline.
5. Build/package and benchmark live GPU cost/startup/seek behavior.
6. User playback A/B becomes the decisive quality/temporal test; continue research/build iteration afterward.
7. Keep every major implementation/replay result online before long experiments.
