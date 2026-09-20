#include <cassert>
#include <iostream>

#include "../../Source/MaxineInteropPolicy.h"
#include "../../Source/MaxineSpatialPolicy.h"

int main()
{
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

	// Disabling video-processor resizing leaves a modest final enlargement for
	// the selected shader without increasing Maxine's output pixel count.
	assert((ResolveMaxineShaderFinishSize(
		{1280, 720}, {1920, 1080}) == MaxineSpatialSize{1728, 972}));
	assert((ResolveMaxineShaderFinishSize(
		{960, 540}, {1920, 1080}) == MaxineSpatialSize{1728, 972}));
	assert((ResolveMaxineShaderFinishSize(
		{1280, 720}, {1678, 944}) == MaxineSpatialSize{1510, 850}));

	// A very small enlargement still gives Maxine half of the requested change
	// and leaves at least one pixel for the final shader pass.
	assert((ResolveMaxineShaderFinishSize(
		{1280, 720}, {1282, 722}) == MaxineSpatialSize{1281, 721}));

	// No enlargement means there is no shader-finish target to invent.
	assert((ResolveMaxineShaderFinishSize(
		{1920, 1080}, {1280, 720}) == MaxineSpatialSize{1280, 720}));

	std::cout << "All Maxine spatial policy tests passed\n";
	return 0;
}
