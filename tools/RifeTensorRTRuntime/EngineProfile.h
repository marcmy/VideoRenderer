#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

struct RifeEngineProfile final {
    uint32_t minWidth, minHeight, maxWidth, maxHeight;
};

constexpr RifeEngineProfile GetRifeEngineProfile(uint32_t width, uint32_t height, bool boost) noexcept
{
    return boost ? RifeEngineProfile{width, height, width, height}
        : RifeEngineProfile{std::min(128u, width), std::min(128u, height),
            std::max(3840u, width), std::max(2176u, height)};
}

inline std::string RifeEngineProfileCacheSuffix(uint32_t width, uint32_t height, bool boost)
{
    if (boost) return "_" + std::to_string(width) + 'x' + std::to_string(height) + "_static";
    const auto profile = GetRifeEngineProfile(width, height, false);
    std::string suffix = "_dynamic_max" + std::to_string(profile.maxWidth) + 'x' + std::to_string(profile.maxHeight);
    // Keep the existing >=128-pixel cache namespace. A small profile cannot
    // reuse those plans, which exclude its current dimensions.
    if (profile.minWidth < 128 || profile.minHeight < 128) {
        suffix += "_min" + std::to_string(profile.minWidth) + 'x' + std::to_string(profile.minHeight);
    }
    return suffix;
}
