#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

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

constexpr RifeEngineProfile GetRifeEngineProfile(uint32_t width, uint32_t height, bool boost) noexcept
{
    return boost ? RifeEngineProfile{width, height, width, height, width, height}
        : RifeEngineProfile{std::min(128u, width), std::min(128u, height),
            kRifeDynamicOptWidth, kRifeDynamicOptHeight,
            std::max(3840u, width), std::max(2176u, height)};
}

inline std::string RifeEngineProfileCacheSuffix(uint32_t width, uint32_t height, bool boost)
{
    if (boost) return "_" + std::to_string(width) + 'x' + std::to_string(height) + "_static";
    const auto profile = GetRifeEngineProfile(width, height, false);
    std::string suffix = "_dynamic_max" + std::to_string(profile.maxWidth) + 'x' + std::to_string(profile.maxHeight)
        + "_opt" + std::to_string(profile.optWidth) + 'x' + std::to_string(profile.optHeight);
    // Keep the existing >=128-pixel cache namespace. A small profile cannot
    // reuse those plans, which exclude its current dimensions.
    if (profile.minWidth < 128 || profile.minHeight < 128) {
        suffix += "_min" + std::to_string(profile.minWidth) + 'x' + std::to_string(profile.minHeight);
    }
    return suffix;
}
