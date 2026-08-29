# SVP4 interpolation archaeology / native NVOF research log

Date: 2026-08-29
Branch: `research/svp4-interpolation-archaeology`
Frozen live baseline: `baseline/nvof-temporal-motion-salvage` @ `54720e00b65dc698130430db6d2a86dc41237a91`

This file is the durable handoff / lab notebook for the ongoing native-NVOF interpolation work. Keep the frozen baseline untouched. Experimental live builds must branch from the baseline or from a clearly named experiment branch; do not merge PR #25 while this work is ongoing.

## 1. Golden baseline architecture

Native NVIDIA Optical Flow:

- NVOF API 5.0, D3D11
- BGRA8 input
- `R16G16_SINT` motion vectors, S10.5 (`int16 / 32`)
- 4x4 native flow grid
- bidirectional flow: forward = B->A, backward = A->B
- NVOF `PerfSlow`
- temporal hints deliberately disabled per pair
- hardware cost output disabled in live renderer

Current golden synthesis lineage:

1. coarse bidirectional NVOF validation;
2. native B->A seed when valid;
3. asymmetric `-A->B` seed when only reverse direction validates;
4. neither direction valid => unsupported unless strict raw-forward salvage passes;
5. JFA nearest-seed propagation;
6. edge-aware full-resolution dense flow;
7. inverse warp;
8. local repair field;
9. photometric + topology guards;
10. both-directions-invalid local temporal fallback;
11. scene-cut early return.

Strict raw-forward salvage currently requires:

- neither round-trip direction passes the normal 20 px threshold;
- mapped B->A endpoint in bounds;
- endpoint RGB MAD <= 0.025;
- 5-point local source-frame temporal RGB MAD >= 0.030;
- local B->midpoint Jacobian approximately rigid: determinant 0.75..1.25, sigmaMin > 0.75, sigmaMax < 1.25.

Dense/JFA production constants recovered from source:

- `GridSize = 4.0`
- `SpatialSigma = 1.25`
- `ColorSigma = 0.10`
- `InfillSigma = 8.0`
- JFA starts at the largest power of two below the max flow dimension and halves to 1.

Repair constants:

- `FlowSigma = 1.25`
- `MaskSigma = 1.0`
- `FlowRadius = 2`
- `MaskRadius = 4`
- `MaskDilation = 1.5`

Known unresolved baseline concern: dense upsample still falls back to raw B->A flow if all weighted validated/JFA candidates collapse (`weightSum <= 1e-5`). This was pathological in an older Bilbo capture (~983 pixels) but much rarer in newer captures. Do not casually change it in the golden branch.

## 2. Known failed approaches — do not repeat blindly

Rejected or dangerous experiments:

- global whole-frame repeat at 25% bad cells;
- global 25-40% safety crossfade;
- regional 7x7 hard frame reject;
- naive global bad-percentage presentation gate;
- hardware NVOF cost used directly to choose/modify synthesis motion;
- temporal hints as presumed solution;
- simple motion-disagreement global gate;
- aggressive topology/Jacobian rejection as the primary solution;
- same-coordinate reverse-flow negation treated as true geometric inversion;
- use-all-raw-B->A seeds;
- naive dual raw inverse warp;
- simple global affine motion;
- direct coverage ownership => trust bad raw vector;
- broad independent-JFA algorithm-13 proxy: enough authority, but creates chunky/block geometry;
- anchored `median(golden, alternate, temporal)` restricted only to existing unsupported core: safe, but too conservative on the main Boromir failures.

## 3. Capture corpus facts

### Bilbo / older pathological pair

- both-directions-invalid ~1%
- catastrophic ~4-5%
- large coherent actor island caused visible face/body tearing
- current local repair/fallback architecture improved it substantially
- must be treated as an important non-regression case

### Boromir fling — `MPCVR-NVOF-Captures2.zip`

`022518`
- both invalid ~12.14%
- catastrophic ~40.73%
- visually less severe than the three later captures

`022530`
- both invalid ~37.92%
- catastrophic ~52.29%
- largest unsupported component ~21.80%
- same-shot extreme motion, not a cut

`022539`
- both invalid ~41.13%
- catastrophic ~58.57%
- largest unsupported component ~28.34%
- same-shot extreme motion, not a cut

`022550`
- both invalid ~18.82%
- catastrophic ~52.81%

Important inference: simply raising the 20 px consistency threshold is wrong. In the worst unsupported regions round-trip error can exceed 200 px while raw endpoint color agreement remains deceptively good because motion blur makes incorrect surfaces look photometrically plausible.

## 4. SVP4 reverse-engineering findings

User supplied an installed SVP4 directory snapshot (`SVP 4.zip`, RIFE folder omitted). Clean-room goal: extract architecture and heuristics; do not copy proprietary implementation text verbatim.

### 4.1 NVOF preset

SVP's `nvof.q=2` maps to the same highest-quality/SLOW NVOF preset already used by our baseline. Remaining quality differences are primarily reconstruction, visibility, artifact masking, temporal context, and heuristics after optical flow.

### 4.2 Coverage / ownership

SVP projects motion-block footprints toward the intermediate time and builds directional coverage separately for each endpoint.

Recovered characteristics:

- tolerant local 3x3-style accumulated coverage rather than a brittle binary hole test;
- normalized approximately with an internal `/8` behavior;
- directional correction strength capped around ~80%.

Critical interpretation: coverage is ownership evidence, not permission to resurrect a vector that failed validation.

### 4.3 Artifact / vector confidence

SVP associates vectors with error/quality and uses that as soft artifact-mask input. Architecturally the mask behaves like a nonlinear function of error, block area, artifact-mask strength and sharpness.

On the NVOF path, native hardware cost can be adapted into that confidence concept. In our renderer, live cost output remains disabled because prior Turing testing caused severe startup/seek and sustained-playback stalls. Treat hardware cost only as a possible offline oracle, not a live dependency.

### 4.4 Synthesis families

Recovered high-level semantics from embedded OpenCL / runtime dispatch:

- algorithm 11: simple bidirectional motion-compensated synthesis;
- algorithm 13: robust / dynamic median;
- algorithm 21: cover/uncover-aware synthesis;
- algorithm 22: cover/uncover + robust median family;
- algorithm 23: cover/uncover + additional motion hypotheses from neighboring frame pairs.

Algorithm 13 constructs three hypotheses:

1. forward motion-compensated candidate;
2. backward motion-compensated candidate;
3. safe/original-frame temporal candidate;

and applies a channelwise median.

This is attractive because a grotesquely wrong warp often becomes an intensity outlier that the median can reject without explicitly understanding the failure.

### 4.5 `force13`, `blocks13`, field classifier

Recovered defaults from the supplied install:

- `scene.adaptive = 210`
- `scene.force13 = true`
- `blocks = 20`
- `blocks13 = 0`
- `ignore = 4%`
- severity thresholds: `zero=200`, `m1=1600`, `m2=2800`, `scene=4000`
- luma exponent/default `1.5`

Classifier behavior recovered from `svpflow2.dll`:

- ignore the outer border;
- compute a luminance-normalized per-vector matching error;
- count severity-band occupancy;
- normal bad-field decision uses `blocks` occupancy;
- `blocks13` is a second lower occupancy threshold that can yield a special robust-only state when explicitly enabled;
- the raw special state is carried out-of-band while the ordinary quality class is clamped to 0;
- `force13=true` causes algorithm 13 when either the special `blocks13` state is active or the normal quality class is >0;
- for an otherwise selected interpolation algorithm >=11, the robust flag literally substitutes algorithm 13 for that synthesized frame;
- `scene.adaptive=210` is a separate per-quality behavior mapping, not the same switch as force-to-13.

Important correction versus early speculation: stock `blocks13=0` means the special lower threshold is disabled by default. Ordinary stock `force13=true` is therefore driven by the normal `blocks=20` classifier unless a profile overrides `blocks13`.

## 5. Clean-room field-quality classifier

Goal: reproduce the *architecture* using our own image-domain signal rather than proprietary matching cost.

For each 4x4 NVOF cell at representative source pixel `p`:

- B->A hypothesis compares `B(p)` with `A(p + F_BA)`;
- A->B hypothesis compares `A(p)` with `B(p + F_AB)`;
- use luminance Y;
- scale one representative sample to a 4x4-equivalent SAD;
- normalize by local source luminance.

Practical clean-room score used in replay:

```text
q_BA = 16 * 255 * abs(Y_B(p) - Y_A(p + F_BA)) / max(Y_B(p), 1/255)
q_AB = 16 * 255 * abs(Y_A(p) - Y_B(p + F_AB)) / max(Y_A(p), 1/255)
q    = 0.5 * (q_BA + q_AB)
```

Ignore approximately the outer 4% of the flow field. `q >= 1600` is the m1-or-worse population. Trigger suspicious-field mode when >=20% of interior cells are m1-or-worse.

A full 4x4 block SAD was tested first. A single representative sample per direction produces nearly identical occupancy, so a live implementation can be very cheap.

Representative measured occupancy:

| Pair | m1+ occupancy | Result |
| --- | ---: | --- |
| Bilbo | ~0.7% | good / local-only |
| Boromir 022518 | ~7.6% | good / local-only |
| Boromir 022530 | ~33-36% | suspicious |
| Boromir 022539 | ~22-28% | suspicious |
| Boromir 022550 | ~20-21% | suspicious, near boundary |

This is materially different from catastrophic-flow percentage. Example: `022518` has roughly 40% catastrophic flow cells but only ~7.6% photometrically bad cells, so the new field-health classifier correctly leaves it on the golden path.

On the older Chamber corpus, clean close-ups remain around ~8-13%, while the visually nasty smeared/warped same-shot pairs land around ~28-35%. Actual cuts also score high but are intercepted first by the independent scene-cut rule (`correlation < 0.15 && MAD > 0.055`).

## 6. Exact offline golden replay

The current CPU/offline harness reproduces:

- golden Seed validation policy including temporal-motion-gated forward salvage;
- JFA seed propagation and tie-breaking;
- dense 5x5 edge-aware upsample using production constants;
- repair field / feathered catastrophic and unsupported masks;
- inverse Warp behavior and local fallback.

Validation on a Boromir captured midpoint reaches approximately **0.22 RGB LSB/channel average difference** versus the actual renderer capture. This is accurate enough for pixel-level A/B experimentation.

## 7. Dynamic-median experiments

### 7.1 Broad independent A/B JFA median — rejected

Proxy for global algorithm 13:

```text
median(A-side motion-compensated hypothesis,
       B-side motion-compensated hypothesis,
       temporal reference)
```

It suppresses long streaks but can replace them with chunky / block-like geometry. Our independently reconstructed directional hypotheses are not strong enough to grant global authority the way SVP can.

### 7.2 Anchored median — safe but too weak

```text
median(golden output,
       independent alternate candidate,
       temporal reference)
```

Restricted to golden unsupported/risk regions, this is robust to grotesque alternate warps but changes too little on the main Boromir failures because golden is already near temporal fallback in much of the unsupported core.

### 7.3 COV-FB55 live experiment

A separate live experiment was built to test directional ownership in the safest possible location: pixels already near golden temporal fallback. It preserves golden motion geometry and biases the fallback only mildly (max 55/45 instead of 50/50) according to directional coverage.

Purpose: isolate whether ownership evidence is useful. Do not confuse its result with the field-quality / dynamic-median work; these answer different questions.

## 8. New promising direction: valid-vector midpoint inverse-flow splat

Current strongest offline candidate.

Motivation: the remaining Boromir failure resembles a bad invented inverse mapping inside unsupported islands. JFA necessarily propagates a nearest trustworthy seed through a hole and the dense upsampler then constructs motion there. Instead, project only validated motion support into midpoint space.

Prototype:

1. take consistency-valid native motion vectors;
2. project each valid vector footprint toward the intermediate time;
3. Gaussian-splat target->source displacement / support into an intermediate inverse-flow map;
4. normalize by accumulated support;
5. backward-warp the real frames through the splatted midpoint map;
6. use that reconstruction as an *alternate hypothesis*, not as unconditional output;
7. combine through a robust channelwise median with golden and safe temporal reference.

Raw splat reconstruction alone is not acceptable: it can contain bright ownership/splat outliers.

Promising formulation:

```text
median(
    golden renderer hypothesis,
    valid-vector midpoint-splat hypothesis,
    safe temporal reference
)
```

Observed behavior so far:

- visibly reduces some of the large Boromir stretched/rubbery streaks;
- robust median rejects the worst raw splat outliers;
- does not show the same block/chunky geometry seen in the independent-JFA median;
- changes enough pixels to matter on force13-class fields instead of becoming a microscopic patch.

Approximate >1 LSB modification rates versus golden for one `MED20` prototype:

- Boromir 022530: ~26.7%
- Boromir 022539: ~22.6%
- Boromir 022550: ~16.0%
- older same-shot difficult 000820: ~13.1%
- older difficult 001431: ~18.3%
- Bilbo: exact no-op because field gate does not open
- known cuts: exact no-op because scene-cut handling wins first

Restricting splat-median only to the golden unsupported mask is again too conservative (~1% meaningful changes on the worst Boromir frames). This supports the same architectural idea as SVP `force13`: once the field classifier says the field is globally in a bad reconstruction regime, the *robust reconstruction operator* may need broader authority while local confidence still controls how much the alternate hypothesis contributes.

Initial splat footprint sweep (roughly 3x3 through 7x7, sigma ~0.6..1.5) is surprisingly insensitive: the result appears to depend more on the representation change (project trustworthy support into midpoint space) than on a lucky Gaussian parameter.

## 9. Current working architecture

```text
scene cut?
  yes -> existing scene-cut hold / no interpolation
  no
   |
   v
clean-room field-health classifier
  <20% m1+ -> golden baseline unchanged
  >=20% m1+ -> robust-field mode
                    |
                    v
        build alternate midpoint hypothesis
        from trustworthy projected support
                    |
                    v
        channelwise robust median with
        golden + safe temporal reference
```

Important separation of questions:

1. Is this vector trustworthy?
2. Which endpoint geometrically owns/covers the intermediate pixel?
3. Is the entire field entering a regime where ordinary synthesis is unreliable?
4. Which robust reconstruction operator should be used in that regime?

Do not collapse those questions into one threshold or one mask.

## 10. Next research tasks

1. Finish cross-corpus parameter sweep for midpoint splat footprint, support normalization and confidence.
2. Determine a confidence rule that prevents broad low-value changes while retaining Boromir streak reduction.
3. Compare symmetric A/B splat reconstruction with ownership-weighted A/B contribution.
4. Test temporally biased safe reference / `area_blend` style fallback rather than assuming 50/50 A+B is always optimal.
5. Quantify local changes on Bilbo, clean ordinary pairs, severe same-shot motion, and cuts.
6. If a clear cross-corpus win remains, create a new isolated live-test branch from the frozen baseline and build it; do not mutate the baseline branch.
7. Keep this document updated after each meaningful result so a future conversation can resume from GitHub alone.

## 11. Current live experiment status

COV-FB55 test build exists separately. User is testing it while research continues. It is not yet the preferred final architecture; it is an ownership-isolation experiment.

Current research preference for a possible next build, subject to further replay validation:

**20% clean-room field-health gate + valid-vector midpoint inverse-flow splat + robust median with golden and safe temporal reference.**
