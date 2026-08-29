# SVP4 interpolation archaeology — part 8: exact-grid live proxy and FP16 resolved maps

Date: 2026-08-29
Branch: `research/svp4-interpolation-archaeology`
Frozen baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`.

This continuation closes several translation gaps between the offline V1.2 replay and an actual D3D11/Shader Model 5 implementation.

## 1. Exact 4-pixel grid sampling

Earlier CPU prototypes used `cv2.resize` to expand coarse flow-grid maps to frame resolution. The real NVOF grid is physically spaced every four source pixels, while captures such as 1918x803 are not exact multiples of four. `cv2.resize` therefore introduces a tiny whole-image stretch relative to the live-natural coordinate system.

The live-natural coarse coordinate is:

```text
coarse = pixel / 4
```

or, when sampled through a normalized linear texture sampler:

```text
uv = (pixel / 4 + 0.5) / FlowSize
```

A full 18-capture replay compared `cv2.resize` with exact-grid bilinear sampling.

### 1.1 Result

The candidate was not benefiting from the resize artifact.

For the 3x3 Gaussian V1.2 reference, the worst non-cut difference between resize and exact-grid sampling was approximately:

```text
>1 LSB : 0.133% of pixels
p99    : 0.42 LSB
```

For 2x2 bilinear splat, the worst was approximately:

```text
>1 LSB : 0.231% of pixels
p99    : 0.50 LSB
```

The actual correction footprint versus golden changed negligibly.

Conclusion:

**Use exact `pixel / 4` flow-grid alignment in the live shader.**

## 2. Visual falsification: 2x2 bilinear vs 3x3 Gaussian

Exact-grid visual comparison sheets were generated for the high-value captures:

- Boromir `022530`
- Boromir `022539`
- older difficult `020950`
- older difficult `234925`
- older difficult `001431`
- gray-zone `013416`

At normal inspection, 2x2 and 3x3 outputs are visually indistinguishable. Amplified differences are sparse edge/detail energy rather than:

- block/chunky geometry;
- bright splat outliers;
- broad low-frequency image replacement;
- mottled repair-field artifacts.

This is sufficient to retain **2x2 bilinear** as the preferred live scatter footprint.

The 1x1 nearest-target lower-bound experiment remained surprisingly close in final image space but materially worsened A/B hypothesis agreement on difficult captures. Therefore 2x2 is the current performance/quality sweet spot.

## 3. Fully stacked live proxy

A CPU replay was built that stacks the implementation approximations expected in the eventual shader:

1. 2x2 bilinear midpoint projection;
2. packed directional q/error/in-bounds metadata;
3. weight-scale-60 integer accumulation using nearest-even rounding;
4. exact `pixel/4` coarse-map sampling;
5. existing 15->25 field authority ramp;
6. qscale 1200;
7. ownership exponent 3;
8. local q ramp 1200->2400;
9. support ramp .03->.22;
10. robust alpha cap .60;
11. hard-cut bypass.

This live proxy was compared directly against the exact-grid float 3x3 Gaussian V1.2 reference.

Representative results:

### Boromir 022530

```text
exact-grid float 3x3:
  >1 LSB vs golden ~= 9.93%
  >4 LSB           ~= 2.19%

live proxy:
  >1 LSB vs golden ~= 9.84%
  >4 LSB           ~= 2.21%

live proxy vs 3x3 reference:
  >1 LSB ~= 0.238%
  >4 LSB ~= 0.0026%
  p99    ~= 0.49 LSB
```

### Boromir 022539

```text
live proxy vs 3x3:
  >1 LSB ~= 0.160%
  >4 LSB ~= 0.005%
  p99    ~= 0.20 LSB
```

### Boromir 022550

```text
live proxy vs 3x3:
  >1 LSB ~= 0.031%
  p99    ~= 0.056 LSB
```

### Older controls

`234925`:

```text
>1 LSB ~= 0.155%
>4 LSB ~= 0.015%
p99    ~= 0.31 LSB
```

`001431`:

```text
>1 LSB ~= 0.126%
>4 LSB ~= 0.033%
p99    ~= 0.27 LSB
```

Known hard cuts remain exact no-ops.

Conclusion:

**The live approximations do not compound into a meaningful new reconstruction behavior.**

## 4. Real scatter atomic count

The theoretical 2x2 upper bound is:

```text
4 targets * 3 scalar atomic adds * 2 directions
= 24 atomic adds / source flow cell
```

However, packed q/error confidence and weight quantization eliminate many weak contributions before an atomic is issued.

Measured examples for 1918x803 captures:

```text
022530 : ~4.08 atomic adds/cell, ~0.39 million/frame
022539 : ~3.87 atomic adds/cell, ~0.37 million/frame
022550 : ~7.52 atomic adds/cell, ~0.73 million/frame
```

The busiest tested capture was approximately:

```text
~11.17 atomic adds/cell
~1.08 million total atomic adds/synthetic frame
```

This makes the scatter stage substantially less concerning than the original worst-case 3x3 estimate.

## 5. RegionGate packed-metadata correctness fix

Once `UnsafeCellMap` carries packed metadata, the current RegionGate behavior:

```text
UnsafeCellMap[cell] != 0
```

is invalid because nearly every valid cell will contain nonzero q/error metadata.

RegionGate must explicitly test the catastrophic bit from the Part 7 encoding:

```text
CatastrophicBit = 1u << 30

unsafe = (UnsafeCellMap[cell] & CatastrophicBit) != 0
```

Failing to make this change would classify almost the entire flow grid as catastrophic and corrupt both regional telemetry and any logic built on it.

This is a required live implementation change, not an optional optimization.

## 6. Seed-side q cost

The field/local q signal does not require a separate full-frame analysis pass.

Seed already has both native flow fields and both source frames bound. The new q calculation requires one representative cell sample per direction.

For each coarse cell:

```text
B->A:
  source luminance at B(pixel)
  matched luminance at A(pixel + F_BA)

A->B:
  source luminance at A(pixel)
  matched luminance at B(pixel + F_AB)
```

The matched endpoint samples can use the synthesizer's existing linear-clamp sampler instead of manually loading four texels each. This keeps the additional frame-sampling cost small relative to the existing NVOF validation path.

At 1920x1080, the 4x4 grid contains only about 130k cells; at 3840x2160, about 518k cells.

## 7. Interpolation phase semantics

The native backend itself computes a general interpolation coordinate:

```text
MidpointTime = clamp(
    (outputTimestamp - previousTimestamp)
    / (inputTimestamp - previousTimestamp),
    0, 1)
```

so the dense synthesizer is technically capable of arbitrary `t`.

However, the current DX11 scheduler explicitly requests:

```text
lastInput + (currentInput - lastInput) / 2
```

for normal operation after startup.

Therefore today's live renderer is genuinely a midpoint-insertion path and the t=0.5 capture corpus is representative.

Even so, the new splat should not hard-code 0.5 because phase-correct generalization is essentially free.

For source A at t=0 with A->B flow `F_AB`:

```text
targetCell = sourceCell + t * F_AB / GridSize
inverse displacement = -t * F_AB
```

For source B at t=1 with B->A flow `F_BA`:

```text
targetCell = sourceCell + (1 - t) * F_BA / GridSize
inverse displacement = -(1 - t) * F_BA
```

At t=0.5 these reduce exactly to the current prototypes.

A future-proof robust-authority envelope can also use:

```text
phaseEnvelope = 4 * t * (1 - t)
```

which is 1 at the current midpoint and naturally vanishes at real endpoints. This has not yet been adopted into the selected midpoint build because the current scheduler always requests t=0.5; it remains a safe generalization candidate.

## 8. FP16 resolved splat maps

The resolved directional maps were tested after conversion through IEEE half precision.

Proposed layout per coarse direction:

```text
R16G16B16A16_FLOAT

xy = inverse displacement in pixels
z  = normalized projected support
w  = auxiliary q value / spare metadata
```

A full 18-capture replay compared FP16 resolved maps to the FP32 live proxy.

Result:

**Zero captures produced any pixels differing by more than 1 RGB LSB.**

Representative p99 FP16-vs-FP32 errors:

```text
022530 : ~0.0022 LSB
022539 : ~0.0009 LSB
020950 : ~0.0010 LSB
001431 : ~0.0010 LSB
```

Mean errors were correspondingly microscopic.

Maximum observed coarse displacement was well within half-float's useful range, including the much larger values seen on cut captures.

Conclusion:

**FP16 is effectively lossless for the resolved V1.2 maps and halves their bandwidth versus R32G32B32A32_FLOAT.**

## 9. D3D11 format support

Microsoft's Direct3D Feature Level 11.0 format-support table marks `DXGI_FORMAT_R16G16B16A16_FLOAT` as hardware-required for:

- Texture2D;
- shader load;
- filtered shader sampling;
- typed UAV creation;
- UAV typed store.

Typed UAV *load* is optional at FL11.0, but the proposed design does not require it:

- Resolve writes the texture as a UAV;
- the resource is unbound;
- Warp later reads it as an SRV with normal linear sampling.

Therefore a valid Feature Level 11.0 device is required to support the exact operations V1.2 needs on the FP16 maps.

A runtime format-support assertion may still be useful for diagnostics, but an R32 fallback is not architecturally required for conforming FL11.0 hardware.

## 10. Updated preferred live layout

The resource plan is now cleaner than Part 7's initial R32 reuse proposal.

Preferred design:

```text
existing resources:
  SeedMap
  UnsafeCellCount
  packed UnsafeCellMap
  RepairCandidate
  RepairField
  RegionReject packed telemetry/field count
  DenseFlow

new resources:
  one flat RWStructuredBuffer<int> accumulator
  two flow-sized R16G16B16A16_FLOAT resolved splat maps

new shaders:
  SplatScatter
  SplatResolve
```

The resolved maps remain symmetric and compact instead of reusing an R32 repair texture for only one direction.

The additional persistent coarse-map memory is small, while Warp bandwidth is half of an R32G32B32A32 implementation.

## 11. Current preferred implementation candidate

For the first adaptive-splat live build, the preferred performance representation is now:

```text
2x2 bilinear midpoint scatter
31-bit packed Seed metadata
weight scale 60 integer accumulation
one structured accumulator UAV
nearest-even HLSL round()
two FP16 resolved directional maps
exact pixel/4 grid sampling
field smoothstep 15% -> 25%
qscale 1200
round-trip confidence exp(-error/8)
ownership support^3
local q smoothstep 1200 -> 2400
support smoothstep .03 -> .22
max robust authority .60
no authority blur
hard-cut bypass before robust output
```

The principal remaining pre-build research tasks are:

1. arbitrary-phase swap/end-point symmetry test;
2. exact HLSL resource-binding / unbinding audit;
3. build-time shader compilation proof for structured-buffer atomics + FP16 resolve maps;
4. optional GPU-timing instrumentation plan for the live test;
5. keep the golden baseline untouched and implement only on a new experimental branch.
