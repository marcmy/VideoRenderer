/*
 * Driver-provided NVIDIA Optical Flow API discovery.
 *
 * NVIDIA installs nvofapi.dll/nvofapi64.dll with the display driver. The
 * renderer loads it from System32 and resolves exports at runtime so native
 * optical flow remains optional and requires no redistributable SDK binary.
 */

#include "stdafx.h"
#include "NvidiaOpticalFlowNative.h"

#include <filesystem>
#include <format>

namespace {

using PFN_NvOFGetMaxSupportedApiVersion = int (WINAPI*)(uint32_t* version);

#ifdef _WIN64
constexpr wchar_t kNvofModuleName[] = L"nvofapi64.dll";
#else
constexpr wchar_t kNvofModuleName[] = L"nvofapi.dll";
#endif

HMODULE LoadSystemModule(const wchar_t* moduleName)
{
	wchar_t systemDirectory[MAX_PATH] = {};
	const UINT length = GetSystemDirectoryW(systemDirectory, std::size(systemDirectory));
	if (!length || length >= std::size(systemDirectory)) {
		return nullptr;
	}

	const std::filesystem::path modulePath =
		std::filesystem::path(systemDirectory) / moduleName;
	return LoadLibraryW(modulePath.c_str());
}

} // namespace

CNvidiaOpticalFlowNativeProbe::Result CNvidiaOpticalFlowNativeProbe::Probe()
{
	Result result;
	result.moduleName = kNvofModuleName;

	HMODULE module = LoadSystemModule(kNvofModuleName);
	if (!module) {
		result.status = std::format(
			L"{} is not available from the NVIDIA display driver (Win32 error {})",
			kNvofModuleName, GetLastError());
		return result;
	}

	const auto releaseModule = wil::scope_exit([&] {
		FreeLibrary(module);
	});

	const auto getMaxSupportedApiVersion =
		reinterpret_cast<PFN_NvOFGetMaxSupportedApiVersion>(
			GetProcAddress(module, "NvOFGetMaxSupportedApiVersion"));
	const FARPROC createD3D11 = GetProcAddress(module, "NvOFAPICreateInstanceD3D11");

	if (!getMaxSupportedApiVersion || !createD3D11) {
		result.status = std::format(
			L"{} is missing the required native NVOF exports",
			kNvofModuleName);
		return result;
	}

	uint32_t apiVersion = 0;
	const int status = getMaxSupportedApiVersion(&apiVersion);
	if (status != 0) {
		result.status = std::format(
			L"NvOFGetMaxSupportedApiVersion failed with status {}", status);
		return result;
	}

	result.apiMajor = apiVersion >> 4;
	result.apiMinor = apiVersion & 0x0f;
	result.available = true;
	result.status = std::format(
		L"Native NVIDIA Optical Flow API {}.{} is available through {}",
		result.apiMajor, result.apiMinor, result.moduleName);
	return result;
}
