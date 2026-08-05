/*
 * Driver-provided NVIDIA Optical Flow API discovery.
 *
 * The NVOF API implementation is installed with the NVIDIA display driver.
 * This probe does not depend on or redistribute the Optical Flow SDK runtime.
 */

#pragma once

#include <string>

class CNvidiaOpticalFlowNativeProbe
{
public:
	struct Result {
		bool available = false;
		unsigned apiMajor = 0;
		unsigned apiMinor = 0;
		std::wstring moduleName;
		std::wstring status;
	};

	static Result Probe();
};
