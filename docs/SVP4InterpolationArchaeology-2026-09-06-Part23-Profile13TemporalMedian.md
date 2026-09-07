# SVP4 interpolation archaeology — Part 23: active profile 13 and temporal median correction

Date: 2026-09-06
Branch: `research/svp4-grid24-profile13`

## Runtime symptom

The first grid-24 vector-source test presented at the expected doubled cadence in MPC-HC statistics, but visually looked close to the source frame rate. That symptom exposed a synthesis/control mismatch left over from the Part 21 experiment rather than a presentation-rate failure.

## Proven active-profile mismatch

The installed SVP profile used for this configuration stores:

```text
fi_shader = 13
fi_adaptive = 210
fi_mode = 4
fi_masking = 100
nvof_grid = 24
nvof_quality = 2
```

The first grid-24 test deliberately retained the earlier requested-algorithm-21/force13 control path to isolate vector-source geometry. It therefore was not yet matching the active SVP profile's requested shader.

The new control path requests algorithm 13 for ordinary classes C0/C1/C2, retains the recovered adaptive phases `128/128/64`, and keeps C3 on the separate cut/fallback path.

## Exact algorithm-13 correction

The recovered algorithm-13 combination is the channelwise median of:

```text
warpA
warpB
temporal = blend256(A, B, phase)
```

Equivalently:

```text
out13 = clamp(temporal, min(warpA, warpB), max(warpA, warpB))
```

The previous live shader incorrectly applied a separate mask-like phase transform to the unwarped temporal hypothesis:

```text
phase 128 -> temporal weight 0.80 instead of 0.50
phase  64 -> temporal weight 0.10 instead of 0.25
```

That pushes synthetic frames much closer to a real endpoint and can make a true doubled presentation cadence look visually close to the original frame rate.

The corrected shader now uses the actual recovered phase directly before the median/clamp.

## Validation boundary

The deterministic audit now checks the active profile control map and 10,000 randomized channelwise algorithm-13 median identities in addition to the existing classifier, grid geometry, packed-motion, and coverage checks. Release x64 and x86 both compile successfully.

This remains a research test. The important runtime gate is the same diverse non-LOTR material that exposed the earlier distortion and cadence problems. If the Ctrl+J frame-interpolation telemetry repeatedly reports `C3, algo0`, then the next issue is classifier/source-domain fidelity rather than presentation timing or algorithm-13 temporal weighting.
