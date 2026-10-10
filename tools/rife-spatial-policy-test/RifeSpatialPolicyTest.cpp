#include <cassert>
#include <iostream>
#include <random>

#include "../../Source/RifeSpatialPolicy.h"

int main()
{
    assert((ResolveRifeWorkingSize({1920, 1080}, {1280, 720}, RIFE_RESOLUTION_Source, 720) == RifeSpatialSize{1920, 1080}));
    assert((ResolveRifeWorkingSize({1920, 1080}, {1280, 720}, RIFE_RESOLUTION_Display, 720) == RifeSpatialSize{1280, 720}));
    assert((ResolveRifeWorkingSize({1080, 1920}, {608, 1080}, RIFE_RESOLUTION_Display, 720) == RifeSpatialSize{607, 1080}));
    assert((ResolveRifeWorkingSize({1920, 1080}, {}, RIFE_RESOLUTION_Display, 720) == RifeSpatialSize{1920, 1080}));
    assert((ResolveRifeWorkingSize({854, 480}, {1920, 1080}, RIFE_RESOLUTION_Display, 720) == RifeSpatialSize{854, 480}));
    assert((ResolveRifeWorkingSize({1080, 1920}, {}, RIFE_RESOLUTION_Limit, 720) == RifeSpatialSize{720, 1280}));
    assert((ResolveRifeWorkingSize({1920, 804}, {}, RIFE_RESOLUTION_Limit, 720) == RifeSpatialSize{1719, 720}));
    assert((ResolveRifeWorkingSize({854, 480}, {}, RIFE_RESOLUTION_Limit, 720) == RifeSpatialSize{854, 480}));
    assert((ResolveRifeWorkingSize({1920, 1080}, {}, RIFE_RESOLUTION_Limit, 0) == RifeSpatialSize{1920, 1080}));
    assert((ResolveRifeWorkingSize({1920, 1080}, {}, -1, 720) == RifeSpatialSize{1920, 1080}));
    assert((ResolveRifeWorkingSize({}, {1920, 1080}, RIFE_RESOLUTION_Display, 720) == RifeSpatialSize{}));
    const auto rotated = ResolveRifeContentSize(720, 480, true, 16, 9, 90);
    assert((ResolveRifeWorkingSize(rotated, {}, RIFE_RESOLUTION_Limit, 240) == RifeSpatialSize{240, 426}));
    const auto limit720 = ResolveRifeWorkingSize({1920, 1080}, {}, RIFE_RESOLUTION_Limit, 720);
    assert((AlignRifeSize(limit720, 32) == RifeSpatialSize{1280, 736}));
    assert((AlignRifeSize(limit720, 128) == RifeSpatialSize{1280, 768}));

    std::mt19937 random(2832);
    for (int i = 0; i < 20000; ++i) {
        const RifeSpatialSize source{1u + random() % 32768u, 1u + random() % 32768u};
        const RifeSpatialSize display{1u + random() % 8192u, 1u + random() % 8192u};
        const auto fit = ResolveRifeWorkingSize(source, display, RIFE_RESOLUTION_Display, 720);
        assert(fit.width && fit.height && fit.width <= source.width && fit.height <= source.height);
        assert(fit.width <= display.width && fit.height <= display.height);
        // Less than one pixel of aspect rounding (minimum-one-pixel thin images excepted).
        if (fit.width > 1 && fit.height > 1) {
            const int64_t error = static_cast<int64_t>(fit.width) * source.height
                - static_cast<int64_t>(fit.height) * source.width;
            assert(error < source.width && error > -static_cast<int64_t>(source.height));
        }
        assert(ResolveRifeWorkingSize(source, display, RIFE_RESOLUTION_Source, 720) == source);
        const int limit = 64 + static_cast<int>(random() % (4320u - 64u + 1u));
        const auto capped = ResolveRifeWorkingSize(source, display, RIFE_RESOLUTION_Limit, limit);
        assert(capped.width && capped.height && capped.width <= source.width && capped.height <= source.height);
        assert(std::min(capped.width, capped.height) <= static_cast<uint32_t>(limit));
        for (const uint32_t alignment : {32u, 128u}) {
            const auto padded = AlignRifeSize(capped, alignment);
            assert(padded.width >= capped.width && padded.height >= capped.height);
            assert(padded.width % alignment == 0 && padded.height % alignment == 0);
            assert(padded.width - capped.width < alignment && padded.height - capped.height < alignment);
        }
    }
    {
        const auto size = ResolveRifeContentSize(1920, 1080, false, 0, 0, 0);
        assert(size.width == 1920 && size.height == 1080);
        const auto aligned = AlignRifeSize(size);
        assert(aligned.width == 1920 && aligned.height == 1088);
        const auto aligned128 = AlignRifeSize(size, 128);
        assert(aligned128.width == 1920 && aligned128.height == 1152);
        assert(AlignRifeDimension(576, 128) == 640);
    }

    {
        const auto size = ResolveRifeContentSize(3840, 2160, false, 0, 0, 0);
        assert(size.width == 3840 && size.height == 2160);
        const auto aligned = AlignRifeSize(size);
        assert(aligned.width == 3840 && aligned.height == 2176);
    }

    {
        const auto size = ResolveRifeContentSize(720, 480, true, 16, 9, 0);
        assert(size.width == 853 && size.height == 480);

        const auto rotated90 = ResolveRifeContentSize(720, 480, true, 16, 9, 90);
        const auto rotated270 = ResolveRifeContentSize(720, 480, true, 16, 9, 270);
        assert(rotated90.width == 480 && rotated90.height == 853);
        assert(rotated270.width == 480 && rotated270.height == 853);

        const auto aligned = AlignRifeSize(rotated90);
        assert(aligned.width == 480 && aligned.height == 864);

        const auto vpScaled = ResolveRifeVpIntermediateSize(
            {720, 480}, rotated90, true, false, 90);
        assert(vpScaled.width == 853 && vpScaled.height == 480);

        const auto shaderScaled = ResolveRifeVpIntermediateSize(
            {720, 480}, rotated90, false, false, 90);
        assert(shaderScaled.width == 720 && shaderScaled.height == 480);

        const auto maxineInput = ResolveRifeVpIntermediateSize(
            {720, 480}, rotated90, true, true, 90);
        assert(maxineInput.width == 720 && maxineInput.height == 480);
    }

    {
        const auto invalidAspect = ResolveRifeContentSize(640, 360, true, 16, 0, 0);
        assert(invalidAspect.width == 640 && invalidAspect.height == 360);
    }

    std::cout << "RIFE spatial policy: fixed fixtures and 20000 randomized geometry cases passed\n";
    return 0;
}
