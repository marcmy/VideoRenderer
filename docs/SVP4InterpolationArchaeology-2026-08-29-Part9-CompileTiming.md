# SVP4 interpolation archaeology — part 9: SM5 compile proof, phase symmetry, and live timing

Date: 2026-08-29
Branch: `research/svp4-interpolation-archaeology`
Frozen baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`.

This checkpoint records the transition from CPU live-proxy validation to compile-proven Shader Model 5 interfaces and a non-stalling performance-measurement plan for the eventual Build #2.

## 1. Production scheduler versus generic interpolation phase

The native backend computes a generic normalized interpolation phase:

```text
t = clamp(
    (outputTimestamp - previousTimestamp)
    / (inputTimestamp - previousTimestamp),
    0, 1)
```

However, the current DX11 renderer intentionally requests the exact midpoint between consecutive real source frames:

```text
requested = previousInput + (currentInput - previousInput) / 2
```

Therefore the current capture corpus at `t=0.5` is representative of today's production scheduler.

The adaptive splat should still use phase-correct formulas because generalization is essentially free.

For previous/A with A->B motion:

```text
projection scale = t
midpoint target = cell + t * F_AB / GridSize
inverse warp displacement = -t * F_AB
```

For next/B with B->A motion:

```text
projection scale = 1 - t
midpoint target = cell + (1 - t) * F_BA / GridSize
inverse warp displacement = -(1 - t) * F_BA
```

Generalized A/B alternate ownership should also retain ordinary temporal proximity:

```text
weightA = (1 - t) * supportA^3
weightB = t       * supportB^3
```

and the safe temporal hypothesis becomes:

```text
safe = lerp(A, B, t)
```

A future-proof robust envelope may use:

```text
phaseEnvelope = 4 * t * (1 - t)
```

which is 1 at today's exact midpoint and naturally becomes 0 at the real endpoints.

## 2. Arbitrary-phase swap symmetry

`tools/research/splat_phase_symmetry_v12.py` tests the generalized fixed-point 2x2 splat under the transformation:

```text
A <-> B
t <-> 1 - t
forward flow <-> backward flow
```

For binary-exact/complement-friendly phases such as:

```text
0, .25, .4, .5, .6, .75, 1
```

the directional maps and final alternate reconstruction matched exactly in the tested captures.

The endpoints also collapse to their real source images to numerical noise.

### 2.1 Floating phase corner case

At awkward decimal values such as `t=.1`, directly computing both `t` and `1-t` in floating point can produce values whose round-trip complement is not bit-identical. Because the scatter is deliberately fixed point, a tiny number of contributions can cross integer-rounding ties.

The effect is future-only and tiny, but measurable in a hypothetical A/B swap:

- Boromir `022530`, t=.1: mean alternate difference about 0.0019 RGB LSB, sparse maximum around 3.7 LSB;
- older `001431`, t=.1: mean around 0.0037 LSB, sparse maximum around 8.8 LSB.

Today's scheduler always requests `.5`, so this has no effect on Build #2.

## 3. Exact future arbitrary-phase fix

`tools/research/splat_phase_quantized_symmetry_v12.py` quantizes phase exactly once:

```text
phaseQ = round(t * 65536)
phaseA = phaseQ / 65536
phaseB = (65536 - phaseQ) / 65536
```

This guarantees exactly complementary binary phases.

Tested values included:

```text
.1, .2, .3, ~1/3, .5, .7, .9
```

on Boromir `022530` and old difficult `001431`.

Result:

**The tested A/B-swapped final alternate reconstructions became exactly identical.**

Thus arbitrary-phase support has a known clean extension if the scheduler ever moves beyond exact midpoint insertion. Do not add this complexity to the first Build #2 solely for today's t=.5 path.

## 4. Research-only Shader Model 5 implementation proof

The archaeology branch now contains compile-only HLSL prototypes:

```text
tools/research/hlsl/AdaptiveSplatScatter.hlsl
tools/research/hlsl/AdaptiveSplatResolve.hlsl
tools/research/hlsl/AdaptiveSplatWarpProbe.hlsl
```

and workflow:

```text
.github/workflows/nvof-adaptive-splat-compile.yml
```

The workflow uses Windows Server 2022 and the Windows 10 SDK `fxc.exe`, compiling all shaders as:

```text
/T cs_5_0 /E main /WX /O3
```

Successful run:

```text
workflow: NVOF Adaptive Splat SM5 Compile Proof
run id:   33278240108
commit:   6a108a66e52022fd7c44f5893f4de287f01bf110
result:   success
```

Compiler used:

```text
Windows Kits 10.0.26100.0 x86 fxc.exe
```

This is important because the previous COV-FB55 experiment exposed a real SM5 compiler limitation on component interlocked operations. The new adaptive-splat path avoids that design entirely.

## 5. Compile-proven interfaces

The successful proof establishes that `fxc` accepts all of the following under `cs_5_0`:

- scalar `InterlockedAdd` into `RWStructuredBuffer<int>`;
- read-only `StructuredBuffer<int>` resolve input;
- `RWTexture2DArray<float4>` resolve output;
- `Texture2DArray<float4>` filtered Warp sampling;
- seven-slot-style Warp resource layout represented by the compile probe;
- ordinary typed float4 UAV stores suitable for an `R16G16B16A16_FLOAT` runtime resource.

Current preferred resolved-map object is therefore one 2-slice texture array:

```text
DXGI_FORMAT_R16G16B16A16_FLOAT
ArraySize = 2
slice 0 = previous/A map
slice 1 = next/B map
```

One texture array is cleaner than two separate textures:

- one texture object;
- one Resolve UAV;
- one Warp SRV;
- symmetric resource lifetime;
- same 16 bytes/coarse cell total for both slices.

## 6. `fxc` assembly observations

The successful compile artifact contains `.asm` and `.cso` files for all three research shaders.

Approximate static instruction-slot counts reported by `fxc`:

```text
AdaptiveSplatScatter    ~319 slots
AdaptiveSplatResolve     ~49 slots
AdaptiveSplatWarpProbe   ~83 slots
```

Static assembly structure:

### Scatter

```text
24 atomic_iadd instructions statically present
4 exp instructions statically present
```

The 24 atomics correspond exactly to the unrolled maximum:

```text
2 directions * 4 bilinear targets * 3 scalar accumulators
```

Runtime execution is much lower because:

- field authority can early-return the whole thread on clean fields;
- invalid/OOB directions exit before scatter;
- quantized low-confidence weights of zero skip individual targets.

The measured CPU replay predicts only about 3.9-4.1 atomic adds per cell on severe `022530/022539`, not the static maximum 24.

### Resolve

Assembly contains:

```text
6 structured-buffer loads
2 typed texture-array UAV stores
0 texture samples
0 atomics
```

This is an intentionally small flow-grid pass.

### Warp probe

The compile probe contains six filtered texture samples in its standalone minimal version, including two texture-array slice samples and endpoint warps. The production Warp will also retain the existing dense/repair work, so this probe is a compatibility proof rather than a final instruction-count estimate.

## 7. Required baseline binding changes

Golden `UnbindCompute()` currently clears only:

```text
5 SRVs
4 UAVs
```

The adaptive path needs more.

Seed will bind the existing packed RegionReject/field counter as an additional UAV (`u4`).

Production Warp with the 2-slice texture-array design is expected to use:

```text
t0 previous frame
t1 next frame
t2 dense flow
t3 quality/scene-cut counter
t4 golden RepairField
t5 packed RegionReject / field count
t6 resolved splat texture array
```

Therefore leaving the current unbind counts unchanged risks resource leakage/hazards between passes.

Recommended simple fix for the experiment:

```text
UnbindCompute:
    clear 8 SRV slots
    clear 8 UAV slots
```

The unused null slots are harmless and avoid revisiting the helper every time the experiment grows by one binding.

## 8. Warp constant-buffer reuse

Golden Warp's current 32-byte constants contain two fields that are no longer referenced by the golden Warp shader:

```text
FlowCellCount
RepeatBadFraction
```

They can be replaced with `uint2 FlowSize` without increasing the constant-buffer size:

```text
uint2 FrameSize;
uint2 FlowSize;
float MidpointTime;
float RepairGridSize;
float2 Padding;
```

Warp can then derive the same 4%-border interior denominator as Seed and convert the packed field m1 count into the 15->25% authority ramp entirely on GPU.

## 9. Seed sampling cost

Seed is the natural place to calculate the clean-room q metadata because it already has:

- both native flow fields;
- both endpoint frames;
- the directional consistency calculations.

For one representative sample per 4x4 cell, q requires approximately:

- current previous/A luminance;
- current next/B luminance;
- one linear-filtered A endpoint at `pixel + F_BA`;
- one linear-filtered B endpoint at `pixel + F_AB`.

The endpoint samples can use the synthesizer's already-existing linear clamp sampler rather than four manual loads each.

Approximate flow-cell count:

```text
1920x1080: ~129,600 cells
3840x2160: ~518,400 cells
```

This remains substantially cheaper than a full-resolution extra analysis pass.

## 10. Non-stalling GPU timing plan for Build #2

The native backend already reports a CPU wall-clock `submit ms`, but D3D11 work is asynchronous, so that value does not isolate the incremental GPU cost of Scatter+Resolve.

For the experimental Build #2, use D3D11 timestamp queries around only the two new passes.

Per timing slot:

```text
D3D11_QUERY_TIMESTAMP_DISJOINT  disjoint
D3D11_QUERY_TIMESTAMP           start
D3D11_QUERY_TIMESTAMP           end
```

Issue:

```text
Begin(disjoint)
End(start)
Dispatch Scatter
Dispatch Resolve
End(end)
End(disjoint)
```

When reading old slots:

```text
GetData(..., D3D11_ASYNC_GETDATA_DONOTFLUSH)
```

Important rules:

- never spin/poll until S_OK;
- never call GetData in a loop;
- one nonblocking attempt per pending old slot is sufficient;
- if data is not ready, leave the slot pending;
- if all timing slots are pending, simply skip timing the current frame;
- discard a sample if `D3D11_QUERY_DATA_TIMESTAMP_DISJOINT::Disjoint` is true;
- calculate milliseconds only when start/end/disjoint are all ready:

```text
ms = 1000 * (endTimestamp - startTimestamp) / Frequency
```

Microsoft documents that timestamp differences are reliable only when the surrounding disjoint query reports `Disjoint == FALSE`, and warns specifically that repeatedly polling `GetData` with `DONOTFLUSH` can prevent progress because the command buffer is never flushed.

A small free-slot pool (for example four timing slots) is preferable to a fixed overwrite ring: pending query data must never be overwritten just because N submissions elapsed.

The timing telemetry is for the experimental build. It should be removable after performance is characterized.

## 11. Current pre-build status

The architecture has now passed:

- 18-capture reconstruction replay;
- hard-cut no-op testing;
- gray-zone field-authority testing;
- exact-grid sampling validation;
- packed-metadata validation;
- fixed-point overflow/quality validation;
- HLSL rounding validation;
- 2x2-vs-3x3 visual falsification;
- FP16 resolved-map validation;
- Shader Model 5 `fxc` compilation for structured atomics, array resolve, and Warp sampling;
- present-midpoint scheduler audit;
- arbitrary-phase symmetry research with a proven future fix.

The preferred Build #2 implementation remains:

```text
2x2 bilinear adaptive midpoint splat
31-bit packed Seed metadata
weight-60 scalar integer atomics
one structured accumulator UAV
one two-slice FP16 resolved texture array
exact GridSize=4 sampling
field authority smoothstep 15% -> 25%
qscale 1200
continuous round-trip confidence
support^3 ownership
local q smoothstep 1200 -> 2400
local support smoothstep .03 -> .22
max robust authority .60
channelwise median(golden, alternate, temporal)
no authority blur
hard-cut bypass
```

Do not add SVP's nearest-endpoint `area_blend=0.4` temporal bias to this first build. It remains a separate later playback experiment.
