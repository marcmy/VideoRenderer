#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

struct RifeEngineProfile final {
    uint32_t minWidth, minHeight, optWidth, optHeight, maxWidth, maxHeight;
};

// The shared dynamic plan is tuned for padded 1080p regardless of the source
// that first builds it. Using the current size made the plan's tactics depend
// on whichever video happened to open first: a plan first built for 384x480
// measured 13.55 ms at 1920x1088 on an RTX 2070 SUPER versus 12.18 ms for this
// optimization point (and 12.09 ms for a fixed-shape Boost plan).
constexpr uint32_t kRifeDynamicOptWidth = 1920;
constexpr uint32_t kRifeDynamicOptHeight = 1088;
// Models with internal 128-pixel padding (RIFE 4.25 Lite) reject 1088 rows as
// an optimization point, so their shared plan is tuned for 1080p padded to 128.
constexpr uint32_t kRifeDynamicOptHeight128 = 1152;

constexpr RifeEngineProfile GetRifeEngineProfile(uint32_t width, uint32_t height, bool boost) noexcept
{
    return boost ? RifeEngineProfile{width, height, width, height, width, height}
        : RifeEngineProfile{std::min(128u, width), std::min(128u, height),
            kRifeDynamicOptWidth, kRifeDynamicOptHeight,
            std::max(3840u, width), std::max(2176u, height)};
}

// Profiles to load or build, in order. A model whose shape constraints reject
// one optimization point falls back to the next; plans for the first point
// keep their existing cache names.
inline std::vector<RifeEngineProfile> GetRifeEngineProfileCandidates(uint32_t width, uint32_t height, bool boost)
{
    const auto primary = GetRifeEngineProfile(width, height, boost);
    if (boost) return {primary};
    auto aligned = primary;
    aligned.optHeight = kRifeDynamicOptHeight128;
    return {primary, aligned};
}

inline std::string RifeEngineProfileCacheSuffix(const RifeEngineProfile& profile, bool boost)
{
    if (boost) return "_" + std::to_string(profile.optWidth) + 'x' + std::to_string(profile.optHeight) + "_static";
    std::string suffix = "_dynamic_max" + std::to_string(profile.maxWidth) + 'x' + std::to_string(profile.maxHeight)
        + "_opt" + std::to_string(profile.optWidth) + 'x' + std::to_string(profile.optHeight);
    // Keep the existing >=128-pixel cache namespace. A small profile cannot
    // reuse those plans, which exclude its current dimensions.
    if (profile.minWidth < 128 || profile.minHeight < 128) {
        suffix += "_min" + std::to_string(profile.minWidth) + 'x' + std::to_string(profile.minHeight);
    }
    return suffix;
}

inline std::string RifeEngineProfileCacheSuffix(uint32_t width, uint32_t height, bool boost)
{
    return RifeEngineProfileCacheSuffix(GetRifeEngineProfile(width, height, boost), boost);
}
