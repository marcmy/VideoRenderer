#!/usr/bin/env python3
"""Replay modern SVP 4.3.0.165+ algorithm-13 area-blend behavior on MPCVR NVOF captures.

Research-only.  This corrects the older Part-14 direct algorithm-13 diagnostic
by including the proprietary area_blend phase remap introduced with the 2019
SAD-masking / 13th-shader change.  It intentionally leaves the live renderer
and Build 3 untouched.

At exact 2x, scene.adaptive=210 gives C1 phase 128 and C2 phase 64.  The current
proprietary DLL maps the source-blend phase using area_blend b as:

  p_mask = p*b                 for phase <= 126
  p_mask = 1-(1-p)*b           for phase > 126

with raw-plugin default b=0.4.  The optional score-derived area mask is replayed
using modern software 4x4 luma SAD; its exact proprietary NVOF score scaling is
still a domain caveat, so area-mask results are sensitivity evidence rather
than a claim of byte-identical proprietary rendering.
"""
from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
from numba import njit, prange
from PIL import Image

SIGMA = 1.25
RADIUS = 3
QSCALE = 1600


@dataclass(frozen=True)
class Classification:
    value: int
    considered: int
    zero_skipped: int
    m1_count: int
    m2_count: int
    scene_count: int
    required: int
    border_x: int
    border_y: int


def metadata(capture: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in (capture / "metadata.txt").read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def load_rgb(capture: Path, name: str) -> np.ndarray:
    return np.asarray(Image.open(capture / name).convert("RGB"), np.float32) / 255.0


def load_flow(capture: Path, name: str) -> np.ndarray:
    meta = metadata(capture)
    h = int(meta["flow_height"])
    w = int(meta["flow_width"])
    return np.fromfile(capture / name, dtype="<i2").reshape(h, w, 2).astype(np.float32) / 32.0


def load_raw(capture: Path, name: str) -> np.ndarray:
    meta = metadata(capture)
    h = int(meta["flow_height"])
    w = int(meta["flow_width"])
    return np.fromfile(capture / name, dtype="<i2").reshape(h, w, 2).astype(np.int32)


@njit
def sample_flow_scalar(flow: np.ndarray, px: float, py: float, grid: float = 4.0):
    h, w, _ = flow.shape
    gx = px / grid
    gy = py / grid
    bx = math.floor(gx)
    by = math.floor(gy)
    fx = gx - bx
    fy = gy - by
    x0 = 0 if bx < 0 else w - 1 if bx > w - 1 else bx
    x1 = 0 if bx + 1 < 0 else w - 1 if bx + 1 > w - 1 else bx + 1
    y0 = 0 if by < 0 else h - 1 if by > h - 1 else by
    y1 = 0 if by + 1 < 0 else h - 1 if by + 1 > h - 1 else by + 1
    o0 = flow[y0, x0, 0] * (1 - fx) + flow[y0, x1, 0] * fx
    o1 = flow[y0, x0, 1] * (1 - fx) + flow[y0, x1, 1] * fx
    q0 = flow[y1, x0, 0] * (1 - fx) + flow[y1, x1, 0] * fx
    q1 = flow[y1, x0, 1] * (1 - fx) + flow[y1, x1, 1] * fx
    return o0 * (1 - fy) + q0 * fy, o1 * (1 - fy) + q1 * fy


@njit(parallel=True)
def consistency_fields(forward: np.ndarray, backward: np.ndarray):
    h, w, _ = forward.shape
    fe = np.empty((h, w), np.float32)
    be = np.empty((h, w), np.float32)
    for y in prange(h):
        for x in range(w):
            px = x * 4.0
            py = y * 4.0
            fx, fy = forward[y, x]
            bx, by = backward[y, x]
            sbx, sby = sample_flow_scalar(backward, px + fx, py + fy, 4.0)
            sfx, sfy = sample_flow_scalar(forward, px + bx, py + by, 4.0)
            fe[y, x] = math.sqrt((fx + sbx) ** 2 + (fy + sby) ** 2)
            be[y, x] = math.sqrt((bx + sfx) ** 2 + (by + sfy) ** 2)
    return fe, be


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


def remap(image: np.ndarray, x: np.ndarray, y: np.ndarray) -> np.ndarray:
    return cv2.remap(
        image,
        x.astype(np.float32),
        y.astype(np.float32),
        cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REPLICATE,
    )


def y709(path: Path) -> np.ndarray:
    rgb = np.asarray(Image.open(path).convert("RGB"), np.float32)
    y = 16.0 + 0.182586 * rgb[..., 0] + 0.614231 * rgb[..., 1] + 0.062007 * rgb[..., 2]
    return np.rint(np.clip(y, 16, 235)).astype(np.uint8)


def sad_luma(source: np.ndarray, reference: np.ndarray, raw: np.ndarray):
    gh, gw, _ = raw.shape
    height, width = source.shape
    disp = np.trunc(raw.astype(np.float64) / 32.0).astype(np.int32)
    score = np.zeros((gh, gw), np.int32)
    sums = np.zeros((gh, gw), np.int32)
    gy = np.arange(gh)[:, None]
    gx = np.arange(gw)[None, :]
    for oy in range(4):
        yy = np.minimum(gy * 4 + oy, height - 1)
        for ox in range(4):
            xx = np.minimum(gx * 4 + ox, width - 1)
            ry = np.clip(yy + disp[..., 1], 0, height - 1)
            rx = np.clip(xx + disp[..., 0], 0, width - 1)
            sv = source[yy, xx].astype(np.int16)
            rv = reference[ry, rx].astype(np.int16)
            score += np.abs(sv - rv)
            sums += sv
    return score.astype(np.uint32), (sums >> 4).astype(np.uint8)


def pair_luma(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    value = (a.astype(np.float64) + b.astype(np.float64)) / 510.0
    out = np.floor(np.power(value, 1.5) * 255.0).astype(np.int32)
    out = np.where(out < 21, 20, out)
    return (out & 255).astype(np.uint8)


def qnorm(score: np.ndarray, luma: np.ndarray) -> np.ndarray:
    return ((score.astype(np.uint64) * 255) // np.maximum(luma.astype(np.uint64), 1)).astype(np.float32)


def classify(scores: np.ndarray, luma: np.ndarray) -> Classification:
    h, w = scores.shape
    bx = max(int(w * 0.04), 1)
    by = max(int(h * 0.04), 1)
    zero_skip_limit = (2 * w * h) // 3
    q = ((scores.astype(np.uint64) * 255) // np.maximum(luma.astype(np.uint64), 1)).astype(np.int64)
    interior = q[by : h - by, bx : w - bx]
    zero_skipped = min(int(np.sum(interior < 200)), zero_skip_limit)
    considered = int(interior.size - zero_skipped)
    m1 = int(np.sum((interior >= 1600) & (interior < 2800)))
    m2 = int(np.sum((interior >= 2800) & (interior < 4000)))
    scene = int(np.sum(interior >= 4000))
    required = (20 * considered) // 100
    high = scene + m2
    mid = high + m1
    value = 3 if scene >= required else 2 if high >= required else 1 if mid >= required else 0
    return Classification(value, considered, zero_skipped, m1, m2, scene, required, bx, by)


def modern_fields(capture: Path):
    a_y = y709(capture / "frame-A.bmp")
    b_y = y709(capture / "frame-B.bmp")
    ba = load_raw(capture, "flow-forward-B-to-A-s10.5.bin")
    ab = load_raw(capture, "flow-backward-A-to-B-s10.5.bin")
    score_ba, luma_b = sad_luma(b_y, a_y, ba)
    score_ab, luma_a = sad_luma(a_y, b_y, ab)
    luma = pair_luma(luma_a, luma_b)
    return (
        score_ba,
        score_ab,
        qnorm(score_ba, luma),
        qnorm(score_ab, luma),
        classify(score_ba, luma),
        classify(score_ab, luma),
    )


def nearest_blend_phase(phase: int, blend: float = 0.4) -> float:
    p = phase / 256.0
    return p * blend if phase <= 126 else 1.0 - (1.0 - p) * blend


def area_mask_grid(score_ba: np.ndarray, score_ab: np.ndarray, area: float = 200.0, sharp: float = 1.0):
    def one(score: np.ndarray) -> np.ndarray:
        value = np.power(4.0 * score.astype(np.float64) * (area / 10000.0) / 16.0, sharp) * 255.0
        return np.clip(np.floor(value), 0, 255).astype(np.float32)

    return np.maximum(one(score_ba), one(score_ab))


def blend255_float(motion: np.ndarray, base: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    m = np.clip(np.rint(motion * 255.0), 0, 255).astype(np.int32)
    b = np.clip(np.rint(base * 255.0), 0, 255).astype(np.int32)
    a = np.clip(np.floor(alpha), 0, 255).astype(np.int32)[..., None]
    return np.clip((m * (255 - a) + b * a + 255) >> 8, 0, 255).astype(np.float32) / 255.0


def metric(output: np.ndarray, reference: np.ndarray) -> dict[str, float]:
    diff = np.mean(np.abs(output - reference), axis=2)
    return {
        "mad": float(diff.mean()),
        "p99": float(np.quantile(diff, 0.99)),
        "gt1": float(100.0 * np.mean(diff > 1.0 / 255.0)),
        "gt4": float(100.0 * np.mean(diff > 4.0 / 255.0)),
        "gt8": float(100.0 * np.mean(diff > 8.0 / 255.0)),
    }


def component_stats(output: np.ndarray, reference: np.ndarray) -> dict[str, float | int]:
    diff = np.mean(np.abs(output - reference), axis=2)
    mask = (diff > 4.0 / 255.0).astype(np.uint8)
    count, _, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    areas = stats[1:, cv2.CC_STAT_AREA] if count > 1 else np.empty(0, np.int32)
    return {
        "largest_px": int(areas.max()) if len(areas) else 0,
        "largest_pct": float(100.0 * areas.max() / mask.size) if len(areas) else 0.0,
        "components_ge1000": int(np.sum(areas >= 1000)),
        "components_ge5000": int(np.sum(areas >= 5000)),
        "large_area_pct": float(100.0 * np.sum(areas[areas >= 1000]) / mask.size) if len(areas) else 0.0,
    }


def process(capture: Path, area: float = 200.0, sharp: float = 1.0, area_blend: float = 0.4) -> dict[str, object]:
    a = load_rgb(capture, "frame-A.bmp")
    b = load_rgb(capture, "frame-B.bmp")
    golden = load_rgb(capture, "midpoint-current.bmp")
    flow_ba = load_flow(capture, "flow-forward-B-to-A-s10.5.bin")
    flow_ab = load_flow(capture, "flow-backward-A-to-B-s10.5.bin")
    err_ba, err_ab = consistency_fields(flow_ba, flow_ab)
    score_ba, score_ab, q_ba, q_ab, class_ba, class_ab = modern_fields(capture)

    result: dict[str, object] = {
        "name": capture.name,
        "baClass": class_ba.value,
        "currentClass": class_ab.value,
        "active": class_ab.value in (1, 2),
        "area": area,
        "areaSharp": sharp,
        "areaBlend": area_blend,
    }
    if class_ab.value not in (1, 2):
        return result

    t = 0.5 if class_ab.value == 1 else 0.25
    phase = 128 if class_ab.value == 1 else 64
    fallback_t = nearest_blend_phase(phase, area_blend)
    height, width = a.shape[:2]
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)

    # A-side uses A->B; B-side uses B->A, matching the Part-14 diagnostic.
    disp_a, _ = splat_q_phase(flow_ab, err_ab, q_ab, err_ab <= 20, SIGMA, RADIUS, QSCALE, t)
    disp_b, _ = splat_q_phase(flow_ba, err_ba, q_ba, err_ba <= 20, SIGMA, RADIUS, QSCALE, 1.0 - t)
    disp_a = cv2.resize(disp_a, (width, height), interpolation=cv2.INTER_LINEAR)
    disp_b = cv2.resize(disp_b, (width, height), interpolation=cv2.INTER_LINEAR)
    warp_a = remap(a, xx + disp_a[..., 0], yy + disp_a[..., 1])
    warp_b = remap(b, xx + disp_b[..., 0], yy + disp_b[..., 1])

    temporal = (1.0 - t) * a + t * b
    fallback = (1.0 - fallback_t) * a + fallback_t * b
    old13 = np.median(np.stack([warp_a, warp_b, temporal], axis=0), axis=0).astype(np.float32)
    modern13 = np.median(np.stack([warp_a, warp_b, fallback], axis=0), axis=0).astype(np.float32)

    mask_grid = area_mask_grid(score_ba, score_ab, area, sharp)
    mask = cv2.resize(mask_grid, (width, height), interpolation=cv2.INTER_LINEAR)
    old13_area = blend255_float(old13, fallback, mask)
    modern13_area = blend255_float(modern13, fallback, mask)

    result.update(
        {
            "phase": phase,
            "t": t,
            "fallbackT": fallback_t,
            "maskMean": float(mask_grid.mean()),
            "maskGt128": float(100.0 * np.mean(mask_grid > 128)),
        }
    )
    for name, output in (
        ("old13", old13),
        ("modern13", modern13),
        ("old13_area", old13_area),
        ("modern13_area", modern13_area),
    ):
        result[name + "_vs_temporal"] = {**metric(output, temporal), **component_stats(output, temporal)}
        if phase == 128:
            result[name + "_vs_golden"] = {**metric(output, golden), **component_stats(output, golden)}
    if phase == 128:
        result["temporal_vs_golden"] = {**metric(temporal, golden), **component_stats(temporal, golden)}
        result["fallback_vs_golden"] = {**metric(fallback, golden), **component_stats(fallback, golden)}
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--area", type=float, default=200.0)
    parser.add_argument("--sharp", type=float, default=1.0)
    parser.add_argument("--area-blend", type=float, default=0.4)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    rows = [process(c.resolve(), args.area, args.sharp, args.area_blend) for c in args.captures]
    if args.json:
        args.json.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(rows, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
