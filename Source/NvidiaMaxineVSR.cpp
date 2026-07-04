/*
 * Optional NVIDIA Maxine Video Super Resolution runtime integration.
 *
 * API declarations below are limited to the ABI required by this file and are
 * based on NVIDIA's MIT-licensed nvCVImage.h, nvCVStatus.h and
 * nvVideoEffects.h headers. No NVIDIA SDK binary is linked or redistributed.
 */

#include "stdafx.h"
#include "NvidiaMaxineVSR.h"
#include "Helper.h"

#include <array>
#include <filesystem>
#include <format>
#include <utility>
#include <vector>

namespace {

#ifdef _WIN64

using NvCV_Status = int;
constexpr NvCV_Status NVCV_SUCCESS = 0;
enum NvCVImage_PixelFormat : int {
	NVCV_FORMAT_UNKNOWN = 0,
	NVCV_RGBA = 6,
	NVCV_BGRA = 7,
};

enum NvCVImage_ComponentType : int {
	NVCV_TYPE_UNKNOWN = 0,
	NVCV_U8 = 1,
};

constexpr unsigned NVCV_INTERLEAVED = 0;
constexpr unsigned NVCV_GPU = 1;

struct CUstream_st;
using CUstream = CUstream_st*;

struct NvCVImage {
	unsigned int width;
	unsigned int height;
	signed int pitch;
	NvCVImage_PixelFormat pixelFormat;
	NvCVImage_ComponentType componentType;
	unsigned char pixelBytes;
	unsigned char componentBytes;
	unsigned char numComponents;
	unsigned char planar;
	unsigned char gpuMem;
	unsigned char colorspace;
	unsigned char reserved[2];
	void* pixels;
	void* deletePtr;
	void (__cdecl* deleteProc)(void* p);
	unsigned long long bufferBytes;
};

struct NvVFX_Object;
using NvVFX_Handle = NvVFX_Object*;

using PFN_NvVFX_GetVersion = NvCV_Status (__cdecl*)(unsigned int* version);
using PFN_NvVFX_CreateEffect = NvCV_Status (__cdecl*)(const char* code, NvVFX_Handle* effect);
using PFN_NvVFX_DestroyEffect = void (__cdecl*)(NvVFX_Handle effect);
using PFN_NvVFX_SetU32 = NvCV_Status (__cdecl*)(NvVFX_Handle effect, const char* paramName, unsigned int value);
using PFN_NvVFX_SetImage = NvCV_Status (__cdecl*)(NvVFX_Handle effect, const char* paramName, NvCVImage* image);
using PFN_NvVFX_SetCudaStream = NvCV_Status (__cdecl*)(NvVFX_Handle effect, const char* paramName, CUstream stream);
using PFN_NvVFX_Load = NvCV_Status (__cdecl*)(NvVFX_Handle effect);
using PFN_NvVFX_Run = NvCV_Status (__cdecl*)(NvVFX_Handle effect, int async);
using PFN_NvVFX_CudaStreamCreate = NvCV_Status (__cdecl*)(CUstream* stream);
using PFN_NvVFX_CudaStreamDestroy = NvCV_Status (__cdecl*)(CUstream stream);

using PFN_NvCVImage_Alloc = NvCV_Status (__cdecl*)(NvCVImage* image, unsigned width, unsigned height,
	NvCVImage_PixelFormat format, NvCVImage_ComponentType type, unsigned layout, unsigned memSpace, unsigned alignment);
using PFN_NvCVImage_Dealloc = void (__cdecl*)(NvCVImage* image);
using PFN_NvCVImage_Transfer = NvCV_Status (__cdecl*)(const NvCVImage* src, NvCVImage* dst, float scale,
	CUstream stream, NvCVImage* tmp);
using PFN_NvCVImage_InitFromD3D11Texture = NvCV_Status (__cdecl*)(NvCVImage* image, ID3D11Texture2D* texture);
using PFN_NvCVImage_MapResource = NvCV_Status (__cdecl*)(NvCVImage* image, CUstream stream);
using PFN_NvCVImage_UnmapResource = NvCV_Status (__cdecl*)(NvCVImage* image, CUstream stream);
using PFN_NvCV_GetErrorStringFromCode = const char* (__cdecl*)(NvCV_Status code);

std::wstring Utf8ToWide(const char* text)
{
	if (!text || !*text) {
		return {};
	}

	const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (size <= 1) {
		return {};
	}

	std::wstring result(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), size);
	result.pop_back();
	return result;
}

const int g_moduleAddressMarker = 0;

std::wstring GetModuleDirectoryFromAddress(const void* address)
{
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(address, &mbi, sizeof(mbi))) {
		return {};
	}

	wchar_t path[MAX_PATH] = {};
	if (!GetModuleFileNameW(static_cast<HMODULE>(mbi.AllocationBase), path, std::size(path))) {
		return {};
	}

	return std::filesystem::path(path).parent_path().wstring();
}

std::vector<std::wstring> GetRuntimeSearchDirectories()
{
	std::vector<std::wstring> dirs;
	auto Add = [&dirs](std::wstring dir) {
		if (dir.empty()) {
			return;
		}
		while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
			dir.pop_back();
		}
		if (dir.empty()) {
			return;
		}
		if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) {
			dirs.emplace_back(std::move(dir));
		}
	};

	Add(GetModuleDirectoryFromAddress(&g_moduleAddressMarker));

	wchar_t path[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, path, std::size(path))) {
		Add(std::filesystem::path(path).parent_path().wstring());
	}

	wchar_t envPath[32768] = {};
	const DWORD envLen = GetEnvironmentVariableW(L"NV_VIDEO_EFFECTS_PATH", envPath, std::size(envPath));
	if (envLen && envLen < std::size(envPath) && _wcsicmp(envPath, L"USE_APP_PATH") != 0) {
		Add(envPath);
	}

	wchar_t programFiles[MAX_PATH] = {};
	const DWORD pfLen = GetEnvironmentVariableW(L"ProgramFiles", programFiles, std::size(programFiles));
	if (pfLen && pfLen < std::size(programFiles)) {
		Add((std::filesystem::path(programFiles) / L"NVIDIA Corporation" / L"NVIDIA Video Effects").wstring());
	}

	return dirs;
}

HMODULE LoadRuntimeLibrary(const std::wstring& directory, const wchar_t* filename)
{
	const auto fullPath = std::filesystem::path(directory) / filename;
	return LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

#endif // _WIN64

} // namespace

struct CNvidiaMaxineVSR::Impl
{
#ifdef _WIN64
	HMODULE hNvCVImage = nullptr;
	HMODULE hNvVideoEffects = nullptr;

	PFN_NvVFX_GetVersion NvVFX_GetVersion = nullptr;
	PFN_NvVFX_CreateEffect NvVFX_CreateEffect = nullptr;
	PFN_NvVFX_DestroyEffect NvVFX_DestroyEffect = nullptr;
	PFN_NvVFX_SetU32 NvVFX_SetU32 = nullptr;
	PFN_NvVFX_SetImage NvVFX_SetImage = nullptr;
	PFN_NvVFX_SetCudaStream NvVFX_SetCudaStream = nullptr;
	PFN_NvVFX_Load NvVFX_Load = nullptr;
	PFN_NvVFX_Run NvVFX_Run = nullptr;
	PFN_NvVFX_CudaStreamCreate NvVFX_CudaStreamCreate = nullptr;
	PFN_NvVFX_CudaStreamDestroy NvVFX_CudaStreamDestroy = nullptr;

	PFN_NvCVImage_Alloc NvCVImage_Alloc = nullptr;
	PFN_NvCVImage_Dealloc NvCVImage_Dealloc = nullptr;
	PFN_NvCVImage_Transfer NvCVImage_Transfer = nullptr;
	PFN_NvCVImage_InitFromD3D11Texture NvCVImage_InitFromD3D11Texture = nullptr;
	PFN_NvCVImage_MapResource NvCVImage_MapResource = nullptr;
	PFN_NvCVImage_UnmapResource NvCVImage_UnmapResource = nullptr;
	PFN_NvCV_GetErrorStringFromCode NvCV_GetErrorStringFromCode = nullptr;

	NvVFX_Handle effect = nullptr;
	CUstream stream = nullptr;
	NvCVImage d3dInput = {};
	NvCVImage d3dOutput = {};
	NvCVImage gpuInput = {};
	NvCVImage gpuOutput = {};

	ID3D11Texture2D* inputTexture = nullptr;
	ID3D11Texture2D* outputTexture = nullptr;
	unsigned quality = 0;
	bool runtimeAttempted = false;
	bool failed = false;
	unsigned int sdkVersion = 0;
#endif

	std::wstring status = L"Disabled";

#ifdef _WIN64
	template<class T>
	bool LoadProc(HMODULE module, const char* name, T& proc)
	{
		proc = reinterpret_cast<T>(GetProcAddress(module, name));
		return proc != nullptr;
	}

	void SetError(const wchar_t* operation, NvCV_Status code)
	{
		std::wstring detail;
		if (NvCV_GetErrorStringFromCode) {
			detail = Utf8ToWide(NvCV_GetErrorStringFromCode(code));
		}
		if (detail.empty()) {
			status = std::format(L"{} failed ({})", operation, code);
		} else {
			status = std::format(L"{} failed: {} ({})", operation, detail, code);
		}
		DLog(L"NVIDIA Maxine VSR: {}", status);
	}

	void ReleaseImages()
	{
		if (NvCVImage_Dealloc) {
			NvCVImage_Dealloc(&d3dInput);
			NvCVImage_Dealloc(&d3dOutput);
			NvCVImage_Dealloc(&gpuInput);
			NvCVImage_Dealloc(&gpuOutput);
		}
		d3dInput = {};
		d3dOutput = {};
		gpuInput = {};
		gpuOutput = {};
		inputTexture = nullptr;
		outputTexture = nullptr;
	}

	void ResetEffect()
	{
		if (effect && NvVFX_DestroyEffect) {
			NvVFX_DestroyEffect(effect);
		}
		effect = nullptr;
		ReleaseImages();
		if (stream && NvVFX_CudaStreamDestroy) {
			NvVFX_CudaStreamDestroy(stream);
		}
		stream = nullptr;
		quality = 0;
		failed = false;
	}

	void UnloadRuntime()
	{
		ResetEffect();
		if (hNvVideoEffects) {
			FreeLibrary(hNvVideoEffects);
			hNvVideoEffects = nullptr;
		}
		if (hNvCVImage) {
			FreeLibrary(hNvCVImage);
			hNvCVImage = nullptr;
		}
	}

	bool LoadRuntime()
	{
		if (hNvCVImage && hNvVideoEffects) {
			return true;
		}
		if (runtimeAttempted) {
			return false;
		}
		runtimeAttempted = true;

		for (const auto& directory : GetRuntimeSearchDirectories()) {
			HMODULE imageModule = LoadRuntimeLibrary(directory, L"NVCVImage.dll");
			if (!imageModule) {
				continue;
			}
			HMODULE effectsModule = LoadRuntimeLibrary(directory, L"NVVideoEffects.dll");
			if (!effectsModule) {
				FreeLibrary(imageModule);
				continue;
			}

			hNvCVImage = imageModule;
			hNvVideoEffects = effectsModule;
			break;
		}

		if (!hNvCVImage || !hNvVideoEffects) {
			status = L"NVIDIA Video Effects runtime not found";
			return false;
		}

		const bool ok =
			LoadProc(hNvVideoEffects, "NvVFX_GetVersion", NvVFX_GetVersion) &&
			LoadProc(hNvVideoEffects, "NvVFX_CreateEffect", NvVFX_CreateEffect) &&
			LoadProc(hNvVideoEffects, "NvVFX_DestroyEffect", NvVFX_DestroyEffect) &&
			LoadProc(hNvVideoEffects, "NvVFX_SetU32", NvVFX_SetU32) &&
			LoadProc(hNvVideoEffects, "NvVFX_SetImage", NvVFX_SetImage) &&
			LoadProc(hNvVideoEffects, "NvVFX_SetCudaStream", NvVFX_SetCudaStream) &&
			LoadProc(hNvVideoEffects, "NvVFX_Load", NvVFX_Load) &&
			LoadProc(hNvVideoEffects, "NvVFX_Run", NvVFX_Run) &&
			LoadProc(hNvVideoEffects, "NvVFX_CudaStreamCreate", NvVFX_CudaStreamCreate) &&
			LoadProc(hNvVideoEffects, "NvVFX_CudaStreamDestroy", NvVFX_CudaStreamDestroy) &&
			LoadProc(hNvCVImage, "NvCVImage_Alloc", NvCVImage_Alloc) &&
			LoadProc(hNvCVImage, "NvCVImage_Dealloc", NvCVImage_Dealloc) &&
			LoadProc(hNvCVImage, "NvCVImage_Transfer", NvCVImage_Transfer) &&
			LoadProc(hNvCVImage, "NvCVImage_InitFromD3D11Texture", NvCVImage_InitFromD3D11Texture) &&
			LoadProc(hNvCVImage, "NvCVImage_MapResource", NvCVImage_MapResource) &&
			LoadProc(hNvCVImage, "NvCVImage_UnmapResource", NvCVImage_UnmapResource);

		LoadProc(hNvCVImage, "NvCV_GetErrorStringFromCode", NvCV_GetErrorStringFromCode);

		if (!ok) {
			status = L"NVIDIA Video Effects runtime is missing required exports";
			UnloadRuntime();
			return false;
		}

		const NvCV_Status versionStatus = NvVFX_GetVersion(&sdkVersion);
		if (versionStatus != NVCV_SUCCESS) {
			SetError(L"NvVFX_GetVersion", versionStatus);
			UnloadRuntime();
			return false;
		}

		status = std::format(L"Runtime {}.{}.{} loaded",
			(sdkVersion >> 24) & 0xff,
			(sdkVersion >> 16) & 0xff,
			(sdkVersion >> 8) & 0xff);
		DLog(L"NVIDIA Maxine VSR: {}", status);
		return true;
	}

	bool EnsureEffect(ID3D11Texture2D* input, ID3D11Texture2D* output, unsigned requestedQuality)
	{
		if (!LoadRuntime() || failed) {
			return false;
		}

		if (effect && inputTexture == input && outputTexture == output && quality == requestedQuality) {
			return true;
		}

		ResetEffect();

		NvCV_Status code = NvVFX_CudaStreamCreate(&stream);
		if (code != NVCV_SUCCESS) {
			SetError(L"NvVFX_CudaStreamCreate", code);
			failed = true;
			return false;
		}

		code = NvCVImage_InitFromD3D11Texture(&d3dInput, input);
		if (code != NVCV_SUCCESS) {
			SetError(L"NvCVImage_InitFromD3D11Texture(input)", code);
			failed = true;
			ResetEffect();
			failed = true;
			return false;
		}
		code = NvCVImage_InitFromD3D11Texture(&d3dOutput, output);
		if (code != NVCV_SUCCESS) {
			SetError(L"NvCVImage_InitFromD3D11Texture(output)", code);
			failed = true;
			ResetEffect();
			failed = true;
			return false;
		}

		if ((d3dInput.pixelFormat != NVCV_RGBA && d3dInput.pixelFormat != NVCV_BGRA)
				|| d3dInput.componentType != NVCV_U8 || d3dInput.planar != NVCV_INTERLEAVED
				|| (d3dOutput.pixelFormat != NVCV_RGBA && d3dOutput.pixelFormat != NVCV_BGRA)
				|| d3dOutput.componentType != NVCV_U8 || d3dOutput.planar != NVCV_INTERLEAVED) {
			status = L"Only RGBA/BGRA 8-bit D3D11 textures are supported";
			DLog(L"NVIDIA Maxine VSR: {}", status);
			ResetEffect();
			failed = true;
			return false;
		}

		code = NvCVImage_Alloc(&gpuInput, d3dInput.width, d3dInput.height, d3dInput.pixelFormat,
			NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32);
		if (code != NVCV_SUCCESS) {
			SetError(L"NvCVImage_Alloc(input)", code);
			ResetEffect();
			failed = true;
			return false;
		}
		code = NvCVImage_Alloc(&gpuOutput, d3dOutput.width, d3dOutput.height, d3dOutput.pixelFormat,
			NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32);
		if (code != NVCV_SUCCESS) {
			SetError(L"NvCVImage_Alloc(output)", code);
			ResetEffect();
			failed = true;
			return false;
		}

		code = NvVFX_CreateEffect("VideoSuperRes", &effect);
		if (code != NVCV_SUCCESS) {
			SetError(L"NvVFX_CreateEffect(VideoSuperRes)", code);
			ResetEffect();
			failed = true;
			return false;
		}

		const std::array<std::pair<const wchar_t*, NvCV_Status>, 4> setupResults = {{
			{L"NvVFX_SetImage(input)", NvVFX_SetImage(effect, "SrcImage0", &gpuInput)},
			{L"NvVFX_SetImage(output)", NvVFX_SetImage(effect, "DstImage0", &gpuOutput)},
			{L"NvVFX_SetCudaStream", NvVFX_SetCudaStream(effect, "CudaStream", stream)},
			{L"NvVFX_SetU32(QualityLevel)", NvVFX_SetU32(effect, "QualityLevel", requestedQuality)},
		}};
		for (const auto& [operation, result] : setupResults) {
			if (result != NVCV_SUCCESS) {
				SetError(operation, result);
				ResetEffect();
				failed = true;
				return false;
			}
		}

		code = NvVFX_Load(effect);
		if (code != NVCV_SUCCESS) {
			SetError(L"NvVFX_Load", code);
			ResetEffect();
			failed = true;
			return false;
		}

		inputTexture = input;
		outputTexture = output;
		quality = requestedQuality;
		status = std::format(L"Active, quality {}", quality);
		return true;
	}
#endif // _WIN64
};

CNvidiaMaxineVSR::CNvidiaMaxineVSR()
	: m_impl(std::make_unique<Impl>())
{
#ifndef _WIN64
	m_impl->status = L"Requires a 64-bit build";
#endif
}

CNvidiaMaxineVSR::~CNvidiaMaxineVSR()
{
#ifdef _WIN64
	m_impl->UnloadRuntime();
#endif
}

bool CNvidiaMaxineVSR::Process(
	ID3D11DeviceContext* pDeviceContext,
	ID3D11Texture2D* pInputTexture,
	ID3D11Texture2D* pOutputTexture,
	unsigned quality)
{
#ifdef _WIN64
	if (!pDeviceContext || !pInputTexture || !pOutputTexture || quality < 1 || quality > 4) {
		m_impl->status = L"Invalid processing parameters";
		return false;
	}

	D3D11_TEXTURE2D_DESC inputDesc = {};
	D3D11_TEXTURE2D_DESC outputDesc = {};
	pInputTexture->GetDesc(&inputDesc);
	pOutputTexture->GetDesc(&outputDesc);

	if (inputDesc.Width >= outputDesc.Width || inputDesc.Height >= outputDesc.Height
			|| static_cast<unsigned long long>(inputDesc.Width) * outputDesc.Height
				!= static_cast<unsigned long long>(inputDesc.Height) * outputDesc.Width
			|| outputDesc.Width > inputDesc.Width * 4u || outputDesc.Height > inputDesc.Height * 4u) {
		m_impl->status = L"Unsupported VSR scale or aspect ratio";
		return false;
	}

	if (!m_impl->EnsureEffect(pInputTexture, pOutputTexture, quality)) {
		return false;
	}

	ID3D11ShaderResourceView* nullViews[3] = {};
	pDeviceContext->PSSetShaderResources(0, std::size(nullViews), nullViews);
	pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);

	bool inputMapped = false;
	bool outputMapped = false;
	NvCV_Status code = m_impl->NvCVImage_MapResource(&m_impl->d3dInput, m_impl->stream);
	if (code == NVCV_SUCCESS) {
		inputMapped = true;
		code = m_impl->NvCVImage_MapResource(&m_impl->d3dOutput, m_impl->stream);
	}
	if (code == NVCV_SUCCESS) {
		outputMapped = true;
		code = m_impl->NvCVImage_Transfer(&m_impl->d3dInput, &m_impl->gpuInput, 1.0f, m_impl->stream, nullptr);
	}
	if (code == NVCV_SUCCESS) {
		code = m_impl->NvVFX_Run(m_impl->effect, 0);
	}
	if (code == NVCV_SUCCESS) {
		code = m_impl->NvCVImage_Transfer(&m_impl->gpuOutput, &m_impl->d3dOutput, 1.0f, m_impl->stream, nullptr);
	}

	NvCV_Status unmapCode = NVCV_SUCCESS;
	if (outputMapped) {
		unmapCode = m_impl->NvCVImage_UnmapResource(&m_impl->d3dOutput, m_impl->stream);
	}
	if (inputMapped) {
		const NvCV_Status inputUnmapCode = m_impl->NvCVImage_UnmapResource(&m_impl->d3dInput, m_impl->stream);
		if (unmapCode == NVCV_SUCCESS) {
			unmapCode = inputUnmapCode;
		}
	}
	if (code == NVCV_SUCCESS) {
		code = unmapCode;
	}

	if (code != NVCV_SUCCESS) {
		m_impl->SetError(L"Frame processing", code);
		m_impl->ResetEffect();
		m_impl->failed = true;
		return false;
	}

	m_impl->status = std::format(L"Active, quality {}", quality);
	return true;
#else
	UNREFERENCED_PARAMETER(pDeviceContext);
	UNREFERENCED_PARAMETER(pInputTexture);
	UNREFERENCED_PARAMETER(pOutputTexture);
	UNREFERENCED_PARAMETER(quality);
	return false;
#endif
}

void CNvidiaMaxineVSR::Reset()
{
#ifdef _WIN64
	m_impl->ResetEffect();
	m_impl->status = m_impl->hNvVideoEffects ? L"Runtime loaded" : L"Disabled";
#else
	m_impl->status = L"Requires a 64-bit build";
#endif
}

const std::wstring& CNvidiaMaxineVSR::GetStatus() const
{
	return m_impl->status;
}
