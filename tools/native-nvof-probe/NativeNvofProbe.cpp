/*
 * Native NVIDIA Optical Flow D3D11 capability probe.
 *
 * This tool uses only the driver-installed nvofapi64.dll. It does not link to
 * or redistribute NvOFFRUC.dll, the CUDA runtime, or the Optical Flow SDK.
 *
 * The small ABI declarations below mirror the public NVIDIA Optical Flow 2.0
 * D3D11 interface required for session creation and capability discovery.
 */

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kNvOfApiVersion = 0x20; // Public NVOF API 2.0.
constexpr UINT kNvidiaVendorId = 0x10DE;

enum NvOFStatus : int {
    NV_OF_SUCCESS = 0,
    NV_OF_ERR_OF_NOT_AVAILABLE,
    NV_OF_ERR_UNSUPPORTED_DEVICE,
    NV_OF_ERR_DEVICE_DOES_NOT_EXIST,
    NV_OF_ERR_INVALID_PTR,
    NV_OF_ERR_INVALID_PARAM,
    NV_OF_ERR_INVALID_CALL,
    NV_OF_ERR_INVALID_VERSION,
    NV_OF_ERR_OUT_OF_MEMORY,
    NV_OF_ERR_NOT_INITIALIZED,
    NV_OF_ERR_UNSUPPORTED_FEATURE,
    NV_OF_ERR_GENERIC,
};

enum NvOFMode : int {
    NV_OF_MODE_UNDEFINED = 0,
    NV_OF_MODE_OPTICALFLOW,
    NV_OF_MODE_STEREODISPARITY,
};

enum NvOFBufferUsage : int {
    NV_OF_BUFFER_USAGE_UNDEFINED = 0,
    NV_OF_BUFFER_USAGE_INPUT,
    NV_OF_BUFFER_USAGE_OUTPUT,
    NV_OF_BUFFER_USAGE_HINT,
    NV_OF_BUFFER_USAGE_COST,
};

enum NvOFCaps : int {
    NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES = 0,
    NV_OF_CAPS_SUPPORTED_HINT_GRID_SIZES,
    NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE,
    NV_OF_CAPS_SUPPORT_HINT_WITH_ST_MODE,
    NV_OF_CAPS_WIDTH_MIN,
    NV_OF_CAPS_HEIGHT_MIN,
    NV_OF_CAPS_WIDTH_MAX,
    NV_OF_CAPS_HEIGHT_MAX,
    NV_OF_CAPS_SUPPORT_ROI,
    NV_OF_CAPS_SUPPORT_ROI_MAX_NUM,
};

struct NvOFHandle_st;
struct NvOFGPUBufferHandle_st;
using NvOFHandle = NvOFHandle_st*;
using NvOFGPUBufferHandle = NvOFGPUBufferHandle_st*;

using PFN_NvOFGetMaxSupportedApiVersion = NvOFStatus (WINAPI*)(uint32_t* version);
using PFN_NvCreateOpticalFlowD3D11 = NvOFStatus (WINAPI*)(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    NvOFHandle* handle);
using PFN_NvOFInit = NvOFStatus (WINAPI*)(NvOFHandle handle, const void* initParams);
using PFN_NvOFGetSurfaceFormatCountD3D11 = NvOFStatus (WINAPI*)(
    NvOFHandle handle,
    NvOFBufferUsage usage,
    NvOFMode mode,
    uint32_t* count);
using PFN_NvOFGetSurfaceFormatD3D11 = NvOFStatus (WINAPI*)(
    NvOFHandle handle,
    NvOFBufferUsage usage,
    NvOFMode mode,
    DXGI_FORMAT* formats);
using PFN_NvOFRegisterResourceD3D11 = NvOFStatus (WINAPI*)(
    NvOFHandle handle,
    ID3D11Resource* resource,
    NvOFGPUBufferHandle* bufferHandle);
using PFN_NvOFUnregisterResourceD3D11 = NvOFStatus (WINAPI*)(NvOFGPUBufferHandle bufferHandle);
using PFN_NvOFExecute = NvOFStatus (WINAPI*)(NvOFHandle handle, const void* inputParams, void* outputParams);
using PFN_NvOFDestroy = NvOFStatus (WINAPI*)(NvOFHandle handle);
using PFN_NvOFGetLastError = NvOFStatus (WINAPI*)(NvOFHandle handle, char error[], uint32_t* size);
using PFN_NvOFGetCaps = NvOFStatus (WINAPI*)(
    NvOFHandle handle,
    NvOFCaps capability,
    uint32_t* values,
    uint32_t* size);

struct NvOFD3D11ApiFunctionList {
    PFN_NvCreateOpticalFlowD3D11 nvCreateOpticalFlowD3D11 = nullptr;
    PFN_NvOFInit nvOFInit = nullptr;
    PFN_NvOFGetSurfaceFormatCountD3D11 nvOFGetSurfaceFormatCountD3D11 = nullptr;
    PFN_NvOFGetSurfaceFormatD3D11 nvOFGetSurfaceFormatD3D11 = nullptr;
    PFN_NvOFRegisterResourceD3D11 nvOFRegisterResourceD3D11 = nullptr;
    PFN_NvOFUnregisterResourceD3D11 nvOFUnregisterResourceD3D11 = nullptr;
    PFN_NvOFExecute nvOFExecute = nullptr;
    PFN_NvOFDestroy nvOFDestroy = nullptr;
    PFN_NvOFGetLastError nvOFGetLastError = nullptr;
    PFN_NvOFGetCaps nvOFGetCaps = nullptr;
};

static_assert(sizeof(NvOFD3D11ApiFunctionList) == 10 * sizeof(void*));

using PFN_NvOFAPICreateInstanceD3D11 = NvOFStatus (WINAPI*)(
    uint32_t apiVersion,
    NvOFD3D11ApiFunctionList* functionList);

struct ModuleGuard {
    HMODULE value = nullptr;

    ~ModuleGuard()
    {
        if (value) {
            FreeLibrary(value);
        }
    }
};

struct NvOFHandleGuard {
    NvOFHandle value = nullptr;
    PFN_NvOFDestroy destroy = nullptr;

    ~NvOFHandleGuard()
    {
        if (value && destroy) {
            destroy(value);
        }
    }
};

const wchar_t* StatusName(const NvOFStatus status)
{
    switch (status) {
    case NV_OF_SUCCESS: return L"success";
    case NV_OF_ERR_OF_NOT_AVAILABLE: return L"optical flow unavailable";
    case NV_OF_ERR_UNSUPPORTED_DEVICE: return L"unsupported device";
    case NV_OF_ERR_DEVICE_DOES_NOT_EXIST: return L"device no longer exists";
    case NV_OF_ERR_INVALID_PTR: return L"invalid pointer";
    case NV_OF_ERR_INVALID_PARAM: return L"invalid parameter";
    case NV_OF_ERR_INVALID_CALL: return L"invalid call sequence";
    case NV_OF_ERR_INVALID_VERSION: return L"invalid API version";
    case NV_OF_ERR_OUT_OF_MEMORY: return L"out of memory";
    case NV_OF_ERR_NOT_INITIALIZED: return L"not initialized";
    case NV_OF_ERR_UNSUPPORTED_FEATURE: return L"unsupported feature";
    case NV_OF_ERR_GENERIC: return L"generic driver error";
    default: return L"unknown status";
    }
}

std::wstring HResultText(const HRESULT hr)
{
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return stream.str();
}

HMODULE LoadNvofModule()
{
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
    if (!length || length >= std::size(systemDirectory)) {
        return nullptr;
    }

    const std::filesystem::path modulePath =
        std::filesystem::path(systemDirectory) / L"nvofapi64.dll";
    return LoadLibraryW(modulePath.c_str());
}

bool CreateNvidiaD3D11Device(
    ComPtr<IDXGIAdapter1>& selectedAdapter,
    ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11DeviceContext>& context,
    std::wstring& adapterName)
{
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::wcerr << L"CreateDXGIFactory1 failed: " << HResultText(hr) << L'\n';
        return false;
    }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            std::wcerr << L"EnumAdapters1 failed: " << HResultText(hr) << L'\n';
            return false;
        }

        DXGI_ADAPTER_DESC1 description = {};
        adapter->GetDesc1(&description);
        if (description.VendorId != kNvidiaVendorId ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }

        const D3D_FEATURE_LEVEL requestedLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            0,
            requestedLevels,
            static_cast<UINT>(std::size(requestedLevels)),
            D3D11_SDK_VERSION,
            &device,
            &createdLevel,
            &context);
        if (FAILED(hr)) {
            std::wcerr << L"D3D11CreateDevice failed for " << description.Description
                       << L": " << HResultText(hr) << L'\n';
            return false;
        }

        selectedAdapter = adapter;
        adapterName = description.Description;
        return true;
    }

    std::wcerr << L"No NVIDIA hardware adapter was found.\n";
    return false;
}

bool QueryCaps(
    const NvOFD3D11ApiFunctionList& api,
    const NvOFHandle handle,
    const NvOFCaps capability,
    std::vector<uint32_t>& values)
{
    uint32_t size = 0;
    NvOFStatus status = api.nvOFGetCaps(handle, capability, nullptr, &size);
    if (status != NV_OF_SUCCESS) {
        std::wcerr << L"nvOFGetCaps(" << static_cast<int>(capability) << L") size query failed: "
                   << StatusName(status) << L" (" << static_cast<int>(status) << L")\n";
        return false;
    }

    values.assign(size, 0);
    if (!size) {
        return true;
    }

    status = api.nvOFGetCaps(handle, capability, values.data(), &size);
    if (status != NV_OF_SUCCESS) {
        std::wcerr << L"nvOFGetCaps(" << static_cast<int>(capability) << L") value query failed: "
                   << StatusName(status) << L" (" << static_cast<int>(status) << L")\n";
        return false;
    }
    values.resize(size);
    return true;
}

bool QuerySurfaceFormats(
    const NvOFD3D11ApiFunctionList& api,
    const NvOFHandle handle,
    const NvOFBufferUsage usage,
    std::vector<DXGI_FORMAT>& formats)
{
    uint32_t count = 0;
    NvOFStatus status = api.nvOFGetSurfaceFormatCountD3D11(
        handle, usage, NV_OF_MODE_OPTICALFLOW, &count);
    if (status != NV_OF_SUCCESS) {
        std::wcerr << L"Surface-format count query failed: " << StatusName(status)
                   << L" (" << static_cast<int>(status) << L")\n";
        return false;
    }

    formats.assign(count, DXGI_FORMAT_UNKNOWN);
    if (!count) {
        return true;
    }

    status = api.nvOFGetSurfaceFormatD3D11(
        handle, usage, NV_OF_MODE_OPTICALFLOW, formats.data());
    if (status != NV_OF_SUCCESS) {
        std::wcerr << L"Surface-format query failed: " << StatusName(status)
                   << L" (" << static_cast<int>(status) << L")\n";
        return false;
    }
    return true;
}

std::wstring FormatName(const DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_NV12: return L"NV12";
    case DXGI_FORMAT_R8_UNORM: return L"R8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return L"R8G8B8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return L"B8G8R8A8_UNORM";
    case DXGI_FORMAT_R16_SINT: return L"R16_SINT";
    case DXGI_FORMAT_R16G16_SINT: return L"R16G16_SINT";
    case DXGI_FORMAT_R32_UINT: return L"R32_UINT";
    case DXGI_FORMAT_R8_UINT: return L"R8_UINT";
    default: return L"DXGI_FORMAT_" + std::to_wstring(static_cast<unsigned>(format));
    }
}

void PrintFormats(const wchar_t* label, const std::vector<DXGI_FORMAT>& formats)
{
    std::wcout << label << L": ";
    if (formats.empty()) {
        std::wcout << L"none";
    } else {
        for (size_t index = 0; index < formats.size(); ++index) {
            if (index) {
                std::wcout << L", ";
            }
            std::wcout << FormatName(formats[index]);
        }
    }
    std::wcout << L'\n';
}

void PrintGridSizes(const std::vector<uint32_t>& values)
{
    std::wcout << L"Output vector grids: ";
    if (values.empty()) {
        std::wcout << L"none";
    } else {
        for (size_t index = 0; index < values.size(); ++index) {
            if (index) {
                std::wcout << L", ";
            }
            std::wcout << values[index] << L'x' << values[index];
        }
    }
    std::wcout << L'\n';
}

bool GetSingleCapability(
    const NvOFD3D11ApiFunctionList& api,
    const NvOFHandle handle,
    const NvOFCaps capability,
    uint32_t& value)
{
    std::vector<uint32_t> values;
    if (!QueryCaps(api, handle, capability, values) || values.empty()) {
        return false;
    }
    value = values.front();
    return true;
}

} // namespace

int wmain()
{
#ifndef _WIN64
    std::wcerr << L"This probe must be built as a 64-bit executable.\n";
    return 2;
#else
    std::wcout << L"Native NVIDIA Optical Flow D3D11 probe\n"
               << L"======================================\n";

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::wstring adapterName;
    if (!CreateNvidiaD3D11Device(adapter, device, context, adapterName)) {
        return 3;
    }
    std::wcout << L"Adapter: " << adapterName << L'\n';

    ModuleGuard module { LoadNvofModule() };
    if (!module.value) {
        std::wcerr << L"Could not load System32\\nvofapi64.dll (Win32 error "
                   << GetLastError() << L").\n";
        return 4;
    }

    const auto getMaxSupportedApiVersion =
        reinterpret_cast<PFN_NvOFGetMaxSupportedApiVersion>(
            GetProcAddress(module.value, "NvOFGetMaxSupportedApiVersion"));
    const auto createInstanceD3D11 =
        reinterpret_cast<PFN_NvOFAPICreateInstanceD3D11>(
            GetProcAddress(module.value, "NvOFAPICreateInstanceD3D11"));
    if (!getMaxSupportedApiVersion || !createInstanceD3D11) {
        std::wcerr << L"The driver NVOF module is missing required D3D11 exports.\n";
        return 5;
    }

    uint32_t driverApiVersion = 0;
    NvOFStatus status = getMaxSupportedApiVersion(&driverApiVersion);
    if (status != NV_OF_SUCCESS) {
        std::wcerr << L"NvOFGetMaxSupportedApiVersion failed: " << StatusName(status)
                   << L" (" << static_cast<int>(status) << L")\n";
        return 6;
    }

    std::wcout << L"Driver NVOF API: " << (driverApiVersion >> 4)
               << L'.' << (driverApiVersion & 0x0F) << L'\n';
    if (driverApiVersion < kNvOfApiVersion) {
        std::wcerr << L"The driver does not support the required public NVOF API 2.0.\n";
        return 7;
    }

    NvOFD3D11ApiFunctionList api = {};
    status = createInstanceD3D11(kNvOfApiVersion, &api);
    if (status != NV_OF_SUCCESS) {
        std::wcerr << L"NvOFAPICreateInstanceD3D11 failed: " << StatusName(status)
                   << L" (" << static_cast<int>(status) << L")\n";
        return 8;
    }

    if (!api.nvCreateOpticalFlowD3D11 || !api.nvOFGetCaps ||
        !api.nvOFGetSurfaceFormatCountD3D11 || !api.nvOFGetSurfaceFormatD3D11 ||
        !api.nvOFDestroy) {
        std::wcerr << L"The NVOF D3D11 function table is incomplete.\n";
        return 9;
    }

    NvOFHandleGuard opticalFlow;
    opticalFlow.destroy = api.nvOFDestroy;
    status = api.nvCreateOpticalFlowD3D11(device.Get(), context.Get(), &opticalFlow.value);
    if (status != NV_OF_SUCCESS || !opticalFlow.value) {
        std::wcerr << L"nvCreateOpticalFlowD3D11 failed: " << StatusName(status)
                   << L" (" << static_cast<int>(status) << L")\n";
        return 10;
    }
    std::wcout << L"D3D11 NVOF session: created\n";

    std::vector<DXGI_FORMAT> inputFormats;
    std::vector<DXGI_FORMAT> outputFormats;
    std::vector<uint32_t> gridSizes;
    if (!QuerySurfaceFormats(api, opticalFlow.value, NV_OF_BUFFER_USAGE_INPUT, inputFormats) ||
        !QuerySurfaceFormats(api, opticalFlow.value, NV_OF_BUFFER_USAGE_OUTPUT, outputFormats) ||
        !QueryCaps(api, opticalFlow.value, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, gridSizes)) {
        return 11;
    }

    PrintFormats(L"Input formats", inputFormats);
    PrintFormats(L"Output formats", outputFormats);
    PrintGridSizes(gridSizes);

    uint32_t minimumWidth = 0;
    uint32_t minimumHeight = 0;
    uint32_t maximumWidth = 0;
    uint32_t maximumHeight = 0;
    uint32_t roiSupported = 0;
    uint32_t maximumRois = 0;

    if (!GetSingleCapability(api, opticalFlow.value, NV_OF_CAPS_WIDTH_MIN, minimumWidth) ||
        !GetSingleCapability(api, opticalFlow.value, NV_OF_CAPS_HEIGHT_MIN, minimumHeight) ||
        !GetSingleCapability(api, opticalFlow.value, NV_OF_CAPS_WIDTH_MAX, maximumWidth) ||
        !GetSingleCapability(api, opticalFlow.value, NV_OF_CAPS_HEIGHT_MAX, maximumHeight) ||
        !GetSingleCapability(api, opticalFlow.value, NV_OF_CAPS_SUPPORT_ROI, roiSupported)) {
        return 12;
    }

    if (roiSupported) {
        if (!GetSingleCapability(api, opticalFlow.value, NV_OF_CAPS_SUPPORT_ROI_MAX_NUM, maximumRois)) {
            return 13;
        }
    }

    std::wcout << L"Supported dimensions: " << minimumWidth << L'x' << minimumHeight
               << L" through " << maximumWidth << L'x' << maximumHeight << L'\n';
    std::wcout << L"Region-of-interest support: " << (roiSupported ? L"yes" : L"no");
    if (roiSupported) {
        std::wcout << L" (maximum " << maximumRois << L")";
    }
    std::wcout << L'\n';

    std::wcout << L"RESULT: PASS\n";
    return 0;
#endif
}
