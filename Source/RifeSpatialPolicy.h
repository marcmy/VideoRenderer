#pragma once

#include <cstdint>

enum RifeProcessingResolution : int {
	RIFE_RESOLUTION_Source = 0,
	RIFE_RESOLUTION_Display,
	RIFE_RESOLUTION_Limit,
	RIFE_RESOLUTION_COUNT
};

constexpr int RifeProcessingLimitMin = 64;
constexpr int RifeProcessingLimitMax = 4320;
constexpr int RifeProcessingLimitDefault = 720;

struct RifeSpatialSize
{
	uint32_t width = 0;
	uint32_t height = 0;

	constexpr bool operator==(const RifeSpatialSize&) const = default;
};

constexpr uint32_t AlignRifeDimension(
	const uint32_t value,
	const uint32_t alignment = 32u) noexcept
{
	return value && alignment
		? static_cast<uint32_t>(
			((static_cast<uint64_t>(value) + alignment - 1u) / alignment) * alignment)
		: 0u;
}

constexpr RifeSpatialSize AlignRifeSize(
	const RifeSpatialSize size,
	const uint32_t alignment = 32u) noexcept
{
	return {
		AlignRifeDimension(size.width, alignment),
		AlignRifeDimension(size.height, alignment)
	};
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

// Fit the complete oriented picture, never growing it or including model
// padding. Integer rounding keeps both dimensions inside the requested box.
constexpr RifeSpatialSize FitRifeContentSize(
	const RifeSpatialSize source, const RifeSpatialSize bounds) noexcept
{
	if (!source.width || !source.height || !bounds.width || !bounds.height
			|| (source.width <= bounds.width && source.height <= bounds.height)) {
		return source;
	}
	uint64_t numerator, denominator;
	if (static_cast<uint64_t>(bounds.width) * source.height
			<= static_cast<uint64_t>(bounds.height) * source.width) {
		numerator = bounds.width;
		denominator = source.width;
	} else {
		numerator = bounds.height;
		denominator = source.height;
	}
	const auto scale = [&](uint32_t value) {
		const auto result = static_cast<uint32_t>(static_cast<uint64_t>(value) * numerator / denominator);
		return result ? result : 1u;
	};
	return {scale(source.width), scale(source.height)};
}

constexpr RifeSpatialSize ResolveRifeWorkingSize(
	const RifeSpatialSize source, const RifeSpatialSize display,
	const int mode, const int shortEdgeLimit) noexcept
{
	if (mode == RIFE_RESOLUTION_Display) {
		return FitRifeContentSize(source, display);
	}
	if (mode == RIFE_RESOLUTION_Limit && shortEdgeLimit >= RifeProcessingLimitMin
			&& shortEdgeLimit <= RifeProcessingLimitMax && source.width && source.height) {
		const uint32_t shortEdge = source.width < source.height ? source.width : source.height;
		if (shortEdge > static_cast<uint32_t>(shortEdgeLimit)) {
			const uint32_t limit = static_cast<uint32_t>(shortEdgeLimit);
			return {static_cast<uint32_t>(static_cast<uint64_t>(source.width) * limit / shortEdge),
				static_cast<uint32_t>(static_cast<uint64_t>(source.height) * limit / shortEdge)};
		}
	}
	return source;
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
