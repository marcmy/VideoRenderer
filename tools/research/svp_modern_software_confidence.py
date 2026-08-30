#!/usr/bin/env python3
"""Clean-room replay of SVP4's modern software NVOF confidence score.

This tool operates on MPCVR capture directories containing:
  frame-A.bmp
  frame-B.bmp
  metadata.txt
  flow-forward-B-to-A-s10.5.bin
  flow-backward-A-to-B-s10.5.bin

It reconstructs BT.709 limited-range luma from the capture BMPs, computes
the 4x4 motion-displaced SAD score for both NVOF directions, builds the
bidirectional pair-luma map (direction flags == 3), and runs the exact
field classifier from svp_exact_field_classifier.py.

The capture BMP luma is an approximation of SVP's decoder-domain Y plane.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

from svp_exact_field_classifier import Thresholds, build_pair_luma, classify


def read_metadata(capture: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    with (capture / "metadata.txt").open("r", encoding="utf-8") as handle:
        for line in handle:
            if "=" in line:
                key, value = line.strip().split("=", 1)
                values[key] = value
    return values


def load_raw_flow(capture: Path, name: str, width: int, height: int) -> np.ndarray:
    values = np.fromfile(capture / name, dtype="<i2")
    expected = width * height * 2
    if values.size != expected:
        raise ValueError(f"{name}: expected {expected} int16 values, got {values.size}")
    return values.reshape(height, width, 2)


def load_bt709_limited_y(path: Path) -> np.ndarray:
    rgb = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    r = rgb[..., 0]
    g = rgb[..., 1]
    b = rgb[..., 2]
    y = 16.0 + 0.182586 * r + 0.614231 * g + 0.062007 * b
    return np.rint(np.clip(y, 16.0, 235.0)).astype(np.uint8)


def sample_block_edge_replicate(image: np.ndarray, x0: int, y0: int) -> np.ndarray:
    height, width = image.shape
    xs = np.clip(np.arange(x0, x0 + 4), 0, width - 1)
    ys = np.clip(np.arange(y0, y0 + 4), 0, height - 1)
    return image[np.ix_(ys, xs)]


def software_direction_score(
    source_y: np.ndarray,
    reference_y: np.ndarray,
    raw_flow: np.ndarray,
    scale: int = 1,
) -> tuple[np.ndarray, np.ndarray]:
    """Return (score24, source_luma_byte) for one NVOF direction.

    The capture replay currently uses full-resolution MPCVR flow (scale=1).
    `scale` is exposed only for analysis of already-corresponding reduced-source
    data; this function does not resize frames or flow on its own.
    """
    if scale not in (1, 2, 4, 6, 8):
        raise ValueError("proprietary SVP NVOF scale must be one of 1,2,4,6,8")

    grid_h, grid_w, components = raw_flow.shape
    if components != 2:
        raise ValueError("raw_flow must have shape HxWx2")

    scores = np.zeros((grid_h, grid_w), dtype=np.uint32)
    lumas = np.zeros((grid_h, grid_w), dtype=np.uint8)
    scale_sq = scale * scale

    # Proprietary modern confidence uses integer displacement truncated toward 0.
    dx = np.trunc(raw_flow[..., 0].astype(np.float64) / 32.0).astype(np.int32)
    dy = np.trunc(raw_flow[..., 1].astype(np.float64) / 32.0).astype(np.int32)

    for gy in range(grid_h):
        sy = gy * 4
        for gx in range(grid_w):
            sx = gx * 4
            source = sample_block_edge_replicate(source_y, sx, sy)
            reference = sample_block_edge_replicate(
                reference_y, sx + int(dx[gy, gx]), sy + int(dy[gy, gx])
            )

            sad = int(
                np.abs(source.astype(np.int16) - reference.astype(np.int16)).sum()
            )
            scores[gy, gx] = np.uint32((sad * scale_sq) & 0x00FFFFFF)
            # Stored vector luma is the truncated average of the source 4x4 block.
            lumas[gy, gx] = np.uint8(int(source.astype(np.uint32).sum()) >> 4)

    return scores, lumas


def normalized_q(scores: np.ndarray, pair_luma: np.ndarray) -> np.ndarray:
    # Values here are small enough that the proprietary signed-32 wrap does not
    # affect the normal modern-SAD capture replay.
    numer = scores.astype(np.uint64) * np.uint64(255)
    denom = np.maximum(pair_luma.astype(np.uint64), np.uint64(1))
    return (numer // denom).astype(np.uint32)


def analyze_capture(capture: Path) -> dict[str, object]:
    metadata = read_metadata(capture)
    grid_w = int(metadata["flow_width"])
    grid_h = int(metadata["flow_height"])

    a_y = load_bt709_limited_y(capture / "frame-A.bmp")
    b_y = load_bt709_limited_y(capture / "frame-B.bmp")

    forward = load_raw_flow(
        capture, "flow-forward-B-to-A-s10.5.bin", grid_w, grid_h
    )
    backward = load_raw_flow(
        capture, "flow-backward-A-to-B-s10.5.bin", grid_w, grid_h
    )

    # B->A: source B, displaced reference A.
    forward_score, forward_luma = software_direction_score(b_y, a_y, forward)
    # A->B: source A, displaced reference B.
    backward_score, backward_luma = software_direction_score(a_y, b_y, backward)

    # The field entering the DLL helper is NVOF direction flags. Bidirectional=3.
    pair_luma = build_pair_luma(backward_luma, forward_luma, marker=3, gamma=1.5)

    forward_result = classify(forward_score, pair_luma, Thresholds())
    backward_result = classify(backward_score, pair_luma, Thresholds())
    forward_q = normalized_q(forward_score, pair_luma)
    backward_q = normalized_q(backward_score, pair_luma)

    def direction_stats(score: np.ndarray, q: np.ndarray, result: object) -> dict[str, object]:
        bx, by = result.border_x, result.border_y
        interior = q[by : grid_h - by, bx : grid_w - bx]
        if interior.size:
            occupancy = {
                "m1_plus": float(100.0 * np.mean(interior >= 1600)),
                "m2_plus": float(100.0 * np.mean(interior >= 2800)),
                "scene_plus": float(100.0 * np.mean(interior >= 4000)),
            }
        else:
            occupancy = {"m1_plus": 0.0, "m2_plus": 0.0, "scene_plus": 0.0}
        return {
            "classification": result.__dict__,
            "raw_interior_occupancy_pct": occupancy,
            "score_stats": {
                "mean": float(np.mean(score)),
                "p50": float(np.quantile(score, 0.50)),
                "p90": float(np.quantile(score, 0.90)),
                "p99": float(np.quantile(score, 0.99)),
            },
            "normalized_q_stats": {
                "mean": float(np.mean(q)),
                "p50": float(np.quantile(q, 0.50)),
                "p90": float(np.quantile(q, 0.90)),
                "p99": float(np.quantile(q, 0.99)),
            },
        }

    return {
        "capture": capture.name,
        "flow_grid": [grid_w, grid_h],
        "direction_flags": 3,
        "pair_luma_denominator": 510,
        "forward_B_to_A": direction_stats(forward_score, forward_q, forward_result),
        "backward_A_to_B": direction_stats(backward_score, backward_q, backward_result),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, nargs="+")
    parser.add_argument("--write-maps", action="store_true")
    args = parser.parse_args()

    rows: list[dict[str, object]] = []
    for capture in args.captures:
        capture = capture.resolve()
        row = analyze_capture(capture)
        rows.append(row)

        if args.write_maps:
            metadata = read_metadata(capture)
            grid_w = int(metadata["flow_width"])
            grid_h = int(metadata["flow_height"])
            a_y = load_bt709_limited_y(capture / "frame-A.bmp")
            b_y = load_bt709_limited_y(capture / "frame-B.bmp")
            forward = load_raw_flow(
                capture, "flow-forward-B-to-A-s10.5.bin", grid_w, grid_h
            )
            backward = load_raw_flow(
                capture, "flow-backward-A-to-B-s10.5.bin", grid_w, grid_h
            )
            fs, fl = software_direction_score(b_y, a_y, forward)
            bs, bl = software_direction_score(a_y, b_y, backward)
            pl = build_pair_luma(bl, fl, marker=3, gamma=1.5)
            fq = normalized_q(fs, pl)
            bq = normalized_q(bs, pl)
            fs.astype("<u4").tofile(capture / "svp-modern-forward-score-u32.bin")
            bs.astype("<u4").tofile(capture / "svp-modern-backward-score-u32.bin")
            pl.astype("u1").tofile(capture / "svp-modern-pair-luma-u8.bin")
            fq.astype("<u4").tofile(capture / "svp-modern-forward-q-u32.bin")
            bq.astype("<u4").tofile(capture / "svp-modern-backward-q-u32.bin")

    print(json.dumps(rows, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
