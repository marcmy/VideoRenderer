# SVP4 interpolation archaeology — part 5: V1.2 fixed-point live design

Date: 2026-08-29
Branch: `research/svp4-interpolation-archaeology`
Frozen baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`.

This continuation records the final offline work needed to prove that the V1.2 midpoint-splat candidate can be represented with Shader Model 5 integer atomics without materially changing the float-reference reconstruction.

## 1. V1.2 state entering this phase

Current clean-room candidate:

```text
scene cut?
  yes -> golden scene-cut handling; robust authority = 0
  no  -> continue

field health:
  m1 = q >= 1600
  fieldAuthority = smoothstep(15%, 25%, m1+ occupancy)

directional midpoint splat:
  forward/backward round-trip consistency <= 20 px
  directional photo confidence exp(-min(q_direction,8000)/1200)
  round-trip confidence exp(-min(error,40)/8)
  3x3 footprint, sigma = 0.75

A/B ownership:
  support^3

alternate:
  S = support-weighted A/B midpoint-splat warp
  M = channelwise median(golden, S, temporal)

local authority:
  qRisk = smoothstep(1200, 2400, q)
  supportRisk = smoothstep(0.03, 0.22, support)
  alpha = min(qRisk * supportRisk, 0.60) * fieldAuthority

output:
  lerp(golden, M, alpha)

no authority blur
```

The 3x3 footprint replaced the original 7x7 prototype with no observed quality loss and substantially lower scatter traffic. The full-resolution sigma=0.8 authority blur was also removed after proving it was numerically irrelevant at final output.

## 2. Why fixed point is needed

The intended live implementation is Shader Model 5 / D3D11. The midpoint map is naturally a scatter operation, and the portable SM5 path has integer interlocked atomics but not the float atomic accumulation we would want for a literal translation of the CPU prototype.

The live representation therefore needs integer accumulators for:

- accumulated X displacement;
- accumulated Y displacement;
- accumulated support/weight.

Do not quantize the displacement independently. Instead accumulate displacement multiplied by the quantized weight and recover the mean through division by accumulated weight.

## 3. Direct fixed-point formulation

Selected weight scale:

```text
WEIGHT_SCALE = 60
```

For each valid source-vector contribution:

```text
weightQ = round(weight * 60)

sumWeight += weightQ
sumX += round(midpointDxPixels * weightQ)
sumY += round(midpointDyPixels * weightQ)
```

At resolve time:

```text
midpointDx = sumX / sumWeight
midpointDy = sumY / sumWeight
```

This retains subpixel motion through the ratio instead of quantizing each vector to an integer- or quarter-pixel representation before weighting.

### 3.1 Convenient support normalization

For the selected 3x3 sigma=0.75 footprint, the stationary Gaussian kernel quantized with scale 60 has total mass exactly:

```text
200
```

The live support estimate can therefore use approximately:

```text
support = saturate(sumWeight / 200.0)
```

This is both cheap and reproducible.

## 4. 32-bit overflow proof

Native flow is `R16G16_SINT` S10.5, so raw NVOF motion is bounded approximately to:

```text
[-1024, +1023.96875] pixels
```

The midpoint displacement is half of that, at most about +/-512 pixels.

On the 4x4 flow grid, midpoint projection shifts a source flow cell by at most roughly 128 coarse cells. Including the selected radius-1 splat footprint, a deliberately conservative bound for the number of source cells that could converge on one target cell is:

```text
259 * 259 = 67,081 contributors
```

For weight scale 60:

```text
max sumWeight ~= 67,081 * 60
              = 4,024,860

max abs(sumX or sumY)
  ~= 67,081 * 512 * 60
  = 2,060,728,320
```

Signed 32-bit maximum:

```text
2,147,483,647
```

Remaining conservative headroom:

```text
86,755,327
```

Therefore signed `R32_SINT` displacement accumulators and a 32-bit support accumulator are safe under the deliberately pessimistic convergence bound.

Actual corpus measurements are nowhere near this theoretical limit: the largest observed accumulated weight in the 18-capture replay was only 384.

## 5. Float-reference versus weight-60 integer replay

The direct integer formulation was replayed over all 18 current captures against the float V1.2 implementation.

Worst case is Boromir `022530`:

- only ~0.07246% of pixels differ from float V1.2 by more than 1 RGB LSB;
- 99th-percentile difference ~0.3367 LSB;
- mean difference ~0.01149 LSB.

Boromir `022539`:

- ~0.01636% >1 LSB;
- p99 ~0.0829 LSB;
- mean ~0.00405 LSB.

Boromir `022550`:

- ~0.00013% >1 LSB;
- p99 ~0.0185 LSB.

Known hard cuts are exact no-ops at final output because the scene-cut path forces robust authority to zero first.

### 5.1 Full corpus table

| capture | field m1+ | int-even vs float >1 LSB | p99 LSB | mean LSB | even vs old >1 LSB | max weight |
|---|---:|---:|---:|---:|---:|---:|
| 20260825-022518-725-pid28564 | 7.679% | 0.000000% | 0.0000 | 0.00000 | 0.000000% | 320 |
| 20260825-022530-672-pid28564 | 33.597% | 0.072460% | 0.3367 | 0.01149 | 0.000000% | 384 |
| 20260825-022539-208-pid28564 | 23.394% | 0.016362% | 0.0829 | 0.00405 | 0.000000% | 275 |
| 20260825-022550-867-pid28564 | 20.367% | 0.000130% | 0.0185 | 0.00075 | 0.000000% | 308 |
| 20260824-234826-219-pid32508 | 12.798% | 0.000000% | 0.0000 | 0.00000 | 0.000000% | 227 |
| 20260824-234854-252-pid32508 | 8.079% | 0.000000% | 0.0000 | 0.00000 | 0.000000% | 217 |
| 20260824-234925-860-pid32508 | 30.489% | 0.026556% | 0.1277 | 0.00492 | 0.000000% | 263 |
| 20260824-234955-712-pid32508 | 16.661% | 0.000000% | 0.0027 | 0.00014 | 0.000000% | 344 |
| 20260824-235102-889-pid32508 | 29.403% | 0.000195% | 0.0289 | 0.00136 | 0.000000% | 238 |
| 20260825-013339-543-pid20776 | 9.429% | 0.000000% | 0.0000 | 0.00000 | 0.000000% | 239 |
| 20260825-013416-845-pid20776 | 18.531% | 0.000000% | 0.0102 | 0.00068 | 0.000000% | 274 |
| 20260825-020950-720-pid28564 | 28.053% | 0.008506% | 0.2347 | 0.00777 | 0.000000% | 290 |
| 20260825-021001-304-pid28564 | 34.785% | 0.020193% | 0.1306 | 0.00517 | 0.000000% | 357 |
| 20260825-000620-257-pid32508 | 37.653% | 0.000000% | 0.0000 | 0.00000 | 0.000000% | 191 |
| 20260825-000820-480-pid32508 | 27.871% | 0.004870% | 0.0643 | 0.00282 | 0.000065% | 307 |
| 20260825-000853-294-pid32508 | 51.060% | 0.000000% | 0.0000 | 0.00000 | 0.000000% | 192 |
| 20260825-001406-466-pid32508 | 42.204% | 0.000000% | 0.0000 | 0.00000 | 0.000000% | 191 |
| 20260825-001431-319-pid32508 | 27.738% | 0.007030% | 0.2109 | 0.00707 | 0.000000% | 333 |

## 6. HLSL rounding semantics

Microsoft documents HLSL `round()` as nearest integer with halfway cases rounded to the nearest even integer.

The first CPU proof used `floor(v + 0.5)`, which has a different half-tie rule, especially for negative values. Because S10.5 optical flow produces binary-exact fractional displacements, this difference was explicitly tested rather than assumed irrelevant.

A second full-corpus replay implemented nearest-even rounding for both:

- `weight * 60`;
- `midpointDisplacement * weightQ`.

Result:

- nearest-even versus the old proof is below 1 LSB everywhere except `000820`;
- on `000820`, only ~0.000065% of pixels differ by >1 LSB;
- mean nearest-even versus old-proof difference there is ~0.000154 LSB;
- for severe Boromir `022530/022539`, no pixels exceed 1 LSB between the two tie rules.

Therefore the live implementation should simply use native HLSL `round()` and no custom tie emulation.

## 7. Conclusion

The fixed-point representation is no longer an open quality risk.

**Weight scale 60 + direct weighted-displacement integer accumulation is sufficiently transparent to treat as the live V1.2 representation.**

It provides:

- legal SM5 integer atomics;
- proven conservative signed-32-bit overflow safety;
- convenient support normalization (`/200`);
- effectively transparent final output versus the float reference;
- native HLSL rounding semantics with negligible difference from the initial CPU proof.

The remaining pre-build research is now primarily resource-layout / pass-count engineering and continued SVP `area_blend` archaeology, not uncertainty about the fixed-point splat math.
