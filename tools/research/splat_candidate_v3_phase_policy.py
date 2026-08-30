#!/usr/bin/env python3
"""Replay SVP-style adaptive phase + algorithm-13 semantics on MPCVR flows.

Research-only diagnostic. This tool intentionally tests the *direct* policy
translation that was rejected in Part 14; it is not a live candidate.

At an exact 2x output rate, the reconstructed scene.adaptive=210 policy maps:
  class 1 -> phase 128 (t=0.5)
  class 2 -> phase  64 (t=0.25)

With scene.force13 enabled, classes 1 and 2 use algorithm 13. The reconstructed
algorithm-13 kernel takes a channelwise median of the two phase-correct warped
endpoint samples and the unwarped temporal sample.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import cv2
import numpy as np
from numba import njit
from PIL import Image

import splat_candidate_v2_gate_matrix as base

SIGMA = 1.25
RADIUS = 3
DEFAULT_QSCALE = 1600


@njit
def splat_q_phase(flow, err, qual, valid, sigma, radius, qscale, factor):
    h, w, _ = flow.shape
    sx = np.zeros((h, w), np.float32)
    sy = np.zeros((h, w), np.float32)
    ws = np.zeros((h, w), np.float32)
    denom = 2.0 * sigma * sigma
    for y in range(h):
        for x in range(w):
            if not valid[y, x]:
                continue
            fx = flow[y, x, 0]
            fy = flow[y, x, 1]
            tx = x + factor * fx / 4.0
            ty = y + factor * fy / 4.0
            cx = int(math.floor(tx))
            cy = int(math.floor(ty))
            conf = math.exp(-min(err[y, x], 40.0) / 8.0)
            conf *= math.exp(-min(qual[y, x], 8000.0) / qscale)
            dx = -factor * fx
            dy = -factor * fy
            for oy in range(-radius, radius + 1):
                yy = cy + oy
                if yy < 0 or yy >= h:
                    continue
                for ox in range(-radius, radius + 1):
                    xx = cx + ox
                    if xx < 0 or xx >= w:
                        continue
                    ddx = xx - tx
                    ddy = yy - ty
                    kernel = math.exp(-(ddx * ddx + ddy * ddy) / denom) * conf
                    sx[yy, xx] += dx * kernel
                    sy[yy, xx] += dy * kernel
                    ws[yy, xx] += kernel
    out = np.zeros((h, w, 2), np.float32)
    for y in range(h):
        for x in range(w):
            if ws[y, x] > 1.0e-7:
                out[y, x, 0] = sx[y, x] / ws[y, x]
                out[y, x, 1] = sy[y, x] / ws[y, x]
    return out, ws


def image_metric(output: np.ndarray, reference: np.ndarray) -> dict[str, float]:
    diff = np.mean(np.abs(output - reference), axis=2)
    return {
        "mad": float(diff.mean()),
        "p99": float(np.quantile(diff, 0.99)),
        "gt1": float(100.0 * np.mean(diff > 1.0 / 255.0)),
        "gt4": float(100.0 * np.mean(diff > 4.0 / 255.0)),
        "gt8": float(100.0 * np.mean(diff > 8.0 / 255.0)),
    }


def save_image(path: Path, image: np.ndarray) -> None:
    Image.fromarray(np.uint8(np.clip(image * 255.0 + 0.5, 0, 255))).save(path)


def process(capture: Path, out_dir: Path, qscale: int) -> dict[str, object]:
    capture = capture.resolve()
    name = capture.name
    a = base.load_rgb(capture, "frame-A.bmp")
    b = base.load_rgb(capture, "frame-B.bmp")
    golden = base.load_rgb(capture, "midpoint-current.bmp")

    # MPCVR diagnostic naming is opposite the open-svpflow current/previous names.
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
    class_ba, *_ = base.classify(score_ba, pair_luma)
    class_ab, m1_ab, m2_ab, scene_ab = base.classify(score_ab, pair_luma)
    corr, source_mad, cut = base.scene_stats(a, b)

    result: dict[str, object] = {
        "name": name,
        "baClass": int(class_ba),
        "currentClass": int(class_ab),
        "currentM1plus": float(m1_ab),
        "currentM2plus": float(m2_ab),
        "currentScene": float(scene_ab),
        "cut": bool(cut),
        "corr": float(corr),
        "srcMAD": float(source_mad),
    }

    # scene.adaptive=210 at exact 2x: C1 -> phase128, C2 -> phase64.
    if cut or class_ab not in (1, 2):
        result["active"] = False
        return result
    t = 0.5 if class_ab == 1 else 0.25
    result.update({"active": True, "t": t, "phase": int(round(t * 256.0))})

    height, width = a.shape[:2]
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    kernel_sum = sum(
        math.exp(-(x * x + y * y) / (2.0 * SIGMA * SIGMA))
        for y in range(-RADIUS, RADIUS + 1)
        for x in range(-RADIUS, RADIUS + 1)
    )

    # A->B field advances the A-side sample by t. B->A advances the B-side
    # sample by (1-t) back toward A.
    disp_a, weight_a = splat_q_phase(
        flow_ab, err_ab, q_ab, err_ab <= 20, SIGMA, RADIUS, qscale, t
    )
    disp_b, weight_b = splat_q_phase(
        flow_ba, err_ba, q_ba, err_ba <= 20, SIGMA, RADIUS, qscale, 1.0 - t
    )
    cover_a = cv2.resize(np.clip(weight_a / kernel_sum, 0, 1), (width, height))
    cover_b = cv2.resize(np.clip(weight_b / kernel_sum, 0, 1), (width, height))
    disp_a = cv2.resize(disp_a, (width, height))
    disp_b = cv2.resize(disp_b, (width, height))
    warp_a = base.remap(a, xx + disp_a[..., 0], yy + disp_a[..., 1])
    warp_b = base.remap(b, xx + disp_b[..., 0], yy + disp_b[..., 1])
    temporal = (1.0 - t) * a + t * b

    # Direct reconstructed algorithm-13 combination.
    median = np.median(np.stack([warp_a, warp_b, temporal], axis=0), axis=0).astype(
        np.float32
    )

    # Confidence-weighted dual warp is diagnostic only, not algorithm 13.
    wa = np.maximum(cover_a, 1.0e-8) ** 3
    wb = np.maximum(cover_b, 1.0e-8) ** 3
    denom = wa + wb
    blend = np.where(
        (denom > 1.0e-7)[..., None],
        (warp_a * wa[..., None] + warp_b * wb[..., None])
        / np.maximum(denom[..., None], 1.0e-7),
        temporal,
    )

    both = np.minimum(cover_a, cover_b)
    agree = np.mean(np.abs(warp_a - warp_b), axis=2)
    mask = both > 0.1
    result.update(
        {
            "supportMean": float(np.maximum(cover_a, cover_b).mean()),
            "bothMean": float(both.mean()),
            "warpAgree": float(agree[mask].mean()) if np.any(mask) else 1.0,
            "median_vs_temporal": image_metric(median, temporal),
            "blend_vs_temporal": image_metric(blend, temporal),
        }
    )
    if t == 0.5:
        result["median_vs_golden"] = image_metric(median, golden)
        result["blend_vs_golden"] = image_metric(blend, golden)

    out_dir.mkdir(parents=True, exist_ok=True)
    prefix = out_dir / f"{name}_C{class_ab}_P{int(t * 256)}"
    save_image(prefix.with_name(prefix.name + "_MEDIAN.png"), median)
    save_image(prefix.with_name(prefix.name + "_BLEND.png"), blend)
    save_image(prefix.with_name(prefix.name + "_TEMPORAL.png"), temporal)
    save_image(
        prefix.with_name(prefix.name + "_MEDIAN_DIFF_TEMP_X16.png"),
        np.clip(np.abs(median - temporal) * 16.0, 0, 1),
    )
    save_image(
        prefix.with_name(prefix.name + "_MEDIAN_EXPOSED.png"),
        np.power(np.clip(median, 0, 1), 0.45),
    )
    save_image(
        prefix.with_name(prefix.name + "_TEMPORAL_EXPOSED.png"),
        np.power(np.clip(temporal, 0, 1), 0.45),
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--qscale", type=int, default=DEFAULT_QSCALE)
    parser.add_argument("--out-dir", type=Path, default=Path("phase-policy-replay"))
    args = parser.parse_args()
    rows = [process(capture, args.out_dir, args.qscale) for capture in args.captures]
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "summary.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
    print(json.dumps(rows, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
