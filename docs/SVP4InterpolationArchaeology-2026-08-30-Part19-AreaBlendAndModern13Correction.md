# SVP4 Interpolation Archaeology — Part 19: `mask.area_blend` and the Modern Algorithm-13 Correction

Date: 2026-08-30

Status: research-only. The live Build #3 / MASTER-SAFE branch is unchanged.

## Why this part exists

Part 14 rejected a direct transplant of reconstructed SVP algorithm 13 onto the
current MPCVR/NVOF repaired-splat fields. That experiment reproduced the older
open-svpflow-style core:

```text
result = median3(warpA, warpB, temporal(A,B,phase))
```

Further proprietary tracing found a 2019-era `mask.area_blend` transform that is
live in the current `svpflow2.dll` and changes the source-frame mixture used by
the modern renderer. The old replay therefore needed a methodological
correction before its rejection could remain authoritative.

The corrected experiment is implemented in:

```text
tools/research/svp_modern13_area_blend_replay.py
```

## Raw plugin defaults versus Manager profile settings

The supplied current SVP package's `script/override_list.txt` contains the raw
plugin defaults:

```text
smooth.mask.cover      = 100
smooth.mask.area       = 0
smooth.mask.area_sharp = 1.0
smooth.mask.area_blend = 0.4
```

This must not be confused with a normal SVP Manager profile. Current
`script/generate.js` creates `smooth.mask` from profile settings and passes
`profile.fi_masking` into `smooth.mask.area`; it also changes `mask.cover` to 80
for algorithms >=21.

Historically, Manager 4.3.0.165 explicitly changed artifact masking to be on by
default. The corresponding SVPflow release says SAD masking and the 13th shader
were improved. Therefore `mask.area=0` is correctly described as the raw plugin
default, not as proof that ordinary Manager playback has no SAD masking.

Sources:

- https://www.svp-team.com/news/ — Manager / SVPflow 4.3.0.165 release notes.
- https://www.svp-team.com/wiki/Manual:FRC/ — current user-facing description of
  artifact masking and algorithms 13/21/23.

## Historical provenance of `area_blend`

The origin is unusually well documented.

On 2019-06-01, SVP forum user Mystery proposed changing the source-frame blend
inside artifact/SAD-masked regions from 50/50 to approximately 20/80. The stated
motivation was:

- 50/50 replaces interpolation artifacts with double shadows;
- 0/100 nearest-frame replacement causes repaired areas to slide and jump;
- 20/80 keeps the repair temporally attached to the nearer real frame without
  creating a full hard switch.

Chainik identified the discussion specifically as SVP's SAD/artifacts masking,
tested the change, and reported that it was definitely better, especially on
old/low-resolution material.

Source:

- https://www.svp-team.com/forum/viewtopic.php?id=5306

Eight days later, Manager/SVPflow 4.3.0.165 shipped with improved masking and an
improved 13th shader, explicitly crediting Mystery.

## Proprietary binary reconstruction

The current DLL carries `area_blend` into the backend request structure. In the
render path:

```text
0x180028bdd  movsd xmm0, [rdi+0x38]   ; area_blend
0x180028cd9  mov   eax,  [rdi+0x48]   ; directional phase
0x180028cfe  movsd xmm0, [rdi+0x40]   ; area_sharp
```

The kernel setup at approximately `0x18003cb84` converts the integer phase to a
0..1 fraction using 1/256, then applies `area_blend`:

```text
p = phase / 256

if phase <= 126:
    p_mask = p * area_blend
else:
    p_mask = 1 - (1-p) * area_blend
```

The resulting float is stored at `[rbp+0xb8]` and supplied as an OpenCL kernel
argument at approximately `0x18003d2d5`.

For the raw default `area_blend=0.4`:

```text
phase  64 (p=.25) -> p_mask=.10
phase 128 (p=.50) -> p_mask=.80
phase 192 (p=.75) -> p_mask=.90
```

The phase-128 tie follows the later-frame branch. This is recorded as observed
integer behavior; no claim is made that the `126` boundary is an error.

## Historical cross-check: the transform is not gated only by `mask.area`

A 2019 SVPflow 4.3.0.161-vs-.165 comparison independently found that changing
`area_blend` changed algorithms 13 and 23 even with `mask.area=0`. Setting
`mask.area=0` and `area_blend=1.0` made .165 algorithm 13/23 reproduce the older
.161 results in that test.

Source:

- https://blackmickeysvp.blogspot.com/2017/07/svp20170821.html — see the
  2019-06-16 update and its 4.3.0.161/165 PSNR comparison.

This is consistent with the current binary passing the transformed phase into
the render kernel unconditionally rather than treating it merely as a scalar
on an externally enabled area mask.

## Exact Part-14 replay validation

Before changing the source blend, the new replay was required to reproduce the
old phase-aware Part-14 measurements. It does.

Examples:

| capture | class/phase | old Part-14 diagnostic | reproduced |
|---|---:|---:|---:|
| 022530 | C2 / 64 | >4 8.812%, >8 5.143% vs adaptive temporal | exact |
| 022539 | C1 / 128 | >4 18.252%, >8 9.512% vs golden | exact |
| 022550 | C1 / 128 | >4 22.046%, >8 11.962% vs golden | exact |
| 013416 | C2 / 64 | >4 5.858%, >8 3.429% vs adaptive temporal | exact |
| 001431 | C1 / 128 | >4 31.617%, >8 15.826% vs golden | exact |

This validates that the modern-area-blend experiment is using the same
phase-aware repaired-splat geometry as Part 14 rather than the earlier erroneous
all-midpoint sensitivity pass.

## Corrected modern algorithm-13 result

The corrected core replaces the ordinary temporal member of the median with the
nearest-frame-biased source mixture:

```text
old13    = median3(warpA, warpB, temporal(A,B,p))
modern13 = median3(warpA, warpB, temporal(A,B,p_mask))
```

The result does **not** rescue direct algorithm 13 on the MPCVR/NVOF fields.
It often trades isolated/severe error for much larger coherent regions.

### C2 examples — reference is adaptive temporal at phase 64

| capture | variant | >4 | >8 | largest >4 component |
|---|---|---:|---:|---:|
| 022530 | old13 | 8.812% | 5.143% | 0.571% frame |
| 022530 | modern13 | 15.143% | 5.418% | 3.051% frame |
| 013416 | old13 | 5.858% | 3.429% | 0.676% frame |
| 013416 | modern13 | 15.769% | 4.521% | 4.717% frame |

### C1 examples — reference is frozen MPCVR midpoint

| capture | variant | >4 | >8 | largest >4 component |
|---|---|---:|---:|---:|
| 022539 | old13 | 18.252% | 9.512% | 1.809% frame |
| 022539 | modern13 | 31.912% | 12.724% | 10.703% frame |
| 022550 | old13 | 22.046% | 11.962% | 2.513% frame |
| 022550 | modern13 | 27.454% | 15.898% | 12.774% frame |
| 001431 | old13 | 31.617% | 15.826% | 8.425% frame |
| 001431 | modern13 | 31.705% | 16.808% | 12.428% frame |

The structural regression is the important result. The current project has
already shown that large connected artifact islands are visually much more
objectionable than sparse small differences with similar aggregate error.

## `mask.area` reconstruction

The proprietary magnitude-mask generator and the independent reconstruction
agree on the score-derived mask shape:

```text
areaMask = clamp255(
    pow(
        4 * vector_score * areaScale / blockArea,
        areaSharp
    ) * 255
)
```

For the NVOF/8-bit path, positive `mask.area=N` is converted approximately to:

```text
areaScale = N / 10000
```

with raw default `areaSharp=1.0`.

For the replay, the two directional software-SAD score masks are combined by
max, matching the dual-direction final-mask behavior used by algorithm 13.
`area=200` is used as a historically representative sensitivity point.

### `area=200` does not rescue modern 13

Examples:

| capture | modern13 + area200 | >4 | >8 | largest >4 component |
|---|---|---:|---:|---:|
| 022530 C2 | vs adaptive temporal | 10.853% | 1.783% | 2.984% frame |
| 013416 C2 | vs adaptive temporal | 13.198% | 2.001% | 2.697% frame |
| 022539 C1 | vs golden | 30.107% | 9.774% | 10.526% frame |
| 022550 C1 | vs golden | 29.837% | 16.156% | 8.917% frame |
| 001431 C1 | vs golden | 41.057% | 26.157% | 25.636% frame |

The area mask can reduce the >8 tail on some C2 frames, but it does so while
leaving or creating large coherent repair regions. On the difficult C1 fields
it is substantially worse.

The exact proprietary NVOF score-domain scaling remains a caveat for absolute
mask amplitude. That caveat does not affect the stronger conclusion from the
modern core itself: the `.4` phase remap already grows the structural regions
before the optional score mask is introduced.

## `mask.area` versus Build #3 modern-SAD q

`mask.area` is also not an independent confidence signal. Across the restored
18-capture corpus, using the reconstructed `area=200` score mask and the
bidirectional modern-SAD normalized q field:

```text
median Pearson correlation  ~= 0.800
mean   Pearson correlation  ~= 0.794
median Spearman correlation ~= 0.959
mean   Spearman correlation ~= 0.948
```

The very high rank correlation is expected: both originate from the same
4x4 motion-displaced error family; normalized q additionally applies the
pair-luma normalization that Build #3 already uses.

Therefore adding `mask.area` as another authority term would mostly double-count
an existing Build #3 signal.

## Correction to Part 14

Part 14's **method description** was incomplete: its direct algorithm-13 replay
used the older 50/50/adaptive-phase temporal member and did not include the
4.3.0.165+ `area_blend=.4` nearest-frame-biased source mixture.

Part 14's **engineering conclusion survives** after correction:

> Direct SVP algorithm-13 synthesis should not be transplanted onto the current
> MPCVR repaired-splat/NVOF fields.

The corrected modern behavior is not a missing stabilizer. It tends to enlarge
coherent regions and therefore strengthens the reason to keep the frozen/golden
midpoint as the shock absorber in Build #3.

## Decision

No live Build #3 change is justified by `area_blend` or `mask.area`.

The remaining materially different SVP synthesis idea is algorithm 23, because
it introduces information Build #3 does not currently possess: motion from the
adjacent frame pairs. Exact corpus validation of algorithm 23 requires captures
that include those neighboring pair fields; the current 18-capture corpus only
contains the current A/B pair.
