# SVP4 interpolation archaeology — Part 22: grid-24 vector source

Date: 2026-09-06
Branch: `research/svp4-grid24-vector-source`

## Runtime result that invalidated Part 21 as a quality candidate

The Part 21 `SVP4-Faithful-Baseline` test compiled and passed its deterministic
integer/coverage audits, but runtime sampling across non-LOTR material showed
substantially more distortion than the earlier Build 3 experiments. It is a
failed quality candidate and must not be promoted.

That failure is consistent with the older Part 14/18 offline replay evidence:
direct proprietary algo13/algo21-style synthesis over MPCVR's full-resolution
NVOF field produced large coherent warped/artifact islands. Copying the final
SVP renderer while continuing to feed it a different motion field was therefore
not a faithful experiment.

## Newly proven local SVP configuration and vec_src geometry

The installed SVP Manager configuration provides the missing motion-field clue.
The persisted NVOF profiles in `%APPDATA%/SVP4/settings/profiles.cfg` request:

```text
nvof_grid = 24
nvof_quality = 2
```

Installed `SVP 4/script/generate.js` shows how that request is applied. For NVOF,
SVP crops the usable source extent to a multiple of the effective grid and
bicubic-resizes YUV420P8 to:

```text
vec_width  = floor(source_width  / grid) * 4
vec_height = floor(source_height / grid) * 4
```

For grid 24 this is a 1/6-scale vector source. NVIDIA still produces native 4x4
flow on that reduced image, so each returned flow cell corresponds to a 24x24
region of the full-resolution source.

Manager also lowers an oversized grid while either reduced field dimension
would violate the recovered 40x32-cell minimum. Starting from 24 the observed
sequence is:

```text
24 -> 16 -> 8 -> 4
```

Examples reproduced by the deterministic audit:

| source | effective grid | scale | vec_src | flow cells |
|---|---:|---:|---:|---:|
| 1920x1080 | 24 | 6 | 320x180 | 80x45 |
| 1918x803 | 24 | 6 | 316x132 | 79x33 |
| 1280x720 | 16 | 4 | 320x180 | 80x45 |
| 640x480 | 8 | 2 | 320x240 | 80x60 |
| 320x240 | 4 | 1 | 320x240 | 80x60 |

## Recovered reduced-source arithmetic now implemented

The proprietary reduced-source motion packing recovered in Part 12 is:

```text
packed = trunc(raw_S10.5 * scale * precision / 32)
precision = 4 when scale is 1 or 2
precision = 2 when scale is 4, 6, or 8
```

The renderer obtains full-resolution pixel motion by dividing the packed value
by `precision`. The grid-24 path therefore uses scale 6 and precision 2.

The modern software-confidence score is still measured on the native 4x4
`vec_src` luma block, but reduced-source modes apply the recovered compensation:

```text
score = SAD_4x4 * scale^2
```

The live research path now carries the effective grid/scale/precision through:

- NVOF input dimensions and flow allocation;
- software-SAD classification;
- reduced-source vector packing;
- effective-grid interpolation origin and signed truncating division;
- coverage scatter and block-area normalization;
- one-shot capture metadata.

The ordinary BGRA presentation surfaces remain full resolution.

## Proven versus inferred

Proven from the installed Manager script / recovered binary behavior:

- valid NVOF scales are 1, 2, 4, 6, 8;
- effective grid 24 means a 1/6 vector source;
- right/bottom source extent is cropped to a grid multiple before resize;
- grid fallback uses the 40x32 native-flow minimum;
- scale-6 vector precision is 2;
- software SAD is multiplied by scale squared.

Implemented equivalently and deterministically audited:

- grid selection/crop/vector-source dimensions for representative sizes;
- reduced-source integer vector packing;
- signed effective-grid interpolation for 4/8/16/24/32;
- existing classifier and coverage invariants.

Still approximate or deliberately held constant for this isolated test:

- MPCVR's D3D11 video processor performs the BGRA-to-reduced-NV12 conversion in
  one pass; it is not proven byte-identical to SVP's decoder-domain YUV420P8
  bicubic plane resize;
- final synthesis still samples MPCVR's processed BGRA presentation surfaces
  rather than synthesizing Y/UV source planes;
- this experiment retains the Part 21 requested-algo21/force13 control so the
  motion-field change can be evaluated in isolation. Installed Manager data also
  exposes algo13 profile/default paths; reproducing those together with their
  exact area-mask behavior is a separate fidelity step.

## Build 4 gate

This remains a research TEST, not Build 4. The next runtime gate is the diverse
corpus that exposed Part 21/Build 3 distortion, especially the non-LOTR files.
If grid-24 materially changes the failure class, that is evidence that vec_src
geometry was a major missing piece. If broad distortion remains, the next
highest-fidelity step is decoder/source-plane YUV synthesis plus the exact
requested-algorithm/mask path rather than another heuristic repair layer.
