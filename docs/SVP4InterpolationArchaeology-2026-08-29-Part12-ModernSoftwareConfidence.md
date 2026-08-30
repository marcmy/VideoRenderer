# SVP4 interpolation archaeology — part 12: modern software confidence and Build #3 direction

Date: 2026-08-29/30
Branch: `research/svp4-interpolation-archaeology`
Frozen live baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91` and must not be modified.

This note records the most important change in interpretation since Part 11: the normal modern proprietary NVOF path does **not** need NVIDIA hardware-cost output to construct the vector-quality field used by the scene/field classifier. The DLL has a compatibility path that can consume NVIDIA cost, but the modern path can derive its own image-domain matching error from the NVOF vector and luma planes.

That materially changes the next clean-room experiment. The interesting Build #3 path is now:

```text
MPCVR NVOF motion
    +
SVP-style image-domain 4x4 luma matching confidence
    +
our Adaptive Splat / robust-median reconstruction
```

not re-enabling live NVIDIA cost output on Turing.

## 1. Two vector-quality paths

The reconstructed proprietary behavior has two conceptually distinct inputs to the same confidence/classification machinery.

### Compatibility / legacy-cost path

```text
NVIDIA legacy hardware cost
    -> packed vector score
    -> luminance normalization
    -> field classifier
```

This remains useful archaeology and explains the legacy R32 packing studied in Parts 10–11.

### Modern software-confidence path

```text
NVOF vector
    -> integer motion displacement
    -> compare source 4x4 Y block with displaced reference 4x4 Y block
    -> 4x4 absolute-difference SAD
    -> scale-compensated score
    -> luminance normalization
    -> field classifier
```

The important architectural point is that hardware cost is not required for the ordinary modern confidence signal.

This also explains how proprietary SVP can obtain useful NVOF confidence without depending on the cost-output behavior that caused severe stalls in our Turing live-renderer experiments.

## 2. Reconstructed modern score

For the NVOF vector attached to a 4x4 source block, the integer displacement used by the software matching stage is reconstructed as:

```text
dx = trunc(rawFlowX_S10.5 / 32)
dy = trunc(rawFlowY_S10.5 / 32)
```

The image-domain error is then the sum of absolute 8-bit luma differences over the 4x4 block:

```text
sad = sum_4x4(abs(sourceY - displacedReferenceY))
```

For a reduced NVOF source with integer source scale `scale`, the modern score is compensated as:

```text
score = sad * scale^2
```

The vector metadata also carries the source 4x4 luma average used by the later luminance-normalization machinery.

This software score is therefore a direct question about the vector: if this vector is applied, does the referenced image content actually resemble the source 4x4 luma block?

It is a confidence/error signal, not a replacement motion vector.

## 3. Pair-luma normalization: denominator is 510 on the normal bidirectional path

Part 11 had already reconstructed the pair-luma helper:

```text
denom = marker == 3 ? 510.0 : 255.0
v = (previousLuma + currentLuma) / denom
lumaMap = trunc(pow(v, scene.luma) * 255)
if lumaMap < 21:
    lumaMap = 20
lumaMapByte = lumaMap & 0xff
```

The remaining ambiguity is now resolved: the field passed into this decision is the NVOF **direction flags**, not an unrelated marker. Normal bidirectional NVOF has flags value `3`.

Therefore the normal bidirectional pair-luma denominator is **510**, not 255.

With stock `scene.luma = 1.5`, the classifier then uses its already-reconstructed integer normalization:

```text
q = score24 * 255 / max(pairLumaByte, 1)
```

The Part 11 classifier details remain unchanged:

- outer `ignore = 0.04` border exclusion;
- `zero = 200`;
- `m1 = 1600`;
- `m2 = 2800`;
- `scene = 4000`;
- low-score denominator removal capped at `floor(2 * fullGridCells / 3)`;
- ordinary class threshold at 20% considered-cell occupancy.

## 4. Full-corpus software-SAD replay

A clean-room replay was run over the current capture corpus using BT.709 limited-range Y reconstructed from the capture BMPs. This is necessarily an approximation of SVP's original decoder-domain YUV because the diagnostic BMPs have already passed through a video-to-RGB conversion.

The resulting m1+ occupancy was:

| Capture | Previous image-domain q m1+ | SVP-modern SAD m1+ | SVP class |
|---|---:|---:|---:|
| 022518 | 7.68% | 9.03% | 0 |
| 022530 | 33.60% | 29.39% | 1 |
| 022539 | 23.39% | 28.13% | 1 |
| 022550 | 20.37% | 19.98% | 0 |
| Bilbo | 0.74% | 4.51% | 0 |
| 234826 | 12.80% | 17.73% | 0 |
| 234854 | 8.08% | 16.43% | 0 |
| 234955 | 16.66% | 14.62% | 0 |
| 234925 | 30.49% | 20.07% | 1 |
| 235102 | 29.40% | 23.79% | 1 |
| 020950 | 28.05% | 20.85% | 1 |
| 021001 | 34.79% | 22.30% | 1 |
| 013339 | 9.43% | 31.73% | 1 |
| 013416 | 18.53% | 36.02% | 1 |
| 000820 | 27.87% | 22.63% | 1 |
| 001431 | 27.74% | 39.09% | 1 |

Actual cuts classify strongly as expected.

The most interesting distinction remains the Boromir fling:

```text
022518 -> class 0
022530 -> class 1
022539 -> class 1
022550 -> class 0
```

The modern metric therefore promotes the two especially troublesome same-shot fast-motion pairs while leaving the earlier easier pair and the borderline later pair alone. This is cleaner than the earlier simple `15 -> 25%` global q ramp.

## 5. Adaptive Splat substitution and the qscale knee

The modern directional score was substituted into the existing Adaptive Splat V1.2 offline reconstruction as the vector-confidence attenuation term.

Reusing the previous `qscale = 1200` was too conservative because the software-SAD score has different statistics from the old proxy.

For `022530`, percentage of pixels changed by more than 4 LSB relative to the frozen golden reconstruction was:

```text
qscale 1200 -> 0.37%
qscale 1600 -> 2.58%
qscale 2000 -> 5.45%
qscale 2400 -> 6.62%
```

Current Adaptive Splat V1.2 is about `2.22%` on the same metric. Thus `qscale ~= 1600` restores approximately the current correction authority while avoiding the much broader support admitted at 2000+.

`022539` shows the same knee:

```text
qscale 1200 -> 0.68%
qscale 1600 -> 1.77%
qscale 2000 -> 2.40%
qscale 2400 -> 2.91%
```

This makes approximately **1600** the first serious Build #3 confidence scale. It is a research candidate, not yet a live-build constant.

## 6. The dangerous validation case: `013416`

`013416` is particularly important because the modern SVP classifier deliberately promotes it to class 1 while the previous MPCVR-side proxy treated it only as a gray-zone field.

At modern-SAD `qscale = 1600`, the offline reconstruction changes approximately:

```text
0.51% of pixels by >4 LSB
```

Initial visual inspection found the changes sparse and localized, without the obvious chunky/liquid geometry seen in earlier failed broad alternate-field experiments.

That is encouraging but **not sufficient to call the candidate clean**. The outstanding safety test is an exposure-enhanced / difference-amplified crop pass over the affected region, specifically looking for subtle reintroduction of the old liquid/stretched failure mode.

Do not promote Build #3 to a live renderer branch until this check and the rest of the corpus comparison are available again.

## 7. Reduced-source motion conversion is separate from software-SAD displacement

The proprietary DLL also reveals the exact conversion used when writing reduced-source NVOF vectors into SVP's packed downstream motion representation:

```text
packedMotion = trunc(raw_S10.5 * scale * precision / 32)

precision = 4  for scale 1 or 2
precision = 2  for scale 4, 6 or 8
```

This is a different operation from the integer displacement used by the 4x4 software-SAD measurement itself.

The valid proprietary source ratios remain exactly:

```text
1/1, 1/2, 1/4, 1/6, 1/8
```

with effective full-resolution grids:

```text
4, 8, 16, 24, 32
```

Scale 6 / effective grid 24 is therefore a first-class proprietary mode and has its own correct precision behavior. Effective grid 12 is not valid proprietary SVP behavior.

## 8. Consequence for the hardware-cost investigation

The dual R8/R32 replay utility and cost-output archaeology remain useful for compatibility research, but hardware cost is no longer the leading explanation for SVP's conventional interpolation advantage.

In particular, we should **not** re-enable live NVOF output cost merely to obtain confidence on Turing. We already observed severe stalls from that path, and the proprietary modern software-confidence design gives us a cleaner independent route.

The cost-disabled BGRA/NV12/effective-grid sweep is still useful as a separate motion-field experiment because input representation and source downscale can change the NVOF vectors themselves.

Keep these two questions separate:

1. What motion field does NVOF produce for a given source representation/grid?
2. Given that motion field, how trustworthy is each vector for reconstruction?

## 9. Build #3 clean-room direction

The current strongest architecture is:

```text
frozen golden reconstruction
        +
SVP-inspired 4x4 image-domain vector confidence
        +
independent directional ownership / coverage evidence
        +
Adaptive Splat repaired alternate hypothesis
        +
channelwise median shock absorber
        +
field classifier deciding when robust reconstruction is permitted
```

The signals must remain separate:

- **vector confidence** asks whether the motion hypothesis matches image content;
- **coverage/ownership** asks which endpoint geometrically owns the intermediate region;
- **field quality** asks whether this whole motion field has entered a regime where ordinary reconstruction is unreliable;
- **robust median** decides how to combine hypotheses when extra protection is justified.

Do not reinterpret high confidence as permission to trust raw flow directly. Earlier experiments already showed that direct coverage/raw-flow promotion resurrects severe geometry distortion.

## 10. Immediate next work

1. Preserve this modern software-SAD model in committed research tooling rather than leaving it only in transient replay code.
2. Correct the exact classifier oracle's normal bidirectional pair-luma default to direction-flags value `3` / denominator 510.
3. Restore the capture corpus when available and rerun the modern-SAD classifier from the committed tool.
4. Reproduce the qscale sweep around `1200, 1600, 2000, 2400` from committed code.
5. Perform an exposure-enhanced / difference-amplified `013416` crop inspection at qscale 1600.
6. Only after cross-corpus safety is established should a live Build #3 branch be created from the frozen baseline.
