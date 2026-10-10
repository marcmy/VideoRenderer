#pragma once

#include "RifeSpatialPolicy.h"

// NV12's two chroma planes require even storage dimensions. This is storage
// alignment only: inference geometry and the visible crop are unchanged.
constexpr RifeSpatialSize RifeVsrNv12SurfaceSize(const RifeSpatialSize input) noexcept
{
    constexpr uint32_t maxTextureDimension = 16384u;
    if (!input.width || !input.height
            || input.width > maxTextureDimension || input.height > maxTextureDimension) return {};
    return AlignRifeSize(input, 2u);
}

// RIFE uses square-pixel content. For a compressed anamorphic axis, restore
// the encoded pixel proportions in the existing RGB -> NV12 pass, before the
// driver performs the final display resize. Preserve the selected processing
// scale, rotation and the working routes whose axis was not compressed.
constexpr RifeSpatialSize ResolveRifeVsrContentSize(const RifeSpatialSize content,
        const RifeSpatialSize encodedSource, const bool anamorphic, const int rotation) noexcept
{
    if (!anamorphic || !content.width || !content.height
            || !encodedSource.width || !encodedSource.height) return content;
    const bool quarterTurn = rotation == 90 || rotation == 270;
    const uint32_t unchangedAxis = quarterTurn ? content.width : content.height;
    const uint32_t compressedAxis = quarterTurn ? content.height : content.width;
    const uint64_t restoredAxis = (static_cast<uint64_t>(unchangedAxis) * encodedSource.width
        + encodedSource.height / 2u) / encodedSource.height;
    if (restoredAxis <= compressedAxis) return content;
    if (restoredAxis > 16384u) return {};
    return quarterTurn ? RifeSpatialSize{content.width, static_cast<uint32_t>(restoredAxis)}
        : RifeSpatialSize{static_cast<uint32_t>(restoredAxis), content.height};
}

constexpr bool CanUseRifeVideoProcessorUpscale(const bool nativeVsrRequested,
        const bool maxineSelected, const bool supportedRgbSdr,
        const RifeSpatialSize content, const RifeSpatialSize output) noexcept
{
    return nativeVsrRequested && !maxineSelected && supportedRgbSdr
        && content.width && content.height
        && output.width > content.width && output.height > content.height;
}
