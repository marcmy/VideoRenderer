# SVP4 interpolation archaeology — part 3: consolidated adaptive splat V1

Date: 2026-08-29
Parent notebook: `docs/SVP4InterpolationArchaeology-2026-08-29.md`
Part 2: `docs/SVP4InterpolationArchaeology-2026-08-29-Part2.md`
Branch: `research/svp4-interpolation-archaeology`
Frozen live baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`.

This is a durable continuation log. It records the first consolidated adaptive midpoint-splat candidate after the q-risk, q-weighting, ownership and field-ramp experiments. It does not modify the frozen baseline.

## 1. New findings after Part 2

### 1.1 Directional qscale

Directional image-domain matching quality is now used inside the midpoint splat:

```text
base_conf  = exp(-min(roundtrip_error, 40) / 8)
photo_conf = exp(-min(q_direction, 8000) / 1200)
weight     = gaussian_footprint * base_conf * photo_conf
```

`qscale ~= 1200` remains the best current compromise. Compared with no photometric weighting or larger qscales, it cuts A/B splat disagreement sharply on the main Boromir failures without reducing the alternate to a no-op.

### 1.2 Ownership sharpening

The A- and B-side splats carry independent normalized support maps `CA` and `CB`. Symmetric linear support weighting leaves too much indecisive two-sided averaging in visibility/occlusion zones.

Current soft ownership combination:

```text
a = max(CA, eps)^gamma
b = max(CB, eps)^gamma
splat = (warpA * a + warpB * b) / (a + b)
```

Sweep result:

- gamma 1: broadest low-amplitude footprint;
- gamma ~3–4: removes mostly low-amplitude churn while retaining the stronger corrections;
- gamma 8: begins increasing larger deviations again and is too close to hard winner-take-all.

Current V1 uses `gamma = 3`.

This is the clean-room analog of the cover/uncover principle recovered from SVP algorithms 21/22: ownership is a soft directional preference, not permission to trust a failed raw vector.

### 1.3 Hard 20% field switch replaced by a smooth ramp

A hard `m1+ occupancy >= 20%` switch is temporally risky because Boromir `022550` sits close to the boundary (~20.37%). Adjacent synthesized frames could toggle robust reconstruction on/off.

Current field authority:

```text
fieldAuthority = smoothstep(15%, 25%, m1PlusOccupancy)
```

Properties on current corpus:

- <=15%: exact golden path;
- 15–25%: robust path fades in continuously;
- >=25%: full robust-field permission;
- scene cuts force authority to zero because the existing cut path wins first.

Alternative 17–27 and 18–28 ramps were tested but attenuated the borderline `022550` case too aggressively. 15–25 is the best first ramp.

### 1.4 Gray-zone scan

Only two non-cut captures in the current corpus sit materially in the 15–20% gray zone:

- `234955`: field ~16.66%; V1 changes effectively nothing;
- `013416`: field ~18.53%; V1 remains visually/neumerically extremely small.

This indicates the ramp improves temporal continuity without meaningfully disturbing near-clean material.

### 1.5 Out-of-bounds directional quality decision

Experiment: when one directional photometric projection is outside frame, renormalize the combined q score over only the remaining valid direction.

Result: **rejected**.

Example `013416`:

- current convention: m1+ occupancy ~18.53%;
- valid-direction-only renormalization: ~24.4%.

The increase is driven by substantial one-sided out-of-frame projection/visibility, not automatically bad matching. Renormalizing would conflate disocclusion/visibility with field matching failure.

Keep the current separation:

- image-domain q = reconstruction/matching health;
- projected support/OOB/coverage = visibility/ownership.

This matches the architectural separation recovered from SVP.

## 2. Consolidated V1 algorithm

The first complete candidate is implemented by the offline script `splat_candidate_v1.py`.

### 2.1 Global field state

- compute clean-room combined q map;
- m1 threshold remains `q >= 1600`;
- ignore approximately outer 4% of the flow field;
- derive `m1PlusOccupancy`;
- `fieldAuthority = smoothstep(15, 25, m1PlusOccupancy)`;
- if existing scene-cut rule fires (`corr < 0.15 && MAD > 0.055`), `fieldAuthority = 0` and normal cut handling remains authoritative.

### 2.2 Directional midpoint splat

Per direction:

- only native vectors passing their own 20 px round-trip consistency check are projected;
- Gaussian footprint: radius 3 coarse cells, sigma 1.25;
- consistency confidence: `exp(-min(error,40)/8)`;
- directional photometric confidence: `exp(-min(q_direction,8000)/1200)`;
- splat midpoint inverse displacement and accumulated support;
- normalize the accumulated displacement by support;
- backward-warp the real endpoint frames.

### 2.3 Ownership combination

Normalize splat support to `CA` and `CB`, then use soft ownership exponent 3:

```text
aa = max(CA, 1e-8)^3
bb = max(CB, 1e-8)^3
S  = (AW*aa + BW*bb) / max(aa+bb, eps)
```

Where neither side has meaningful support, use the safe temporal reference instead of inventing an alternate.

### 2.4 Robust alternate hypothesis

```text
T = 0.5 * (A + B)
M = channelwise_median(golden, S, T)
```

The raw splat is never unconditional output.

### 2.5 Local authority

```text
Q       = upsample(combined_q)
support = max(CA, CB)
local   = smoothstep(1000,2200,Q)
        * smoothstep(0.03,0.22,support)
local   = gaussian_blur(local, sigma=0.8)
local   = min(local, 0.60)
alpha   = local * fieldAuthority
out     = lerp(golden, M, alpha)
```

Thus V1 has three independent safety layers:

1. field health decides **WHEN** robust reconstruction has authority;
2. directional q/support/ownership decide whether the alternate midpoint hypothesis is credible;
3. local combined q/support decides **WHERE and how much** the robust median may alter golden.

## 3. Full-corpus V1 measurements

The values below are measured from the saved V1 output versus saved golden output. `>1`, `>4`, `>8` are percentages of pixels whose mean absolute RGB change exceeds those 8-bit LSB thresholds. The saved PNG quantization makes these slightly different from the pre-save float metrics but the ranking is stable.

| Capture | Field m1+ | Field authority | >1 LSB | >4 LSB | >8 LSB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Bilbo | 0.74% | 0.000 | 0.000% | 0.000% | 0.000% |
| 022518 | 7.68% | 0.000 | 0.000% | 0.000% | 0.000% |
| 234854 | 8.08% | 0.000 | 0.000% | 0.000% | 0.000% |
| 013339 | 9.43% | 0.000 | 0.000% | 0.000% | 0.000% |
| 234826 | 12.80% | 0.000 | 0.000% | 0.000% | 0.000% |
| 234955 | 16.66% | 0.074 | 0.010% | 0.000% | 0.000% |
| 013416 | 18.53% | 0.286 | 0.569% | 0.004% | 0.001% |
| 022550 | 20.37% | 0.555 | 0.453% | 0.055% | 0.001% |
| 022539 | 23.39% | 0.931 | 3.891% | 1.072% | 0.199% |
| 001431 | 27.74% | 1.000 | 4.081% | 0.115% | 0.003% |
| 000820 | 27.87% | 1.000 | 2.171% | 0.072% | 0.012% |
| 020950 | 28.05% | 1.000 | 5.874% | 0.527% | 0.000% |
| 235102 | 29.40% | 1.000 | 8.331% | 0.439% | 0.000% |
| 234925 | 30.49% | 1.000 | 7.986% | 0.208% | 0.005% |
| 022530 | 33.60% | 1.000 | 9.840% | 1.982% | 0.147% |
| 021001 | 34.79% | 1.000 | 5.016% | 0.766% | 0.024% |
| hard cut 000620 | 37.65% | 0.000 | 0.000% | 0.000% | 0.000% |
| hard cut 001406 | 42.20% | 0.000 | 0.000% | 0.000% | 0.000% |
| hard cut 000853 | 51.06% | 0.000 | 0.000% | 0.000% | 0.000% |

Important observations:

- Bilbo and all <15% controls are bit-for-bit no-ops.
- Actual cuts are bit-for-bit no-ops under V1 because cut handling suppresses robust authority.
- The 15–25 field ramp keeps `022550` deliberately modest instead of granting full robust authority at 20.37%.
- The severe same-shot fields retain enough authority to matter, but their footprint is much smaller than the first broad splat-median prototype.
- `020950/021001` remain important falsification controls because they score globally bad but are not the same artifact as Boromir.

## 4. First visual falsification pass

Side-by-side A/B/GOLD/V1 crops were selected automatically from the highest GOLD-vs-V1 local-change windows and inspected with boosted brightness.

Current qualitative result:

- **022530:** V1 changes the blurred/smeared structure without obvious new block geometry. The correction is subtle relative to the earlier broad splat version but remains spatially coherent.
- **022539:** V1 alters the difficult silhouette/edge region while avoiding the chunky geometry produced by the rejected independent-JFA median. No obvious new high-contrast splat outlier survived the anchored median.
- **020950:** the largest-change region remains visually close to golden; changes are mostly low amplitude. The mottled/block-shaped regression previously seen with REPMED25 is not apparent in the first V1 comparison.
- **021001:** likewise no obvious new block geometry in the largest-change region; changes remain moderate.
- **013416:** changes are visually negligible, consistent with the small gray-zone metrics.
- **022550:** changes are intentionally very small because field authority is only ~0.555.

This is encouraging but is not yet proof of a live win because the corpus has no true ground-truth midpoint. The final judgement still requires live temporal playback and user A/B observation.

## 5. `area_blend` archaeology update

Binary trace recovered an exact parsed default double value of **0.4** for SVP `area_blend`.

This is recorded as an archaeological fact only. Do **not** translate it blindly into a 40/60 endpoint blend. The downstream semantics and interaction with interpolation time still need complete tracing; at t=0.5 endpoint-nearness alone does not justify a fixed asymmetric blend.

## 6. Current status / next research

V1 is the strongest consolidated offline candidate so far, but it is still a candidate, not a promoted baseline.

Next work:

1. finish visual crop review over every high-authority non-cut capture, especially `235102`, `234925`, `000820`, and `001431`;
2. measure temporal-risk proxies across nearby capture pairs where available (field occupancy continuity, alpha footprint continuity, ownership switching);
3. test small parameter perturbations around V1 rather than broad sweeps: qscale 1000–1400, ownership gamma 2.5–4, local q ramp around 1000–2200;
4. determine whether a confidence/hysteresis mechanism is needed beyond the smooth 15–25 field ramp;
5. finish the `area_blend` call-path trace before changing the safe temporal hypothesis;
6. if V1 survives these falsification checks, prepare a separate live-test branch/build from the frozen baseline while continuing research afterward;
7. continue committing every major result and exact experiment script online before long exploratory runs.
