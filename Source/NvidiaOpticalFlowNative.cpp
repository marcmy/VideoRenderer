/*
 * Driver-only NVIDIA Optical Flow frame interpolation backend.
 *
 * NVIDIA installs nvofapi.dll/nvofapi64.dll with the display driver. This
 * implementation owns the D3D11 Optical Flow session and midpoint synthesis
 * shader, and does not load NvOFFRUC.dll or any CUDA runtime.
 */

#include "stdafx.h"
#include "NvidiaOpticalFlowNative.h"
#include "NvidiaOpticalFlowDenseSynthesizer.h"
#include "NvidiaOpticalFlowCapture.h"
#include "Helper.h"

#include <d3d11_4.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <sstream>
#include <vector>

namespace nvof {

constexpr uint32_t ApiVersion50 = 0x50;

enum Status : int {
	Success = 0,
	OpticalFlowNotAvailable,
	UnsupportedDevice,
	DeviceDoesNotExist,
	InvalidPointer,
	InvalidParameter,
	InvalidCall,
	InvalidVersion,
	OutOfMemory,
	NotInitialized,
	UnsupportedFeature,
	GenericError,
};

enum Bool : int {
	False = 0,
	True = 1,
};

enum PerfLevel : int {
	PerfUndefined = 0,
	PerfSlow = 5,
	PerfMedium = 10,
	PerfFast = 20,
};

enum OutputGridSize : int {
	OutputGridUndefined = 0,
	OutputGrid1 = 1,
	OutputGrid2 = 2,
	OutputGrid4 = 4,
};

enum HintGridSize : int {
	HintGridUndefined = 0,
	HintGrid1 = 1,
	HintGrid2 = 2,
	HintGrid4 = 4,
	HintGrid8 = 8,
};

enum Mode : int {
	ModeUndefined = 0,
	ModeOpticalFlow,
	ModeStereoDisparity,
};

enum BufferUsage : int {
	BufferUsageUndefined = 0,
	BufferUsageInput,
	BufferUsageOutput,
	BufferUsageHint,
	BufferUsageCost,
	BufferUsageGlobalFlow,
};

enum BufferFormat : int {
	BufferFormatUndefined = 0,
	BufferFormatGrayscale8,
	BufferFormatNv12,
	BufferFormatAbgr8,
	BufferFormatShort,
	BufferFormatShort2,
	BufferFormatUint,
	BufferFormatUint8,
};

enum StereoDisparityRange : int {
	StereoRangeUndefined = 0,
	StereoRange128 = 128,
	StereoRange256 = 256,
};

enum PredictionDirection : int {
	PredictionForward = 0,
	PredictionBoth = 2,
};

struct HandleStorage;
struct GpuBufferStorage;
struct PrivateDataStorage;
using Handle = HandleStorage*;
using GpuBufferHandle = GpuBufferStorage*;
using PrivateDataHandle = PrivateDataStorage*;

struct InitParams {
	uint32_t width;
	uint32_t height;
	OutputGridSize outputGridSize;
	HintGridSize hintGridSize;
	Mode mode;
	PerfLevel performance;
	Bool enableExternalHints;
	Bool enableOutputCost;
	PrivateDataHandle privateData;
	StereoDisparityRange disparityRange;
	Bool enableRoi;
	PredictionDirection predictionDirection = PredictionForward;
	Bool enableGlobalFlow = False;
	BufferFormat inputBufferFormat = BufferFormatAbgr8;
};

struct RoiRect {
	uint32_t startX;
	uint32_t startY;
	uint32_t width;
	uint32_t height;
};

struct ExecuteInputParams {
	GpuBufferHandle inputFrame;
	GpuBufferHandle referenceFrame;
	GpuBufferHandle externalHints;
	Bool disableTemporalHints;
	uint32_t padding;
	PrivateDataHandle privateData;
	uint32_t padding2;
	uint32_t roiCount;
	RoiRect* roiData;
};

struct ExecuteOutputParams {
	GpuBufferHandle outputBuffer;
	GpuBufferHandle outputCostBuffer;
	PrivateDataHandle privateData;
	GpuBufferHandle backwardOutputBuffer;
	GpuBufferHandle backwardOutputCostBuffer;
	GpuBufferHandle globalFlowBuffer;
};

static_assert(sizeof(InitParams) == (sizeof(void*) == 8 ? 64 : 56));
static_assert(sizeof(ExecuteInputParams) == (sizeof(void*) == 8 ? 56 : 36));
static_assert(sizeof(ExecuteOutputParams) == (sizeof(void*) == 8 ? 48 : 24));

using GetMaxSupportedApiVersionFn = Status (WINAPI*)(uint32_t* version);
using CreateOpticalFlowD3D11Fn = Status (WINAPI*)(
	ID3D11Device* device, ID3D11DeviceContext* context, Handle* handle);
using InitFn = Status (WINAPI*)(Handle handle, const InitParams* params);
using GetSurfaceFormatCountD3D11Fn = Status (WINAPI*)(
	Handle handle, BufferUsage usage, Mode mode, uint32_t* count);
using GetSurfaceFormatD3D11Fn = Status (WINAPI*)(
	Handle handle, BufferUsage usage, Mode mode, DXGI_FORMAT* formats);
using RegisterResourceD3D11Fn = Status (WINAPI*)(
	Handle handle, ID3D11Resource* resource, GpuBufferHandle* buffer);
using UnregisterResourceD3D11Fn = Status (WINAPI*)(GpuBufferHandle buffer);
using ExecuteFn = Status (WINAPI*)(
	Handle handle, const ExecuteInputParams* input, ExecuteOutputParams* output);
using DestroyFn = Status (WINAPI*)(Handle handle);
using GetLastErrorFn = Status (WINAPI*)(Handle handle, char error[], uint32_t* size);
using GetCapsFn = Status (WINAPI*)(
	Handle handle, int capability, uint32_t* values, uint32_t* size);

struct D3D11FunctionList {
	CreateOpticalFlowD3D11Fn createOpticalFlowD3D11 = nullptr;
	InitFn initialize = nullptr;
	GetSurfaceFormatCountD3D11Fn getSurfaceFormatCountD3D11 = nullptr;
	GetSurfaceFormatD3D11Fn getSurfaceFormatD3D11 = nullptr;
	RegisterResourceD3D11Fn registerResourceD3D11 = nullptr;
	UnregisterResourceD3D11Fn unregisterResourceD3D11 = nullptr;
	ExecuteFn execute = nullptr;
	DestroyFn destroy = nullptr;
	GetLastErrorFn getLastError = nullptr;
	GetCapsFn getCaps = nullptr;
};

static_assert(sizeof(D3D11FunctionList) == 10 * sizeof(void*));

using CreateInstanceD3D11Fn = Status (WINAPI*)(
	uint32_t apiVersion, D3D11FunctionList* functionList);

} // namespace nvof

namespace {

#ifdef _WIN64
constexpr wchar_t NvofModuleName[] = L"nvofapi64.dll";
#else
constexpr wchar_t NvofModuleName[] = L"nvofapi.dll";
#endif

constexpr UINT FlowGridSize = 4;
constexpr int CapsSupportedOutputGridSizes = 0;

const wchar_t* StatusName(const nvof::Status status)
{
	switch (status) {
	case nvof::Success: return L"success";
	case nvof::OpticalFlowNotAvailable: return L"optical flow unavailable";
	case nvof::UnsupportedDevice: return L"unsupported device";
	case nvof::DeviceDoesNotExist: return L"device no longer exists";
	case nvof::InvalidPointer: return L"invalid pointer";
	case nvof::InvalidParameter: return L"invalid parameter";
	case nvof::InvalidCall: return L"invalid call sequence";
	case nvof::InvalidVersion: return L"invalid API version";
	case nvof::OutOfMemory: return L"out of memory";
	case nvof::NotInitialized: return L"not initialized";
	case nvof::UnsupportedFeature: return L"unsupported feature";
	case nvof::GenericError: return L"generic driver error";
	default: return L"unknown status";
	}
}

HMODULE LoadSystemNvofModule()
{
	wchar_t systemDirectory[MAX_PATH] = {};
	const UINT length = GetSystemDirectoryW(systemDirectory, std::size(systemDirectory));
	if (!length || length >= std::size(systemDirectory)) {
		return nullptr;
	}
	return LoadLibraryW(
		(std::filesystem::path(systemDirectory) / NvofModuleName).c_str());
}

std::wstring FormatName(const DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM: return L"R8G8B8A8_UNORM";
	case DXGI_FORMAT_B8G8R8A8_UNORM: return L"B8G8R8A8_UNORM";
	case DXGI_FORMAT_NV12: return L"NV12";
	case DXGI_FORMAT_R16G16_SINT: return L"R16G16_SINT";
	case DXGI_FORMAT_R8_UINT: return L"R8_UINT";
	case DXGI_FORMAT_R32_UINT: return L"R32_UINT";
	default: return std::format(L"DXGI_FORMAT_{}", static_cast<unsigned>(format));
	}
}

std::wstring JoinFormats(const std::vector<DXGI_FORMAT>& formats)
{
	std::wstring result;
	for (const auto format : formats) {
		if (!result.empty()) {
			result.append(L", ");
		}
		result.append(FormatName(format));
	}
	return result.empty() ? L"none" : result;
}

std::wstring JoinGridSizes(const std::vector<uint32_t>& grids)
{
	std::wstring result;
	for (const auto grid : grids) {
		if (!result.empty()) {
			result.append(L", ");
		}
		result.append(std::format(L"{}x{}", grid, grid));
	}
	return result.empty() ? L"none" : result;
}


} // namespace

struct CNvidiaOpticalFlowNative::Impl
{
	struct RegisteredSurface {
		CComPtr<ID3D11Texture2D> texture;
		CComPtr<ID3D11ShaderResourceView> view;
		nvof::GpuBufferHandle nvofHandle = nullptr;
	};

	std::recursive_mutex apiMutex;
	std::unique_lock<std::recursive_mutex> inputTransaction;
	HMODULE module = nullptr;
	nvof::D3D11FunctionList api = {};
	nvof::Handle session = nullptr;
	CComPtr<ID3D11Device> device;
	CComPtr<ID3D11DeviceContext> context;
	CComPtr<ID3D11Multithread> multithread;
	std::array<RegisteredSurface, 2> inputs;
	RegisteredSurface forwardFlow;
	RegisteredSurface backwardFlow;
	RegisteredSurface forwardCost;
	RegisteredSurface backwardCost;
	bool costCaptureEnabled = false;
	CComPtr<ID3D11Texture2D> outputTexture;
	CComPtr<ID3D11ShaderResourceView> outputView;
	CComPtr<ID3D11UnorderedAccessView> outputUav;
	std::unique_ptr<CNvidiaOpticalFlowDenseSynthesizer> denseSynthesizer;
	UINT width = 0;
	UINT height = 0;
	UINT flowWidth = 0;
	UINT flowHeight = 0;
	unsigned writeIndex = 0;
	unsigned currentIndex = 0;
	bool warmedUp = false;
	bool outputValid = false;
	bool hasExecutedFlow = false;
	double previousTimestamp = 0.0;
	bool havePreviousTimestamp = false;
	unsigned apiMajor = 0;
	unsigned apiMinor = 0;
	std::wstring status = L"Disabled";
	std::wstring runtimeInfo;
	double processTimeMs = 0.0;

	std::wstring DriverError(const nvof::Status code) const
	{
		std::wstring result = std::format(
			L"{} ({})", StatusName(code), static_cast<int>(code));
		if (api.getLastError && session) {
			char message[512] = {};
			uint32_t size = static_cast<uint32_t>(std::size(message));
			if (api.getLastError(session, message, &size) == nvof::Success && size) {
				result.append(L": ");
				for (uint32_t i = 0; i < size && i < std::size(message); ++i) {
					if (!message[i]) break;
					result.push_back(static_cast<wchar_t>(
						static_cast<unsigned char>(message[i])));
				}
			}
		}
		return result;
	}

	void Unregister(RegisteredSurface& surface)
	{
		if (surface.nvofHandle && api.unregisterResourceD3D11) {
			api.unregisterResourceD3D11(surface.nvofHandle);
		}
		surface.nvofHandle = nullptr;
		surface.view.Release();
		surface.texture.Release();
	}


	void SoftResetUnlocked()
	{
		writeIndex = currentIndex = 0;
		warmedUp = false;
		outputValid = false;
		hasExecutedFlow = false;
		havePreviousTimestamp = false;
		previousTimestamp = 0.0;
		processTimeMs = 0.0;
		status = session ? L"Native NVOF reset; waiting for frames" : L"Disabled";
	}

	void ResetUnlocked()
	{
		Unregister(forwardCost);
		Unregister(backwardCost);
		Unregister(forwardFlow);
		Unregister(backwardFlow);
		Unregister(inputs[0]);
		Unregister(inputs[1]);
		if (session && api.destroy) {
			api.destroy(session);
		}
		session = nullptr;
		outputUav.Release();
		outputView.Release();
		outputTexture.Release();
		if (denseSynthesizer) {
			denseSynthesizer->Reset();
			denseSynthesizer.reset();
		}
		multithread.Release();
		context.Release();
		device.Release();
		if (module) {
			FreeLibrary(module);
			module = nullptr;
		}
		api = {};
		width = height = flowWidth = flowHeight = 0;
		writeIndex = currentIndex = 0;
		warmedUp = outputValid = hasExecutedFlow = false;
		havePreviousTimestamp = false;
		previousTimestamp = 0.0;
		processTimeMs = 0.0;
		apiMajor = apiMinor = 0;
		runtimeInfo.clear();
		costCaptureEnabled = false;
	}

	bool Fail(const std::wstring& message)
	{
		status = message;
		DLog(L"Native NVIDIA frame interpolation: {}", status);
		const std::wstring saved = status;
		ResetUnlocked();
		status = saved;
		return false;
	}

	bool QueryFormats(const nvof::BufferUsage usage, std::vector<DXGI_FORMAT>& formats)
	{
		uint32_t count = 0;
		nvof::Status code = api.getSurfaceFormatCountD3D11(
			session, usage, nvof::ModeOpticalFlow, &count);
		if (code != nvof::Success) {
			return false;
		}
		formats.assign(count, DXGI_FORMAT_UNKNOWN);
		if (!count) {
			return true;
		}
		code = api.getSurfaceFormatD3D11(
			session, usage, nvof::ModeOpticalFlow, formats.data());
		return code == nvof::Success;
	}

	bool QueryCapability(const int capability, std::vector<uint32_t>& values)
	{
		if (!api.getCaps) {
			return false;
		}
		uint32_t count = 0;
		nvof::Status code = api.getCaps(session, capability, nullptr, &count);
		if (code != nvof::Success) {
			return false;
		}
		values.assign(count, 0);
		if (!count) {
			return true;
		}
		code = api.getCaps(session, capability, values.data(), &count);
		if (code != nvof::Success) {
			return false;
		}
		values.resize(count);
		return true;
	}

	bool CreateInputSurface(RegisteredSurface& surface)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &surface.texture);
		if (FAILED(hr)) {
			status = std::format(L"CreateTexture2D(native input) failed ({})", HR2Str(hr));
			return false;
		}
		hr = device->CreateShaderResourceView(surface.texture, nullptr, &surface.view);
		if (FAILED(hr)) {
			status = std::format(L"CreateShaderResourceView(native input) failed ({})", HR2Str(hr));
			return false;
		}
		const nvof::Status code = api.registerResourceD3D11(
			session, surface.texture, &surface.nvofHandle);
		if (code != nvof::Success) {
			status = std::format(L"NvOFRegisterResourceD3D11(input) failed: {}", DriverError(code));
			return false;
		}
		return true;
	}

	bool CreateFlowSurface(RegisteredSurface& surface)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = flowWidth;
		desc.Height = flowHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16_SINT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET |
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &surface.texture);
		if (FAILED(hr)) {
			status = std::format(L"CreateTexture2D(native flow) failed ({})", HR2Str(hr));
			return false;
		}
		hr = device->CreateShaderResourceView(surface.texture, nullptr, &surface.view);
		if (FAILED(hr)) {
			status = std::format(L"CreateShaderResourceView(native flow) failed ({})", HR2Str(hr));
			return false;
		}
		const nvof::Status code = api.registerResourceD3D11(
			session, surface.texture, &surface.nvofHandle);
		if (code != nvof::Success) {
			status = std::format(L"NvOFRegisterResourceD3D11(flow) failed: {}", DriverError(code));
			return false;
		}
		return true;
	}


	bool CreateCostSurface(RegisteredSurface& surface)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = flowWidth;
		desc.Height = flowHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8_UINT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &surface.texture);
		if (FAILED(hr)) {
			status = std::format(L"CreateTexture2D(native diagnostic cost) failed ({})", HR2Str(hr));
			return false;
		}
		hr = device->CreateShaderResourceView(surface.texture, nullptr, &surface.view);
		if (FAILED(hr)) {
			status = std::format(L"CreateShaderResourceView(native diagnostic cost) failed ({})", HR2Str(hr));
			return false;
		}
		const nvof::Status code = api.registerResourceD3D11(
			session, surface.texture, &surface.nvofHandle);
		if (code != nvof::Success) {
			status = std::format(L"NvOFRegisterResourceD3D11(diagnostic cost) failed: {}", DriverError(code));
			return false;
		}
		return true;
	}

	bool CreateSynthesisResources()
	{
		D3D11_TEXTURE2D_DESC outputDesc = {};
		outputDesc.Width = width;
		outputDesc.Height = height;
		outputDesc.MipLevels = 1;
		outputDesc.ArraySize = 1;
		outputDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		outputDesc.SampleDesc.Count = 1;
		outputDesc.Usage = D3D11_USAGE_DEFAULT;
		outputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		HRESULT hr = device->CreateTexture2D(&outputDesc, nullptr, &outputTexture);
		if (FAILED(hr)) {
			status = std::format(L"CreateTexture2D(native midpoint) failed ({})", HR2Str(hr));
			return false;
		}
		hr = device->CreateShaderResourceView(outputTexture, nullptr, &outputView);
		if (FAILED(hr)) {
			status = std::format(L"CreateShaderResourceView(native midpoint) failed ({})", HR2Str(hr));
			return false;
		}
		hr = device->CreateUnorderedAccessView(outputTexture, nullptr, &outputUav);
		if (FAILED(hr)) {
			status = std::format(L"CreateUnorderedAccessView(native midpoint) failed ({})", HR2Str(hr));
			return false;
		}


		denseSynthesizer = std::make_unique<CNvidiaOpticalFlowDenseSynthesizer>();
		if (!denseSynthesizer->Initialize(device, width, height, flowWidth, flowHeight, status)) {
			return false;
		}

		return true;
	}

	bool Initialize(ID3D11Device* requestedDevice, const UINT requestedWidth, const UINT requestedHeight)
	{
		std::lock_guard<std::recursive_mutex> lock(apiMutex);
		if (!requestedDevice || !requestedWidth || !requestedHeight) {
			status = L"Invalid native NVOF device or dimensions";
			return false;
		}
		if (session && device == requestedDevice && width == requestedWidth && height == requestedHeight) {
			return true;
		}

		ResetUnlocked();
		module = LoadSystemNvofModule();
		if (!module) {
			return Fail(std::format(L"{} is not available from the NVIDIA display driver", NvofModuleName));
		}

		const auto getVersion = reinterpret_cast<nvof::GetMaxSupportedApiVersionFn>(
			GetProcAddress(module, "NvOFGetMaxSupportedApiVersion"));
		const auto createInstance = reinterpret_cast<nvof::CreateInstanceD3D11Fn>(
			GetProcAddress(module, "NvOFAPICreateInstanceD3D11"));
		if (!getVersion || !createInstance) {
			return Fail(L"The display-driver NVOF module is missing required D3D11 exports");
		}

		uint32_t version = 0;
		nvof::Status code = getVersion(&version);
		if (code != nvof::Success || version < nvof::ApiVersion50) {
			return Fail(std::format(L"NVOF API 5.0 is unavailable: {}", StatusName(code)));
		}
		apiMajor = version >> 4;
		apiMinor = version & 0x0f;

		code = createInstance(nvof::ApiVersion50, &api);
		if (code != nvof::Success || !api.createOpticalFlowD3D11 ||
				!api.initialize || !api.getSurfaceFormatCountD3D11 ||
				!api.getSurfaceFormatD3D11 || !api.registerResourceD3D11 ||
				!api.unregisterResourceD3D11 || !api.execute || !api.destroy || !api.getCaps) {
			return Fail(L"Could not create the NVOF D3D11 function table");
		}

		device = requestedDevice;
		device->GetImmediateContext(&context);
		if (!context) {
			return Fail(L"The renderer D3D11 immediate context is unavailable");
		}
		context->QueryInterface(IID_PPV_ARGS(&multithread));
		if (multithread) {
			multithread->SetMultithreadProtected(TRUE);
		}

		width = requestedWidth;
		height = requestedHeight;
		flowWidth = (width + FlowGridSize - 1) / FlowGridSize;
		flowHeight = (height + FlowGridSize - 1) / FlowGridSize;

		code = api.createOpticalFlowD3D11(device, context, &session);
		if (code != nvof::Success || !session) {
			return Fail(std::format(L"NvCreateOpticalFlowD3D11 failed: {}", DriverError(code)));
		}

		std::vector<uint32_t> outputGridSizes;
		if (!QueryCapability(CapsSupportedOutputGridSizes, outputGridSizes)) {
			return Fail(L"Could not query native NVOF output-grid capabilities");
		}
		if (std::find(outputGridSizes.begin(), outputGridSizes.end(), FlowGridSize) == outputGridSizes.end()) {
			return Fail(std::format(
				L"Native NVOF 4x4 flow is required by the validated synthesis pipeline; GPU/driver supports: {}",
				JoinGridSizes(outputGridSizes)));
		}

		std::vector<DXGI_FORMAT> inputFormats;
		std::vector<DXGI_FORMAT> outputFormats;
		if (!QueryFormats(nvof::BufferUsageInput, inputFormats) ||
				!QueryFormats(nvof::BufferUsageOutput, outputFormats)) {
			return Fail(L"Could not query native NVOF D3D11 surface formats");
		}
		if (std::find(inputFormats.begin(), inputFormats.end(), DXGI_FORMAT_B8G8R8A8_UNORM) == inputFormats.end()) {
			return Fail(std::format(
				L"Native NVOF cannot consume the renderer BGRA8 surface; supported input formats: {}",
				JoinFormats(inputFormats)));
		}
		if (std::find(outputFormats.begin(), outputFormats.end(), DXGI_FORMAT_R16G16_SINT) == outputFormats.end()) {
			return Fail(std::format(
				L"Native NVOF R16G16_SINT flow output is unavailable; supported formats: {}",
				JoinFormats(outputFormats)));
		}

		// Hardware cost output is intentionally disabled in the live renderer.
		// On Turing it can severely stall startup/seeks and sustained playback.
		// Real-frame capture still records source frames, flow, consistency, and midpoint.
		costCaptureEnabled = false;

		nvof::InitParams init = {};
		init.width = width;
		init.height = height;
		init.outputGridSize = nvof::OutputGrid4;
		init.hintGridSize = nvof::HintGridUndefined;
		init.mode = nvof::ModeOpticalFlow;
		init.performance = nvof::PerfSlow;
		init.enableExternalHints = nvof::False;
		init.enableOutputCost = nvof::False;
		init.disparityRange = nvof::StereoRangeUndefined;
		init.enableRoi = nvof::False;
		init.predictionDirection = nvof::PredictionBoth;
		init.enableGlobalFlow = nvof::False;
		init.inputBufferFormat = nvof::BufferFormatAbgr8;
		code = api.initialize(session, &init);
		if (code != nvof::Success) {
			return Fail(std::format(L"NvOFInit(native forward/backward) failed: {}", DriverError(code)));
		}

		if (!CreateInputSurface(inputs[0]) || !CreateInputSurface(inputs[1]) ||
				!CreateFlowSurface(forwardFlow) || !CreateFlowSurface(backwardFlow) ||
				(costCaptureEnabled && (!CreateCostSurface(forwardCost) || !CreateCostSurface(backwardCost))) ||
				!CreateSynthesisResources()) {
			const std::wstring saved = status;
			ResetUnlocked();
			status = saved;
			return false;
		}

		runtimeInfo = std::format(
			L"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow (GPU grids: {}); validated jump-flood dense flow + edge-aware next-frame warp + frame quality gate; live cost disabled",
			apiMajor, apiMinor, JoinGridSizes(outputGridSizes));
		status = std::format(L"Native NVOF ready, {}x{}", width, height);
		DLog(L"Native NVIDIA frame interpolation: {}", runtimeInfo);
		return true;
	}

	bool DispatchMidpoint(const float midpointTime)
	{
		if (!denseSynthesizer) {
			status = L"Native NVOF dense synthesizer is unavailable";
			return false;
		}
		return denseSynthesizer->Dispatch(
			context,
			inputs[currentIndex].view,
			inputs[writeIndex].view,
			forwardFlow.view,
			backwardFlow.view,
			outputUav,
			midpointTime,
			status);
	}

	bool BeginInputFrame(ID3D11Texture2D** texture)
	{
		if (inputTransaction.owns_lock()) {
			return false;
		}
		inputTransaction = std::unique_lock<std::recursive_mutex>(apiMutex);
		if (!texture || !session) {
			inputTransaction.unlock();
			return false;
		}
		writeIndex = warmedUp ? (currentIndex ^ 1u) : 0u;
		*texture = inputs[writeIndex].texture;
		return *texture != nullptr;
	}

	void CancelInputFrame()
	{
		if (inputTransaction.owns_lock()) {
			inputTransaction.unlock();
		}
	}

	bool SubmitInputFrame(const double inputTimestamp, const double outputTimestamp,
		bool& outputReady, bool& repeated)
	{
		outputReady = false;
		repeated = false;
		if (!inputTransaction.owns_lock() || !session) {
			if (inputTransaction.owns_lock()) inputTransaction.unlock();
			return false;
		}

		auto Finish = [this]() {
			if (inputTransaction.owns_lock()) inputTransaction.unlock();
		};

		if (!warmedUp) {
			currentIndex = writeIndex;
			warmedUp = true;
			havePreviousTimestamp = true;
			previousTimestamp = inputTimestamp;
			status = L"Native NVOF primed; waiting for the next source frame";
			Finish();
			return true;
		}
		if (!havePreviousTimestamp || inputTimestamp <= previousTimestamp) {
			status = L"Native NVOF received a non-increasing source timestamp";
			Finish();
			return false;
		}

		const double interval = inputTimestamp - previousTimestamp;
		const float midpointTime = static_cast<float>(std::clamp(
			(outputTimestamp - previousTimestamp) / interval, 0.0, 1.0));

		nvof::ExecuteInputParams input = {};
		input.inputFrame = inputs[writeIndex].nvofHandle;
		input.referenceFrame = inputs[currentIndex].nvofHandle;
		input.disableTemporalHints = hasExecutedFlow ? nvof::False : nvof::True;
		nvof::ExecuteOutputParams output = {};
		output.outputBuffer = forwardFlow.nvofHandle;
		output.backwardOutputBuffer = backwardFlow.nvofHandle;
		if (costCaptureEnabled) {
			output.outputCostBuffer = forwardCost.nvofHandle;
			output.backwardOutputCostBuffer = backwardCost.nvofHandle;
		}

		const auto started = std::chrono::steady_clock::now();
		const nvof::Status code = api.execute(session, &input, &output);
		if (code != nvof::Success) {
			status = std::format(L"NvOFExecute(native forward/backward) failed: {}", DriverError(code));
			Finish();
			return false;
		}
		if (!DispatchMidpoint(midpointTime)) {
			Finish();
			return false;
		}

		if (IsNativeNvofCaptureRequested()) {
			NativeNvofCaptureInputs capture = {};
			capture.device = device;
			capture.context = context;
			capture.firstFrame = inputs[currentIndex].texture;
			capture.secondFrame = inputs[writeIndex].texture;
			capture.midpointFrame = outputTexture;
			capture.forwardFlow = forwardFlow.texture;
			capture.backwardFlow = backwardFlow.texture;
			capture.forwardCost = costCaptureEnabled ? forwardCost.texture.p : nullptr;
			capture.backwardCost = costCaptureEnabled ? backwardCost.texture.p : nullptr;
			capture.frameWidth = width;
			capture.frameHeight = height;
			capture.flowWidth = flowWidth;
			capture.flowHeight = flowHeight;
			capture.midpointTime = midpointTime;
			capture.firstTimestamp = previousTimestamp;
			capture.secondTimestamp = inputTimestamp;

			std::wstring captureDirectory;
			std::wstring captureError;
			if (CaptureNativeNvofFramePair(capture, captureDirectory, captureError)) {
				DLog(L"Native NVOF diagnostic capture saved to {}", captureDirectory);
			} else {
				DLog(L"Native NVOF diagnostic capture failed: {}", captureError);
			}
		}

		processTimeMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();

		hasExecutedFlow = true;
		outputValid = true;
		outputReady = true;
		currentIndex = writeIndex;
		previousTimestamp = inputTimestamp;
		status = std::format(
			L"Native NVOF active ({:.2f} ms submit, t={:.3f})",
			processTimeMs, midpointTime);
		Finish();
		return true;
	}
};

CNvidiaOpticalFlowNativeProbe::Result CNvidiaOpticalFlowNativeProbe::Probe()
{
	Result result;
	result.moduleName = NvofModuleName;
	HMODULE module = LoadSystemNvofModule();
	if (!module) {
		result.status = std::format(L"{} is not available from the NVIDIA display driver", NvofModuleName);
		return result;
	}
	const auto getVersion = reinterpret_cast<nvof::GetMaxSupportedApiVersionFn>(
		GetProcAddress(module, "NvOFGetMaxSupportedApiVersion"));
	const FARPROC createD3D11 = GetProcAddress(module, "NvOFAPICreateInstanceD3D11");
	if (getVersion && createD3D11) {
		uint32_t version = 0;
		if (getVersion(&version) == nvof::Success) {
			result.apiMajor = version >> 4;
			result.apiMinor = version & 0x0f;
			result.available = true;
			result.status = std::format(
				L"Native NVIDIA Optical Flow API {}.{} is available through {}",
				result.apiMajor, result.apiMinor, result.moduleName);
		}
	}
	if (result.status.empty()) {
		result.status = std::format(L"{} is missing required native NVOF exports", NvofModuleName);
	}
	FreeLibrary(module);
	return result;
}

CNvidiaOpticalFlowNative::CNvidiaOpticalFlowNative()
	: m_impl(std::make_unique<Impl>())
{
#ifndef _WIN64
	m_impl->status = L"Native NVOF interpolation requires a 64-bit build";
#endif
}

CNvidiaOpticalFlowNative::~CNvidiaOpticalFlowNative()
{
#ifdef _WIN64
	if (m_impl->inputTransaction.owns_lock()) {
		m_impl->inputTransaction.unlock();
	}
	std::lock_guard<std::recursive_mutex> lock(m_impl->apiMutex);
	m_impl->ResetUnlocked();
#endif
}

bool CNvidiaOpticalFlowNative::Initialize(ID3D11Device* device, const UINT width, const UINT height)
{
#ifdef _WIN64
	return m_impl->Initialize(device, width, height);
#else
	UNREFERENCED_PARAMETER(device);
	UNREFERENCED_PARAMETER(width);
	UNREFERENCED_PARAMETER(height);
	return false;
#endif
}

void CNvidiaOpticalFlowNative::Reset()
{
#ifdef _WIN64
	if (m_impl->inputTransaction.owns_lock()) {
		m_impl->inputTransaction.unlock();
	}
	std::lock_guard<std::recursive_mutex> lock(m_impl->apiMutex);
	// Playback discontinuities only invalidate frame/temporal history.
	// Preserve the initialized driver session and D3D11 resources so a seek
	// cannot force synchronous shader compilation/session reconstruction.
	m_impl->SoftResetUnlocked();
#endif
}

bool CNvidiaOpticalFlowNative::BeginInputFrame(ID3D11Texture2D** texture)
{
#ifdef _WIN64
	return m_impl->BeginInputFrame(texture);
#else
	UNREFERENCED_PARAMETER(texture);
	return false;
#endif
}

void CNvidiaOpticalFlowNative::CancelInputFrame()
{
#ifdef _WIN64
	m_impl->CancelInputFrame();
#endif
}

bool CNvidiaOpticalFlowNative::SubmitInputFrame(const double inputTimestamp, const double outputTimestamp,
	bool& outputReady, bool& repeated)
{
#ifdef _WIN64
	return m_impl->SubmitInputFrame(inputTimestamp, outputTimestamp, outputReady, repeated);
#else
	UNREFERENCED_PARAMETER(inputTimestamp);
	UNREFERENCED_PARAMETER(outputTimestamp);
	outputReady = false;
	repeated = false;
	return false;
#endif
}

bool CNvidiaOpticalFlowNative::AcquireCurrentFrame(ID3D11Texture2D** texture, ID3D11ShaderResourceView** view)
{
#ifdef _WIN64
	std::lock_guard<std::recursive_mutex> lock(m_impl->apiMutex);
	if (!texture || !view || !m_impl->warmedUp) return false;
	*texture = m_impl->inputs[m_impl->currentIndex].texture;
	*view = m_impl->inputs[m_impl->currentIndex].view;
	return *texture && *view;
#else
	UNREFERENCED_PARAMETER(texture);
	UNREFERENCED_PARAMETER(view);
	return false;
#endif
}

void CNvidiaOpticalFlowNative::ReleaseCurrentFrame()
{
}

bool CNvidiaOpticalFlowNative::AcquireInterpolatedFrame(ID3D11Texture2D** texture, ID3D11ShaderResourceView** view)
{
#ifdef _WIN64
	std::lock_guard<std::recursive_mutex> lock(m_impl->apiMutex);
	if (!texture || !view || !m_impl->outputValid) return false;
	*texture = m_impl->outputTexture;
	*view = m_impl->outputView;
	return *texture && *view;
#else
	UNREFERENCED_PARAMETER(texture);
	UNREFERENCED_PARAMETER(view);
	return false;
#endif
}

void CNvidiaOpticalFlowNative::ReleaseInterpolatedFrame()
{
}

const std::wstring& CNvidiaOpticalFlowNative::GetStatus() const
{
	return m_impl->status;
}

const std::wstring& CNvidiaOpticalFlowNative::GetRuntimeInfo() const
{
	return m_impl->runtimeInfo;
}

double CNvidiaOpticalFlowNative::GetLastProcessTimeMs() const
{
	return m_impl->processTimeMs;
}
