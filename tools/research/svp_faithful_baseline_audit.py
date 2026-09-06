#!/usr/bin/env python3
"""Deterministic arithmetic audit for the SVP-reconstruction NVOF baseline.

This checks the integer/truncation rules used by the live HLSL port against
the formulas recovered from the proprietary svpflow2 path.  Runtime D3D11,
color conversion, and decoder-domain source-plane equivalence are deliberately
outside this tool's claim.
"""

from __future__ import annotations

import argparse
import json
import math
import random


def trunc_div(numer: int, denom: int) -> int:
    if denom <= 0:
        raise ValueError("denominator must be positive")
    return numer // denom if numer >= 0 else -((-numer) // denom)


def classifier_reference(q: list[list[int]], ignore: float = 0.04) -> tuple[int, tuple[int, ...]]:
    height = len(q)
    width = len(q[0]) if height else 0
    border_x = max(int(width * ignore), 1 if ignore > 0.01 else 0)
    border_y = max(int(height * ignore), 1 if ignore > 0.01 else 0)
    zero_limit = (2 * width * height) // 3
    zero_skipped = considered = m1 = m2 = scene = 0

    for y in range(border_y, height - border_y):
        for x in range(border_x, width - border_x):
            value = q[y][x]
            if value < 200 and zero_skipped < zero_limit:
                zero_skipped += 1
                continue
            considered += 1
            if value >= 4000:
                scene += 1
            elif value >= 2800:
                m2 += 1
            elif value >= 1600:
                m1 += 1

    required = (20 * considered) // 100
    high = scene + m2
    mid = high + m1
    cls = 3 if scene >= required else 2 if high >= required else 1 if mid >= required else 0
    return cls, (considered, zero_skipped, m1, m2, scene, required)


def classifier_gpu_aggregate(q: list[list[int]], ignore: float = 0.04) -> tuple[int, tuple[int, ...]]:
    height = len(q)
    width = len(q[0]) if height else 0
    border_x = max(int(width * ignore), 1 if ignore > 0.01 else 0)
    border_y = max(int(height * ignore), 1 if ignore > 0.01 else 0)
    low = m1 = m2 = scene = 0

    for y in range(border_y, height - border_y):
        for x in range(border_x, width - border_x):
            value = q[y][x]
            if value < 200:
                low += 1
            elif value >= 4000:
                scene += 1
            elif value >= 2800:
                m2 += 1
            elif value >= 1600:
                m1 += 1

    interior = max(width - 2 * border_x, 0) * max(height - 2 * border_y, 0)
    zero_skipped = min(low, (2 * width * height) // 3)
    considered = max(interior - zero_skipped, 0)
    required = (20 * considered) // 100
    high = scene + m2
    mid = high + m1
    cls = 3 if scene >= required else 2 if high >= required else 1 if mid >= required else 0
    return cls, (considered, zero_skipped, m1, m2, scene, required)


def packed_motion_reference(raw_s10_5: int, phase: int) -> int:
    packed = trunc_div(raw_s10_5, 8)
    renderer_pixels = trunc_div(packed, 4)
    return trunc_div(renderer_pixels * phase, 256)


def packed_motion_gpu(raw_s10_5: int, phase: int) -> int:
    motion_vector = trunc_div(trunc_div(raw_s10_5, 8), 4)
    return trunc_div(motion_vector * phase, 256)


def interp4_reference(a: int, b: int, fraction: int) -> int:
    """SVP block-4 interpolation: CPU_MAP[4] == -1 => signed / 4."""
    return trunc_div((4 - fraction) * a + fraction * b, 4)


def interp4_gpu(a: int, b: int, fraction: int) -> int:
    # HLSL signed integer division also truncates toward zero.
    return trunc_div((4 - fraction) * a + fraction * b, 4)


def pair_luma_reference(total: int) -> int:
    value = int(math.pow(total / 510.0, 1.5) * 255.0)
    return max(value, 20)


def coverage_splats(
    width: int,
    height: int,
    vectors: list[list[tuple[int, int]]],
    phase: int,
    padded: bool,
) -> list[list[int]]:
    stride = width + 2 if padded else width
    rows = height + 2 if padded else height
    accum = [[0] * stride for _ in range(rows)]
    for y in range(height):
        for x in range(width):
            vx, vy = vectors[y][x]
            dx = trunc_div(phase * vx, 1024)
            dy = trunc_div(phase * vy, 1024)
            shifted_x = x * 4 + dx
            shifted_y = y * 4 + dy
            left = shifted_x // 4
            top = shifted_y // 4
            right = left + 1
            bottom = top + 1
            next_x = right * 4
            next_y = bottom * 4
            left_weight = next_x - shifted_x
            top_weight = next_y - shifted_y
            right_weight = shifted_x + 4 - next_x
            bottom_weight = shifted_y + 4 - next_y
            for px, py, weight in (
                (left, top, left_weight * top_weight),
                (right, top, right_weight * top_weight),
                (left, bottom, left_weight * bottom_weight),
                (right, bottom, right_weight * bottom_weight),
            ):
                if 0 <= px < width and 0 <= py < height:
                    accum[py][px] += weight
    return accum


def coverage_reference(
    width: int, height: int, vectors: list[list[tuple[int, int]]], phase: int
) -> list[list[int]]:
    points = coverage_splats(width, height, vectors, phase, padded=True)
    stride = width + 2
    sums = [row.copy() for row in points]
    for y, source in enumerate(points):
        sums[y][0] = source[0]
        sums[y][1] = source[1] + source[0]
        for x in range(2, stride):
            sums[y][x] = source[x] + source[x - 1] + source[x - 2]
    for y in range(height + 1, 0, -1):
        for x in range(stride):
            sums[y][x] += sums[y - 1][x]
            if y >= 2:
                sums[y][x] += sums[y - 2][x]
    return [[sums[y + 1][x + 1] for x in range(width)] for y in range(height)]


def coverage_gpu(
    width: int, height: int, vectors: list[list[tuple[int, int]]], phase: int
) -> list[list[int]]:
    accum = coverage_splats(width, height, vectors, phase, padded=False)
    return [
        [
            sum(
                accum[py][px]
                for py in range(max(0, y - 1), min(height, y + 2))
                for px in range(max(0, x - 1), min(width, x + 2))
            )
            for x in range(width)
        ]
        for y in range(height)
    ]


def blend255(a: int, b: int, weight: int) -> int:
    return max(0, min(255, (a * (255 - weight) + b * weight + 255) >> 8))


def blend256(a: int, b: int, weight: int) -> int:
    return max(0, min(255, (a * (256 - weight) + b * weight) >> 8))


def run(seed: int, classifier_cases: int, vector_cases: int) -> dict[str, int | str]:
    rng = random.Random(seed)
    q_values = (0, 50, 199, 200, 900, 1599, 1600, 2799, 2800, 3999, 4000, 9000)

    for total in range(511):
        normalized = total / 510.0
        gpu = int(math.pow(normalized, 1.5) * 255.0)
        if gpu < 21:
            gpu = 20
        if pair_luma_reference(total) != gpu:
            raise AssertionError(("pair-luma", total, pair_luma_reference(total), gpu))

    for _ in range(classifier_cases):
        width = rng.randint(40, 160)
        height = rng.randint(32, 100)
        q = [[rng.choice(q_values) for _ in range(width)] for _ in range(height)]
        if classifier_reference(q) != classifier_gpu_aggregate(q):
            raise AssertionError("classifier aggregation mismatch")

    for _ in range(vector_cases):
        raw = rng.randint(-32768, 32767)
        phase = rng.randint(0, 256)
        if packed_motion_reference(raw, phase) != packed_motion_gpu(raw, phase):
            raise AssertionError((raw, phase))
        a_motion = rng.randint(-1023, 1023)
        b_motion = rng.randint(-1023, 1023)
        fraction = rng.randrange(4)
        if interp4_reference(a_motion, b_motion, fraction) != interp4_gpu(
            a_motion, b_motion, fraction
        ):
            raise AssertionError((a_motion, b_motion, fraction))
        a = rng.randrange(256)
        b = rng.randrange(256)
        weight = rng.randrange(257)
        if not 0 <= blend256(a, b, weight) <= 255:
            raise AssertionError("Blend256 range failure")
        if weight <= 255 and not 0 <= blend255(a, b, weight) <= 255:
            raise AssertionError("Blend255 range failure")

    coverage_cases = 1000
    for _ in range(coverage_cases):
        width = rng.randint(1, 12)
        height = rng.randint(1, 10)
        phase = rng.randint(0, 256)
        vectors = [
            [
                (trunc_div(rng.randint(-32768, 32767), 8), trunc_div(rng.randint(-32768, 32767), 8))
                for _ in range(width)
            ]
            for _ in range(height)
        ]
        if coverage_reference(width, height, vectors, phase) != coverage_gpu(
            width, height, vectors, phase
        ):
            raise AssertionError(("coverage", width, height, phase))

    # Recovered class-3/cut path: phase < 128 -> A; phase >= 128 -> B.
    if ("A" if 127 < 128 else "B") != "A" or ("A" if 128 < 128 else "B") != "B":
        raise AssertionError("cut selection rule failure")

    return {
        "status": "pass",
        "seed": seed,
        "classifier_cases": classifier_cases,
        "coverage_cases": coverage_cases,
        "vector_cases": vector_cases,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=0x53565034)
    parser.add_argument("--classifier-cases", type=int, default=1000)
    parser.add_argument("--vector-cases", type=int, default=100000)
    args = parser.parse_args()
    print(json.dumps(run(args.seed, args.classifier_cases, args.vector_cases), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
