# SVP4 interpolation archaeology — part 17: proprietary adaptive and force13 control flow

Date: 2026-08-30
Branch: `research/svp4-interpolation-archaeology`

This part closes two control-flow questions that remained partly dependent on the independent `open-svpflow` reconstruction: how `scene.adaptive` is decoded and consumed by the proprietary 64-bit `svpflow2.dll`, and how `scene.force13` interacts with the special `blocks13` classifier result.

The live MPCVR Build #3 experiment is intentionally unchanged by this work. It still does **not** implement SVP adaptive phase or direct algorithm 13 synthesis.

## 1. Proprietary `scene.adaptive` decoder is decimal-digit based

The supplied proprietary 64-bit DLL parses `scene.adaptive` with default literal `210` and decomposes it into decimal digits.

The relevant initialization code is around `0x180008e23..0x180008f2a`.

For each of the ones, tens, and hundreds digits the code:

1. extracts the decimal digit,
2. clamps values greater than or equal to 3 to 3,
3. subtracts 1,
4. stores the signed result.

The three values are stored at object offsets:

```text
+0x1f0
+0x1f4
+0x1f8
```

Therefore the stock value decodes exactly as:

```text
210 -> [-1, 0, +1]
```

This is now direct proprietary-binary evidence, not merely compatibility with the independent reconstruction.

## 2. Exact adaptive consumer

The normal render path consumes that table around `0x180014e6b..0x180014eb6`.

The important control flow is equivalent to:

```text
if scene.mode == 3
    and scene_class < 3
    and other render-path guards permit adaptive handling:

    adaptive_mode = adaptive[scene_class]

    if adaptive_mode >= 0
        phase = scene_phase(raw_phase, adaptive_mode)
```

The array index is the scene class itself:

```asm
movsxd rax, [scene_class]
mov    r8d, [object + rax*4 + 0x1f0]
test   r8d, r8d
js     skip_adaptive
...
call   0x1800103e0
```

With the stock table this gives:

```text
C0 -> -1 -> skip adaptive phase processing
C1 ->  0 -> adaptive mode 0
C2 -> +1 -> adaptive mode 1
C3 -> never indexes the table
```

## 3. Proprietary adaptive phase function matches the reconstructed timing model

The called function is at `0x1800103e0`.

The object timing fields used there are:

```text
+0x68 = frame_num
+0x70 = frame_den
```

The function first computes the phase quantum:

```text
step = frame_den * 256 / frame_num
```

It then performs the same left/right interval counting and mode-dependent final transform exposed independently by `open-svpflow` as `Timing::scene_phase_256`.

For normal exact 2x interpolation:

```text
frame_num = 2
frame_den = 1
step = 128
raw midpoint phase = 128
```

The proprietary arithmetic therefore gives:

```text
adaptive mode 0 -> phase 128
adaptive mode 1 -> phase 64
```

Combined with stock `scene.adaptive=210`:

```text
C0 -> ordinary phase 128 at midpoint
C1 -> adaptive mode 0 -> phase 128
C2 -> adaptive mode 1 -> phase 64
```

So the previous interpretation is now corroborated at the proprietary arithmetic level: at exact 2x midpoint, C1 remains a 1/2-time synthesis while C2 requests a 1/4-time synthesis.

## 4. Requested algorithm is not globally rewritten by `force13`

The configured/requested algorithm remains stored at object offset `+0x158`.

The normal proprietary default is algorithm 21. The lower algorithm whitelist recovered from the parser is:

```text
1, 2, 11, 13, 21, 22, 23
```

with an additional proprietary special range:

```text
90..101
```

`scene.force13` is stored as a boolean at `+0x160`, default true.

Rather than overwriting `+0x158`, the main render path derives a per-frame force flag and passes it into the renderer.

Inside the renderer, around `0x180018d78`, the effective algorithm selection is exactly equivalent to:

```text
if force13_this_frame and requested_algo >= 11:
    effective_algo = 13
else:
    effective_algo = requested_algo
```

The key instruction sequence is:

```asm
cmp    r12d, 0x0b      ; requested algorithm >= 11?
mov    ecx, 0x0d       ; candidate algorithm 13
cmovl  ecx, r12d       ; requested < 11: preserve request
test   al, al          ; per-frame force flag
cmove  ecx, r12d       ; force flag false: preserve request
```

Thus `force13` is a runtime per-frame algorithm substitution, not a configuration mutation.

## 5. `blocks13` sentinel is intentionally separated from the scene class

The exact classifier can return `-1` through its `blocks13` threshold path.

The wrapper around the classifier at `0x180011730` deliberately splits that result into two pieces.

After the classifier returns:

```text
raw_class = classifier(...)
blocks13_hit = (raw_class == -1)
returned_class = max(raw_class, 0)
```

The relevant behavior appears twice in the wrapper, including around `0x1800118d9..0x1800118ef` and `0x180011aa7..0x180011ab5`:

```asm
cmp    eax, -1
sete   byte ptr [blocks13_flag]
...
test   eax, eax
cmovns esi, eax
```

The return value therefore normalizes the special `-1` result to class 0 while preserving a separate boolean that records that `blocks13` fired.

This explains why treating `-1` as an ordinary fourth scene class was misleading.

## 6. Exact `force13` input predicate

At the main render call, around `0x18001586b`, the DLL constructs the per-frame force flag from the normalized scene class plus the separate `blocks13` sentinel.

When `scene.force13` is enabled, the important logic is equivalent to:

```text
force13_this_frame =
    (normalized_scene_class > 0)
    OR blocks13_hit
```

When `scene.force13` is disabled, the flag is cleared.

This means the special classifier result has a precise role:

```text
raw classifier -1
    -> normalize to C0 for ordinary scene handling
    -> retain blocks13_hit = true
    -> force algorithm 13 if requested algorithm >= 11
```

In other words, **`blocks13` is an alternate threshold for forcing algorithm 13 without promoting the frame to an ordinary C1/C2 scene class.**

That interpretation follows directly from the proprietary control flow and finally makes the option naming structurally coherent.

Stock `blocks13=0` leaves this path dormant.

## 7. Ordinary C1/C2 and class-3 nuance

For normal non-cut frames, normalized C1 and C2 satisfy `scene_class > 0`, so with stock `force13=true` and a requested algorithm >=11 they enter algorithm-13 territory.

The raw caller predicate is broad enough that class 3 also satisfies the positive-class test. However class-3/cut handling is routed separately by the surrounding scene logic, so this does not mean class 3 becomes an ordinary algorithm-13 interpolation case.

For implementation-level reasoning the useful statement remains:

```text
ordinary C1/C2 -> force13 territory
C3/cut -> separate cut/fallback handling
blocks13 sentinel -1 -> normalize to C0, but independently force13
```

This is slightly more precise than the simplified independent-source predicate that only expresses C1/C2.

## 8. Relationship to independent `open-svpflow`

The independent implementation corroborates the same architecture:

- default adaptive value 210,
- decimal adaptive decode,
- per-class adaptive table lookup that ignores negative entries,
- separate `scene.force13` option,
- effective algorithm 13 for ordinary scene classes 1/2 when requested algorithm is >=11,
- `Timing::scene_phase_256` with the same timing quantum and mode transform.

The proprietary binary now independently confirms the important control-flow pieces, including the additional `blocks13` sentinel plumbing that is easy to miss when looking only at the high-level model.

## 9. Consequences for MPCVR archaeology

The findings do **not** justify transplanting direct algorithm 13 into the current MPCVR motion fields. Part 14 already rejected that synthesis experimentally because it creates large coherent warped islands.

They do sharpen the conceptual model that can inform safer future work:

1. scene confidence and adaptive phase are separate from the synthesis algorithm choice;
2. C2 is temporally more conservative than C1 under stock adaptive mode;
3. algorithm 13 is a per-frame safety substitution, not the base algorithm;
4. `blocks13` provides a hidden force13-only channel that does not alter the ordinary scene class;
5. the current Build #3 dual-direction consensus policy should remain isolated from these newly reconstructed temporal semantics until a bounded golden-anchored experiment can be designed and replayed offline.

## 10. Next archaeology targets

The highest-value remaining control-flow questions are:

- exact `scene.mode=1` and `scene.mode=2` phase behavior and how it differs from adaptive mode 3;
- the meaning and downstream effect of `scene.blend` and its directional flags;
- whether algorithms 21/22/23 are wrappers/compositions around 11/13 and what additional masks/neighbors they introduce;
- the proprietary 90..101 algorithm family;
- whether any useful bounded behavior from these paths can be reproduced without importing the rejected direct-warp geometry.
