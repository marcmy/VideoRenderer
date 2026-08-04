/*
 * Optional NVIDIA Optical Flow frame-rate up-conversion integration.
 *
 * This file intentionally declares only the small ABI surface required to
 * load NvOFFRUC.dll at runtime. NVIDIA SDK headers and binaries are not part
 * of this repository.
 */

#include "stdafx.h"
#include "NvidiaFrameInterpolation.h"
#include "Helper.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <vector>

namespace {

#ifdef _WIN64

enum NvOFFRUCResourceType : int {
	DirectX11Resource = 1,
};

enum NvOFFRUCSurfaceFormat : int {
	ARGBSurface = 1,
};

enum NvOFFRUCCUDAResourceType : int {
	CudaResourceTypeUndefined = -1,
};

enum NvOFFRUCStatus : int {
	NvOFFRUC_SUCCESS = 0,
};

union NvOFFRUCSyncWait {
	struct {
		uint64_t uiFenceValueToWaitOn;
	} FenceWaitValue;
	struct {
		uint64_t uiKeyForRenderTextureAcquire;
		uint64_t uiKeyForInterpTextureAcquire;
	} MutexAcquireKey;
};

union NvOFFRUCSyncSignal {
	struct {
		uint64_t uiFenceValueToSignalOn;
	} FenceSignalValue;
	struct {
		uint64_t uiKeyForRenderTextureRelease;
		uint64_t uiKeyForInterpolateRelease;
	} MutexReleaseKey;
};

struct NvOFFRUCCreateParam {
	uint32_t uiWidth;
	uint32_t uiHeight;
	void* pDevice;
	NvOFFRUCResourceType eResourceType;
	NvOFFRUCSurfaceFormat eSurfaceFormat;
	NvOFFRUCCUDAResourceType eCUDAResourceType;
	uint32_t uiReserved[32];
};

struct NvOFFRUCFrameData {
	void* pFrame;
	double nTimeStamp;
	size_t nCuSurfacePitch;
	bool* bHasFrameRepetitionOccurred;
	uint32_t uiReserved[32];
};

struct NvOFFRUCProcessInParams {
	NvOFFRUCFrameData stFrameDataInput;
	uint32_t bSkipWarp : 1;
	NvOFFRUCSyncWait uSyncWait;
	uint32_t uiReserved[32];
};

struct NvOFFRUCProcessOutParams {
	NvOFFRUCFrameData stFrameDataOutput;
	NvOFFRUCSyncSignal uSyncSignal;
	uint32_t uiReserved[32];
};

struct NvOFFRUCRegisterResourceParam {
	void* pArrResource[10];
	void* pD3D11FenceObj;
	uint32_t uiCount;
};

struct NvOFFRUCUnregisterResourceParam {
	void* pArrResource[10];
	uint32_t uiCount;
};

struct NvOFFRUCHandleObject;
using NvOFFRUCHandle = NvOFFRUCHandleObject*;
using PFN_NvOFFRUCCreate = NvOFFRUCStatus (CALLBACK*)(const NvOFFRUCCreateParam*, NvOFFRUCHandle*);
using PFN_NvOFFRUCRegisterResource = NvOFFRUCStatus (CALLBACK*)(NvOFFRUCHandle, const NvOFFRUCRegisterResourceParam*);
using PFN_NvOFFRUCUnregisterResource = NvOFFRUCStatus (CALLBACK*)(NvOFFRUCHandle, const NvOFFRUCUnregisterResourceParam*);
using PFN_NvOFFRUCProcess = NvOFFRUCStatus (CALLBACK*)(NvOFFRUCHandle, const NvOFFRUCProcessInParams*, const NvOFFRUCProcessOutParams*);
using PFN_NvOFFRUCDestroy = NvOFFRUCStatus (CALLBACK*)(NvOFFRUCHandle);

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
		if (!dir.empty() && std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) {
			dirs.emplace_back(std::move(dir));
		}
	};

	wchar_t envPath[32768] = {};
	for (const wchar_t* envName : {L"NV_OFFRUC_PATH", L"NVIDIA_OPTICAL_FLOW_SDK_PATH"}) {
		ZeroMemory(envPath, sizeof(envPath));
		const DWORD len = GetEnvironmentVariableW(envName, envPath, std::size(envPath));
		if (len && len < std::size(envPath)) {
			const std::filesystem::path root(envPath);
			Add(root.wstring());
			Add((root / L"NvOFFRUC" / L"NvOFFRUCSample" / L"bin" / L"win64").wstring());
		}
	}

	const std::filesystem::path moduleDir(GetModuleDirectoryFromAddress(&g_moduleAddressMarker));
	if (!moduleDir.empty()) {
		Add(moduleDir.wstring());
		Add((moduleDir / L"NvOFFRUC").wstring());
	}

	wchar_t processPath[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, processPath, std::size(processPath))) {
		const std::filesystem::path processDir = std::filesystem::path(processPath).parent_path();
		Add(processDir.wstring());
		Add((processDir / L"NvOFFRUC").wstring());
	}

	return dirs;
}

const wchar_t* StatusName(const NvOFFRUCStatus status)
{
	switch (static_cast<int>(status)) {
	case 0: return L"success";
	case 1: return L"not supported";
	case 2: return L"invalid pointer";
	case 3: return L"invalid parameter";
	case 4: return L"invalid handle";
	case 5: return L"out of system memory";
	case 6: return L"out of video memory";
	case 9: return L"optical-flow failure";
	case 12: return L"incorrect API sequence";
	case 14: return L"pipeline execution failure";
	case 15: return L"synchronization failure";
	default: return L"runtime error";
	}
}

#endif

} // namespace

struct CNvidiaFrameInterpolation::Impl
{
#ifdef _WIN64
	struct SharedTexture {
		CComPtr<ID3D11Texture2D> texture;
		CComPtr<ID3D11ShaderResourceView> view;
		CComPtr<IDXGIKeyedMutex> mutex;
		uint64_t key = 0;
	};

	std::recursive_mutex apiMutex;
	std::unique_lock<std::recursive_mutex> inputTransaction;
	HMODULE hCudaRuntime = nullptr;
	HMODULE hRuntime = nullptr;
	PFN_NvOFFRUCCreate Create = nullptr;
	PFN_NvOFFRUCRegisterResource RegisterResource = nullptr;
	PFN_NvOFFRUCUnregisterResource UnregisterResource = nullptr;
	PFN_NvOFFRUCProcess Process = nullptr;
	PFN_NvOFFRUCDestroy Destroy = nullptr;
	NvOFFRUCHandle handle = nullptr;
	CComPtr<ID3D11Device> device;
	std::array<SharedTexture, 2> inputs;
	SharedTexture output;
	UINT width = 0;
	UINT height = 0;
	unsigned writeIndex = 0;
	unsigned currentIndex = 0;
	bool inputLocked = false;
	bool currentLocked = false;
	bool outputLocked = false;
	bool warmedUp = false;
	std::wstring runtimeDirectory;
#endif

	std::wstring status = L"Disabled";
	std::wstring runtimeInfo;
	double processTimeMs = 0.0;

#ifdef _WIN64
	void SetError(const wchar_t* operation, const NvOFFRUCStatus code)
	{
		status = std::format(L"{} failed: {} ({})", operation, StatusName(code), static_cast<int>(code));
		DLog(L"NVIDIA frame interpolation: {}", status);
	}

	void ReleaseLibraries()
	{
		if (hRuntime) {
			FreeLibrary(hRuntime);
			hRuntime = nullptr;
		}
		if (hCudaRuntime) {
			FreeLibrary(hCudaRuntime);
			hCudaRuntime = nullptr;
		}
		Create = nullptr;
		RegisterResource = nullptr;
		UnregisterResource = nullptr;
		Process = nullptr;
		Destroy = nullptr;
		runtimeDirectory.clear();
		runtimeInfo.clear();
	}

	void ResetResources()
	{
		if (inputLocked) {
			inputs[writeIndex].mutex->ReleaseSync(inputs[writeIndex].key);
			inputLocked = false;
		}
		if (currentLocked) {
			inputs[currentIndex].mutex->ReleaseSync(inputs[currentIndex].key);
			currentLocked = false;
		}
		if (outputLocked) {
			output.mutex->ReleaseSync(output.key);
			outputLocked = false;
		}

		if (handle && UnregisterResource) {
			NvOFFRUCUnregisterResourceParam params = {};
			params.pArrResource[0] = output.texture;
			params.pArrResource[1] = inputs[0].texture;
			params.pArrResource[2] = inputs[1].texture;
			params.uiCount = 3;
			UnregisterResource(handle, &params);
		}
		if (handle && Destroy) {
			Destroy(handle);
		}
		handle = nullptr;
		inputs = {};
		output = {};
		device.Release();
		width = height = 0;
		writeIndex = currentIndex = 0;
		warmedUp = false;
		processTimeMs = 0.0;
	}

	void ResetAll()
	{
		ResetResources();
		ReleaseLibraries();
		status = L"Disabled";
	}

	bool LoadRuntime()
	{
		if (hRuntime) {
			return true;
		}

		for (const auto& directory : GetRuntimeSearchDirectories()) {
			const auto runtimePath = std::filesystem::path(directory) / L"NvOFFRUC.dll";
			if (!std::filesystem::is_regular_file(runtimePath)) {
				continue;
			}

			const auto cudaPath = std::filesystem::path(directory) / L"cudart64_110.dll";
			if (std::filesystem::is_regular_file(cudaPath)) {
				hCudaRuntime = LoadLibraryExW(cudaPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			}
			hRuntime = LoadLibraryExW(runtimePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (hRuntime) {
				runtimeDirectory = directory;
				break;
			}
			if (hCudaRuntime) {
				FreeLibrary(hCudaRuntime);
				hCudaRuntime = nullptr;
			}
		}

		if (!hRuntime) {
			status = L"NvOFFRUC.dll not found; install the NVIDIA Optical Flow SDK runtime and set NV_OFFRUC_PATH";
			return false;
		}

		Create = reinterpret_cast<PFN_NvOFFRUCCreate>(GetProcAddress(hRuntime, "NvOFFRUCCreate"));
		RegisterResource = reinterpret_cast<PFN_NvOFFRUCRegisterResource>(GetProcAddress(hRuntime, "NvOFFRUCRegisterResource"));
		UnregisterResource = reinterpret_cast<PFN_NvOFFRUCUnregisterResource>(GetProcAddress(hRuntime, "NvOFFRUCUnregisterResource"));
		Process = reinterpret_cast<PFN_NvOFFRUCProcess>(GetProcAddress(hRuntime, "NvOFFRUCProcess"));
		Destroy = reinterpret_cast<PFN_NvOFFRUCDestroy>(GetProcAddress(hRuntime, "NvOFFRUCDestroy"));
		if (!Create || !RegisterResource || !UnregisterResource || !Process || !Destroy) {
			status = L"NvOFFRUC.dll is missing required exports";
			ReleaseLibraries();
			return false;
		}

		runtimeInfo = runtimeDirectory;
		status = std::format(L"Runtime loaded from {}", runtimeDirectory);
		return true;
	}

	bool CreateSharedTexture(SharedTexture& target)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &target.texture);
		if (FAILED(hr)) {
			status = std::format(L"CreateTexture2D failed ({})", HR2Str(hr));
			return false;
		}
		hr = device->CreateShaderResourceView(target.texture, nullptr, &target.view);
		if (FAILED(hr)) {
			status = std::format(L"CreateShaderResourceView failed ({})", HR2Str(hr));
			return false;
		}
		hr = target.texture->QueryInterface(IID_PPV_ARGS(&target.mutex));
		if (FAILED(hr)) {
			status = std::format(L"IDXGIKeyedMutex is unavailable ({})", HR2Str(hr));
			return false;
		}
		target.key = 0;
		return true;
	}

	bool Initialize(ID3D11Device* requestedDevice, const UINT requestedWidth, const UINT requestedHeight)
	{
		if (!requestedDevice || !requestedWidth || !requestedHeight) {
			status = L"Invalid frame-interpolation dimensions or device";
			return false;
		}
		if (handle && device == requestedDevice && width == requestedWidth && height == requestedHeight) {
			return true;
		}

		ResetResources();
		if (!LoadRuntime()) {
			return false;
		}

		device = requestedDevice;
		width = requestedWidth;
		height = requestedHeight;
		if (!CreateSharedTexture(inputs[0]) || !CreateSharedTexture(inputs[1]) || !CreateSharedTexture(output)) {
			ResetResources();
			return false;
		}

		NvOFFRUCCreateParam createParams = {};
		createParams.uiWidth = width;
		createParams.uiHeight = height;
		createParams.pDevice = device;
		createParams.eResourceType = DirectX11Resource;
		createParams.eSurfaceFormat = ARGBSurface;
		createParams.eCUDAResourceType = CudaResourceTypeUndefined;
		NvOFFRUCStatus code = Create(&createParams, &handle);
		if (code != NvOFFRUC_SUCCESS) {
			SetError(L"NvOFFRUCCreate", code);
			ResetResources();
			return false;
		}

		NvOFFRUCRegisterResourceParam registerParams = {};
		registerParams.pArrResource[0] = output.texture;
		registerParams.pArrResource[1] = inputs[0].texture;
		registerParams.pArrResource[2] = inputs[1].texture;
		registerParams.pD3D11FenceObj = nullptr;
		registerParams.uiCount = 3;
		code = RegisterResource(handle, &registerParams);
		if (code != NvOFFRUC_SUCCESS) {
			SetError(L"NvOFFRUCRegisterResource", code);
			ResetResources();
			return false;
		}

		status = std::format(L"Ready, {}x{}", width, height);
		return true;
	}

	bool Acquire(SharedTexture& resource, bool& lockFlag)
	{
		if (!resource.mutex || lockFlag) {
			return false;
		}
		const HRESULT hr = resource.mutex->AcquireSync(resource.key, 2000);
		if (hr != S_OK) {
			status = std::format(L"Keyed-mutex acquire failed ({})", HR2Str(hr));
			return false;
		}
		++resource.key;
		lockFlag = true;
		return true;
	}

	void Release(SharedTexture& resource, bool& lockFlag)
	{
		if (resource.mutex && lockFlag) {
			const HRESULT hr = resource.mutex->ReleaseSync(resource.key);
			if (FAILED(hr)) {
				status = std::format(L"Keyed-mutex release failed ({})", HR2Str(hr));
			}
			lockFlag = false;
		}
	}
#endif
};

CNvidiaFrameInterpolation::CNvidiaFrameInterpolation()
	: m_impl(std::make_unique<Impl>())
{
#ifndef _WIN64
	m_impl->status = L"Requires a 64-bit build";
#endif
}

CNvidiaFrameInterpolation::~CNvidiaFrameInterpolation()
{
#ifdef _WIN64
	std::lock_guard<std::recursive_mutex> lock(m_impl->apiMutex);
	m_impl->ResetAll();
#endif
}

bool CNvidiaFrameInterpolation::Initialize(ID3D11Device* device, UINT width, UINT height)
{
#ifdef _WIN64
	std::lock_guard<std::recursive_mutex> lock(m_impl->apiMutex);
	return m_impl->Initialize(device, width, height);
#else
	UNREFERENCED_PARAMETER(device);
	UNREFERENCED_PARAMETER(width);
	UNREFERENCED_PARAMETER(height);
	return false;
#endif
}

void CNvidiaFrameInterpolation::Reset()
{
#ifdef _WIN64
	std::lock_guard<std::recursive_mutex> lock(m_impl->apiMutex);
	m_impl->ResetResources();
	m_impl->status = m_impl->hRuntime ? L"Runtime loaded" : L"Disabled";
#endif
}

bool CNvidiaFrameInterpolation::BeginInputFrame(ID3D11Texture2D** texture)
{
#ifdef _WIN64
	if (m_impl->inputTransaction.owns_lock()) {
		return false;
	}
	m_impl->inputTransaction = std::unique_lock<std::recursive_mutex>(m_impl->apiMutex);
	if (!texture || !m_impl->handle || m_impl->inputLocked) {
		m_impl->inputTransaction.unlock();
		return false;
	}
	m_impl->writeIndex = m_impl->warmedUp ? (m_impl->currentIndex ^ 1u) : 0u;
	auto& input = m_impl->inputs[m_impl->writeIndex];
	if (!m_impl->Acquire(input, m_impl->inputLocked)) {
		m_impl->inputTransaction.unlock();
		return false;
	}
	*texture = input.texture;
	return true;
#else
	UNREFERENCED_PARAMETER(texture);
	return false;
#endif
}

void CNvidiaFrameInterpolation::CancelInputFrame()
{
#ifdef _WIN64
	if (m_impl->inputLocked) {
		m_impl->Release(m_impl->inputs[m_impl->writeIndex], m_impl->inputLocked);
	}
	if (m_impl->inputTransaction.owns_lock()) {
		m_impl->inputTransaction.unlock();
	}
#endif
}

bool CNvidiaFrameInterpolation::SubmitInputFrame(double inputTimestamp, double outputTimestamp,
	bool& outputReady, bool& repeated)
{
	outputReady = false;
	repeated = false;
#ifdef _WIN64
	auto EndTransaction = [this]() {
		if (m_impl->inputTransaction.owns_lock()) {
			m_impl->inputTransaction.unlock();
		}
	};
	if (!m_impl->inputTransaction.owns_lock() || !m_impl->handle || !m_impl->inputLocked) {
		EndTransaction();
		return false;
	}

	auto& input = m_impl->inputs[m_impl->writeIndex];
	m_impl->Release(input, m_impl->inputLocked);

	NvOFFRUCProcessInParams inParams = {};
	NvOFFRUCProcessOutParams outParams = {};
	inParams.stFrameDataInput.pFrame = input.texture;
	inParams.stFrameDataInput.nTimeStamp = inputTimestamp;
	inParams.uSyncWait.MutexAcquireKey.uiKeyForRenderTextureAcquire = input.key;
	inParams.uSyncWait.MutexAcquireKey.uiKeyForInterpTextureAcquire = m_impl->output.key;

	outParams.stFrameDataOutput.pFrame = m_impl->output.texture;
	outParams.stFrameDataOutput.nTimeStamp = outputTimestamp;
	outParams.stFrameDataOutput.bHasFrameRepetitionOccurred = &repeated;
	outParams.uSyncSignal.MutexReleaseKey.uiKeyForRenderTextureRelease = ++input.key;
	outParams.uSyncSignal.MutexReleaseKey.uiKeyForInterpolateRelease = ++m_impl->output.key;

	const auto started = std::chrono::steady_clock::now();
	const NvOFFRUCStatus code = m_impl->Process(m_impl->handle, &inParams, &outParams);
	m_impl->processTimeMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - started).count();
	if (code != NvOFFRUC_SUCCESS) {
		m_impl->SetError(L"NvOFFRUCProcess", code);
		EndTransaction();
		return false;
	}

	m_impl->currentIndex = m_impl->writeIndex;
	outputReady = m_impl->warmedUp;
	m_impl->warmedUp = true;
	m_impl->status = repeated
		? std::format(L"Active, repeated frame ({:.2f} ms)", m_impl->processTimeMs)
		: std::format(L"Active ({:.2f} ms)", m_impl->processTimeMs);
	EndTransaction();
	return true;
#else
	UNREFERENCED_PARAMETER(inputTimestamp);
	UNREFERENCED_PARAMETER(outputTimestamp);
	return false;
#endif
}

bool CNvidiaFrameInterpolation::AcquireCurrentFrame(ID3D11Texture2D** texture, ID3D11ShaderResourceView** view)
{
#ifdef _WIN64
	if (!texture || !view || !m_impl->warmedUp) {
		return false;
	}
	auto& input = m_impl->inputs[m_impl->currentIndex];
	if (!m_impl->Acquire(input, m_impl->currentLocked)) {
		return false;
	}
	*texture = input.texture;
	*view = input.view;
	return true;
#else
	UNREFERENCED_PARAMETER(texture);
	UNREFERENCED_PARAMETER(view);
	return false;
#endif
}

void CNvidiaFrameInterpolation::ReleaseCurrentFrame()
{
#ifdef _WIN64
	m_impl->Release(m_impl->inputs[m_impl->currentIndex], m_impl->currentLocked);
#endif
}

bool CNvidiaFrameInterpolation::AcquireInterpolatedFrame(ID3D11Texture2D** texture, ID3D11ShaderResourceView** view)
{
#ifdef _WIN64
	if (!texture || !view || !m_impl->warmedUp) {
		return false;
	}
	if (!m_impl->Acquire(m_impl->output, m_impl->outputLocked)) {
		return false;
	}
	*texture = m_impl->output.texture;
	*view = m_impl->output.view;
	return true;
#else
	UNREFERENCED_PARAMETER(texture);
	UNREFERENCED_PARAMETER(view);
	return false;
#endif
}

void CNvidiaFrameInterpolation::ReleaseInterpolatedFrame()
{
#ifdef _WIN64
	m_impl->Release(m_impl->output, m_impl->outputLocked);
#endif
}

const std::wstring& CNvidiaFrameInterpolation::GetStatus() const
{
	return m_impl->status;
}

const std::wstring& CNvidiaFrameInterpolation::GetRuntimeInfo() const
{
	return m_impl->runtimeInfo;
}

double CNvidiaFrameInterpolation::GetLastProcessTimeMs() const
{
	return m_impl->processTimeMs;
}
