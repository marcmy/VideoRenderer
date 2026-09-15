#pragma once

#include <algorithm>
#include <cstdint>

struct MaxineSpatialSize
{
	uint32_t width = 0;
	uint32_t height = 0;

	constexpr bool operator==(const MaxineSpatialSize&) const = default;
};

constexpr bool IsMaxinePortrait(const MaxineSpatialSize size) noexcept
{
	return size.height > size.width;
}

constexpr uint32_t ScaleMaxineDimension(
		const uint32_t value, const uint32_t numerator, const uint32_t denominator) noexcept
{
	if (!value || !numerator || !denominator) {
		return 0;
	}

	return static_cast<uint32_t>((static_cast<uint64_t>(value) * numerator + denominator / 2u) / denominator);
}

// Match-output normally follows the aspect-fitted video rectangle exactly.
// When the video's presentation orientation differs from the player surface,
// that rectangle is numerically smaller than the source on both axes (for
// example 720x1280 -> 608x1080 on a 1920x1080 surface). Treat that case by
// resolution class: map the player's short edge to the source's short edge
// while preserving the source aspect ratio, then return the target in the
// orientation of the texture Maxine will process.
constexpr MaxineSpatialSize ResolveMaxineMatchOutputBaseSize(
		const MaxineSpatialSize source,
		const MaxineSpatialSize fittedOutput,
		const MaxineSpatialSize playerOutput,
		const bool presentationPortrait) noexcept
{
	if (!source.width || !source.height || !fittedOutput.width || !fittedOutput.height) {
		return {};
	}

	const bool sourcePortrait = IsMaxinePortrait(source);
	const bool playerPortrait = playerOutput.width && playerOutput.height
		? IsMaxinePortrait(playerOutput)
		: presentationPortrait;

	if (presentationPortrait == playerPortrait || !playerOutput.width || !playerOutput.height) {
		return sourcePortrait == presentationPortrait
			? fittedOutput
			: MaxineSpatialSize{ fittedOutput.height, fittedOutput.width };
	}

	const uint32_t sourceShort = std::min(source.width, source.height);
	const uint32_t playerShort = std::min(playerOutput.width, playerOutput.height);
	const uint32_t fittedShort = std::min(fittedOutput.width, fittedOutput.height);
	const uint32_t targetShort = std::max(playerShort, fittedShort);
	return {
		ScaleMaxineDimension(source.width, targetShort, sourceShort),
		ScaleMaxineDimension(source.height, targetShort, sourceShort)
	};
}
