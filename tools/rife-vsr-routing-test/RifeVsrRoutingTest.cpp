#include <cassert>
#include <cstdint>
#include <iostream>
#include "../../Source/RifeVideoProcessorPolicy.h"

int main()
{
    // Exact reported clip: preserve RIFE's content and padding, but restore
    // the encoded proportions for the driver input. Final display size is
    // independent of the declared source aspect and the NV12 input size.
    const auto content = ResolveRifeContentSize(720, 480, true, 180, 311, 0);
    assert((content == RifeSpatialSize{278, 480}));
    const auto padded = AlignRifeSize(content, 32);
    assert((padded == RifeSpatialSize{288, 480}));
    assert(RifeVsrNv12SurfaceSize(padded) == padded);
    assert(CanUseRifeVideoProcessorUpscale(true, false, true, content, {626, 1080}));
    const auto driverContent = ResolveRifeVsrContentSize(content, {720, 480}, true, 0);
    assert((driverContent == RifeSpatialSize{720, 480}));
    assert(RifeVsrNv12SurfaceSize(driverContent) == driverContent);
    assert(CanUseRifeVideoProcessorUpscale(true, false, true, content, {1920, 1080}));

    // A source declared as 16:9 has different inference geometry. A player
    // display override alone does not change the source metadata above.
    const auto widescreen = ResolveRifeContentSize(720, 480, true, 16, 9, 0);
    assert((widescreen == RifeSpatialSize{853, 480}));
    assert(CanUseRifeVideoProcessorUpscale(true, false, true, widescreen, {1920, 1080}));
    assert(ResolveRifeVsrContentSize(widescreen, {720, 480}, true, 0) == widescreen);
    assert(!CanUseRifeVideoProcessorUpscale(true, true, true, content, {626, 1080}));
    assert(!CanUseRifeVideoProcessorUpscale(true, false, false, content, {626, 1080}));
    assert(!CanUseRifeVideoProcessorUpscale(false, false, true, content, {626, 1080}));

    // Preserve the working portrait downscale route; no new presentation pass.
    assert(!CanUseRifeVideoProcessorUpscale(true, false, true, {720, 1280}, {608, 1080}));

    // Preserve the confirmed working 720x406 NV12 route and square-pixel
    // inputs. TensorRT padding must not affect the restored pixel proportions.
    const auto workingClip = ResolveRifeContentSize(720, 406, true, 45900, 25781, 0);
    assert((workingClip == RifeSpatialSize{723, 406}));
    assert(ResolveRifeVsrContentSize(workingClip, {720, 406}, true, 0) == workingClip);
    assert((ResolveRifeVsrContentSize({1280, 720}, {1280, 720}, false, 0)
        == RifeSpatialSize{1280, 720}));
    assert((ResolveRifeVsrContentSize({278, 480}, {720, 480}, false, 0) == content));

    // Custom/display processing sizes retain their scale rather than growing
    // the driver's input all the way back to the original resolution.
    const auto reduced = ResolveRifeWorkingSize(content, {}, RIFE_RESOLUTION_Limit, 128);
    assert((reduced == RifeSpatialSize{128, 221}));
    const auto reducedDriver = ResolveRifeVsrContentSize(reduced, {720, 480}, true, 0);
    assert((reducedDriver == RifeSpatialSize{332, 221}));
    assert((RifeVsrNv12SurfaceSize(reducedDriver) == RifeSpatialSize{332, 222}));

    for (const int rotation : {0, 90, 180, 270}) {
        const bool quarterTurn = rotation == 90 || rotation == 270;
        const auto oriented = ResolveRifeContentSize(720, 480, true, 180, 311, rotation);
        const auto orientedDriver = ResolveRifeVsrContentSize(oriented, {720, 480}, true, rotation);
        assert((orientedDriver == (quarterTurn ? RifeSpatialSize{480, 720} : RifeSpatialSize{720, 480})));
        // Vary the processing scale, including odd dimensions, while keeping
        // the unchanged axis exact and the restored axis within source size.
        for (uint32_t height = 64; height <= 480; ++height) {
            const uint32_t width = static_cast<uint32_t>(static_cast<uint64_t>(height) * 278 / 480);
            const RifeSpatialSize scaled = quarterTurn ? RifeSpatialSize{height, width}
                : RifeSpatialSize{width, height};
            const auto result = ResolveRifeVsrContentSize(scaled, {720, 480}, true, rotation);
            const auto storage = RifeVsrNv12SurfaceSize(result);
            assert(storage.width >= result.width && storage.height >= result.height);
            assert(storage.width % 2 == 0 && storage.height % 2 == 0);
            assert(quarterTurn ? result.width == height : result.height == height);
            const uint32_t restoredAxis = quarterTurn ? result.height : result.width;
            assert(restoredAxis > width && restoredAxis <= 720);
            assert(static_cast<uint64_t>(restoredAxis) * 480 <= static_cast<uint64_t>(height) * 720 + 240);
            assert(static_cast<uint64_t>(restoredAxis) * 480 + 240 >= static_cast<uint64_t>(height) * 720);
        }
    }
    assert(ResolveRifeVsrContentSize(content, {}, true, 0) == content);
    assert((ResolveRifeVsrContentSize({}, {720, 480}, true, 0) == RifeSpatialSize{}));
    assert((ResolveRifeVsrContentSize({1, 16384}, {UINT32_MAX, 1}, true, 0) == RifeSpatialSize{}));

    // Storage alignment must not stretch odd visible crops or wrap at limits.
    assert((RifeVsrNv12SurfaceSize({279, 481}) == RifeSpatialSize{280, 482}));
    assert((RifeVsrNv12SurfaceSize({16383, 16383}) == RifeSpatialSize{16384, 16384}));
    assert((RifeVsrNv12SurfaceSize({16384, 16384}) == RifeSpatialSize{16384, 16384}));
    assert((RifeVsrNv12SurfaceSize({0, 480}) == RifeSpatialSize{}));
    assert((RifeVsrNv12SurfaceSize({UINT32_MAX, 480}) == RifeSpatialSize{}));
    assert((RifeVsrNv12SurfaceSize({480, 16385}) == RifeSpatialSize{}));

    std::cout << "RIFE VSR encoded proportions, 1668 scaled/rotated cases and routing tests passed\n";
}
