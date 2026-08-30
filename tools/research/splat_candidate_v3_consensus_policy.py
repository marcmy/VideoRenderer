#!/usr/bin/env python3
"""Constrained Build #3 policy replay using dual-direction field consensus.

Research-only. This keeps the frozen MPCVR midpoint as the primary hypothesis
and the Part-12/13 modern 4x4 luma-SAD confidence as a local attenuation term.
It does *not* transplant SVP algorithm 13 directly.

Global authority is independent clean-room policy:
  * both B->A and A->B modern-SAD classifiers must be ordinary unhealthy
    classes 1 or 2;
  * authority ramps continuously with min(direction m1+ occupancy);
  * class-3/cut fields are disabled;
  * unusually large whole-frame source MAD suppresses the alternate hypothesis.

Defaults were selected from the restored archaeology corpus and are candidates
for further validation, not live renderer constants.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

import splat_candidate_v2_gate_matrix as base

SIGMA = 1.25
RADIUS = 3
ALPHA_CAP = 0.60
DEFAULT_QSCALE = 1600


def save_image(path: Path, image: np.ndarray) -> None:
    Image.fromarray(np.uint8(np.clip(image * 255.0 + 0.5, 0, 255))).save(path)


def process(
    capture: Path,
    out_dir: Path,
    qscale: int,
    occupancy_start: float,
    occupancy_full: float,
    mad_start: float,
    mad_stop: float,
    save: bool,
) -> dict[str, object]:
    capture = capture.resolve()
    name = capture.name
    a = base.load_rgb(capture, "frame-A.bmp")
    b = base.load_rgb(capture, "frame-B.bmp")
    golden = base.load_rgb(capture, "midpoint-current.bmp")
    temporal = 0.5 * (a + b)

    flow_ba = base.load_flow(capture, "flow-forward-B-to-A-s10.5.bin")
    flow_ab = base.load_flow(capture, "flow-backward-A-to-B-s10.5.bin")
    err_ba, err_ab = base.consistency_fields(flow_ba, flow_ab)

    a_y = base.y709(capture / "frame-A.bmp")
    b_y = base.y709(capture / "frame-B.bmp")
    raw_ba = base.load_raw(capture, "flow-forward-B-to-A-s10.5.bin")
    raw_ab = base.load_raw(capture, "flow-backward-A-to-B-s10.5.bin")
    score_ba, luma_b = base.dscore(b_y, a_y, raw_ba)
    score_ab, luma_a = base.dscore(a_y, b_y, raw_ab)
    pair_luma = base.pair_luma(luma_a, luma_b)
    q_ba = base.qnorm(score_ba, pair_luma)
    q_ab = base.qnorm(score_ab, pair_luma)
    class_ba, m1_ba, m2_ba, scene_ba = base.classify(score_ba, pair_luma)
    class_ab, m1_ab, m2_ab, scene_ab = base.classify(score_ab, pair_luma)

    corr, source_mad, cut = base.scene_stats(a, b)
    dual_m1 = min(m1_ba, m1_ab)
    ordinary_consensus = 0 < class_ba < 3 and 0 < class_ab < 3
    occupancy_gain = float(
        base.smoothstep(
            occupancy_start, occupancy_full, np.array(dual_m1, dtype=np.float32)
        )
    )
    mad_guard = float(
        1.0
        - base.smoothstep(mad_start, mad_stop, np.array(source_mad, dtype=np.float32))
    )
    global_gain = occupancy_gain * mad_guard if ordinary_consensus and not cut else 0.0

    height, width = a.shape[:2]
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    kernel_sum = sum(
        math.exp(-(x * x + y * y) / (2.0 * SIGMA * SIGMA))
        for y in range(-RADIUS, RADIUS + 1)
        for x in range(-RADIUS, RADIUS + 1)
    )

    # Keep the safe midpoint Adaptive Splat reconstruction from the previous
    # candidate; only its global authority policy changes here.
    disp_a, weight_a = base.splat_q(
        flow_ab, err_ab, q_ab, err_ab <= 20, SIGMA, RADIUS, qscale
    )
    disp_b, weight_b = base.splat_q(
        flow_ba, err_ba, q_ba, err_ba <= 20, SIGMA, RADIUS, qscale
    )
    cover_a = cv2.resize(np.clip(weight_a / kernel_sum, 0, 1), (width, height))
    cover_b = cv2.resize(np.clip(weight_b / kernel_sum, 0, 1), (width, height))
    disp_a = cv2.resize(disp_a, (width, height))
    disp_b = cv2.resize(disp_b, (width, height))
    warp_a = base.remap(a, xx + disp_a[..., 0], yy + disp_a[..., 1])
    warp_b = base.remap(b, xx + disp_b[..., 0], yy + disp_b[..., 1])

    wa = np.maximum(cover_a, 1.0e-8) ** 3
    wb = np.maximum(cover_b, 1.0e-8) ** 3
    denom = wa + wb
    splat = np.where(
        (denom > 1.0e-7)[..., None],
        (warp_a * wa[..., None] + warp_b * wb[..., None])
        / np.maximum(denom[..., None], 1.0e-7),
        temporal,
    )
    robust = np.median(np.stack([golden, splat, temporal], axis=0), axis=0).astype(
        np.float32
    )

    q_full = cv2.resize(0.5 * (q_ba + q_ab), (width, height))
    support = np.maximum(cover_a, cover_b)
    local = base.smoothstep(1000, 2200, q_full) * base.smoothstep(0.03, 0.22, support)
    local = cv2.GaussianBlur(local.astype(np.float32), (0, 0), 0.8)
    local = np.minimum(local, ALPHA_CAP)
    alpha = local * global_gain
    output = golden * (1.0 - alpha[..., None]) + robust * alpha[..., None]

    result: dict[str, object] = {
        "name": name,
        "BA": {
            "class": int(class_ba),
            "m1plus": float(m1_ba),
            "m2plus": float(m2_ba),
            "scene": float(scene_ba),
        },
        "AB": {
            "class": int(class_ab),
            "m1plus": float(m1_ab),
            "m2plus": float(m2_ab),
            "scene": float(scene_ab),
        },
        "dualM1": float(dual_m1),
        "ordinaryConsensus": bool(ordinary_consensus),
        "corr": float(corr),
        "srcMAD": float(source_mad),
        "cut": bool(cut),
        "occupancyGain": float(occupancy_gain),
        "madGuard": float(mad_guard),
        "globalGain": float(global_gain),
        "alphaMean": float(alpha.mean()),
        "alphaGt05": float(100.0 * np.mean(alpha > 0.05)),
        **base.metric(output, golden),
    }

    if save:
        out_dir.mkdir(parents=True, exist_ok=True)
        prefix = out_dir / name
        save_image(prefix.with_name(prefix.name + "_OUT.png"), output)
        save_image(
            prefix.with_name(prefix.name + "_DIFF_X32.png"),
            np.clip(np.abs(output - golden) * 32.0, 0, 1),
        )
        save_image(
            prefix.with_name(prefix.name + "_ALPHA.png"),
            np.repeat(alpha[..., None], 3, axis=2),
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--qscale", type=int, default=DEFAULT_QSCALE)
    parser.add_argument("--occupancy-start", type=float, default=20.0)
    parser.add_argument("--occupancy-full", type=float, default=27.0)
    parser.add_argument("--mad-start", type=float, default=0.075)
    parser.add_argument("--mad-stop", type=float, default=0.095)
    parser.add_argument("--out-dir", type=Path, default=Path("consensus-policy-replay"))
    parser.add_argument("--no-save", action="store_true")
    args = parser.parse_args()

    rows = [
        process(
            capture,
            args.out_dir,
            args.qscale,
            args.occupancy_start,
            args.occupancy_full,
            args.mad_start,
            args.mad_stop,
            not args.no_save,
        )
        for capture in args.captures
    ]
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "summary.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
    print(json.dumps(rows, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
