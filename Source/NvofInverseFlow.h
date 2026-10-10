#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Forward flow lives in the input frame. Its inverse lives at the mapped
// position in the reference frame, not at the same array index. The reader
// supplies backward vectors in pixels at integer flow-grid coordinates.
template<class ReadBackward>
bool SampleNvofInverseFlow(
    const uint32_t x, const uint32_t y, const float forwardX, const float forwardY,
    const uint32_t width, const uint32_t height, const uint32_t gridSize,
    ReadBackward&& readBackward, float& backwardX, float& backwardY) noexcept
{
    if (!width || !height || !gridSize || x >= width || y >= height
            || !std::isfinite(forwardX) || !std::isfinite(forwardY)) {
        return false;
    }
    const float mappedX = static_cast<float>(x) + forwardX / gridSize;
    const float mappedY = static_cast<float>(y) + forwardY / gridSize;
    if (!std::isfinite(mappedX) || !std::isfinite(mappedY)
            || mappedX < 0.0f || mappedY < 0.0f
            || mappedX > width - 1.0f || mappedY > height - 1.0f) {
        // Out-of-view correspondences cannot establish inverse consistency.
        return false;
    }

    const auto x0 = static_cast<uint32_t>(mappedX);
    const auto y0 = static_cast<uint32_t>(mappedY);
    const auto x1 = std::min(x0 + 1, width - 1);
    const auto y1 = std::min(y0 + 1, height - 1);
    const float dx = mappedX - x0;
    const float dy = mappedY - y0;
    const auto a = readBackward(x0, y0);
    const auto b = readBackward(x1, y0);
    const auto c = readBackward(x0, y1);
    const auto d = readBackward(x1, y1);
    const auto interpolate = [dx, dy](const float a, const float b, const float c, const float d) {
        return (a + dx * (b - a)) + dy * ((c + dx * (d - c)) - (a + dx * (b - a)));
    };
    backwardX = interpolate(a[0], b[0], c[0], d[0]);
    backwardY = interpolate(a[1], b[1], c[1], d[1]);
    return std::isfinite(backwardX) && std::isfinite(backwardY);
}
