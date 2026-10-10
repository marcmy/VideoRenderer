#pragma once
#include <dxgiformat.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

constexpr bool RifeSupportedSurfaceFormat(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}
constexpr unsigned RifeSurfaceBytes(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
}
inline float RifeHalfToFloat(uint16_t half) noexcept {
    const uint32_t sign = (half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 31u;
    const uint32_t fraction = half & 1023u;
    if (!exponent) {
        const float magnitude = std::ldexp(static_cast<float>(fraction), -24);
        return half & 0x8000u ? -magnitude : magnitude;
    }
    const uint32_t bits = sign | ((exponent == 31u ? 255u : exponent + 112u) << 23) | (fraction << 13);
    return std::bit_cast<float>(bits);
}
inline uint8_t RifeAnalysisByte(float value) noexcept {
    if (std::isnan(value)) return 0;
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}
inline void RifeAnalysisBgra(const uint16_t* rgba, uint8_t* bgra) noexcept {
    bgra[0] = RifeAnalysisByte(RifeHalfToFloat(rgba[2]));
    bgra[1] = RifeAnalysisByte(RifeHalfToFloat(rgba[1]));
    bgra[2] = RifeAnalysisByte(RifeHalfToFloat(rgba[0]));
    bgra[3] = 255;
}
