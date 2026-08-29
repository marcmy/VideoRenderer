# SVP4 interpolation archaeology — part 7: packed metadata and live resource design

Date: 2026-08-29
Branch: `research/svp4-interpolation-archaeology`
Frozen baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`.

This continuation moves the clean-room V1.2 candidate from offline reconstruction math toward a practical D3D11 / Shader Model 5 implementation.

## 1. Main implementation problem

The initial literal implementation would have required the splat pass to recompute:

- both directional round-trip errors;
- both directional image-domain q scores;
- endpoint in-bounds state;
- catastrophic state;

while also reading both source frames and both native flow fields.

That would duplicate work already performed naturally in Seed and make the new scatter stage unnecessarily expensive.

The existing flow-sized `UnsafeCellMap` is only `R32_UINT` and currently stores essentially a catastrophic 0/1 flag, so it has enough unused bandwidth to become a compact per-cell metadata map.

## 2. Selected 31-bit metadata encoding

A practical clean-room encoding is:

```text
bits  0.. 9 : q_BtoA / 8, clamped 0..1023
bits 10..19 : q_AtoB / 8, clamped 0..1023
bits 20..23 : ceil(BtoA_roundtrip_error / 2), clamped 0..15
bits 24..27 : ceil(AtoB_roundtrip_error / 2), clamped 0..15
bit      28 : B->A endpoint in frame
bit      29 : A->B endpoint in frame
bit      30 : catastrophic (existing regional-telemetry meaning)
bit      31 : spare
```

The q range therefore covers approximately:

```text
0 .. 8184
```

which is sufficient because V1.2's directional q confidence explicitly clamps at 8000 before applying:

```text
exp(-q / 1200)
```

The 8-unit q resolution is negligible relative to the 1200/1600/2400 thresholds used by the algorithm.

## 3. Exact preservation of the 20-px validity boundary

The error bin is intentionally conservative:

```text
errorQ = ceil(error / 2)
```

and live validity becomes:

```text
errorQ <= 10
```

This preserves the critical acceptance boundary exactly:

```text
ceil(error / 2) <= 10
    iff
error <= 20
```

Thus the packed representation does **not** accidentally widen or tighten the golden 20-px consistency criterion.

Only the continuous confidence value is approximated, using:

```text
errorApprox = 2 * errorQ
base_conf = exp(-errorApprox / 8)
```

which is deliberately slightly conservative versus the unquantized value.

## 4. Out-of-frame semantics remain separate from q

The clean-room field-health metric deliberately treats an out-of-frame directional endpoint as a visibility/disocclusion condition rather than matching failure.

Therefore Warp reconstructs combined local q from the two packed directions as:

```text
q = 0.5 * (
      (BtoAInBounds ? q_BtoA : 0)
    + (AtoBInBounds ? q_AtoB : 0)
)
```

Do **not** renormalize by the number of surviving directions.

This preserves the earlier architectural separation:

- q = matching/reconstruction health;
- in-bounds / coverage = visibility;
- neither should silently substitute for the other.

## 5. Full-corpus packed-metadata replay

The selected encoding was replayed over all 18 captures while keeping the V1.2 field ramp, qscale, ownership exponent, robust median, local authority ramp, and scene-cut behavior intact.

Compared with the float V1.2 reference, the worst case was Boromir `022530`:

```text
>1 LSB difference : ~0.08506% pixels
p99 difference    : ~0.3569 LSB
mean difference   : ~0.01238 LSB
```

Boromir `022539`:

```text
>1 LSB : ~0.04467%
p99    : ~0.1006 LSB
mean   : ~0.00494 LSB
```

Boromir `022550`:

```text
>1 LSB : ~0.00357%
p99    : ~0.0215 LSB
```

Difficult controls similarly remained well below one-LSB significance for nearly all pixels.

Known hard cuts remain exact final-output no-ops because the scene-cut path disables robust reconstruction first.

Conclusion:

**The packed metadata representation is sufficiently transparent for the live implementation.**

## 6. Regional telemetry / field-count packing

A separate 1x1 allocation is not required for the new field-bad count.

The existing `RegionReject` 1x1 `R32_UINT` texture currently stores only the maximum catastrophic count observed in a radius-3 (7x7) neighborhood. That maximum is at most 49 and therefore needs only six low bits.

The same texture can carry:

```text
bits  0.. 5 : existing max-local-catastrophic telemetry (0..49)
bits  6..31 : field m1+ count
```

During Seed:

```text
if interior && q >= 1600:
    InterlockedAdd(RegionReject[0], 1 << 6)
```

Seed completes before RegionGate dispatch, so the upper field-count bits are stable when RegionGate runs.

RegionGate can preserve them while updating the low telemetry bits:

```text
uint base = RegionReject[0] & ~63u;
InterlockedMax(RegionReject[0], base | min(localUnsafeCount, 63u));
```

Warp then reads:

```text
fieldBadCount = RegionReject[0] >> 6
```

and computes the 15->25% field authority entirely on GPU.

The existing asynchronous telemetry readback can decode both values without another staging resource.

## 7. Seed UAV pressure remains safe

Current golden Seed uses:

```text
u0 SeedMap
u1 UnsafeCellCount
u2 UnsafeCellMap
u3 RepairCandidate
```

Adding the existing `RegionReject` texture as `u4` for field-count increments keeps Seed below the Shader Model 5 / D3D11 UAV pressure relevant to the current path.

No splat accumulators should be bound to Seed; that would duplicate concerns and raise UAV pressure. A dedicated scatter pass remains cleaner.

## 8. One structured accumulator buffer instead of six textures

The midpoint splat conceptually needs six integer accumulators per target flow cell:

```text
A-side sumX
A-side sumY
A-side sumWeight
B-side sumX
B-side sumY
B-side sumWeight
```

These do not need six separate `R32` UAV textures.

A single flat structured buffer can store:

```text
flowCellCount * 6 ints
```

and the shader can atomically update scalar indices:

```text
RWStructuredBuffer<int> SplatAccum;

base = targetIndex * 6;
InterlockedAdd(SplatAccum[base + 0], ...);
InterlockedAdd(SplatAccum[base + 1], ...);
InterlockedAdd(SplatAccum[base + 2], ...);
...
```

This avoids the `RWTexture2D<uint2>` component-atomic compiler limitation encountered in the earlier COV-FB55 experiment.

Microsoft's Shader Model 5 documentation explicitly permits read/write structured-buffer resource variables in interlocked operations, and D3D11 `ClearUnorderedAccessViewUint` supports structured-buffer UAVs; for raw/structured buffer views only the first clear value is used.

A single `ClearUnorderedAccessViewUint(..., {0,0,0,0})` can therefore reset the whole accumulator between synthetic frames without a custom clear compute pass.

## 9. Proposed coarse resolve stage

After scatter, a cheap flow-grid resolve pass converts each direction's integer sums to:

```text
float2 inverseDisplacement
float support
```

The selected weight-60 / 3x3-sigma0.75 representation uses:

```text
support ~= saturate(sumWeight / 200.0)
```

The resolve maps are flow-resolution resources, not full-resolution images.

One existing resource can be reused safely:

- `RepairCandidate` (`m_repairTextures[0]`) is no longer needed after Repair has consumed it and produced `RepairField` in `m_repairTextures[1]`.
- After Repair, `m_repairTextures[0]` can be overwritten with one resolved directional splat map.

The other direction requires one additional flow-sized resolved map unless a later packing optimization proves worthwhile.

This yields a straightforward implementation with:

- one new structured integer accumulator buffer;
- one new flow-sized float4 resolved-splat texture;
- reuse of the existing repair-candidate float4 texture for the other direction;
- one scatter compute shader;
- one resolve compute shader.

## 10. q sampling in Warp without a new q texture

Warp can consume the packed `UnsafeCellMap` as an SRV and manually bilinear-interpolate combined q from four coarse cells.

To match the current offline `cv2.resize` half-pixel convention, the coarse source coordinate for a full-resolution pixel should use the equivalent of:

```text
coarse = (pixel + 0.5) / GridSize - 0.5
```

rather than blindly reusing the existing RepairField sampling convention.

The same alignment issue must be handled consistently for resolved splat maps when translating the offline replay to HLSL.

## 11. New performance question: Gaussian versus bilinear splat

The current V1.2 scatter uses a 3x3 Gaussian footprint.

Worst-case atomic operations per source flow cell if both directions contribute:

```text
3x3 Gaussian:
  9 targets * 3 scalar atomics * 2 directions
  = 54 atomic adds
```

A standard 2x2 bilinear splat would require:

```text
2x2 bilinear:
  4 targets * 3 scalar atomics * 2 directions
  = 24 atomic adds
```

Initial full-corpus float replay shows the bilinear form is surprisingly close to 3x3 V1.2. On Boromir `022530`:

```text
3x3 V1.2: >1 ~9.87%, >4 ~2.18%
2x2:       >1 ~9.79%, >4 ~2.23%

2x2 versus 3x3:
  only ~0.20% pixels differ by >1 LSB
  p99 difference ~0.38 LSB
```

Other captures were similarly close, although A/B hypothesis agreement is slightly worse under 2x2.

Therefore 2x2 is now an optimization candidate, **not yet the selected live footprint**. It requires fixed-point replay and visual falsification before replacing the proven 3x3 form.

## 12. Round-trip confidence ablation

Because packed metadata can carry conservative 2-px error bins, removing the round-trip confidence factor is not justified merely for implementation simplicity.

Ablation showed that replacing:

```text
exp(-roundtrip / 8)
```

with binary `roundtrip <= 20` validity:

- broadens alternate support;
- consistently worsens A/B warp agreement;
- changes final output only modestly because later robust guards remain active.

The continuous error confidence is therefore useful and cheap enough to retain.

## 13. Current live-design direction

The preferred implementation architecture is currently:

```text
Seed
  -> golden validation/repair candidate
  -> packed directional q/error metadata in UnsafeCellMap
  -> field m1 count in upper RegionReject bits

Repair
  -> golden RepairField

RegionGate
  -> preserves field-count upper bits
  -> writes max-local-catastrophic telemetry low bits

Splat scatter
  -> reads native flows + packed metadata only
  -> writes one structured integer accumulator buffer

Splat resolve
  -> writes two coarse directional inverse-flow/support maps
  -> reuses old RepairCandidate texture for one side
  -> one new float4 coarse texture for the other

existing JFA + Dense
  -> golden reconstruction unchanged

Warp
  -> golden output logic
  -> reads field count, packed q, resolved splats
  -> constructs ownership-weighted alternate
  -> median(golden, alternate, temporal)
  -> applies local q/support authority
```

This keeps the new mechanism isolated from the golden motion-estimation/JFA path and requires no CPU synchronization or NVOF hardware-cost output.
