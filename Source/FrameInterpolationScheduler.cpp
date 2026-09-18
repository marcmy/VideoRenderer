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
#include <cmath>
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

FrameRate SnapSourceRate(FrameRate rate) noexcept
{
    rate = ReduceRate(rate);
    if (!rate.IsValid()) {
        return {};
    }

    constexpr FrameRate commonRates[] = {
        {24, 1}, {25, 1}, {30, 1}, {50, 1}, {60, 1}, {120, 1},
        {24'000, 1'001},
        {30'000, 1'001},
        {60'000, 1'001},
    };
    constexpr long double relativeTolerance = 10.0e-6L; // 10 ppm

    const long double value = static_cast<long double>(rate.numerator) / rate.denominator;
    for (const auto candidate : commonRates) {
        const long double canonical = static_cast<long double>(candidate.numerator) / candidate.denominator;
        if (std::abs(value - canonical) / canonical <= relativeTolerance) {
            return candidate;
        }
    }
    return rate;
}

FrameRate ScaleRate(FrameRate rate, uint32_t numerator, uint32_t denominator = 1) noexcept
{
    if (!rate.IsValid() || numerator == 0 || denominator == 0) {
        return {};
    }

    uint64_t scaledNumerator = static_cast<uint64_t>(rate.numerator) * numerator;
    uint64_t scaledDenominator = static_cast<uint64_t>(rate.denominator) * denominator;
    const uint64_t divisor = std::gcd(scaledNumerator, scaledDenominator);
    scaledNumerator /= divisor;
    scaledDenominator /= divisor;
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
    const FrameRate displayRate,
    const uint32_t maxMultiplierMilli,
    const uint32_t maxOutputFpsMilli) noexcept
{
    m_mode = mode;
    m_customRate = ReduceRate(customRate);
    m_displayRate = ReduceRate(displayRate);
    m_maxMultiplierMilli = maxMultiplierMilli;
    m_maxOutputFpsMilli = maxOutputFpsMilli;
    Reset();
}

FrameRate CFrameInterpolationScheduler::ResolveRequestedRate(const FrameRate sourceRate) const noexcept
{
    const FrameRate multiplierSourceRate = SnapSourceRate(sourceRate);
    switch (m_mode) {
    case FrameInterpolationRateMode::ToScreen:
        return ReduceRate(m_displayRate);
    case FrameInterpolationRateMode::Movie2x:
        return ScaleRate(multiplierSourceRate, 2);
    case FrameInterpolationRateMode::Movie2_5x:
        return ScaleRate(multiplierSourceRate, 5, 2);
    case FrameInterpolationRateMode::Movie3x:
        return ScaleRate(multiplierSourceRate, 3);
    case FrameInterpolationRateMode::Movie4x:
        return ScaleRate(multiplierSourceRate, 4);
    case FrameInterpolationRateMode::Movie5x:
        return ScaleRate(multiplierSourceRate, 5);
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

FrameRate CFrameInterpolationScheduler::ResolveTargetRate(const FrameRate sourceRate) const noexcept
{
    FrameRate rate = ResolveRequestedRate(sourceRate);
    const auto capRate = [&](FrameRate cap) {
        if (cap.IsValid() && static_cast<uint64_t>(cap.numerator) * rate.denominator
                < static_cast<uint64_t>(rate.numerator) * cap.denominator) {
            rate = cap;
        }
    };
    if (m_maxMultiplierMilli) {
        // Reduce the multiplier before scaling to preserve rational NTSC rates
        // and avoid unnecessary overflow with 100ns source durations.
        const FrameRate multiplier = ReduceRate({m_maxMultiplierMilli, 1000});
        capRate(ScaleRate(SnapSourceRate(sourceRate), multiplier.numerator, multiplier.denominator));
    }
    if (m_maxOutputFpsMilli) {
        capRate(ReduceRate({m_maxOutputFpsMilli, 1000}));
    }
    return rate;
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

    // Container timestamps can be quantized to milliseconds even when the
    // nominal rate is rational. Treat a target within that precision of B as
    // the real frame, keeping its presentation on the uniform target grid.
    // Otherwise 2x needlessly infers both the midpoint and an almost-identical
    // endpoint. Include targets just beyond B too, so they are not inferred
    // again near t=0 in the following pair. Bound tolerance to a quarter of
    // either interval so genuine interior targets cannot collapse together.
    const int64_t targetPeriod = static_cast<int64_t>(
        ReferenceTicksPerSecond * targetRate.denominator / targetRate.numerator);
    const int64_t endpointTolerance = std::min({int64_t{10'000},
        (secondTime - firstTime) / 4, targetPeriod / 4});

    while (true) {
        const int64_t presentationTime = TargetTime(m_nextTargetIndex, targetRate);
        if (presentationTime == std::numeric_limits<int64_t>::max()) {
            break;
        }

        if (presentationTime <= firstTime) {
            ++m_nextTargetIndex;
            continue;
        }
        if (presentationTime > secondTime && presentationTime - secondTime > endpointTolerance) {
            break;
        }
        if (std::abs(presentationTime - secondTime) <= endpointTolerance) {
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
