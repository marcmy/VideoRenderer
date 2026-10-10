#pragma once

// Source/output pools can reuse a texture from any cached geometry. CUDA
// registration ownership must therefore be checked against every cache entry,
// not only the previously active runtime. Keep the exact key and other sizes.
template <typename Key>
constexpr bool RifeRuntimeCacheConflicts(const Key& cached, const Key& target) noexcept
{
    return cached.device == target.device
        && cached.width == target.width
        && cached.height == target.height
        && !(cached == target);
}
