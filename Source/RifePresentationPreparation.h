#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// An original frame can be just as obsolete as a synthetic one after a stall.
// Only discard it when a strictly newer frame from this segment is ready now.
inline bool RifePresentationSuperseded(const uint64_t generation, const int64_t time,
    const uint64_t nextGeneration, const int64_t nextTime,
    const int64_t nextDeadline, const int64_t now)
{
    return generation == nextGeneration && nextTime > time && nextDeadline <= now;
}

// One frame may be enhanced in advance. Keep the lead within one output
// interval and 16 ms so preparation cannot run an unbounded GPU queue ahead.
inline int64_t RifePresentationPreparationLead(const int64_t frameInterval,
    const double enhancementMs, const bool enabled)
{
    if (!enabled || frameInterval <= 0 || !std::isfinite(enhancementMs) || enhancementMs < 0) {
        return 0;
    }
    constexpr int64_t maximumLead = 160000; // 100 ns units: 16 ms.
    const double requested = std::min<double>(maximumLead, (enhancementMs + 1.0) * 10000.0);
    return std::min<int64_t>(frameInterval, static_cast<int64_t>(std::ceil(requested)));
}

struct RifePreparedPresentationKey {
    uint64_t generation = 0;
    int64_t time = 0;
    uint32_t surface = UINT32_MAX;

    void Clear() { surface = UINT32_MAX; }
    bool Matches(const uint64_t currentGeneration, const int64_t currentTime,
        const uint32_t currentSurface) const
    {
        return surface != UINT32_MAX && surface == currentSurface
            && time == currentTime && generation == currentGeneration;
    }
};
