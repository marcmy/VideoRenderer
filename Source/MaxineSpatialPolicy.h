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
// When the video's presentation orientation differs from the physical display,
// use the fitted extent along the display's short-resolution axis as the source
// resolution class. This keeps portrait 720p content eligible in an auto-sized
// portrait window on a landscape 1080p display while still following window
// size changes and preserving zoom beyond the visible display edge.
constexpr MaxineSpatialSize ResolveMaxineMatchOutputBaseSize(
		const MaxineSpatialSize source,
		const MaxineSpatialSize fittedOutput,
		const MaxineSpatialSize displayOutput,
		const bool presentationPortrait) noexcept
{
	if (!source.width || !source.height || !fittedOutput.width || !fittedOutput.height) {
		return {};
	}

	const bool sourcePortrait = IsMaxinePortrait(source);
	const bool displayPortrait = displayOutput.width && displayOutput.height
		? IsMaxinePortrait(displayOutput)
		: presentationPortrait;

	if (presentationPortrait == displayPortrait || !displayOutput.width || !displayOutput.height) {
		return sourcePortrait == presentationPortrait
			? fittedOutput
			: MaxineSpatialSize{ fittedOutput.height, fittedOutput.width };
	}

	const uint32_t sourceShort = std::min(source.width, source.height);
	const uint32_t displayShort = std::min(displayOutput.width, displayOutput.height);
	const uint32_t fittedShort = std::min(fittedOutput.width, fittedOutput.height);
	const uint32_t displayAxisExtent = displayPortrait ? fittedOutput.width : fittedOutput.height;
	const uint32_t targetShort = std::max(fittedShort, std::min(displayAxisExtent, displayShort));
	return {
		ScaleMaxineDimension(source.width, targetShort, sourceShort),
		ScaleMaxineDimension(source.height, targetShort, sourceShort)
	};
}

// Leave a small final enlargement for the selected shader scaler when video-
// processor resizing is disabled. Rendering Maxine at 90% of the fitted output
// reduces its pixel workload while giving Jinc/Catmull-Rom a real resampling
// step. Keep at least half of each requested enlargement in the Maxine pass so
// very small output changes do not collapse back to the source size.
constexpr MaxineSpatialSize ResolveMaxineShaderFinishSize(
		const MaxineSpatialSize source, const MaxineSpatialSize fittedOutput) noexcept
{
	if (!source.width || !source.height || !fittedOutput.width || !fittedOutput.height) {
		return {};
	}

	auto ResolveDimension = [](const uint32_t sourceValue, const uint32_t outputValue) {
		if (outputValue <= sourceValue) {
			return outputValue;
		}

		const uint32_t ninetyPercent = ScaleMaxineDimension(outputValue, 9u, 10u);
		const uint32_t halfway = sourceValue + (outputValue - sourceValue + 1u) / 2u;
		return std::min(outputValue - 1u, std::max(ninetyPercent, halfway));
	};

	return {
		ResolveDimension(source.width, fittedOutput.width),
		ResolveDimension(source.height, fittedOutput.height)
	};
}
