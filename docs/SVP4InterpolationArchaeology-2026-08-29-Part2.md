# SVP4 interpolation archaeology — part 2: splat confidence experiments

Date: 2026-08-29
Parent lab notebook: `docs/SVP4InterpolationArchaeology-2026-08-29.md`
Branch: `research/svp4-interpolation-archaeology`
Frozen baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`.

This is a durable continuation log. It records experiments performed after the first lab notebook was committed. No result here changes the frozen live baseline by itself.

## 1. Midpoint-splat recap

The current promising alternate reconstruction is built by projecting only trustworthy native motion support toward the intermediate frame, accumulating a coarse inverse-flow/support field, normalizing that field, backward-warping A and B, and then using the result only as an alternate hypothesis inside:

```text
median(golden, splat_reconstruction, temporal_reference)
```

The raw splat is not safe as unconditional output. The median is important because it suppresses bright / gross splat outliers.

A broad field-gated splat-median visibly reduced some Boromir streak/rubber geometry, but its spatial authority was too large (often ~20–30% of pixels >1 RGB LSB from golden on severe frames). Restricting it only to the golden unsupported core was too conservative (~1% meaningful change). The research question therefore became: what should control the splat-median's *local* authority inside a globally suspicious field?

## 2. Rejected local gate: distance from accepted golden seed

Hypothesis:

> JFA/dense synthesis should be most speculative deep inside regions far from a real accepted golden seed, so splat-median authority can be tied to nearest-seed distance.

Experiment reproduced the exact golden accepted-seed policy (including temporal-motion-gated raw-forward salvage), propagated nearest accepted seeds with the same JFA structure, and derived coarse/full-resolution distance-to-seed risk.

Result: **rejected**.

On active Boromir `022530`, `022539`, and `022550`, seed-distance-gated variants produced essentially zero pixels above 1 LSB of change once the gate was made selective enough to be meaningful.

Interpretation:

The remaining bad geometry is not confined to deep JFA holes. A substantial part of the failure comes from vectors at or near accepted seeds that are geometrically/round-trip plausible enough to survive validation but still form a poor reconstruction field under heavy motion blur.

This strongly supports SVP's architectural separation between:

1. consistency / geometric vector validation, and
2. matching-error / artifact confidence.

Do not use nearest-seed distance as the primary robust-mode local authority signal.

## 3. Local clean-room q-map gate

The same luminance-normalized image-domain matching score used by the global 20% field classifier was promoted to a spatial quality map.

For coarse cell `p`:

```text
q_BA = 16 * 255 * abs(Y_B(p) - Y_A(p + F_BA)) / max(Y_B(p), 1/255)
q_AB = 16 * 255 * abs(Y_A(p) - Y_B(p + F_AB)) / max(Y_A(p), 1/255)
q    = 0.5 * (q_BA + q_AB)
```

The coarse q map is bilinearly upsampled and used continuously, not as a binary hard patch mask. Projected splat support is also required.

Representative local-authority forms tested:

```text
QLO_ANY  = smoothstep(600, 1600, q)  * support
QM1_ANY  = smoothstep(1000, 2200, q) * support
QHI_ANY  = smoothstep(1600, 3200, q) * support
```

with support itself smoothly gated (approximately `.03..22` or `.05..30` depending on prototype) and final authority capped, commonly at 0.60.

Two-sided-support variants (`*_BOTH`) were also tested. They are safer but can be unnecessarily restrictive around genuine occlusion/disocclusion, so the current focus is `ANY` support plus directional quality inside the splat.

### 3.1 Representative measurements, 60% authority cap

`234925`, field m1+ ~30.49%:

- full splat-median: ~48.44% pixels >1 LSB
- `QM1_ANY60`: ~15.18% >1 LSB, ~1.19% >4 LSB
- `QHI_ANY60`: ~9.98% >1 LSB, ~1.08% >4 LSB

`020950`, field m1+ ~28.05%:

- full: ~19.69% >1 LSB
- `QM1_ANY60`: ~10.08% >1 LSB, ~1.50% >4 LSB
- `QHI_ANY60`: ~9.02% >1 LSB, ~1.48% >4 LSB

`021001`, field m1+ ~34.79%:

- full: ~28.34% >1 LSB
- `QM1_ANY60`: ~17.00% >1 LSB, ~2.02% >4 LSB
- `QHI_ANY60`: ~13.10% >1 LSB, ~1.76% >4 LSB

Boromir `022530`, field m1+ ~33.60%:

- full: ~28.91% >1, ~12.71% >4, ~6.10% >8 LSB
- `QM1_ANY60`: ~17.78% >1, ~7.26% >4, ~0.87% >8 LSB
- `QHI_ANY60`: ~13.71% >1, ~7.22% >4, ~0.87% >8 LSB

Boromir `022539`, field m1+ ~23.39%:

- full: ~24.83% >1, ~7.54% >4, ~3.48% >8 LSB
- `QM1_ANY60`: ~11.22% >1, ~3.82% >4, ~1.30% >8 LSB
- `QHI_ANY60`: ~7.75% >1, ~3.80% >4, ~1.30% >8 LSB

Boromir `022550`, field m1+ ~20.37%:

- full: ~19.38% >1, ~3.73% >4 LSB
- `QM1_ANY60`: ~7.29% >1, ~0.53% >4 LSB
- `QHI_ANY60`: ~3.90% >1, ~0.43% >4 LSB

Interpretation:

The q map is much better than seed-distance gating. It removes broad low-value background authority while retaining multi-LSB changes in the actual severe-motion regions. Initial visual inspection did not show the mottled/block-shaped regression that killed the earlier repair-field-median experiment.

`QM1_ANY60` and the stricter `QHI_ANY60` remain the main post-reconstruction authority candidates. No final threshold has been chosen yet.

## 4. New refinement: photometric quality inside the splat itself

A vector can pass the 20 px forward/backward round-trip test yet still be photometrically dubious. Using such a vector at full splat weight undermines the whole point of separating geometric consistency from reconstruction quality.

New splat weight:

```text
base_conf = exp(-min(roundtrip_error, 40) / 8)
photo_conf = exp(-min(q_direction, 8000) / qscale)
weight = gaussian_footprint * base_conf * photo_conf
```

where `q_direction` is the per-direction clean-room photometric quality for that specific endpoint/vector.

Tested `qscale`: 1200, 2000, 3200, 5000, plus BASE with no q weighting.

All results below use a `QM1_ANY`-style post q-map gate and final 0.60 authority cap.

### 4.1 Boromir 022539

BASE:

- >1 LSB ~11.23%
- >4 LSB ~3.82%
- >8 LSB ~1.30%
- mean A/B warped disagreement where both sides have support ~0.02776 (normalized RGB)

QS1200:

- >1 ~7.38%
- >4 ~1.31%
- >8 ~0.31%
- A/B disagreement ~0.01351
- mean projected support also falls (alternate becomes more selective)

QS2000:

- >1 ~9.74%
- >4 ~1.62%
- >8 ~0.71%
- A/B disagreement ~0.01671

QS3200:

- >1 ~10.39%
- >4 ~1.70%
- A/B disagreement ~0.02079

QS5000:

- >1 ~10.73%
- >4 ~2.36%

### 4.2 Boromir 022530

BASE:

- >1 ~17.78%
- >4 ~7.26%
- >8 ~0.87%
- A/B disagreement ~0.03236

QS1200:

- >1 ~13.66%
- >4 ~2.21%
- >8 ~0.17%
- A/B disagreement ~0.01375

QS2000:

- >1 ~16.44%
- >4 ~4.41%
- >8 ~0.48%
- A/B disagreement ~0.01777

QS3200:

- >1 ~17.03%
- >4 ~4.44%
- A/B disagreement ~0.02477

QS5000:

- >1 ~17.32%
- >4 ~5.53%

### 4.3 Older difficult control 020950

BASE:

- >1 ~10.09%
- >4 ~1.50%

QS1200:

- >1 ~7.27%
- >4 ~0.55%

QS2000:

- >1 ~9.57%
- >4 ~1.49%

Higher qscales remain near ~9.7–9.8% >1 and ~1.49% >4.

### 4.4 Interpretation

This is a strong architectural result:

**directional photometric quality should influence which vectors are allowed to build the alternate midpoint representation, not merely how much of the finished alternate image is blended afterward.**

At `qscale ~= 1200`, mean disagreement between the independently splatted A/B hypotheses drops by roughly half on the two worst Boromir captures. High-magnitude output authority also falls sharply.

However, this is not automatically a win. QS1200 may make the alternate hypothesis too sparse/conservative and could remove some useful Boromir streak repair. Visual inspection is required before selecting QS1200 over a weaker photometric penalty such as QS2000.

## 5. Current two-level quality architecture

The research is converging on using the same *kind* of clean-room image-domain quality at two distinct levels:

```text
GLOBAL:
  interior fraction(q >= m1) >= 20%
       -> field enters robust reconstruction regime

LOCAL / DIRECTIONAL:
  q_direction downweights questionable vectors while splatting
  q_combined + projected support controls final alternate authority
```

This preserves the separation learned from SVP:

- round-trip consistency: is the vector geometrically self-consistent?
- directional matching quality: does the endpoint actually match convincingly?
- field occupancy: is ordinary reconstruction globally in a dangerous regime?
- coverage/support: is there an alternate hypothesis at this midpoint location?
- robust median: reject an outlier hypothesis without blindly trusting it.

## 6. Current next actions

1. Visually compare BASE vs QS1200 vs QS2000 on Boromir `022530/022539` actor/problem crops and on `020950/021001` controls.
2. Check whether QS1200 removes the visible streak reduction or merely removes harmful/low-value authority.
3. Test asymmetric ownership-aware A/B combination using directional support and directional q instead of a symmetric support-weighted splat blend.
4. Revisit the safe temporal hypothesis / `area_blend` behavior only after alternate-hypothesis quality is stable.
5. If a stable candidate survives the cross-corpus visual test, make the next isolated live build from the frozen baseline.
6. Continue committing research scripts and conclusions online before further long exploratory runs.
