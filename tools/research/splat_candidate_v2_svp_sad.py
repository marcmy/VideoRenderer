#!/usr/bin/env python3
"""Adaptive Splat Build #3 replay using SVP-style modern software confidence.

This is research-only. It does not modify the live renderer or frozen baseline.

The candidate keeps the V1.2 reconstruction shape but replaces the old
photometric q proxy with the reconstructed modern SVP 4x4 luma-SAD confidence.
The exact SVP classifier gates robust reconstruction only for class-1 fields.
A qscale sweep around the observed knee is emitted for corpus comparison.

For qscale=1600 the tool also writes amplified-difference and exposure-lifted
crops around >4-LSB changes, intended especially for the 013416 safety check.
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


DEFAULT_QSCALES = (1200, 1600, 2000, 2400)
SIGMA = 1.25
RADIUS = 3
ALPHA_CAP = 0.60


def metric(output: np.ndarray, golden: np.ndarray) -> dict[str, float]:
    diff = np.mean(np.abs(output - golden), axis=2)
    return {
        "mad": float(diff.mean()),
        "chg1": float(100.0 * np.mean(diff > 1.0 / 255.0)),
        "chg4": float(100.0 * np.mean(diff > 4.0 / 255.0)),
        "chg8": float(100.0 * np.mean(diff > 8.0 / 255.0)),
        "p99": float(np.quantile(diff, 0.99)),
    }


def modern_q_fields(capture: Path) -> tuple[np.ndarray, np.ndarray, dict[str, object]]:
    metadata = read_metadata(capture)
    grid_w = int(metadata["flow_width"])
    grid_h = int(metadata["flow_height"])

    a_y = load_bt709_limited_y(capture / "frame-A.bmp")
    b_y = load_bt709_limited_y(capture / "frame-B.bmp")
    forward_raw = load_raw_flow(
        capture, "flow-forward-B-to-A-s10.5.bin", grid_w, grid_h
    )
    backward_raw = load_raw_flow(
        capture, "flow-backward-A-to-B-s10.5.bin", grid_w, grid_h
    )

    forward_score, forward_luma = software_direction_score(b_y, a_y, forward_raw)
    backward_score, backward_luma = software_direction_score(a_y, b_y, backward_raw)
    pair_luma = build_pair_luma(backward_luma, forward_luma, marker=3, gamma=1.5)

    forward_q = normalized_q(forward_score, pair_luma).astype(np.float32)
    backward_q = normalized_q(backward_score, pair_luma).astype(np.float32)
    forward_class = classify(forward_score, pair_luma, Thresholds())
    backward_class = classify(backward_score, pair_luma, Thresholds())

    info = {
        "direction_flags": 3,
        "pair_luma_denominator": 510,
        "forward_class": forward_class.__dict__,
        "backward_class": backward_class.__dict__,
    }
    return forward_q, backward_q, info


def gaussian_kernel_sum(sigma: float, radius: int) -> float:
    denom = 2.0 * sigma * sigma
    return sum(
        math.exp(-(x * x + y * y) / denom)
        for y in range(-radius, radius + 1)
        for x in range(-radius, radius + 1)
    )


def exposure_lift(image: np.ndarray, gamma: float = 0.45) -> np.ndarray:
    return np.clip(np.power(np.clip(image, 0.0, 1.0), gamma), 0.0, 1.0)


def change_bbox(
    output: np.ndarray, golden: np.ndarray, threshold_lsb: float = 4.0, pad: int = 64
) -> tuple[int, int, int, int] | None:
    diff = np.mean(np.abs(output - golden), axis=2)
    ys, xs = np.nonzero(diff > threshold_lsb / 255.0)
    if xs.size == 0:
        return None
    height, width = diff.shape
    x0 = max(0, int(xs.min()) - pad)
    x1 = min(width, int(xs.max()) + pad + 1)
    y0 = max(0, int(ys.min()) - pad)
    y1 = min(height, int(ys.max()) + pad + 1)
    return x0, y0, x1, y1


def save_diagnostics(
    out_dir: Path,
    name: str,
    qscale: int,
    output: np.ndarray,
    golden: np.ndarray,
    alpha: np.ndarray,
) -> dict[str, object]:
    prefix = out_dir / f"{name}_QS{qscale}"
    Image.fromarray(to8(output)).save(prefix.with_name(prefix.name + "_OUT.png"))
    Image.fromarray(to8(golden)).save(prefix.with_name(prefix.name + "_GOLD.png"))

    diff = np.abs(output - golden)
    Image.fromarray(to8(np.clip(diff * 8.0, 0.0, 1.0))).save(
        prefix.with_name(prefix.name + "_DIFF_X8.png")
    )
    Image.fromarray(to8(np.clip(diff * 16.0, 0.0, 1.0))).save(
        prefix.with_name(prefix.name + "_DIFF_X16.png")
    )
    Image.fromarray(to8(np.repeat(alpha[..., None], 3, axis=2))).save(
        prefix.with_name(prefix.name + "_ALPHA.png")
    )

    bbox = change_bbox(output, golden)
    result: dict[str, object] = {"bbox_gt4": bbox}
    if bbox is None:
        return result

    x0, y0, x1, y1 = bbox
    gold_crop = golden[y0:y1, x0:x1]
    out_crop = output[y0:y1, x0:x1]
    diff_crop = diff[y0:y1, x0:x1]

    Image.fromarray(to8(gold_crop)).save(
        prefix.with_name(prefix.name + "_CROP_GOLD.png")
    )
    Image.fromarray(to8(out_crop)).save(
        prefix.with_name(prefix.name + "_CROP_OUT.png")
    )
    Image.fromarray(to8(exposure_lift(gold_crop))).save(
        prefix.with_name(prefix.name + "_CROP_GOLD_EXPOSED.png")
    )
    Image.fromarray(to8(exposure_lift(out_crop))).save(
        prefix.with_name(prefix.name + "_CROP_OUT_EXPOSED.png")
    )
    Image.fromarray(to8(np.clip(diff_crop * 16.0, 0.0, 1.0))).save(
        prefix.with_name(prefix.name + "_CROP_DIFF_X16.png")
    )
    Image.fromarray(to8(np.clip(diff_crop * 32.0, 0.0, 1.0))).save(
        prefix.with_name(prefix.name + "_CROP_DIFF_X32.png")
    )
    return result


def process(
    capture: Path,
    qscales: tuple[int, ...] = DEFAULT_QSCALES,
    save: bool = True,
    out_dir: Path | None = None,
) -> dict[str, object]:
    capture = capture.resolve()
    name = capture.name
    if out_dir is None:
        out_dir = Path("/mnt/data/vr_work/splat_candidate_v2_svp_sad")
    out_dir.mkdir(parents=True, exist_ok=True)

    a = load_rgb(capture, "frame-A.bmp")
    b = load_rgb(capture, "frame-B.bmp")
    golden = load_rgb(capture, "midpoint-current.bmp")
    temporal = 0.5 * (a + b)
    forward = load_flow(capture, "flow-forward-B-to-A-s10.5.bin")
    backward = load_flow(capture, "flow-backward-A-to-B-s10.5.bin")
    forward_err, backward_err = consistency_fields(forward, backward)
    forward_q, backward_q, field_info = modern_q_fields(capture)

    corr, source_mad, cut = scene_stats(a, b)
    height, width = a.shape[:2]
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    kernel_sum = gaussian_kernel_sum(SIGMA, RADIUS)

    # The previous corpus replay that produced Part 12's single SVP class used
    # the current B->A direction. Keep both results in telemetry, but use that
    # direction for the Build #3 gate so the committed replay matches the study.
    field_class = int(field_info["forward_class"]["value"])
    active = (field_class == 1) and not cut

    combined_q = 0.5 * (forward_q + backward_q)
    q_full = cv2.resize(combined_q, (width, height), interpolation=cv2.INTER_LINEAR)

    result: dict[str, object] = {
        "name": name,
        "field_class": field_class,
        "active_class1": bool(active),
        "cut": bool(cut),
        "corr": float(corr),
        "srcMAD": float(source_mad),
        "field": field_info,
        "variants": {},
    }

    for qscale in qscales:
        # Backward A->B field reconstructs the A-side hypothesis.
        da, wa = splat_q(
            backward,
            backward_err,
            backward_q,
            backward_err <= 20,
            SIGMA,
            RADIUS,
            qscale,
        )
        # Forward B->A field reconstructs the B-side hypothesis.
        db, wb = splat_q(
            forward,
            forward_err,
            forward_q,
            forward_err <= 20,
            SIGMA,
            RADIUS,
            qscale,
        )

        ca = cv2.resize(
            np.clip(wa / kernel_sum, 0.0, 1.0),
            (width, height),
            interpolation=cv2.INTER_LINEAR,
        )
        cb = cv2.resize(
            np.clip(wb / kernel_sum, 0.0, 1.0),
            (width, height),
            interpolation=cv2.INTER_LINEAR,
        )
        da_full = cv2.resize(da, (width, height), interpolation=cv2.INTER_LINEAR)
        db_full = cv2.resize(db, (width, height), interpolation=cv2.INTER_LINEAR)

        aw = remap(a, xx + da_full[..., 0], yy + da_full[..., 1])
        bw = remap(b, xx + db_full[..., 0], yy + db_full[..., 1])

        # Preserve V1.2's confidence-cubed directional ownership.
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

        support = np.maximum(ca, cb)
        local = smoothstep(1000, 2200, q_full) * smoothstep(0.03, 0.22, support)
        local = cv2.GaussianBlur(local.astype(np.float32), (0, 0), 0.8)
        local = np.minimum(local, ALPHA_CAP)
        alpha = local if active else np.zeros_like(local)
        output = golden * (1.0 - alpha[..., None]) + robust * alpha[..., None]

        both = np.minimum(ca, cb)
        agree = np.mean(np.abs(aw - bw), axis=2)
        mask = both > 0.1

        variant: dict[str, object] = {
            "out": metric(output, golden),
            "full_robust": metric(robust, golden),
            "alphaMean": float(alpha.mean()),
            "alphaGt05": float(100.0 * np.mean(alpha > 0.05)),
            "supportMean": float(support.mean()),
            "bothMean": float(both.mean()),
            "warpAgree": float(agree[mask].mean()) if np.any(mask) else 1.0,
        }
        if save:
            variant["diagnostics"] = save_diagnostics(
                out_dir, name, qscale, output, golden, alpha
            )
        result["variants"][str(qscale)] = variant

    print(json.dumps(result), flush=True)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument(
        "--qscales",
        default=",".join(str(value) for value in DEFAULT_QSCALES),
        help="comma-separated confidence scales",
    )
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--no-save", action="store_true")
    args = parser.parse_args()

    qscales = tuple(int(value) for value in args.qscales.split(",") if value.strip())
    rows = [
        process(
            capture,
            qscales=qscales,
            save=not args.no_save,
            out_dir=args.out_dir,
        )
        for capture in args.captures
    ]
    print(json.dumps(rows, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
