#pragma once

#include <cstdint>

struct RifeSpatialSize
{
	uint32_t width = 0;
	uint32_t height = 0;

	constexpr bool operator==(const RifeSpatialSize&) const = default;
};

constexpr uint32_t AlignRifeDimension(const uint32_t value) noexcept
{
	return value ? (value + 31u) & ~31u : 0u;
}

constexpr RifeSpatialSize AlignRifeSize(const RifeSpatialSize size) noexcept
{
	return { AlignRifeDimension(size.width), AlignRifeDimension(size.height) };
}

constexpr RifeSpatialSize ResolveRifeContentSize(
	const uint32_t sourceWidth,
	const uint32_t sourceHeight,
	const bool anamorphic,
	const uint32_t aspectX,
	const uint32_t aspectY,
	const int rotation) noexcept
{
	if (!sourceWidth || !sourceHeight) {
		return {};
	}

	uint32_t width = sourceWidth;
	if (anamorphic && aspectX && aspectY) {
		const uint64_t scaled = static_cast<uint64_t>(sourceHeight) * aspectX;
		width = static_cast<uint32_t>((scaled + aspectY / 2u) / aspectY);
	}

	if (rotation == 90 || rotation == 270) {
		return { sourceHeight, width };
	}
	return { width, sourceHeight };
}

constexpr RifeSpatialSize ResolveRifeVpIntermediateSize(
	const RifeSpatialSize sourceSize,
	const RifeSpatialSize contentSize,
	const bool vpScaling,
	const bool maxineOwnsScaling,
	const int rotation) noexcept
{
	if (!vpScaling || maxineOwnsScaling) {
		return sourceSize;
	}

	if (rotation == 90 || rotation == 270) {
		return { contentSize.height, contentSize.width };
	}
	return contentSize;
}
