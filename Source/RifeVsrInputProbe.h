#pragma once

#include "RifeSpatialPolicy.h"

// A/B is meaningful only when the bridge and decoder have identical visible
// and storage geometry. Reduced inference, crops and rotations are excluded.
constexpr bool CanCompareRifeVsrInputs(const RifeSpatialSize driverContent,
        const RifeSpatialSize encodedContent, const RifeSpatialSize decoderStorage,
        const int rotation, const bool progressive, const bool bt709Limited,
        const bool fullSourceRect) noexcept
{
    return rotation == 0 && progressive && bt709Limited && fullSourceRect
        && encodedContent.width && encodedContent.height
        && driverContent == encodedContent
        && decoderStorage.width >= encodedContent.width
        && decoderStorage.height >= encodedContent.height
        && decoderStorage.width <= 16384u && decoderStorage.height <= 16384u
        && !(decoderStorage.width & 1u) && !(decoderStorage.height & 1u);
}
