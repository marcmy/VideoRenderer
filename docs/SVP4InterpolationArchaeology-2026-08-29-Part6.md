# SVP4 interpolation archaeology — part 6: exact `area_blend` semantics

Date: 2026-08-29
Branch: `research/svp4-interpolation-archaeology`
Frozen baseline remains `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`.

This continuation records the completed end-to-end trace of SVP4's `smooth.mask.area_blend` parameter from profile parsing to the GPU synthesis kernel.

## 1. What is now proven

`area_blend` is not merely an unused profile/UI value and it is not an A-vs-B geometric ownership weight.

It modifies the **temporal coordinate used for masked / artifact-fallback synthesis**, biasing that fallback toward the temporally nearest real source frame.

The profile default in the supplied SVP4 installation is exactly:

```text
smooth.mask.area_blend = 0.4
```

The complete recovered path is:

```text
JSON/profile option
  -> main SVP config
  -> renderer object
  -> per-frame render-parameter structure
  -> temporal-coordinate transform
  -> 4-byte OpenCL kernel argument
```

## 2. Parsing / storage trace

The default `double 0.4` is present in `svpflow2.dll` at virtual address approximately:

```text
0x180056a60
```

The `area_blend` option is parsed around:

```text
0x180008c71 .. 0x180008ca7
```

and the resulting value is retained in the main configuration structure.

In the active mode-2 renderer construction path, the value is copied into the renderer object at offset approximately:

```text
+0x38
```

The active render method is reached through the object's virtual dispatch and loads this field directly.

## 3. Per-frame parameter handoff

Inside the active per-frame rendering method (around `0x1800287e0` in the supplied binary), the renderer loads:

```text
[rdi + 0x38]
```

as a double, converts it to float, and stores it into the compact per-frame render-parameter structure.

The pointer passed downstream identifies the `area_blend` float as structure offset:

```text
+0x0c
```

The downstream GPU-synthesis setup receives this same per-frame structure and accesses the value as:

```text
[r14 + 0x0c]
```

where `r14` is the render-parameter pointer.

Thus the parameter's presence in live synthesis is proven independently of its profile name.

## 4. Exact temporal-coordinate transform

The active synthesis function also reads the discrete interpolation phase `p` from the per-frame parameter structure.

A binary constant used in the computation is exactly:

```text
0.00390625 = 1 / 256
```

so the ordinary normalized interpolation coordinate is:

```text
t = p / 256
```

The function then transforms this coordinate using `area_blend`.

Recovered behavior:

```text
if p <= 126:
    t_area = t * area_blend
else:
    t_area = 1 - (1 - t) * area_blend
```

The second constant in the branch is exactly float `1.0`.

Equivalent conceptual form:

```text
first half:
    pull t toward 0

second half:
    pull t toward 1
```

## 5. Meaning of the parameter

The edge cases make the semantics unambiguous.

### `area_blend = 1.0`

```text
t_area = t
```

Normal temporal position is preserved.

This agrees with historical observations that setting `area_blend=1.0` largely restored the older renderer behavior after this feature was introduced.

### `area_blend = 0.0`

```text
first half  -> t_area = 0
second half -> t_area = 1
```

Masked fallback snaps completely to the nearest real endpoint.

### Default `area_blend = 0.4`

Fallback retains only 40% of the ordinary distance from the nearest real endpoint.

Examples:

```text
ordinary t = 0.25 -> t_area = 0.10
ordinary t = 0.40 -> t_area = 0.16
ordinary t = 0.60 -> t_area = 0.84
ordinary t = 0.75 -> t_area = 0.90
```

Near the midpoint the masked temporal coordinate therefore jumps from roughly the 0.2 neighborhood to roughly the 0.8 neighborhood instead of lingering around a 50/50 double-image blend.

This is a deliberate ghost-reduction behavior.

## 6. GPU kernel binding

The computed `t_area` float is subsequently passed as a **4-byte OpenCL kernel argument** in the active synthesis setup path.

This establishes the full chain:

```text
profile 0.4
  -> parsed config
  -> live renderer field
  -> per-frame parameter
  -> exact nearest-endpoint temporal transform
  -> GPU synthesis kernel
```

The exact friendly source-level kernel variable name has not been recovered and should not be invented, but the value's role and formula are established.

## 7. Relationship to the clean-room MPCVR work

This result strengthens an earlier architectural inference but also corrects one possible over-interpretation.

`area_blend` should be understood as:

> **temporal positioning of the safe/artifact fallback hypothesis**

not as:

> A-vs-B ownership weighting of the motion-compensated hypotheses.

Those are separate mechanisms in SVP.

The clean-room V1.2 candidate currently keeps these concepts separate as well:

- projected A/B support + `support^3` controls ownership of the alternate motion-compensated hypothesis;
- field q determines when robust mode is allowed;
- local q/support controls how much robust alternate reconstruction can influence golden output;
- the safe temporal hypothesis is still the golden 50/50 midpoint reference in current offline V1.2.

## 8. Important caution about importing `0.4`

The numerical coincidence

```text
1 - area_blend = 0.6
```

and the independently selected V1.2 maximum robust authority

```text
alpha_max = 0.60
```

is interesting, but these are **not currently proven to be the same mechanism**.

SVP's `area_blend` acts on the temporal coordinate of its safe/masked reconstruction. V1.2's `0.60` is a blend cap between golden output and the robust median result.

Do not claim an exact formula relationship without further evidence.

## 9. Implication for a possible future experiment

After V1.2 midpoint-splat quality is stable, a separate clean-room experiment can test a nearest-endpoint-biased safe hypothesis inspired by this behavior.

However, this must be handled carefully for MPCVR's actual interpolation cadence. If the renderer synthesizes a single exact midpoint for a 2x path, blindly choosing the second-half branch at `t=0.5` would arbitrarily favor one endpoint and could introduce judder or temporal asymmetry.

Therefore:

- keep the current symmetric safe midpoint for the first V1.2 live build;
- treat nearest-endpoint temporal bias as a separate later experiment;
- validate it over actual temporal playback, not only still captures.

## 10. Current conclusion

`area_blend` archaeology is no longer an unresolved parameter-semantic question.

**It is a nearest-real-frame temporal bias for masked/artifact fallback synthesis.**

That behavior likely contributes to SVP's reduced double-image appearance in bad regions, but it should not be folded into V1.2 until the midpoint-splat live path itself is validated.
