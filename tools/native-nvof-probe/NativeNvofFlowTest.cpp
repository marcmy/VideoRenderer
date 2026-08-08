/*
 * Driver-only NVIDIA Optical Flow execution test.
 *
 * Creates two synthetic D3D11 frames with a known horizontal translation,
 * executes the public NVOF API, validates the returned S10.5 vectors, and
 * writes a color-coded NativeNvofFlow.bmp visualization.
 */

#include "NativeNvofApi.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT NvidiaVendorId = 0x10DE;
constexpr uint32_t TestWidth = 640;
constexpr uint32_t TestHeight = 360;
constexpr uint32_t GridSize = 4;
constexpr uint32_t ShiftPixels = 16;

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
    if (!length || length >= std::size(systemDirectory)) {
        return nullptr;
    }

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
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
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

uint32_t PatternHash(uint32_t x, uint32_t y)
{
    uint32_t value = x * 0x1f123bb5u ^ y * 0x5f356495u ^ 0x9e3779b9u;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

std::vector<uint8_t> MakeFirstFrame()
{
    std::vector<uint8_t> pixels(
        static_cast<size_t>(TestWidth) * TestHeight * 4);

    for (uint32_t y = 0; y < TestHeight; ++y) {
        for (uint32_t x = 0; x < TestWidth; ++x) {
            const uint32_t hash = PatternHash(x, y);
            const bool checker = (((x / 16) ^ (y / 16)) & 1u) != 0;
            const size_t offset =
                (static_cast<size_t>(y) * TestWidth + x) * 4;

            pixels[offset + 0] = static_cast<uint8_t>(
                (hash & 0x7fu) + (checker ? 96u : 16u));
            pixels[offset + 1] = static_cast<uint8_t>(
                ((hash >> 8) & 0x7fu) + ((x + y) & 0x3fu));
            pixels[offset + 2] = static_cast<uint8_t>(
                ((hash >> 16) & 0x7fu) + (checker ? 32u : 112u));
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}

std::vector<uint8_t> ShiftRight(const std::vector<uint8_t>& source)
{
    std::vector<uint8_t> shifted(
        static_cast<size_t>(TestWidth) * TestHeight * 4, 0);

    for (uint32_t y = 0; y < TestHeight; ++y) {
        for (uint32_t x = ShiftPixels; x < TestWidth; ++x) {
            const size_t sourceOffset =
                (static_cast<size_t>(y) * TestWidth + x - ShiftPixels) * 4;
            const size_t destinationOffset =
                (static_cast<size_t>(y) * TestWidth + x) * 4;
            std::copy_n(
                source.data() + sourceOffset, 4,
                shifted.data() + destinationOffset);
        }
    }

    for (size_t alpha = 3; alpha < shifted.size(); alpha += 4) {
        shifted[alpha] = 255;
    }
    return shifted;
}

bool CreateInputTexture(
    ID3D11Device* device,
    const std::vector<uint8_t>& pixels,
    ComPtr<ID3D11Texture2D>& texture)
{
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = TestWidth;
    description.Height = TestHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData = {};
    initialData.pSysMem = pixels.data();
    initialData.SysMemPitch = TestWidth * 4;

    const HRESULT result =
        device->CreateTexture2D(&description, &initialData, &texture);
    if (FAILED(result)) {
        std::wcerr << L"CreateTexture2D(input) failed: "
                   << HResultText(result) << L'\n';
        return false;
    }
    return true;
}

bool CreateOutputTextures(
    ID3D11Device* device,
    uint32_t gridWidth,
    uint32_t gridHeight,
    ComPtr<ID3D11Texture2D>& output,
    ComPtr<ID3D11Texture2D>& staging)
{
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = gridWidth;
    description.Height = gridHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R16G16_SINT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET |
                            D3D11_BIND_SHADER_RESOURCE |
                            D3D11_BIND_UNORDERED_ACCESS;

    HRESULT result = device->CreateTexture2D(&description, nullptr, &output);
    if (FAILED(result)) {
        std::wcerr << L"CreateTexture2D(flow output) failed: "
                   << HResultText(result) << L'\n';
        return false;
    }

    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    result = device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        std::wcerr << L"CreateTexture2D(flow staging) failed: "
                   << HResultText(result) << L'\n';
        return false;
    }
    return true;
}

double Median(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }

    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    const double upper = values[middle];
    if ((values.size() & 1u) != 0) {
        return upper;
    }

    std::nth_element(
        values.begin(), values.begin() + middle - 1,
        values.begin() + middle);
    return (values[middle - 1] + upper) * 0.5;
}

bool SaveBitmap(
    const std::filesystem::path& path,
    const std::vector<nvof::FlowVector>& flow,
    uint32_t gridWidth,
    uint32_t gridHeight)
{
    std::vector<uint8_t> pixels(
        static_cast<size_t>(TestWidth) * TestHeight * 4);

    for (uint32_t y = 0; y < TestHeight; ++y) {
        const uint32_t vectorY = std::min(y / GridSize, gridHeight - 1);
        for (uint32_t x = 0; x < TestWidth; ++x) {
            const uint32_t vectorX = std::min(x / GridSize, gridWidth - 1);
            const nvof::FlowVector& vector =
                flow[static_cast<size_t>(vectorY) * gridWidth + vectorX];

            const double flowX = static_cast<double>(vector.x) / 32.0;
            const double flowY = static_cast<double>(vector.y) / 32.0;
            const double normalizedX =
                std::clamp(flowX / 32.0, -1.0, 1.0);
            const double normalizedY =
                std::clamp(flowY / 32.0, -1.0, 1.0);
            const double magnitude =
                std::clamp(std::hypot(flowX, flowY) / 32.0, 0.0, 1.0);

            const auto Byte = [](double value) {
                return static_cast<uint8_t>(std::lround(
                    std::clamp(value, 0.0, 255.0)));
            };

            const size_t offset =
                (static_cast<size_t>(y) * TestWidth + x) * 4;
            pixels[offset + 0] = Byte(128.0 - 127.0 * normalizedX);
            pixels[offset + 1] = Byte(
                128.0 + 96.0 * normalizedY + 31.0 * magnitude);
            pixels[offset + 2] = Byte(128.0 + 127.0 * normalizedX);
            pixels[offset + 3] = 255;
        }
    }

    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits =
        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize =
        fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());

    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = static_cast<LONG>(TestWidth);
    infoHeader.biHeight = -static_cast<LONG>(TestHeight);
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = static_cast<DWORD>(pixels.size());

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    file.write(
        reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    file.write(
        reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    file.write(
        reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size()));
    return file.good();
}

} // namespace

int wmain()
{
#ifndef _WIN64
    std::wcerr << L"This test must be built as a 64-bit executable.\n";
    return 2;
#else
    std::wcout << L"Native NVIDIA Optical Flow execution test\n"
               << L"=========================================\n";

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::wstring adapterName;
    if (!CreateDevice(device, context, adapterName)) {
        return 3;
    }
    std::wcout << L"Adapter: " << adapterName << L'\n';

    ModuleGuard module { LoadDriverModule() };
    if (!module.module) {
        std::wcerr << L"Could not load System32\\nvofapi64.dll.\n";
        return 4;
    }

    const auto getVersion =
        reinterpret_cast<nvof::GetMaxSupportedApiVersionFn>(
            GetProcAddress(module.module, "NvOFGetMaxSupportedApiVersion"));
    const auto createInstance =
        reinterpret_cast<nvof::CreateInstanceD3D11Fn>(
            GetProcAddress(module.module, "NvOFAPICreateInstanceD3D11"));
    if (!getVersion || !createInstance) {
        std::wcerr << L"Required NVOF exports are missing.\n";
        return 5;
    }

    uint32_t driverVersion = 0;
    nvof::Status status = getVersion(&driverVersion);
    if (status != nvof::Success || driverVersion < nvof::ApiVersion20) {
        std::wcerr << L"The installed driver does not support NVOF API 2.0.\n";
        return 6;
    }
    std::wcout << L"Driver NVOF API: " << (driverVersion >> 4)
               << L'.' << (driverVersion & 0x0f) << L'\n';

    nvof::D3D11FunctionList api = {};
    status = createInstance(nvof::ApiVersion20, &api);
    if (status != nvof::Success || !api.createOpticalFlowD3D11 ||
        !api.initialize || !api.registerResourceD3D11 ||
        !api.unregisterResourceD3D11 || !api.execute || !api.destroy) {
        std::wcerr << L"Could not create the NVOF D3D11 function table.\n";
        return 7;
    }

    SessionGuard session;
    session.destroy = api.destroy;
    status = api.createOpticalFlowD3D11(
        device.Get(), context.Get(), &session.handle);
    if (status != nvof::Success || !session.handle) {
        PrintFailure(L"createOpticalFlowD3D11", status, api, session.handle);
        return 8;
    }
    std::wcout << L"D3D11 NVOF session: created\n";

    nvof::InitParams init = {};
    init.width = TestWidth;
    init.height = TestHeight;
    init.outputGridSize = nvof::OutputGrid4;
    init.hintGridSize = nvof::HintGridUndefined;
    init.mode = nvof::ModeOpticalFlow;
    init.performance = nvof::PerfSlow;
    init.enableExternalHints = nvof::False;
    init.enableOutputCost = nvof::False;
    init.disparityRange = nvof::StereoRangeUndefined;
    init.enableRoi = nvof::False;

    status = api.initialize(session.handle, &init);
    if (status != nvof::Success) {
        PrintFailure(L"nvOFInit", status, api, session.handle);
        return 9;
    }

    const uint32_t gridWidth = (TestWidth + GridSize - 1) / GridSize;
    const uint32_t gridHeight = (TestHeight + GridSize - 1) / GridSize;
    std::wcout << L"NVOF initialized: " << TestWidth << L'x' << TestHeight
               << L", 4x4 grid, output " << gridWidth << L'x'
               << gridHeight << L'\n';

    const std::vector<uint8_t> firstPixels = MakeFirstFrame();
    const std::vector<uint8_t> secondPixels = ShiftRight(firstPixels);

    ComPtr<ID3D11Texture2D> firstFrame;
    ComPtr<ID3D11Texture2D> secondFrame;
    ComPtr<ID3D11Texture2D> flowOutput;
    ComPtr<ID3D11Texture2D> flowStaging;
    if (!CreateInputTexture(device.Get(), firstPixels, firstFrame) ||
        !CreateInputTexture(device.Get(), secondPixels, secondFrame) ||
        !CreateOutputTextures(
            device.Get(), gridWidth, gridHeight, flowOutput, flowStaging)) {
        return 10;
    }

    ResourceGuard firstResource;
    ResourceGuard secondResource;
    ResourceGuard outputResource;
    firstResource.unregisterResource = api.unregisterResourceD3D11;
    secondResource.unregisterResource = api.unregisterResourceD3D11;
    outputResource.unregisterResource = api.unregisterResourceD3D11;

    status = api.registerResourceD3D11(
        session.handle, firstFrame.Get(), &firstResource.handle);
    if (status != nvof::Success) {
        PrintFailure(L"register first frame", status, api, session.handle);
        return 11;
    }
    status = api.registerResourceD3D11(
        session.handle, secondFrame.Get(), &secondResource.handle);
    if (status != nvof::Success) {
        PrintFailure(L"register second frame", status, api, session.handle);
        return 12;
    }
    status = api.registerResourceD3D11(
        session.handle, flowOutput.Get(), &outputResource.handle);
    if (status != nvof::Success) {
        PrintFailure(L"register flow output", status, api, session.handle);
        return 13;
    }
    std::wcout << L"D3D11 resources: registered\n";

    nvof::ExecuteInputParams input = {};
    input.inputFrame = secondResource.handle;
    input.referenceFrame = firstResource.handle;
    input.disableTemporalHints = nvof::True;

    nvof::ExecuteOutputParams output = {};
    output.outputBuffer = outputResource.handle;

    status = api.execute(session.handle, &input, &output);
    if (status != nvof::Success) {
        PrintFailure(L"nvOFExecute", status, api, session.handle);
        return 14;
    }
    std::wcout << L"nvOFExecute: submitted\n";

    context->CopyResource(flowStaging.Get(), flowOutput.Get());
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT mapResult =
        context->Map(flowStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapResult)) {
        std::wcerr << L"Map(flow output) failed: "
                   << HResultText(mapResult) << L'\n';
        return 15;
    }

    std::vector<nvof::FlowVector> flow(
        static_cast<size_t>(gridWidth) * gridHeight);
    for (uint32_t y = 0; y < gridHeight; ++y) {
        const auto* source = reinterpret_cast<const nvof::FlowVector*>(
            static_cast<const uint8_t*>(mapped.pData) +
            static_cast<size_t>(y) * mapped.RowPitch);
        std::copy_n(
            source, gridWidth,
            flow.data() + static_cast<size_t>(y) * gridWidth);
    }
    context->Unmap(flowStaging.Get(), 0);

    std::vector<double> signedX;
    std::vector<double> signedY;
    std::vector<double> absoluteX;
    std::vector<double> absoluteY;
    size_t strongHorizontalCount = 0;

    const uint32_t borderX = gridWidth / 5;
    const uint32_t borderY = gridHeight / 5;
    for (uint32_t y = borderY; y < gridHeight - borderY; ++y) {
        for (uint32_t x = borderX; x < gridWidth - borderX; ++x) {
            const nvof::FlowVector& vector =
                flow[static_cast<size_t>(y) * gridWidth + x];
            const double xPixels = static_cast<double>(vector.x) / 32.0;
            const double yPixels = static_cast<double>(vector.y) / 32.0;
            signedX.push_back(xPixels);
            signedY.push_back(yPixels);
            absoluteX.push_back(std::abs(xPixels));
            absoluteY.push_back(std::abs(yPixels));
            if (std::abs(xPixels) >= 8.0 && std::abs(yPixels) <= 4.0) {
                ++strongHorizontalCount;
            }
        }
    }

    const double medianX = Median(signedX);
    const double medianY = Median(signedY);
    const double medianAbsoluteX = Median(absoluteX);
    const double medianAbsoluteY = Median(absoluteY);
    const double strongHorizontalRatio = signedX.empty() ? 0.0 :
        static_cast<double>(strongHorizontalCount) /
        static_cast<double>(signedX.size());

    std::wcout << std::fixed << std::setprecision(2)
               << L"Known source translation: +" << ShiftPixels << L" px X\n"
               << L"Signed median flow: X=" << medianX
               << L", Y=" << medianY << L" px\n"
               << L"Absolute median flow: X=" << medianAbsoluteX
               << L", Y=" << medianAbsoluteY << L" px\n"
               << L"Strong horizontal vectors: "
               << strongHorizontalRatio * 100.0 << L"%\n";

    const std::filesystem::path bitmapPath =
        std::filesystem::current_path() / L"NativeNvofFlow.bmp";
    if (SaveBitmap(bitmapPath, flow, gridWidth, gridHeight)) {
        std::wcout << L"Flow visualization: " << bitmapPath.wstring() << L'\n';
    } else {
        std::wcerr << L"Warning: could not write NativeNvofFlow.bmp.\n";
    }

    const bool passed =
        medianAbsoluteX >= 8.0 && medianAbsoluteX <= 24.0 &&
        medianAbsoluteY <= 4.0 && strongHorizontalRatio >= 0.60;

    std::wcout << L"FLOW RESULT: " << (passed ? L"PASS" : L"FAIL") << L'\n';
    return passed ? 0 : 16;
#endif
}
