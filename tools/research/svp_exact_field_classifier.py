#!/usr/bin/env python3
"""Exact clean-room replay of SVP4's field-quality classifier loop.

The formulas in this file are reconstructed from the supplied svpflow2.dll
binary and independently cross-checked against open-svpflow where possible.
It intentionally preserves integer/truncation behavior that looks odd.

The core classifier consumes already-packed 24-bit vector scores and the
per-cell luma-normalization byte used by the proprietary classifier. Helpers
also reproduce the NVOF legacy-cost score/luma packing and pair-luma map.
"""
from __future__ import annotations

from dataclasses import dataclass
import argparse
import json
import math
from pathlib import Path

import numpy as np


@dataclass(frozen=True)
class Thresholds:
    blocks: int = 20
    blocks13: int = 0
    zero: int = 200
    m1: int = 1600
    m2: int = 2800
    scene: int = 4000
    ignore: float = 0.04


@dataclass(frozen=True)
class Classification:
    value: int
    considered: int
    zero_skipped: int
    m1_count: int
    m2_count: int
    scene_count: int
    required: int
    required13: int
    border_x: int
    border_y: int


def _trunc_div_signed(numer: int, denom: int) -> int:
    if denom <= 0:
        raise ValueError("denominator must be positive")
    return numer // denom if numer >= 0 else -((-numer) // denom)


def _signed32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def normalized_score(score24: int, luma_byte: int) -> int:
    """Match the DLL's 32-bit `(score*255)/max(luma,1)` sequence."""
    score24 &= 0x00FFFFFF
    product = _signed32((score24 << 8) - score24)
    return _trunc_div_signed(product, max(int(luma_byte) & 0xFF, 1))


def classify(scores: np.ndarray, luma_map: np.ndarray,
             thresholds: Thresholds = Thresholds()) -> Classification:
    scores = np.asarray(scores, dtype=np.uint32)
    luma_map = np.asarray(luma_map, dtype=np.uint8)
    if scores.ndim != 2 or luma_map.shape != scores.shape:
        raise ValueError("scores and luma_map must be same-shaped 2D arrays")

    height, width = scores.shape
    bx = int(width * thresholds.ignore)
    by = int(height * thresholds.ignore)
    minimum = 1 if thresholds.ignore > 0.01 else 0
    bx = max(bx, minimum)
    by = max(by, minimum)

    full_count = width * height
    zero_skip_limit = (2 * full_count) // 3
    zero_skipped = considered = 0
    m1_count = m2_count = scene_count = 0

    x0, x1 = bx, width - bx - 1
    y0, y1 = by, height - by - 1
    if x0 <= x1 and y0 <= y1:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                q = normalized_score(int(scores[y, x]), int(luma_map[y, x]))
                if q < thresholds.zero and zero_skipped < zero_skip_limit:
                    zero_skipped += 1
                    continue

                considered += 1
                if q >= thresholds.scene:
                    scene_count += 1
                elif q >= thresholds.m2:
                    m2_count += 1
                elif q >= thresholds.m1:
                    m1_count += 1

    required = (thresholds.blocks * considered) // 100
    high = scene_count + m2_count
    mid = high + m1_count
    required13 = 0

    if scene_count >= required:
        value = 3
    elif high >= required:
        value = 2
    elif mid >= required:
        value = 1
    elif thresholds.blocks13 > 0:
        required13 = (thresholds.blocks13 * considered) // 100
        value = -1 if mid >= required13 else 0
    else:
        value = 0

    return Classification(value, considered, zero_skipped, m1_count,
                          m2_count, scene_count, required, required13, bx, by)


def cost_shift(scale: int) -> int:
    """open-svpflow/SVP NVOF score scaling reconstructed from vector packing."""
    scale = max(int(scale), 1)
    return min(int(math.log2(scale) * 2.0), 31)


def pack_nvof_direction(cost_r32: np.ndarray, luma_plane: np.ndarray,
                        scale: int = 1) -> tuple[np.ndarray, np.ndarray]:
    """Pack legacy R32 cost + 4x4 luma into SVP vector score/luma fields.

    `luma_plane` is the actual 8-bit NVOF input Y plane for this direction.
    This helper does not attempt to reconstruct Y from RGB because SVP's NVOF
    path uses YUV420P8/NV12 and that source conversion is a separate variable.
    """
    cost = np.asarray(cost_r32, dtype=np.uint32)
    y = np.asarray(luma_plane, dtype=np.uint8)
    if cost.ndim != 2 or y.ndim != 2:
        raise ValueError("cost and luma_plane must be 2D")

    gh, gw = cost.shape
    if y.shape[0] < gh * 4 or y.shape[1] < gw * 4:
        raise ValueError("luma plane is smaller than the 4x4 cost grid extent")

    scores = np.empty_like(cost, dtype=np.uint32)
    lumas = np.empty_like(cost, dtype=np.uint8)
    shift = cost_shift(scale)

    for gy in range(gh):
        for gx in range(gw):
            block = y[gy*4:min((gy+1)*4, y.shape[0]),
                      gx*4:min((gx+1)*4, y.shape[1])]
            block_sum = int(block.astype(np.uint32).sum()) & 0xFFFF
            raw1 = ((int(cost[gy, gx]) << shift) +
                    ((block_sum & 0xFFF0) << 20)) & 0xFFFFFFFF
            scores[gy, gx] = raw1 & 0x00FFFFFF
            lumas[gy, gx] = (raw1 >> 24) & 0xFF
    return scores, lumas


def build_pair_luma(previous_luma: np.ndarray, current_luma: np.ndarray,
                    marker: int = 4, gamma: float = 1.5) -> np.ndarray:
    """Reproduce the DLL helper at ~0x180005e60."""
    previous = np.asarray(previous_luma, dtype=np.uint8)
    current = np.asarray(current_luma, dtype=np.uint8)
    if previous.shape != current.shape:
        raise ValueError("directional luma arrays must have the same shape")

    denom = 510.0 if int(marker) == 3 else 255.0
    out = np.empty(previous.shape, dtype=np.uint8)
    for index in np.ndindex(previous.shape):
        value = (int(previous[index]) + int(current[index])) / denom
        scaled = int(math.pow(value, gamma) * 255.0)
        if scaled < 21:
            scaled = 20
        out[index] = scaled & 0xFF
    return out


def classify_nvof_pair(previous_cost: np.ndarray, current_cost: np.ndarray,
                       previous_y: np.ndarray, current_y: np.ndarray,
                       scale: int = 1, marker: int = 4, gamma: float = 1.5,
                       thresholds: Thresholds = Thresholds()) -> Classification:
    _, previous_luma = pack_nvof_direction(previous_cost, previous_y, scale)
    current_score, current_luma = pack_nvof_direction(current_cost, current_y, scale)
    luma_map = build_pair_luma(previous_luma, current_luma, marker, gamma)
    return classify(current_score, luma_map, thresholds)


def _load_raw(path: Path, dtype: np.dtype, width: int, height: int) -> np.ndarray:
    values = np.fromfile(path, dtype=dtype)
    expected = width * height
    if values.size != expected:
        raise ValueError(f"{path}: expected {expected} cells, got {values.size}")
    return values.reshape(height, width)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scores", type=Path, required=True,
                        help="raw little-endian u32 score array (low 24 bits used)")
    parser.add_argument("--luma", type=Path, required=True,
                        help="raw u8 classifier luma-map array")
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--blocks", type=int, default=20)
    parser.add_argument("--blocks13", type=int, default=0)
    parser.add_argument("--zero", type=int, default=200)
    parser.add_argument("--m1", type=int, default=1600)
    parser.add_argument("--m2", type=int, default=2800)
    parser.add_argument("--scene", type=int, default=4000)
    parser.add_argument("--ignore", type=float, default=0.04)
    args = parser.parse_args()

    scores = _load_raw(args.scores, np.dtype("<u4"), args.width, args.height)
    luma = _load_raw(args.luma, np.dtype("u1"), args.width, args.height)
    result = classify(scores, luma, Thresholds(args.blocks, args.blocks13,
        args.zero, args.m1, args.m2, args.scene, args.ignore))
    print(json.dumps(result.__dict__, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
