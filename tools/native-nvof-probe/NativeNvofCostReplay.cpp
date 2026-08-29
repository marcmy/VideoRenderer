/*
 * Offline NVIDIA Optical Flow hardware-cost replay tool.
 *
 * This intentionally runs in a standalone process instead of enabling NVOF
 * output cost inside MPC Video Renderer. Live output-cost was previously shown
 * to stall startup/seeks and sustained playback on Turing. The tool consumes
 * frame-A.bmp + frame-B.bmp from an existing diagnostic capture, creates its
 * own D3D11/NVOF session, executes exactly one bidirectional 4x4-grid pair with
 * 8-bit cost enabled, then writes replay flow and cost data into a new
 * nvof-cost-replay-* subdirectory.
 */

#include "NativeNvofApi.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT NvidiaVendorId = 0x10DE;
constexpr uint32_t GridSize = 4;

struct ModuleGuard {
    HMODULE module = nullptr;
    ~ModuleGuard() { if (module) FreeLibrary(module); }
};

struct SessionGuard {
    nvof::Handle handle = nullptr;
    nvof::DestroyFn destroy = nullptr;
    ~SessionGuard() { if (handle && destroy) destroy(handle); }
};

struct ResourceGuard {
    nvof::GpuBufferHandle handle = nullptr;
    nvof::UnregisterResourceD3D11Fn unregisterResource = nullptr;
    ~ResourceGuard() { if (handle && unregisterResource) unregisterResource(handle); }
};

struct Image32 {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;
};

struct OutputSurface {
    ComPtr<ID3D11Texture2D> gpu;
    ComPtr<ID3D11Texture2D> staging;
    ResourceGuard registered;
};

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

std::wstring HResultText(const HRESULT result)
{
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(result);
    return stream.str();
}

void PrintFailure(
    const wchar_t* operation,
    const nvof::Status status,
    const nvof::D3D11FunctionList& api,
    const nvof::Handle session)
{
    std::wcerr << operation << L" failed: " << StatusName(status)
               << L" (" << static_cast<int>(status) << L")";
    if (api.getLastError && session) {
        char message[512] = {};
        uint32_t size = static_cast<uint32_t>(std::size(message));
        if (api.getLastError(session, message, &size) == nvof::Success && size) {
            std::wcerr << L" - ";
            for (uint32_t i = 0; i < size && i < std::size(message); ++i) {
                if (!message[i]) break;
                std::wcerr << static_cast<wchar_t>(static_cast<unsigned char>(message[i]));
            }
        }
    }
    std::wcerr << L'\n';
}

HMODULE LoadDriverModule()
{
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(
        systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
    if (!length || length >= std::size(systemDirectory)) return nullptr;
    return LoadLibraryW(
        (std::filesystem::path(systemDirectory) / L"nvofapi64.dll").c_str());
}

bool CreateDevice(
    ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11DeviceContext>& context,
    std::wstring& adapterName)
{
    ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        std::wcerr << L"CreateDXGIFactory1 failed: " << HResultText(result) << L'\n';
        return false;
    }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) {
            std::wcerr << L"EnumAdapters1 failed: " << HResultText(result) << L'\n';
            return false;
        }

        DXGI_ADAPTER_DESC1 description = {};
        adapter->GetDesc1(&description);
        if (description.VendorId != NvidiaVendorId ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_11_0;
        result = D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &device, &createdLevel, &context);
        if (FAILED(result)) {
            std::wcerr << L"D3D11CreateDevice failed: " << HResultText(result) << L'\n';
            return false;
        }
        adapterName = description.Description;
        return true;
    }

    std::wcerr << L"No NVIDIA hardware adapter was found.\n";
    return false;
}

bool ReadBmp32(const std::filesystem::path& path, Image32& image)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::wcerr << L"Could not open " << path.wstring() << L'\n';
        return false;
    }

    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};
    file.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
    file.read(reinterpret_cast<char*>(&infoHeader), sizeof(infoHeader));
    if (!file || fileHeader.bfType != 0x4D42 ||
        infoHeader.biSize < sizeof(BITMAPINFOHEADER) ||
        infoHeader.biWidth <= 0 || infoHeader.biHeight == 0 ||
        infoHeader.biPlanes != 1 || infoHeader.biBitCount != 32 ||
        infoHeader.biCompression != BI_RGB) {
        std::wcerr << L"Unsupported BMP (need uncompressed 32-bit BGRA): "
                   << path.wstring() << L'\n';
        return false;
    }

    image.width = static_cast<uint32_t>(infoHeader.biWidth);
    const bool topDown = infoHeader.biHeight < 0;
    image.height = static_cast<uint32_t>(
        topDown ? -static_cast<int64_t>(infoHeader.biHeight)
                : static_cast<int64_t>(infoHeader.biHeight));
    const size_t rowBytes = static_cast<size_t>(image.width) * 4u;
    image.bgra.resize(rowBytes * image.height);

    file.seekg(static_cast<std::streamoff>(fileHeader.bfOffBits), std::ios::beg);
    std::vector<uint8_t> row(rowBytes);
    for (uint32_t sourceRow = 0; sourceRow < image.height; ++sourceRow) {
        file.read(reinterpret_cast<char*>(row.data()),
                  static_cast<std::streamsize>(row.size()));
        if (!file) {
            std::wcerr << L"BMP pixel data is truncated: " << path.wstring() << L'\n';
            return false;
        }
        const uint32_t destinationRow = topDown
            ? sourceRow : image.height - 1u - sourceRow;
        std::copy(row.begin(), row.end(),
                  image.bgra.begin() + static_cast<size_t>(destinationRow) * rowBytes);
    }
    return true;
}

bool CreateInputTexture(
    ID3D11Device* device,
    const Image32& image,
    ComPtr<ID3D11Texture2D>& texture)
{
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = image.width;
    description.Height = image.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData = {};
    initialData.pSysMem = image.bgra.data();
    initialData.SysMemPitch = image.width * 4u;
    const HRESULT result = device->CreateTexture2D(&description, &initialData, &texture);
    if (FAILED(result)) {
        std::wcerr << L"CreateTexture2D(input) failed: " << HResultText(result) << L'\n';
        return false;
    }
    return true;
}

bool CreateOutputSurface(
    ID3D11Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    const wchar_t* label,
    OutputSurface& surface)
{
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (format == DXGI_FORMAT_R16G16_SINT) {
        description.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }

    HRESULT result = device->CreateTexture2D(&description, nullptr, &surface.gpu);
    if (FAILED(result)) {
        std::wcerr << L"CreateTexture2D(" << label << L") failed: "
                   << HResultText(result) << L'\n';
        return false;
    }

    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    result = device->CreateTexture2D(&description, nullptr, &surface.staging);
    if (FAILED(result)) {
        std::wcerr << L"CreateTexture2D(" << label << L" staging) failed: "
                   << HResultText(result) << L'\n';
        return false;
    }
    return true;
}

bool QueryFormats(
    const nvof::D3D11FunctionList& api,
    nvof::Handle session,
    nvof::BufferUsage usage,
    std::vector<DXGI_FORMAT>& formats)
{
    if (!api.getSurfaceFormatCountD3D11 || !api.getSurfaceFormatD3D11) return false;
    uint32_t count = 0;
    nvof::Status status = api.getSurfaceFormatCountD3D11(
        session, usage, nvof::ModeOpticalFlow, &count);
    if (status != nvof::Success) return false;
    formats.assign(count, DXGI_FORMAT_UNKNOWN);
    if (!count) return true;
    status = api.getSurfaceFormatD3D11(
        session, usage, nvof::ModeOpticalFlow, formats.data());
    return status == nvof::Success;
}

bool HasFormat(const std::vector<DXGI_FORMAT>& formats, DXGI_FORMAT format)
{
    return std::find(formats.begin(), formats.end(), format) != formats.end();
}

bool RegisterResource(
    const nvof::D3D11FunctionList& api,
    nvof::Handle session,
    ID3D11Resource* resource,
    const wchar_t* label,
    ResourceGuard& guard)
{
    guard.unregisterResource = api.unregisterResourceD3D11;
    const nvof::Status status = api.registerResourceD3D11(
        session, resource, &guard.handle);
    if (status != nvof::Success) {
        PrintFailure(label, status, api, session);
        return false;
    }
    return true;
}

bool ReadSurfaceBytes(
    ID3D11DeviceContext* context,
    OutputSurface& surface,
    uint32_t width,
    uint32_t height,
    uint32_t bytesPerPixel,
    std::vector<uint8_t>& bytes)
{
    context->CopyResource(surface.staging.Get(), surface.gpu.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT result = context->Map(
        surface.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        std::wcerr << L"Map(output staging) failed: " << HResultText(result) << L'\n';
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(width) * bytesPerPixel;
    bytes.resize(rowBytes * height);
    for (uint32_t y = 0; y < height; ++y) {
        const auto* source = static_cast<const uint8_t*>(mapped.pData)
            + static_cast<size_t>(y) * mapped.RowPitch;
        std::copy_n(source, rowBytes,
                    bytes.data() + static_cast<size_t>(y) * rowBytes);
    }
    context->Unmap(surface.staging.Get(), 0);
    return true;
}

bool WriteBinary(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

bool WriteGrayBmpUpscaled(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& values,
    uint32_t gridWidth,
    uint32_t gridHeight,
    uint32_t frameWidth,
    uint32_t frameHeight)
{
    std::vector<uint8_t> pixels(
        static_cast<size_t>(frameWidth) * frameHeight * 4u);
    for (uint32_t y = 0; y < frameHeight; ++y) {
        const uint32_t gy = std::min(y / GridSize, gridHeight - 1u);
        for (uint32_t x = 0; x < frameWidth; ++x) {
            const uint32_t gx = std::min(x / GridSize, gridWidth - 1u);
            const uint8_t value = values[static_cast<size_t>(gy) * gridWidth + gx];
            const size_t offset = (static_cast<size_t>(y) * frameWidth + x) * 4u;
            pixels[offset + 0] = value;
            pixels[offset + 1] = value;
            pixels[offset + 2] = value;
            pixels[offset + 3] = 255;
        }
    }

    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = static_cast<DWORD>(fileHeader.bfOffBits + pixels.size());
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = static_cast<LONG>(frameWidth);
    infoHeader.biHeight = -static_cast<LONG>(frameHeight);
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = static_cast<DWORD>(pixels.size());

    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    file.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    file.write(reinterpret_cast<const char*>(pixels.data()),
               static_cast<std::streamsize>(pixels.size()));
    return file.good();
}

std::filesystem::path MakeOutputDirectory(const std::filesystem::path& capture)
{
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    const std::wstring name = std::format(
        L"nvof-cost-replay-{:04}{:02}{:02}-{:02}{:02}{:02}",
        time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
    const auto output = capture / name;
    std::filesystem::create_directories(output);
    return output;
}

double MeanCost(const std::vector<uint8_t>& cost)
{
    if (cost.empty()) return 0.0;
    const uint64_t total = std::accumulate(
        cost.begin(), cost.end(), uint64_t{0});
    return static_cast<double>(total) / static_cast<double>(cost.size());
}

uint8_t PercentileCost(std::vector<uint8_t> values, double fraction)
{
    if (values.empty()) return 0;
    const size_t index = std::min(
        values.size() - 1u,
        static_cast<size_t>(std::floor(fraction * static_cast<double>(values.size() - 1u))));
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}

bool CompareFlowFile(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& replay,
    double& meanAbsPixels,
    double& p99AbsPixels)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto length = file.tellg();
    if (length < 0 || static_cast<size_t>(length) != replay.size()) return false;
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> original(replay.size());
    file.read(reinterpret_cast<char*>(original.data()),
              static_cast<std::streamsize>(original.size()));
    if (!file) return false;

    const size_t componentCount = replay.size() / sizeof(int16_t);
    const auto* a = reinterpret_cast<const int16_t*>(original.data());
    const auto* b = reinterpret_cast<const int16_t*>(replay.data());
    std::vector<double> errors;
    errors.reserve(componentCount);
    double total = 0.0;
    for (size_t i = 0; i < componentCount; ++i) {
        const double error = std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])) / 32.0;
        errors.push_back(error);
        total += error;
    }
    meanAbsPixels = componentCount ? total / static_cast<double>(componentCount) : 0.0;
    if (errors.empty()) {
        p99AbsPixels = 0.0;
    } else {
        const size_t index = static_cast<size_t>(0.99 * static_cast<double>(errors.size() - 1u));
        std::nth_element(errors.begin(), errors.begin() + index, errors.end());
        p99AbsPixels = errors[index];
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
#ifndef _WIN64
    std::wcerr << L"NativeNvofCostReplay must be built as 64-bit.\n";
    return 2;
#else
    if (argc != 2) {
        std::wcerr << L"Usage: NativeNvofCostReplay.exe <capture-directory>\n"
                   << L"The directory must contain frame-A.bmp and frame-B.bmp.\n";
        return 2;
    }

    const std::filesystem::path capture = std::filesystem::absolute(argv[1]);
    Image32 first;
    Image32 second;
    if (!ReadBmp32(capture / L"frame-A.bmp", first) ||
        !ReadBmp32(capture / L"frame-B.bmp", second)) {
        return 3;
    }
    if (first.width != second.width || first.height != second.height) {
        std::wcerr << L"frame-A.bmp and frame-B.bmp dimensions do not match.\n";
        return 4;
    }

    std::wcout << L"Offline NVIDIA Optical Flow cost replay\n"
               << L"=======================================\n"
               << L"Capture: " << capture.wstring() << L'\n'
               << L"Frames: " << first.width << L'x' << first.height << L'\n';

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::wstring adapterName;
    if (!CreateDevice(device, context, adapterName)) return 5;
    std::wcout << L"Adapter: " << adapterName << L'\n';

    ModuleGuard module { LoadDriverModule() };
    if (!module.module) {
        std::wcerr << L"Could not load System32\\nvofapi64.dll.\n";
        return 6;
    }

    const auto getVersion = reinterpret_cast<nvof::GetMaxSupportedApiVersionFn>(
        GetProcAddress(module.module, "NvOFGetMaxSupportedApiVersion"));
    const auto createInstance = reinterpret_cast<nvof::CreateInstanceD3D11Fn>(
        GetProcAddress(module.module, "NvOFAPICreateInstanceD3D11"));
    if (!getVersion || !createInstance) {
        std::wcerr << L"Required NVOF exports are missing.\n";
        return 7;
    }

    uint32_t driverVersion = 0;
    nvof::Status status = getVersion(&driverVersion);
    if (status != nvof::Success || driverVersion < nvof::ApiVersion50) {
        std::wcerr << L"The installed driver does not support NVOF API 5.0.\n";
        return 8;
    }
    std::wcout << L"Driver NVOF API: " << (driverVersion >> 4)
               << L'.' << (driverVersion & 0x0f) << L'\n';

    nvof::D3D11FunctionList api = {};
    status = createInstance(nvof::ApiVersion50, &api);
    if (status != nvof::Success || !api.createOpticalFlowD3D11 ||
        !api.initialize || !api.getSurfaceFormatCountD3D11 ||
        !api.getSurfaceFormatD3D11 || !api.registerResourceD3D11 ||
        !api.unregisterResourceD3D11 || !api.execute || !api.destroy) {
        std::wcerr << L"Could not create the NVOF D3D11 function table.\n";
        return 9;
    }

    SessionGuard session;
    session.destroy = api.destroy;
    status = api.createOpticalFlowD3D11(device.Get(), context.Get(), &session.handle);
    if (status != nvof::Success || !session.handle) {
        PrintFailure(L"createOpticalFlowD3D11", status, api, session.handle);
        return 10;
    }

    std::vector<DXGI_FORMAT> inputFormats;
    std::vector<DXGI_FORMAT> outputFormats;
    std::vector<DXGI_FORMAT> costFormats;
    if (!QueryFormats(api, session.handle, nvof::BufferUsageInput, inputFormats) ||
        !QueryFormats(api, session.handle, nvof::BufferUsageOutput, outputFormats) ||
        !QueryFormats(api, session.handle, nvof::BufferUsageCost, costFormats)) {
        std::wcerr << L"Could not query NVOF D3D11 surface formats.\n";
        return 11;
    }
    if (!HasFormat(inputFormats, DXGI_FORMAT_B8G8R8A8_UNORM) ||
        !HasFormat(outputFormats, DXGI_FORMAT_R16G16_SINT) ||
        !HasFormat(costFormats, DXGI_FORMAT_R8_UINT)) {
        std::wcerr << L"Required BGRA8 / R16G16_SINT / R8_UINT NVOF formats are unavailable.\n";
        return 12;
    }

    nvof::InitParams init = {};
    init.width = first.width;
    init.height = first.height;
    init.outputGridSize = nvof::OutputGrid4;
    init.hintGridSize = nvof::HintGridUndefined;
    init.mode = nvof::ModeOpticalFlow;
    init.performance = nvof::PerfSlow;
    init.enableExternalHints = nvof::False;
    init.enableOutputCost = nvof::True;
    init.disparityRange = nvof::StereoRangeUndefined;
    init.enableRoi = nvof::False;
    init.predictionDirection = nvof::PredictionBoth;
    init.enableGlobalFlow = nvof::False;
    init.inputBufferFormat = nvof::BufferFormatAbgr8;
    status = api.initialize(session.handle, &init);
    if (status != nvof::Success) {
        PrintFailure(L"NvOFInit(cost-enabled)", status, api, session.handle);
        return 13;
    }

    const uint32_t gridWidth = (first.width + GridSize - 1u) / GridSize;
    const uint32_t gridHeight = (first.height + GridSize - 1u) / GridSize;

    ComPtr<ID3D11Texture2D> firstFrame;
    ComPtr<ID3D11Texture2D> secondFrame;
    if (!CreateInputTexture(device.Get(), first, firstFrame) ||
        !CreateInputTexture(device.Get(), second, secondFrame)) {
        return 14;
    }

    ResourceGuard firstResource;
    ResourceGuard secondResource;
    if (!RegisterResource(api, session.handle, firstFrame.Get(), L"register frame A", firstResource) ||
        !RegisterResource(api, session.handle, secondFrame.Get(), L"register frame B", secondResource)) {
        return 15;
    }

    OutputSurface forwardFlow;
    OutputSurface backwardFlow;
    OutputSurface forwardCost;
    OutputSurface backwardCost;
    if (!CreateOutputSurface(device.Get(), gridWidth, gridHeight, DXGI_FORMAT_R16G16_SINT,
            L"forward flow", forwardFlow) ||
        !CreateOutputSurface(device.Get(), gridWidth, gridHeight, DXGI_FORMAT_R16G16_SINT,
            L"backward flow", backwardFlow) ||
        !CreateOutputSurface(device.Get(), gridWidth, gridHeight, DXGI_FORMAT_R8_UINT,
            L"forward cost", forwardCost) ||
        !CreateOutputSurface(device.Get(), gridWidth, gridHeight, DXGI_FORMAT_R8_UINT,
            L"backward cost", backwardCost)) {
        return 16;
    }

    if (!RegisterResource(api, session.handle, forwardFlow.gpu.Get(), L"register forward flow", forwardFlow.registered) ||
        !RegisterResource(api, session.handle, backwardFlow.gpu.Get(), L"register backward flow", backwardFlow.registered) ||
        !RegisterResource(api, session.handle, forwardCost.gpu.Get(), L"register forward cost", forwardCost.registered) ||
        !RegisterResource(api, session.handle, backwardCost.gpu.Get(), L"register backward cost", backwardCost.registered)) {
        return 17;
    }

    nvof::ExecuteInputParams input = {};
    input.inputFrame = secondResource.handle;    // B
    input.referenceFrame = firstResource.handle; // A
    input.disableTemporalHints = nvof::True;

    nvof::ExecuteOutputParams output = {};
    output.outputBuffer = forwardFlow.registered.handle;                 // B -> A
    output.outputCostBuffer = forwardCost.registered.handle;
    output.backwardOutputBuffer = backwardFlow.registered.handle;        // A -> B
    output.backwardOutputCostBuffer = backwardCost.registered.handle;

    const auto started = std::chrono::steady_clock::now();
    status = api.execute(session.handle, &input, &output);
    if (status != nvof::Success) {
        PrintFailure(L"NvOFExecute(cost-enabled bidirectional)", status, api, session.handle);
        return 18;
    }

    // Readback is intentionally blocking: this is an offline, one-pair tool.
    context->Flush();
    std::vector<uint8_t> forwardFlowBytes;
    std::vector<uint8_t> backwardFlowBytes;
    std::vector<uint8_t> forwardCostBytes;
    std::vector<uint8_t> backwardCostBytes;
    if (!ReadSurfaceBytes(context.Get(), forwardFlow, gridWidth, gridHeight, 4u, forwardFlowBytes) ||
        !ReadSurfaceBytes(context.Get(), backwardFlow, gridWidth, gridHeight, 4u, backwardFlowBytes) ||
        !ReadSurfaceBytes(context.Get(), forwardCost, gridWidth, gridHeight, 1u, forwardCostBytes) ||
        !ReadSurfaceBytes(context.Get(), backwardCost, gridWidth, gridHeight, 1u, backwardCostBytes)) {
        return 19;
    }
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    const auto out = MakeOutputDirectory(capture);
    if (!WriteBinary(out / L"flow-forward-B-to-A-s10.5.bin", forwardFlowBytes) ||
        !WriteBinary(out / L"flow-backward-A-to-B-s10.5.bin", backwardFlowBytes) ||
        !WriteBinary(out / L"cost-forward-B-to-A-r8.bin", forwardCostBytes) ||
        !WriteBinary(out / L"cost-backward-A-to-B-r8.bin", backwardCostBytes) ||
        !WriteGrayBmpUpscaled(out / L"cost-forward-B-to-A.bmp", forwardCostBytes,
            gridWidth, gridHeight, first.width, first.height) ||
        !WriteGrayBmpUpscaled(out / L"cost-backward-A-to-B.bmp", backwardCostBytes,
            gridWidth, gridHeight, first.width, first.height)) {
        std::wcerr << L"Could not write one or more replay output files.\n";
        return 20;
    }

    double forwardMeanDiff = 0.0;
    double forwardP99Diff = 0.0;
    double backwardMeanDiff = 0.0;
    double backwardP99Diff = 0.0;
    const bool comparedForward = CompareFlowFile(
        capture / L"flow-forward-B-to-A-s10.5.bin", forwardFlowBytes,
        forwardMeanDiff, forwardP99Diff);
    const bool comparedBackward = CompareFlowFile(
        capture / L"flow-backward-A-to-B-s10.5.bin", backwardFlowBytes,
        backwardMeanDiff, backwardP99Diff);

    const double forwardMeanCost = MeanCost(forwardCostBytes);
    const double backwardMeanCost = MeanCost(backwardCostBytes);
    const uint8_t forwardP50 = PercentileCost(forwardCostBytes, 0.50);
    const uint8_t forwardP90 = PercentileCost(forwardCostBytes, 0.90);
    const uint8_t forwardP99 = PercentileCost(forwardCostBytes, 0.99);
    const uint8_t backwardP50 = PercentileCost(backwardCostBytes, 0.50);
    const uint8_t backwardP90 = PercentileCost(backwardCostBytes, 0.90);
    const uint8_t backwardP99 = PercentileCost(backwardCostBytes, 0.99);

    std::ofstream manifest(out / L"replay-summary.txt");
    if (!manifest) {
        std::wcerr << L"Could not write replay-summary.txt.\n";
        return 21;
    }
    manifest << "NVOF hardware-cost offline replay\n";
    manifest << "adapter=" << std::filesystem::path(adapterName).string() << "\n";
    manifest << "api=" << (driverVersion >> 4) << '.' << (driverVersion & 0x0f) << "\n";
    manifest << "frame=" << first.width << 'x' << first.height << "\n";
    manifest << "grid=" << gridWidth << 'x' << gridHeight << "\n";
    manifest << std::fixed << std::setprecision(4);
    manifest << "execute_plus_readback_ms=" << elapsedMs << "\n";
    manifest << "forward_cost_mean=" << forwardMeanCost << "\n";
    manifest << "forward_cost_p50=" << static_cast<unsigned>(forwardP50) << "\n";
    manifest << "forward_cost_p90=" << static_cast<unsigned>(forwardP90) << "\n";
    manifest << "forward_cost_p99=" << static_cast<unsigned>(forwardP99) << "\n";
    manifest << "backward_cost_mean=" << backwardMeanCost << "\n";
    manifest << "backward_cost_p50=" << static_cast<unsigned>(backwardP50) << "\n";
    manifest << "backward_cost_p90=" << static_cast<unsigned>(backwardP90) << "\n";
    manifest << "backward_cost_p99=" << static_cast<unsigned>(backwardP99) << "\n";
    if (comparedForward) {
        manifest << "captured_forward_flow_mean_abs_component_px=" << forwardMeanDiff << "\n";
        manifest << "captured_forward_flow_p99_abs_component_px=" << forwardP99Diff << "\n";
    }
    if (comparedBackward) {
        manifest << "captured_backward_flow_mean_abs_component_px=" << backwardMeanDiff << "\n";
        manifest << "captured_backward_flow_p99_abs_component_px=" << backwardP99Diff << "\n";
    }
    manifest.close();

    std::wcout << std::fixed << std::setprecision(2)
               << L"Cost-enabled execute + blocking readback: " << elapsedMs << L" ms\n"
               << L"Forward cost: mean=" << forwardMeanCost
               << L", p50=" << static_cast<unsigned>(forwardP50)
               << L", p90=" << static_cast<unsigned>(forwardP90)
               << L", p99=" << static_cast<unsigned>(forwardP99) << L'\n'
               << L"Backward cost: mean=" << backwardMeanCost
               << L", p50=" << static_cast<unsigned>(backwardP50)
               << L", p90=" << static_cast<unsigned>(backwardP90)
               << L", p99=" << static_cast<unsigned>(backwardP99) << L'\n';
    if (comparedForward || comparedBackward) {
        std::wcout << L"Replay-vs-capture flow component difference:\n";
        if (comparedForward) {
            std::wcout << L"  forward mean=" << forwardMeanDiff
                       << L" px, p99=" << forwardP99Diff << L" px\n";
        }
        if (comparedBackward) {
            std::wcout << L"  backward mean=" << backwardMeanDiff
                       << L" px, p99=" << backwardP99Diff << L" px\n";
        }
    }
    std::wcout << L"Output: " << out.wstring() << L'\n';
    return 0;
#endif
}
