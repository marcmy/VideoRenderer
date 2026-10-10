#include <cassert>
#include <iostream>

#include "../../Source/MaxineInteropPolicy.h"
#include "../../Source/MaxineSpatialPolicy.h"
#include "../../Source/MaxineRunFailure.h"
#include "../../Source/RifeVideoProcessorPolicy.h"

int main()
{
	// Exact failing anamorphic case: 720 stored pixels must not be sent
	// directly to a 626-wide AI target while enlarging the height to 1080.
	assert((ResolveMaxineUpscaleEnvelope({720, 480}, {626, 1080}) == MaxineSpatialSize{720, 1243}));
	assert((ResolveMaxineUpscaleEnvelope({480, 720}, {1080, 626}) == MaxineSpatialSize{1243, 720}));
	assert((ResolveMaxineUpscaleEnvelope({1280, 720}, {1920, 1080}) == MaxineSpatialSize{1920, 1080}));
	assert((ResolveMaxineUpscaleEnvelope({278, 480}, {1080, 1865}) == MaxineSpatialSize{1080, 1865}));
	assert((ResolveMaxineUpscaleEnvelope({1920, 1080}, {1280, 720}) == MaxineSpatialSize{1280, 720}));
	assert((ResolveMaxineUpscaleEnvelope({720, 480}, {90, 1080}) == MaxineSpatialSize{}));
	assert((ResolveMaxineUpscaleEnvelope({}, {1920, 1080}) == MaxineSpatialSize{}));
	// Native RTX VSR is a presentation pass on unpadded content, with Maxine
	// exclusion and a precision-preserving fallback for unsupported RGB/HDR.
	assert(CanUseRifeVideoProcessorUpscale(true, false, true, {1280, 720}, {1920, 1080}));
	assert(CanUseRifeVideoProcessorUpscale(true, false, true, {278, 480}, {626, 1080}));
	assert(!CanUseRifeVideoProcessorUpscale(true, true, true, {1280, 720}, {1920, 1080}));
	assert(!CanUseRifeVideoProcessorUpscale(false, false, true, {1280, 720}, {1920, 1080}));
	assert(!CanUseRifeVideoProcessorUpscale(true, false, false, {1280, 720}, {1920, 1080}));
	assert(!CanUseRifeVideoProcessorUpscale(true, false, true, {1920, 1080}, {1280, 720}));
	assert(!CanUseRifeVideoProcessorUpscale(true, false, true, {1280, 720}, {1920, 720}));
	MaxineRunFailure failure;
	const MaxineRunBinding rejected{{720, 480}, {626, 1080}, 3, -1, 7};
	const MaxineRunBinding rife{{278, 480}, {1080, 1865}, 3, -1, 7};
	assert(!failure.HasDifferentBinding(rife));
	failure.Record(rejected);
	for (int frame = 0; frame < 10000; ++frame) assert(!failure.HasDifferentBinding(rejected));
	assert(failure.HasDifferentBinding(rife));
	auto different = rejected;
	different.mode = 18;
	assert(failure.HasDifferentBinding(different));
	different = rejected;
	different.adapter = 8;
	assert(failure.HasDifferentBinding(different));
	failure.Clear();
	assert(!failure.HasDifferentBinding(rife));

	// RIFE presentation textures can otherwise satisfy every direct-input
	// condition while still being owned by RIFE's persistent CUDA registration.
	assert(CanUseDirectMaxineInput(true, false));
	assert(!CanUseDirectMaxineInput(true, true));
	assert(!CanUseDirectMaxineInput(false, false));

	// Ordinary landscape playback keeps the exact aspect-fitted output size.
	assert((ResolveMaxineMatchOutputBaseSize(
		{1280, 720}, {1440, 1080}, {1920, 1080}, false) == MaxineSpatialSize{1440, 1080}));

	// Portrait 720p is the rotated equivalent of 1280x720. On a 1080p player
	// surface it should therefore resolve to the 1080p class, not be rejected
	// because the aspect-fitted rectangle is only 608x1080.
	assert((ResolveMaxineMatchOutputBaseSize(
		{720, 1280}, {608, 1080}, {1920, 1080}, true) == MaxineSpatialSize{1080, 1920}));

	// An auto-sized portrait player window on a landscape 1080p display still
	// uses the display's vertical resolution axis for the portrait source class.
	// It should match the same 944-line class reached after widening the window,
	// rather than suppressing VSR because the fitted width is only 531 pixels.
	assert((ResolveMaxineMatchOutputBaseSize(
		{720, 1280}, {531, 944}, {1920, 1080}, true) == MaxineSpatialSize{944, 1678}));
	assert((ResolveMaxineMatchOutputBaseSize(
		{720, 1280}, {1216, 2160}, {1920, 1080}, true) == MaxineSpatialSize{1216, 2162}));

	// A raw landscape texture rotated 90 degrees for presentation gets the same
	// class target, returned in the raw texture's orientation for Maxine.
	assert((ResolveMaxineMatchOutputBaseSize(
		{1280, 720}, {608, 1080}, {1920, 1080}, true) == MaxineSpatialSize{1920, 1080}));

	// A portrait source already at the 1080p class does not get enlarged merely
	// because it is pillarboxed on a landscape player surface.
	assert((ResolveMaxineMatchOutputBaseSize(
		{1080, 1920}, {608, 1080}, {1920, 1080}, true) == MaxineSpatialSize{1080, 1920}));

	// Same-orientation 4:3 and ultrawide sources retain the old fitted-output
	// behavior, avoiding an implicit oversample for unusual aspect ratios.
	assert((ResolveMaxineMatchOutputBaseSize(
		{640, 480}, {1440, 1080}, {1920, 1080}, false) == MaxineSpatialSize{1440, 1080}));
	assert((ResolveMaxineMatchOutputBaseSize(
		{1280, 540}, {1920, 810}, {1920, 1080}, false) == MaxineSpatialSize{1920, 810}));

	// Portrait player surfaces work symmetrically for landscape content.
	assert((ResolveMaxineMatchOutputBaseSize(
		{1280, 720}, {1080, 608}, {1080, 1920}, false) == MaxineSpatialSize{1920, 1080}));

	// If the player surface is temporarily unavailable, preserve the historical
	// fitted-output behavior rather than inventing a target.
	assert((ResolveMaxineMatchOutputBaseSize(
		{720, 1280}, {608, 1080}, {}, true) == MaxineSpatialSize{608, 1080}));

	std::cout << "All Maxine spatial policy tests passed\n";
	return 0;
}
