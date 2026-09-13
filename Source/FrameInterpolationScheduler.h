/*
 * (C) 2018-2026 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <vector>

struct FrameRate {
    uint32_t numerator = 0;
    uint32_t denominator = 1;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return numerator != 0 && denominator != 0;
    }
};

enum class FrameInterpolationRateMode : uint8_t {
    Disabled = 0,
    ToScreen,
    Movie2x,
    Movie2_5x,
    Movie3x,
    Movie4x,
    Movie5x,
    Fixed60,
    Fixed72,
    Fixed90,
    Fixed120,
    Custom,
};

struct FrameInterpolationTarget {
    int64_t presentationTime = 0;
    double timestep = 0.0;
    bool exactSource = false; // Use the real second frame within source timestamp precision.
};

class CFrameInterpolationScheduler {
public:
    void Reset() noexcept;
    void Configure(FrameInterpolationRateMode mode, FrameRate customRate, FrameRate displayRate) noexcept;

    [[nodiscard]] std::vector<FrameInterpolationTarget> Schedule(
        int64_t firstTime,
        int64_t secondTime,
        FrameRate sourceRate);

private:
    [[nodiscard]] FrameRate ResolveTargetRate(FrameRate sourceRate) const noexcept;
    [[nodiscard]] int64_t TargetTime(uint64_t index, FrameRate rate) const noexcept;

    FrameInterpolationRateMode m_mode = FrameInterpolationRateMode::Disabled;
    FrameRate m_customRate = {};
    FrameRate m_displayRate = {};
    FrameRate m_activeTargetRate = {};
    int64_t m_anchorTime = 0;
    uint64_t m_nextTargetIndex = 1;
    bool m_anchored = false;
};
