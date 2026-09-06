# SVP4 Interpolation Archaeology — Part 21: Faithful Native-NVOF Baseline

Date: 2026-09-06

Status: research/test only. Build #3 remains untouched at
`build3/nvof-modern-sad-consensus-latest-master` commit
`4d5d855ba863b156bba11e0973f0e84fc88c13f2`.

## Motivation

Build #3 removed the original startup hitch, but diverse runtime samples exposed
substantial interpolation distortion outside the earlier Lord of the Rings test
set. Its quality heuristics are therefore no longer a candidate foundation for
Build #4.

This branch starts again from the recovered SVP4 semantics. The goal is to keep
the native NVIDIA Optical Flow source while copying SVP's input, classifier,
control, coverage, and renderer rules before adding any MPCVR-specific repair.

## Live research baseline

The native path now uses:

- BGRA presentation surfaces kept separately from the optical-flow surfaces;
- D3D11 video-processor conversion to NV12 before optical flow;
- NVOF on NV12, 4x4 output grid, PerfSlow, bidirectional prediction, temporal
  hints disabled;
- no live NVOF cost surface, because the RTX 2070 SUPER/Turing cost path had
  previously stalled badly;
- the recovered 4x4 Y software-SAD classifier with pair-luma normalization,
  gamma 1.5, and the proprietary thresholds;
- requested algorithm 21 with `scene.force13=true` and adaptive 210 behavior;
- direct SVP-style 4x4 motion interpolation and coverage masks instead of the
  Build #3 dense/JFA/repair/topology pipeline.

At MPCVR's exact 2x midpoint, the recovered control path is:

```text
C0 -> algo21, phase 128
C1 -> algo13, phase 128
C2 -> algo13, phase 64
C3 -> cut/source selection
```

## Proven motion semantics

The NVOF SHORT2 field is S10.5. The generated source-8-bit vector payload packs
NVOF motion as `raw / 8` with marker 4; the renderer then divides by marker 4,
producing the pixel displacement `trunc(raw / 32)` before temporal phase.

Direction wiring was checked against the reconstruction: the two live NVOF
outputs are used as the recovered backward/forward pair expected by the
renderer. A simple swapped-field explanation for the old distortion does not
survive this audit.

### Signed block-4 interpolation correction

A fidelity bug was found in the first faithful-baseline shader. Its block-4
motion interpolation used arithmetic right shift:

```text
sum >> 2
```

SVP has `CPU_MAP[4] == -1` for this path and therefore performs signed integer
division:

```text
sum / 4
```

The distinction matters for negative motion. C++/HLSL signed division truncates
toward zero, while arithmetic right shift rounds negative values downward.

Across 200,000 randomized signed interpolation samples, the old shader differed
from the recovered SVP rule in 50,074 cases (25.037%), always by the expected
one-pixel rounding bias. The live shader now uses `/ 4`, and the generated
bytecode was regenerated.

## Coverage-border audit

SVP's CPU reconstruction uses a padded `(grid_w + 2) x (grid_h + 2)`
accumulator, followed by horizontal and vertical three-cell sums. The GPU
prototype stores only the real grid and directly sums an in-bounds centered
3x3 neighborhood.

Those forms were compared over 1,000 randomized grids, phases, and signed
motion fields. Their accumulated coverage values were identical, including at
all borders. The compact GPU representation is therefore retained; padding is
an implementation detail here rather than a semantic difference.

## Deterministic audit

`tools/research/svp_faithful_baseline_audit.py` currently checks:

- 1,000 randomized proprietary classifier fields;
- the complete 0..510 pair-luma LUT domain for denominator 510 / gamma 1.5;
- 100,000 randomized signed NVOF vector/phase cases;
- signed block-4 motion interpolation;
- Blend255/Blend256 arithmetic and cut midpoint selection;
- 1,000 randomized coverage-grid equivalence cases.

Current result:

```json
{
  "classifier_cases": 1000,
  "coverage_cases": 1000,
  "seed": 1398165556,
  "status": "pass",
  "vector_cases": 100000
}
```

## Build evidence

After regenerating all baseline HLSL bytecode:

- Release x64: passed, zero compiler warnings/errors;
- Release x86: passed, zero compiler warnings/errors;
- `git diff --check`: passed.

This is compile/static evidence only. It does not establish interpolation
quality or even prove that every D3D11 NV12 view/conversion combination succeeds
on the target RTX 2070 SUPER driver at runtime.

## Remaining fidelity boundary

SVP renders decoder/source YUV planes. MPCVR reaches this research path after
its normal processing has produced BGRA frames. The branch converts those BGRA
frames to NV12 so NVOF and the classifier see the same luma representation, but
the final algo13/algo21 warp still samples the processed BGRA presentation
surfaces.

That means motion geometry and control are now close to the recovered SVP path,
while the final per-channel synthesis domain is not yet source-plane YUV.
If the diverse runtime corpus still shows coherent distortion after the signed
interpolation correction, the next baseline should move synthesis itself to Y
and chroma planes and convert the completed synthetic frame back to MPCVR's
presentation format afterward.

## Algorithm 23

Algorithm 23 is intentionally deferred from this baseline. Its adjacent fields
have been resolved as:

```text
prev1 = P -> A
next0 = C -> B
```

The historical 18-capture corpus does not contain those adjacent fields, so it
cannot exact-replay algorithm 23. The adjacent-capture research branch remains
the route to a valid corpus once the faithful 13/21 baseline is runtime-sane.

## Build #4 gate

There is no Build #4 candidate yet.

The next artifact is a TEST baseline and must be exercised on the non-LOTR
files that exposed Build #3, plus materially different resolutions, codecs,
frame rates, and motion patterns. Promotion is justified only if quality is
consistently good across that diverse corpus. A success on the original LOTR
samples alone is insufficient.
