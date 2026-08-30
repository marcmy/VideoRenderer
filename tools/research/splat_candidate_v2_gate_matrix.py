#!/usr/bin/env python3
"""Compare global gates for the modern-SAD Adaptive Splat replay.

Research-only. This tool exists because the original Part 12 / Build #3 replay
used the B->A direction's class-1 result as its global gate. Later binary
archaeology showed that proprietary SVP's bidirectional scene classifier scores
the *current* A->B vector array while using both directions' luma bytes for the
normalization map.

The historical gate remains available explicitly for reproducibility, but is
not labeled as SVP-faithful. No live renderer code is modified by this script.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

from robust_independent_test import (
    consistency_fields,
    load_flow,
    load_rgb,
    smoothstep,
    to8,
)
from splat_inverse_prototype import remap, scene_stats
from splat_quality_weight_sweep import splat_q
from svp_exact_field_classifier import Thresholds, build_pair_luma, classify
from svp_modern_software_confidence import (
    load_bt709_limited_y,
    load_raw_flow,
    normalized_q,
    read_metadata,
    software_direction_score,
)

SIGMA = 1.25
RADIUS = 3
ALPHA_CAP = 0.60
DEFAULT_QSCALE = 1600

GATE_NAMES = (
    "historical-ba-class1",
    "svp-current-class1",
    "svp-current-class12",
    "both-class1",
    "either-class1",
    "local-only",
)


def metric(output: np.ndarray, golden: np.ndarray) -> dict[str, float]:
    diff = np.mean(np.abs(output - golden), axis=2)
    return {
        "mad": float(diff.mean()),
        "chg1": float(100.0 * np.mean(diff > 1.0 / 255.0)),
        "chg4": float(100.0 * np.mean(diff > 4.0 / 255.0)),
        "chg8": float(100.0 * np.mean(diff > 8.0 / 255.0)),
        "p99": float(np.quantile(diff, 0.99)),
    }


def gaussian_kernel_sum() -> float:
    denom = 2.0 * SIGMA * SIGMA
    return sum(
        math.exp(-(x * x + y * y) / denom)
        for y in range(-RADIUS, RADIUS + 1)
        for x in range(-RADIUS, RADIUS + 1)
    )


def modern_fields(capture: Path) -> tuple[np.ndarray, np.ndarray, dict[str, object]]:
    metadata = read_metadata(capture)
    grid_w = int(metadata["flow_width"])
    grid_h = int(metadata["flow_height"])

    a_y = load_bt709_limited_y(capture / "frame-A.bmp")
    b_y = load_bt709_limited_y(capture / "frame-B.bmp")
    ba_raw = load_raw_flow(
        capture, "flow-forward-B-to-A-s10.5.bin", grid_w, grid_h
    )
    ab_raw = load_raw_flow(
        capture, "flow-backward-A-to-B-s10.5.bin", grid_w, grid_h
    )

    # MPCVR capture naming:
    #   forward  = B -> A
    #   backward = A -> B
    # Proprietary/open-svpflow naming for the generated bidirectional payload:
    #   previous = B -> A
    #   current  = A -> B
    ba_score, ba_luma = software_direction_score(b_y, a_y, ba_raw)
    ab_score, ab_luma = software_direction_score(a_y, b_y, ab_raw)
    pair_luma = build_pair_luma(ba_luma, ab_luma, marker=3, gamma=1.5)

    ba_class = classify(ba_score, pair_luma, Thresholds())
    ab_class = classify(ab_score, pair_luma, Thresholds())
    return (
        normalized_q(ba_score, pair_luma).astype(np.float32),
        normalized_q(ab_score, pair_luma).astype(np.float32),
        {
            "historical_ba": ba_class.__dict__,
            "svp_current_ab": ab_class.__dict__,
            "direction_flags": 3,
            "pair_luma_denominator": 510,
        },
    )


def gate_enabled(name: str, ba_class: int, ab_class: int) -> bool:
    if name == "historical-ba-class1":
        return ba_class == 1
    if name == "svp-current-class1":
        return ab_class == 1
    if name == "svp-current-class12":
        return ab_class in (1, 2)
    if name == "both-class1":
        return ba_class == 1 and ab_class == 1
    if name == "either-class1":
        return ba_class == 1 or ab_class == 1
    if name == "local-only":
        return True
    raise ValueError(f"unknown gate: {name}")


def exposure_lift(image: np.ndarray, gamma: float = 0.45) -> np.ndarray:
    return np.power(np.clip(image, 0.0, 1.0), gamma)


def save_diagnostics(
    out_dir: Path,
    capture_name: str,
    gate: str,
    output: np.ndarray,
    golden: np.ndarray,
    alpha: np.ndarray,
) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    prefix = out_dir / f"{capture_name}_{gate}"
    Image.fromarray(to8(output)).save(prefix.with_name(prefix.name + "_OUT.png"))
    Image.fromarray(to8(np.repeat(alpha[..., None], 3, axis=2))).save(
        prefix.with_name(prefix.name + "_ALPHA.png")
    )

    diff = np.abs(output - golden)
    Image.fromarray(to8(np.clip(diff * 16.0, 0.0, 1.0))).save(
        prefix.with_name(prefix.name + "_DIFF_X16.png")
    )
    Image.fromarray(to8(np.clip(diff * 32.0, 0.0, 1.0))).save(
        prefix.with_name(prefix.name + "_DIFF_X32.png")
    )

    mean_diff = np.mean(diff, axis=2)
    ys, xs = np.nonzero(mean_diff > 4.0 / 255.0)
    if xs.size == 0:
        return
    x0 = max(0, int(xs.min()) - 64)
    x1 = min(golden.shape[1], int(xs.max()) + 65)
    y0 = max(0, int(ys.min()) - 64)
    y1 = min(golden.shape[0], int(ys.max()) + 65)

    gold_crop = golden[y0:y1, x0:x1]
    out_crop = output[y0:y1, x0:x1]
    diff_crop = diff[y0:y1, x0:x1]
    Image.fromarray(to8(exposure_lift(gold_crop))).save(
        prefix.with_name(prefix.name + "_CROP_GOLD_EXPOSED.png")
    )
    Image.fromarray(to8(exposure_lift(out_crop))).save(
        prefix.with_name(prefix.name + "_CROP_OUT_EXPOSED.png")
    )
    Image.fromarray(to8(np.clip(diff_crop * 32.0, 0.0, 1.0))).save(
        prefix.with_name(prefix.name + "_CROP_DIFF_X32.png")
    )


def process(
    capture: Path,
    qscale: int = DEFAULT_QSCALE,
    gates: tuple[str, ...] = GATE_NAMES,
    out_dir: Path | None = None,
) -> dict[str, object]:
    capture = capture.resolve()
    name = capture.name
    a = load_rgb(capture, "frame-A.bmp")
    b = load_rgb(capture, "frame-B.bmp")
    golden = load_rgb(capture, "midpoint-current.bmp")
    temporal = 0.5 * (a + b)

    ba_flow = load_flow(capture, "flow-forward-B-to-A-s10.5.bin")
    ab_flow = load_flow(capture, "flow-backward-A-to-B-s10.5.bin")
    ba_err, ab_err = consistency_fields(ba_flow, ab_flow)
    ba_q, ab_q, field = modern_fields(capture)

    ba_class = int(field["historical_ba"]["value"])
    ab_class = int(field["svp_current_ab"]["value"])
    corr, source_mad, cut = scene_stats(a, b)

    height, width = a.shape[:2]
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    kernel_sum = gaussian_kernel_sum()

    # A-side hypothesis is carried by A->B flow; B-side by B->A flow.
    da, wa = splat_q(
        ab_flow, ab_err, ab_q, ab_err <= 20, SIGMA, RADIUS, qscale
    )
    db, wb = splat_q(
        ba_flow, ba_err, ba_q, ba_err <= 20, SIGMA, RADIUS, qscale
    )
    ca = cv2.resize(np.clip(wa / kernel_sum, 0.0, 1.0), (width, height))
    cb = cv2.resize(np.clip(wb / kernel_sum, 0.0, 1.0), (width, height))
    da_full = cv2.resize(da, (width, height))
    db_full = cv2.resize(db, (width, height))

    aw = remap(a, xx + da_full[..., 0], yy + da_full[..., 1])
    bw = remap(b, xx + db_full[..., 0], yy + db_full[..., 1])
    aa = np.maximum(ca, 1.0e-8) ** 3
    bb = np.maximum(cb, 1.0e-8) ** 3
    denom = aa + bb
    splat = np.where(
        (denom > 1.0e-7)[..., None],
        (aw * aa[..., None] + bw * bb[..., None])
        / np.maximum(denom[..., None], 1.0e-7),
        temporal,
    )
    robust = np.median(np.stack([golden, splat, temporal], axis=0), axis=0).astype(
        np.float32
    )

    q_full = cv2.resize(0.5 * (ba_q + ab_q), (width, height))
    support = np.maximum(ca, cb)
    local = smoothstep(1000, 2200, q_full) * smoothstep(0.03, 0.22, support)
    local = cv2.GaussianBlur(local.astype(np.float32), (0, 0), 0.8)
    local = np.minimum(local, ALPHA_CAP)

    result: dict[str, object] = {
        "name": name,
        "qscale": qscale,
        "cut": bool(cut),
        "corr": float(corr),
        "srcMAD": float(source_mad),
        "field": field,
        "gates": {},
    }
    for gate in gates:
        enabled = gate_enabled(gate, ba_class, ab_class) and not cut
        alpha = local if enabled else np.zeros_like(local)
        output = golden * (1.0 - alpha[..., None]) + robust * alpha[..., None]
        result["gates"][gate] = {
            "enabled": bool(enabled),
            **metric(output, golden),
            "alphaMean": float(alpha.mean()),
            "alphaGt05": float(100.0 * np.mean(alpha > 0.05)),
        }
        if out_dir is not None and gate in (
            "historical-ba-class1",
            "svp-current-class1",
            "svp-current-class12",
        ):
            save_diagnostics(out_dir, name, gate, output, golden, alpha)

    print(json.dumps(result), flush=True)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--qscale", type=int, default=DEFAULT_QSCALE)
    parser.add_argument(
        "--gates",
        default=",".join(GATE_NAMES),
        help="comma-separated gate names",
    )
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--json", type=Path, default=None)
    args = parser.parse_args()

    gates = tuple(item.strip() for item in args.gates.split(",") if item.strip())
    unknown = sorted(set(gates) - set(GATE_NAMES))
    if unknown:
        parser.error(f"unknown gate(s): {', '.join(unknown)}")

    rows = [
        process(capture, qscale=args.qscale, gates=gates, out_dir=args.out_dir)
        for capture in args.captures
    ]
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(rows, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
