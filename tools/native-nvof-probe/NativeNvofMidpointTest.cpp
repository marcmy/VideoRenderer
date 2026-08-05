/*
 * Driver-only NVIDIA Optical Flow midpoint synthesis test.
 *
 * Generates a synthetic frame pair with a known +16 pixel translation,
 * requests forward and backward NVOF vectors in one execute call, uses those
 * vectors to synthesize the t=0.5 frame, and compares it with the exact +8
 * pixel ground truth.
 */

#include "NativeNvofApi.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT NvidiaVendorId = 0x10DE;
constexpr uint32_t TestWidth = 640;
constexpr uint32_t TestHeight = 360;
constexpr uint32_t GridSize = 4;
constexpr uint32_t ShiftPixels = 16;
constexpr uint32_t MidpointShiftPixels = ShiftPixels / 2;
constexpr float MidpointTime = 0.5f;

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

struct Float2 {
    float x = 0.0f;
    float y = 0.0f;
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
                std::wcerr << static_cast<wchar_t>(
                    static_cast<unsigned char>(message[i]));
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
        std::wcerr << L"CreateDXGIFactory1 failed: "
                   << HResultText(result) << L'\n';
        return false;
    }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result)) {
            std::wcerr << L"EnumAdapters1 failed: "
                       << HResultText(result) << L'\n';
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
            std::wcerr << L"D3D11CreateDevice failed: "
                       << HResultText(result) << L'\n';
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

std::vector<uint8_t> ShiftRight(
    const std::vector<uint8_t>& source,
    const uint32_t shiftPixels)
{
    std::vector<uint8_t> shifted(
        static_cast<size_t>(TestWidth) * TestHeight * 4, 0);

    for (uint32_t y = 0; y < TestHeight; ++y) {
        for (uint32_t x = shiftPixels; x < TestWidth; ++x) {
            const size_t sourceOffset =
                (static_cast<size_t>(y) * TestWidth + x - shiftPixels) * 4;
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

bool CreateFlowTextures(
    ID3D11Device* device,
    const uint32_t gridWidth,
    const uint32_t gridHeight,
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

bool ReadFlowTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* output,
    ID3D11Texture2D* staging,
    const uint32_t gridWidth,
    const uint32_t gridHeight,
    std::vector<nvof::FlowVector>& flow)
{
    context->CopyResource(staging, output);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT result =
        context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        std::wcerr << L"Map(flow output) failed: "
                   << HResultText(result) << L'\n';
        return false;
    }

    flow.resize(static_cast<size_t>(gridWidth) * gridHeight);
    for (uint32_t y = 0; y < gridHeight; ++y) {
        const auto* source = reinterpret_cast<const nvof::FlowVector*>(
            static_cast<const uint8_t*>(mapped.pData) +
            static_cast<size_t>(y) * mapped.RowPitch);
        std::copy_n(
            source, gridWidth,
            flow.data() + static_cast<size_t>(y) * gridWidth);
    }

    context->Unmap(staging, 0);
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

Float2 SampleFlow(
    const std::vector<nvof::FlowVector>& flow,
    const uint32_t gridWidth,
    const uint32_t gridHeight,
    const float x,
    const float y)
{
    const float gridX = x / static_cast<float>(GridSize);
    const float gridY = y / static_cast<float>(GridSize);
    const float clampedX = std::clamp(
        gridX, 0.0f, static_cast<float>(gridWidth - 1));
    const float clampedY = std::clamp(
        gridY, 0.0f, static_cast<float>(gridHeight - 1));

    const uint32_t x0 = static_cast<uint32_t>(std::floor(clampedX));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(clampedY));
    const uint32_t x1 = std::min(x0 + 1, gridWidth - 1);
    const uint32_t y1 = std::min(y0 + 1, gridHeight - 1);
    const float tx = clampedX - static_cast<float>(x0);
    const float ty = clampedY - static_cast<float>(y0);

    const auto Fetch = [&](const uint32_t sx, const uint32_t sy) {
        const nvof::FlowVector& vector =
            flow[static_cast<size_t>(sy) * gridWidth + sx];
        return Float2 {
            static_cast<float>(vector.x) / 32.0f,
            static_cast<float>(vector.y) / 32.0f,
        };
    };

    const Float2 f00 = Fetch(x0, y0);
    const Float2 f10 = Fetch(x1, y0);
    const Float2 f01 = Fetch(x0, y1);
    const Float2 f11 = Fetch(x1, y1);

    const auto Interpolate = [tx, ty](
        const float v00, const float v10,
        const float v01, const float v11) {
        const float top = v00 + (v10 - v00) * tx;
        const float bottom = v01 + (v11 - v01) * tx;
        return top + (bottom - top) * ty;
    };

    return Float2 {
        Interpolate(f00.x, f10.x, f01.x, f11.x),
        Interpolate(f00.y, f10.y, f01.y, f11.y),
    };
}

bool SamplePixels(
    const std::vector<uint8_t>& pixels,
    const float x,
    const float y,
    std::array<float, 4>& sample)
{
    if (x < 0.0f || y < 0.0f ||
        x > static_cast<float>(TestWidth - 1) ||
        y > static_cast<float>(TestHeight - 1)) {
        return false;
    }

    const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
    const uint32_t x1 = std::min(x0 + 1, TestWidth - 1);
    const uint32_t y1 = std::min(y0 + 1, TestHeight - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    const auto Fetch = [&](const uint32_t sx, const uint32_t sy, const size_t channel) {
        const size_t offset =
            (static_cast<size_t>(sy) * TestWidth + sx) * 4 + channel;
        return static_cast<float>(pixels[offset]);
    };

    for (size_t channel = 0; channel < sample.size(); ++channel) {
        const float v00 = Fetch(x0, y0, channel);
        const float v10 = Fetch(x1, y0, channel);
        const float v01 = Fetch(x0, y1, channel);
        const float v11 = Fetch(x1, y1, channel);
        const float top = v00 + (v10 - v00) * tx;
        const float bottom = v01 + (v11 - v01) * tx;
        sample[channel] = top + (bottom - top) * ty;
    }
    return true;
}

std::vector<uint8_t> SynthesizeMidpoint(
    const std::vector<uint8_t>& firstPixels,
    const std::vector<uint8_t>& secondPixels,
    const std::vector<nvof::FlowVector>& forwardFlow,
    const std::vector<nvof::FlowVector>& backwardFlow,
    const uint32_t gridWidth,
    const uint32_t gridHeight)
{
    std::vector<uint8_t> midpoint(
        static_cast<size_t>(TestWidth) * TestHeight * 4, 0);

    for (uint32_t y = 0; y < TestHeight; ++y) {
        for (uint32_t x = 0; x < TestWidth; ++x) {
            const Float2 inputToReference = SampleFlow(
                forwardFlow, gridWidth, gridHeight,
                static_cast<float>(x), static_cast<float>(y));
            const Float2 referenceToInput = SampleFlow(
                backwardFlow, gridWidth, gridHeight,
                static_cast<float>(x), static_cast<float>(y));

            const float firstX =
                static_cast<float>(x) - MidpointTime * referenceToInput.x;
            const float firstY =
                static_cast<float>(y) - MidpointTime * referenceToInput.y;
            const float secondX =
                static_cast<float>(x) - MidpointTime * inputToReference.x;
            const float secondY =
                static_cast<float>(y) - MidpointTime * inputToReference.y;

            std::array<float, 4> firstSample = {};
            std::array<float, 4> secondSample = {};
            const bool firstValid =
                SamplePixels(firstPixels, firstX, firstY, firstSample);
            const bool secondValid =
                SamplePixels(secondPixels, secondX, secondY, secondSample);

            const size_t destinationOffset =
                (static_cast<size_t>(y) * TestWidth + x) * 4;
            for (size_t channel = 0; channel < 4; ++channel) {
                float value = 0.0f;
                if (firstValid && secondValid) {
                    value = (firstSample[channel] + secondSample[channel]) * 0.5f;
                } else if (firstValid) {
                    value = firstSample[channel];
                } else if (secondValid) {
                    value = secondSample[channel];
                } else if (channel == 3) {
                    value = 255.0f;
                }

                midpoint[destinationOffset + channel] =
                    static_cast<uint8_t>(std::lround(
                        std::clamp(value, 0.0f, 255.0f)));
            }
        }
    }

    return midpoint;
}

bool SaveBitmap(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& pixels)
{
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

std::vector<uint8_t> MakeDifferenceImage(
    const std::vector<uint8_t>& actual,
    const std::vector<uint8_t>& expected)
{
    std::vector<uint8_t> difference(actual.size(), 255);
    for (size_t offset = 0; offset < actual.size(); offset += 4) {
        const int blue = std::abs(
            static_cast<int>(actual[offset + 0]) -
            static_cast<int>(expected[offset + 0]));
        const int green = std::abs(
            static_cast<int>(actual[offset + 1]) -
            static_cast<int>(expected[offset + 1]));
        const int red = std::abs(
            static_cast<int>(actual[offset + 2]) -
            static_cast<int>(expected[offset + 2]));
        const uint8_t amplified = static_cast<uint8_t>(std::min(
            255, std::max({ blue, green, red }) * 8));
        difference[offset + 0] = amplified;
        difference[offset + 1] = amplified;
        difference[offset + 2] = amplified;
        difference[offset + 3] = 255;
    }
    return difference;
}

void FlowStatistics(
    const std::vector<nvof::FlowVector>& flow,
    const uint32_t gridWidth,
    const uint32_t gridHeight,
    double& medianX,
    double& medianY)
{
    std::vector<double> xValues;
    std::vector<double> yValues;
    const uint32_t borderX = gridWidth / 5;
    const uint32_t borderY = gridHeight / 5;

    for (uint32_t y = borderY; y < gridHeight - borderY; ++y) {
        for (uint32_t x = borderX; x < gridWidth - borderX; ++x) {
            const nvof::FlowVector& vector =
                flow[static_cast<size_t>(y) * gridWidth + x];
            xValues.push_back(static_cast<double>(vector.x) / 32.0);
            yValues.push_back(static_cast<double>(vector.y) / 32.0);
        }
    }

    medianX = Median(std::move(xValues));
    medianY = Median(std::move(yValues));
}

void ErrorStatistics(
    const std::vector<uint8_t>& actual,
    const std::vector<uint8_t>& expected,
    double& meanAbsoluteError,
    unsigned& percentile95,
    unsigned& maximumError)
{
    std::vector<unsigned> errors;
    uint64_t totalError = 0;
    const uint32_t border = ShiftPixels * 2;

    for (uint32_t y = border; y < TestHeight - border; ++y) {
        for (uint32_t x = border; x < TestWidth - border; ++x) {
            const size_t offset =
                (static_cast<size_t>(y) * TestWidth + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel) {
                const unsigned error = static_cast<unsigned>(std::abs(
                    static_cast<int>(actual[offset + channel]) -
                    static_cast<int>(expected[offset + channel])));
                errors.push_back(error);
                totalError += error;
            }
        }
    }

    if (errors.empty()) {
        meanAbsoluteError = 255.0;
        percentile95 = 255;
        maximumError = 255;
        return;
    }

    std::sort(errors.begin(), errors.end());
    meanAbsoluteError = static_cast<double>(totalError) /
        static_cast<double>(errors.size());
    const size_t percentileIndex = std::min(
        errors.size() - 1,
        (errors.size() * 95) / 100);
    percentile95 = errors[percentileIndex];
    maximumError = errors.back();
}

} // namespace

int wmain()
{
#ifndef _WIN64
    std::wcerr << L"This test must be built as a 64-bit executable.\n";
    return 2;
#else
    std::wcout << L"Native NVIDIA Optical Flow midpoint synthesis test\n"
               << L"==================================================\n";

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
    if (status != nvof::Success || driverVersion < nvof::ApiVersion50) {
        std::wcerr << L"The installed driver does not support NVOF API 5.0.\n";
        return 6;
    }
    std::wcout << L"Driver NVOF API: " << (driverVersion >> 4)
               << L'.' << (driverVersion & 0x0f) << L'\n';

    nvof::D3D11FunctionList api = {};
    status = createInstance(nvof::ApiVersion50, &api);
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
    init.predictionDirection = nvof::PredictionBoth;
    init.enableGlobalFlow = nvof::False;
    init.inputBufferFormat = nvof::BufferFormatAbgr8;

    status = api.initialize(session.handle, &init);
    if (status != nvof::Success) {
        PrintFailure(L"nvOFInit(forward/backward)", status, api, session.handle);
        return 9;
    }

    const uint32_t gridWidth = (TestWidth + GridSize - 1) / GridSize;
    const uint32_t gridHeight = (TestHeight + GridSize - 1) / GridSize;
    std::wcout << L"NVOF initialized: " << TestWidth << L'x' << TestHeight
               << L", 4x4 grid, forward + backward "
               << gridWidth << L'x' << gridHeight << L'\n';

    const std::vector<uint8_t> firstPixels = MakeFirstFrame();
    const std::vector<uint8_t> secondPixels =
        ShiftRight(firstPixels, ShiftPixels);
    const std::vector<uint8_t> expectedPixels =
        ShiftRight(firstPixels, MidpointShiftPixels);

    ComPtr<ID3D11Texture2D> firstFrame;
    ComPtr<ID3D11Texture2D> secondFrame;
    ComPtr<ID3D11Texture2D> forwardOutput;
    ComPtr<ID3D11Texture2D> forwardStaging;
    ComPtr<ID3D11Texture2D> backwardOutput;
    ComPtr<ID3D11Texture2D> backwardStaging;
    if (!CreateInputTexture(device.Get(), firstPixels, firstFrame) ||
        !CreateInputTexture(device.Get(), secondPixels, secondFrame) ||
        !CreateFlowTextures(
            device.Get(), gridWidth, gridHeight,
            forwardOutput, forwardStaging) ||
        !CreateFlowTextures(
            device.Get(), gridWidth, gridHeight,
            backwardOutput, backwardStaging)) {
        return 10;
    }

    ResourceGuard firstResource;
    ResourceGuard secondResource;
    ResourceGuard forwardResource;
    ResourceGuard backwardResource;
    firstResource.unregisterResource = api.unregisterResourceD3D11;
    secondResource.unregisterResource = api.unregisterResourceD3D11;
    forwardResource.unregisterResource = api.unregisterResourceD3D11;
    backwardResource.unregisterResource = api.unregisterResourceD3D11;

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
        session.handle, forwardOutput.Get(), &forwardResource.handle);
    if (status != nvof::Success) {
        PrintFailure(L"register forward output", status, api, session.handle);
        return 13;
    }
    status = api.registerResourceD3D11(
        session.handle, backwardOutput.Get(), &backwardResource.handle);
    if (status != nvof::Success) {
        PrintFailure(L"register backward output", status, api, session.handle);
        return 14;
    }
    std::wcout << L"D3D11 resources: registered\n";

    nvof::ExecuteInputParams input = {};
    input.inputFrame = secondResource.handle;
    input.referenceFrame = firstResource.handle;
    input.disableTemporalHints = nvof::True;

    nvof::ExecuteOutputParams output = {};
    output.outputBuffer = forwardResource.handle;
    output.backwardOutputBuffer = backwardResource.handle;

    status = api.execute(session.handle, &input, &output);
    if (status != nvof::Success) {
        PrintFailure(L"nvOFExecute(forward/backward)", status, api, session.handle);
        return 15;
    }
    std::wcout << L"nvOFExecute: forward/backward submitted\n";

    context->Flush();

    std::vector<nvof::FlowVector> forwardFlow;
    std::vector<nvof::FlowVector> backwardFlow;
    if (!ReadFlowTexture(
            context.Get(), forwardOutput.Get(), forwardStaging.Get(),
            gridWidth, gridHeight, forwardFlow) ||
        !ReadFlowTexture(
            context.Get(), backwardOutput.Get(), backwardStaging.Get(),
            gridWidth, gridHeight, backwardFlow)) {
        return 16;
    }

    double forwardMedianX = 0.0;
    double forwardMedianY = 0.0;
    double backwardMedianX = 0.0;
    double backwardMedianY = 0.0;
    FlowStatistics(
        forwardFlow, gridWidth, gridHeight,
        forwardMedianX, forwardMedianY);
    FlowStatistics(
        backwardFlow, gridWidth, gridHeight,
        backwardMedianX, backwardMedianY);

    std::wcout << std::fixed << std::setprecision(2)
               << L"Known translation A -> B: +" << ShiftPixels << L" px X\n"
               << L"Forward flow (B -> A): X=" << forwardMedianX
               << L", Y=" << forwardMedianY << L" px\n"
               << L"Backward flow (A -> B): X=" << backwardMedianX
               << L", Y=" << backwardMedianY << L" px\n"
               << L"Forward/backward cancellation: X="
               << forwardMedianX + backwardMedianX
               << L", Y=" << forwardMedianY + backwardMedianY << L" px\n";

    const std::vector<uint8_t> midpointPixels = SynthesizeMidpoint(
        firstPixels, secondPixels,
        forwardFlow, backwardFlow,
        gridWidth, gridHeight);
    const std::vector<uint8_t> differencePixels =
        MakeDifferenceImage(midpointPixels, expectedPixels);

    double meanAbsoluteError = 0.0;
    unsigned percentile95 = 0;
    unsigned maximumError = 0;
    ErrorStatistics(
        midpointPixels, expectedPixels,
        meanAbsoluteError, percentile95, maximumError);

    std::wcout << L"Expected midpoint translation: +"
               << MidpointShiftPixels << L" px X\n"
               << L"Safe-region mean absolute error: "
               << meanAbsoluteError << L" / 255\n"
               << L"Safe-region 95th percentile error: "
               << percentile95 << L" / 255\n"
               << L"Safe-region maximum error: "
               << maximumError << L" / 255\n";

    const std::filesystem::path outputDirectory =
        std::filesystem::current_path();
    const std::filesystem::path midpointPath =
        outputDirectory / L"NativeNvofMidpoint.bmp";
    const std::filesystem::path expectedPath =
        outputDirectory / L"NativeNvofMidpointExpected.bmp";
    const std::filesystem::path differencePath =
        outputDirectory / L"NativeNvofMidpointDiff.bmp";

    const bool savedMidpoint = SaveBitmap(midpointPath, midpointPixels);
    const bool savedExpected = SaveBitmap(expectedPath, expectedPixels);
    const bool savedDifference = SaveBitmap(differencePath, differencePixels);
    if (savedMidpoint && savedExpected && savedDifference) {
        std::wcout << L"Synthesized midpoint: " << midpointPath.wstring() << L'\n'
                   << L"Expected midpoint: " << expectedPath.wstring() << L'\n'
                   << L"Amplified difference: "
                   << differencePath.wstring() << L'\n';
    } else {
        std::wcerr << L"Warning: one or more midpoint bitmaps could not be written.\n";
    }

    const bool flowPassed =
        forwardMedianX <= -8.0 && forwardMedianX >= -24.0 &&
        backwardMedianX >= 8.0 && backwardMedianX <= 24.0 &&
        std::abs(forwardMedianY) <= 4.0 &&
        std::abs(backwardMedianY) <= 4.0 &&
        std::abs(forwardMedianX + backwardMedianX) <= 4.0 &&
        std::abs(forwardMedianY + backwardMedianY) <= 4.0;
    const bool imagePassed =
        meanAbsoluteError <= 5.0 && percentile95 <= 12;
    const bool passed = flowPassed && imagePassed;

    std::wcout << L"MIDPOINT RESULT: "
               << (passed ? L"PASS" : L"FAIL") << L'\n';
    return passed ? 0 : 17;
#endif
}
