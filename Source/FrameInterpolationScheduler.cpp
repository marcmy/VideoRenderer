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

#include "FrameInterpolationScheduler.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace {

constexpr uint64_t ReferenceTicksPerSecond = 10'000'000ULL;

FrameRate ReduceRate(FrameRate rate) noexcept
{
    if (!rate.IsValid()) {
        return {};
    }
    const uint32_t divisor = std::gcd(rate.numerator, rate.denominator);
    rate.numerator /= divisor;
    rate.denominator /= divisor;
    return rate;
}

FrameRate ScaleRate(FrameRate rate, uint32_t numerator, uint32_t denominator = 1) noexcept
{
    if (!rate.IsValid() || numerator == 0 || denominator == 0) {
        return {};
    }

    const uint64_t scaledNumerator = static_cast<uint64_t>(rate.numerator) * numerator;
    const uint64_t scaledDenominator = static_cast<uint64_t>(rate.denominator) * denominator;
    if (scaledNumerator > std::numeric_limits<uint32_t>::max()
            || scaledDenominator > std::numeric_limits<uint32_t>::max()) {
        return {};
    }

    return ReduceRate({
        static_cast<uint32_t>(scaledNumerator),
        static_cast<uint32_t>(scaledDenominator),
    });
}

bool SameRate(FrameRate first, FrameRate second) noexcept
{
    first = ReduceRate(first);
    second = ReduceRate(second);
    return first.numerator == second.numerator && first.denominator == second.denominator;
}

} // namespace

void CFrameInterpolationScheduler::Reset() noexcept
{
    m_activeTargetRate = {};
    m_anchorTime = 0;
    m_nextTargetIndex = 1;
    m_anchored = false;
}

void CFrameInterpolationScheduler::Configure(
    const FrameInterpolationRateMode mode,
    const FrameRate customRate,
    const FrameRate displayRate) noexcept
{
    m_mode = mode;
    m_customRate = ReduceRate(customRate);
    m_displayRate = ReduceRate(displayRate);
    Reset();
}

FrameRate CFrameInterpolationScheduler::ResolveTargetRate(const FrameRate sourceRate) const noexcept
{
    switch (m_mode) {
    case FrameInterpolationRateMode::ToScreen:
        return ReduceRate(m_displayRate);
    case FrameInterpolationRateMode::Movie2x:
        return ScaleRate(sourceRate, 2);
    case FrameInterpolationRateMode::Movie2_5x:
        return ScaleRate(sourceRate, 5, 2);
    case FrameInterpolationRateMode::Movie3x:
        return ScaleRate(sourceRate, 3);
    case FrameInterpolationRateMode::Movie4x:
        return ScaleRate(sourceRate, 4);
    case FrameInterpolationRateMode::Movie5x:
        return ScaleRate(sourceRate, 5);
    case FrameInterpolationRateMode::Fixed60:
        return {60, 1};
    case FrameInterpolationRateMode::Fixed72:
        return {72, 1};
    case FrameInterpolationRateMode::Fixed90:
        return {90, 1};
    case FrameInterpolationRateMode::Fixed120:
        return {120, 1};
    case FrameInterpolationRateMode::Custom:
        return ReduceRate(m_customRate);
    case FrameInterpolationRateMode::Disabled:
    default:
        return {};
    }
}

int64_t CFrameInterpolationScheduler::TargetTime(const uint64_t index, const FrameRate rate) const noexcept
{
    if (!m_anchored || !rate.IsValid()) {
        return m_anchorTime;
    }

    const uint64_t periodNumerator = ReferenceTicksPerSecond * rate.denominator;
    if (index > std::numeric_limits<uint64_t>::max() / periodNumerator) {
        return std::numeric_limits<int64_t>::max();
    }

    const uint64_t scaled = index * periodNumerator;
    const uint64_t roundedDelta = (scaled + rate.numerator / 2ULL) / rate.numerator;
    if (roundedDelta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    if (m_anchorTime > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(roundedDelta)) {
        return std::numeric_limits<int64_t>::max();
    }

    return m_anchorTime + static_cast<int64_t>(roundedDelta);
}

std::vector<FrameInterpolationTarget> CFrameInterpolationScheduler::Schedule(
    const int64_t firstTime,
    const int64_t secondTime,
    const FrameRate sourceRate)
{
    std::vector<FrameInterpolationTarget> targets;
    if (secondTime <= firstTime) {
        return targets;
    }

    const FrameRate targetRate = ResolveTargetRate(sourceRate);
    if (!targetRate.IsValid()) {
        return targets;
    }

    if (!m_anchored || !SameRate(targetRate, m_activeTargetRate)) {
        m_anchorTime = firstTime;
        m_nextTargetIndex = 1;
        m_activeTargetRate = targetRate;
        m_anchored = true;
    }

    while (true) {
        const int64_t presentationTime = TargetTime(m_nextTargetIndex, targetRate);
        if (presentationTime == std::numeric_limits<int64_t>::max()) {
            break;
        }

        if (presentationTime <= firstTime) {
            ++m_nextTargetIndex;
            continue;
        }
        if (presentationTime > secondTime) {
            break;
        }
        if (presentationTime == secondTime) {
            targets.push_back({presentationTime, 1.0, true});
            ++m_nextTargetIndex;
            break;
        }

        const double timestep = static_cast<double>(presentationTime - firstTime)
            / static_cast<double>(secondTime - firstTime);
        targets.push_back({presentationTime, std::clamp(timestep, 0.0, 1.0), false});
        ++m_nextTargetIndex;
    }

    return targets;
}
