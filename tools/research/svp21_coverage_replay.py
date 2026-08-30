#!/usr/bin/env python3
"""Research-only replay of SVP algorithm-21/22 directional coverage masks on MPCVR NVOF captures.

The proprietary svpflow2 mask routine was reconstructed as:
  1. phase-scale the opposite-direction vector using marker<<8;
  2. splat each 4x4 block footprint bilinearly into the vector grid;
  3. apply a 3x3 coverage window;
  4. convert uncovered block area to an 8-bit mask using mask.cover;
  5. algorithm 21 cross-feeds the two warped hypotheses by their directional masks;
  6. algorithm 22 clamps the ordinary temporal base between those corrected hypotheses.

For full-resolution NVOF captures, --vector-divisor=8 models the independently
reconstructed NVOF payload packing (S10.5 raw / 8 stored with marker=4).  The
alternate --vector-divisor=32 is useful as a sensitivity check for feeding
already marker-divided pixel vectors into the same mask arithmetic.

This tool is archaeology only.  It does not modify renderer source or Build 3.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image


def trunc_div(a: np.ndarray, b: int) -> np.ndarray:
    a = a.astype(np.int64, copy=False)
    return np.where(a < 0, -((-a) // b), a // b).astype(np.int32)


def parse_meta(capture: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in (capture / "metadata.txt").read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            out[key.strip()] = value.strip()
    return out


def read_rgb8(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def read_flow_raw(capture: Path, filename: str) -> np.ndarray:
    meta = parse_meta(capture)
    w = int(meta["flow_width"])
    h = int(meta["flow_height"])
    raw = np.fromfile(capture / filename, dtype="<i2")
    return raw.reshape(h, w, 2).astype(np.int32)


def grid_coords(width: int, height: int, grid_w: int, grid_h: int,
                origin: int = 2, block: int = 4) -> tuple[np.ndarray, np.ndarray]:
    xs = np.arange(width, dtype=np.float32)
    ys = np.arange(height, dtype=np.float32)
    gx = np.where(xs >= origin, (xs - origin) / block, 0.0)
    gy = np.where(ys >= origin, (ys - origin) / block, 0.0)
    gx = np.clip(gx, 0, grid_w - 1)
    gy = np.clip(gy, 0, grid_h - 1)
    return (
        np.broadcast_to(gx[None, :], (height, width)).copy(),
        np.broadcast_to(gy[:, None], (height, width)).copy(),
    )


def upsample_grid(grid: np.ndarray, width: int, height: int,
                  origin: int = 2, block: int = 4, floor: bool = False) -> np.ndarray:
    gh, gw = grid.shape[:2]
    mx, my = grid_coords(width, height, gw, gh, origin, block)
    out = cv2.remap(
        grid.astype(np.float32), mx, my, cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REPLICATE,
    )
    return np.floor(out) if floor else out


def coverage_mask(raw_opposite: np.ndarray, phase: int, vector_divisor: int,
                  strength: int = 100, block: int = 4, marker: int = 4) -> np.ndarray:
    # Proprietary mask generator consumes stored vector units and divides by marker<<8.
    vectors = trunc_div(raw_opposite, vector_divisor)
    h, w = vectors.shape[:2]
    denom = marker << 8
    dx = trunc_div(vectors[..., 0] * int(phase), denom)
    dy = trunc_div(vectors[..., 1] * int(phase), denom)

    yy, xx = np.mgrid[0:h, 0:w]
    shifted_x = xx * block + dx
    shifted_y = yy * block + dy
    left = np.floor_divide(shifted_x, block)
    top = np.floor_divide(shifted_y, block)
    right = left + 1
    bottom = top + 1
    next_x = right * block
    next_y = bottom * block
    left_weight = next_x - shifted_x
    top_weight = next_y - shifted_y
    right_weight = shifted_x + block - next_x
    bottom_weight = shifted_y + block - next_y

    points = np.zeros((h, w), dtype=np.int32)

    def add(x: np.ndarray, y: np.ndarray, weight: np.ndarray) -> None:
        valid = (x >= 0) & (x < w) & (y >= 0) & (y < h)
        np.add.at(points, (y[valid], x[valid]), weight[valid].astype(np.int32))

    add(left, top, left_weight * top_weight)
    add(right, top, top_weight * right_weight)
    add(right, bottom, right_weight * bottom_weight)
    add(left, bottom, left_weight * bottom_weight)

    summed = cv2.boxFilter(
        points, cv2.CV_32S, (3, 3), normalize=False,
        borderType=cv2.BORDER_CONSTANT,
    )
    area = block * block
    covered = summed >> 3
    remaining = np.maximum(area - covered, 0)
    value = np.trunc(
        remaining.astype(np.float64) * (strength / 100.0) * 256.0 / area
    ).astype(np.int32)
    return np.minimum(value, 255).astype(np.uint8)


def motion_grid(raw: np.ndarray, phase: int, vector_divisor: int = 8,
                marker: int = 4) -> np.ndarray:
    # Stored NVOF vector -> renderer pixel vector -> phase-scaled integer displacement.
    stored = trunc_div(raw, vector_divisor)
    pixels = trunc_div(stored, marker)
    return trunc_div(pixels * int(phase), 256)


def warp_nearest(image: np.ndarray, dx: np.ndarray, dy: np.ndarray) -> np.ndarray:
    h, w = image.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    sx = np.clip(xx + dx.astype(np.int32), 0, w - 1)
    sy = np.clip(yy + dy.astype(np.int32), 0, h - 1)
    return image[sy, sx]


def blend256(a: np.ndarray, b: np.ndarray, weight: int | np.ndarray) -> np.ndarray:
    aa = a.astype(np.int32)
    bb = b.astype(np.int32)
    ww = np.asarray(weight, dtype=np.int32)
    if ww.ndim == 2:
        ww = ww[..., None]
    return np.clip((aa * (256 - ww) + bb * ww) >> 8, 0, 255).astype(np.uint8)


def blend255(a: np.ndarray, b: np.ndarray, weight: np.ndarray) -> np.ndarray:
    aa = a.astype(np.int32)
    bb = b.astype(np.int32)
    ww = weight.astype(np.int32)
    if ww.ndim == 2:
        ww = ww[..., None]
    return np.clip((aa * (255 - ww) + bb * ww + 255) >> 8, 0, 255).astype(np.uint8)


def clamp_between(value: np.ndarray, a: np.ndarray, b: np.ndarray) -> np.ndarray:
    return np.minimum(np.maximum(value, np.minimum(a, b)), np.maximum(a, b)).astype(np.uint8)


def metric(output: np.ndarray, golden: np.ndarray) -> dict[str, float]:
    diff = np.mean(np.abs(output.astype(np.float32) - golden.astype(np.float32)), axis=2)
    return {
        "mad_lsb": float(diff.mean()),
        "chg4_pct": float(np.mean(diff > 4.0) * 100.0),
        "chg8_pct": float(np.mean(diff > 8.0) * 100.0),
        "p99_lsb": float(np.quantile(diff, 0.99)),
    }


def component_stats(output: np.ndarray, golden: np.ndarray) -> dict[str, float | int]:
    diff = np.mean(np.abs(output.astype(np.float32) - golden.astype(np.float32)), axis=2)
    mask = (diff > 4.0).astype(np.uint8)
    n, _, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    areas = stats[1:, cv2.CC_STAT_AREA] if n > 1 else np.empty(0, dtype=np.int32)
    return {
        "components": int(len(areas)),
        "largest_px": int(areas.max()) if len(areas) else 0,
        "largest_pct": float(areas.max() / mask.size * 100.0) if len(areas) else 0.0,
        "components_ge1000": int(np.sum(areas >= 1000)),
        "components_ge5000": int(np.sum(areas >= 5000)),
        "large_area_pct": float(np.sum(areas[areas >= 1000]) / mask.size * 100.0) if len(areas) else 0.0,
    }


def replay(capture: Path, phase: int, vector_divisor: int, mask_cover: int) -> dict[str, object]:
    meta = parse_meta(capture)
    width = int(meta["frame_width"])
    height = int(meta["frame_height"])
    a = read_rgb8(capture / "frame-A.bmp")
    b = read_rgb8(capture / "frame-B.bmp")
    golden = read_rgb8(capture / "midpoint-current.bmp")
    ba = read_flow_raw(capture, "flow-forward-B-to-A-s10.5.bin")
    ab = read_flow_raw(capture, "flow-backward-A-to-B-s10.5.bin")

    # The SVP renderer associates source A with previous/B->A and source B with current/A->B.
    motion_a = motion_grid(ba, phase)
    motion_b = motion_grid(ab, 256 - phase)
    dx_a = upsample_grid(motion_a[..., 0], width, height, floor=True)
    dy_a = upsample_grid(motion_a[..., 1], width, height, floor=True)
    dx_b = upsample_grid(motion_b[..., 0], width, height, floor=True)
    dy_b = upsample_grid(motion_b[..., 1], width, height, floor=True)
    warp_a = warp_nearest(a, dx_a, dy_a)
    warp_b = warp_nearest(b, dx_b, dy_b)

    # coverage(which=0) uses the opposite/current A->B field at inverse phase;
    # coverage(which=1) uses opposite/previous B->A at phase.
    mask_a_grid = coverage_mask(ab, 256 - phase, vector_divisor, mask_cover)
    mask_b_grid = coverage_mask(ba, phase, vector_divisor, mask_cover)
    mask_a = np.floor(upsample_grid(mask_a_grid, width, height)).clip(0, 255).astype(np.uint8)
    mask_b = np.floor(upsample_grid(mask_b_grid, width, height)).clip(0, 255).astype(np.uint8)

    temporal = blend256(a, b, phase)
    mode11 = blend256(warp_a, warp_b, phase)
    mode13 = clamp_between(temporal, warp_a, warp_b)
    corrected_a = blend255(warp_a, warp_b, mask_a)
    corrected_b = blend255(warp_b, warp_a, mask_b)
    mode21 = blend256(corrected_a, corrected_b, phase)
    mode22 = clamp_between(temporal, corrected_a, corrected_b)

    result: dict[str, object] = {
        "capture": capture.name,
        "phase": phase,
        "vector_divisor": vector_divisor,
        "mask_cover": mask_cover,
        "mask_a": {
            "mean": float(mask_a_grid.mean()),
            "gt32_pct": float(np.mean(mask_a_grid > 32) * 100.0),
            "gt128_pct": float(np.mean(mask_a_grid > 128) * 100.0),
        },
        "mask_b": {
            "mean": float(mask_b_grid.mean()),
            "gt32_pct": float(np.mean(mask_b_grid > 32) * 100.0),
            "gt128_pct": float(np.mean(mask_b_grid > 128) * 100.0),
        },
    }
    for name, image in (
        ("temporal", temporal), ("mode11", mode11), ("mode13", mode13),
        ("mode21", mode21), ("mode22", mode22),
    ):
        result[name] = {**metric(image, golden), **component_stats(image, golden)}
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--phase", type=int, default=128)
    parser.add_argument("--vector-divisor", type=int, default=8)
    parser.add_argument("--mask-cover", type=int, default=100)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    rows = [replay(c.resolve(), args.phase, args.vector_divisor, args.mask_cover) for c in args.captures]
    text = json.dumps(rows, indent=2)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
