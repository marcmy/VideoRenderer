#include <cassert>
#include <iostream>

#include "../../Source/RifeSpatialPolicy.h"

int main()
{
    {
        const auto size = ResolveRifeContentSize(1920, 1080, false, 0, 0, 0);
        assert(size.width == 1920 && size.height == 1080);
        const auto aligned = AlignRifeSize(size);
        assert(aligned.width == 1920 && aligned.height == 1088);
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

    std::cout << "All RIFE spatial policy tests passed\n";
    return 0;
}
