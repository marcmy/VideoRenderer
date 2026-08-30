/*
 * Offline NVIDIA Optical Flow dual-cost replay tool.
 *
 * Runs isolated one-pair NVOF sessions for both modern R8 cost and legacy R32
 * cost. This is deliberately separate from MPC Video Renderer: enabling output
 * cost in the live Turing playback session previously caused severe stalls.
 */

#include "NativeNvofApi.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
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
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
};
struct OutputSurface {
    ComPtr<ID3D11Texture2D> gpu;
    ComPtr<ID3D11Texture2D> staging;
    ResourceGuard registered;
};
struct ReplayResult {
    bool supported = false;
    bool succeeded = false;
    std::wstring failure;
    DXGI_FORMAT costFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t costBytesPerCell = 0;
    double elapsedMs = 0.0;
    std::vector<uint8_t> forwardFlow;
    std::vector<uint8_t> backwardFlow;
    std::vector<uint8_t> forwardCost;
    std::vector<uint8_t> backwardCost;
};

const wchar_t* StatusName(nvof::Status status)
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

std::wstring HResultText(HRESULT result)
{
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return stream.str();
}

std::wstring NvofFailure(const wchar_t* operation, nvof::Status status,
                         const nvof::D3D11FunctionList& api, nvof::Handle session)
{
    std::wostringstream stream;
    stream << operation << L" failed: " << StatusName(status)
           << L" (" << static_cast<int>(status) << L")";
    if (api.getLastError && session) {
        char message[512] = {};
        uint32_t size = static_cast<uint32_t>(std::size(message));
        if (api.getLastError(session, message, &size) == nvof::Success && size) {
            stream << L" - ";
            for (uint32_t i = 0; i < size && i < std::size(message); ++i) {
                if (!message[i]) break;
                stream << static_cast<wchar_t>(static_cast<unsigned char>(message[i]));
            }
        }
    }
    return stream.str();
}

HMODULE LoadDriverModule()
{
    wchar_t directory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(directory, static_cast<UINT>(std::size(directory)));
    if (!length || length >= std::size(directory)) return nullptr;
    return LoadLibraryW((std::filesystem::path(directory) / L"nvofapi64.dll").c_str());
}

bool CreateDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context,
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
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return false;
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (desc.VendorId != NvidiaVendorId || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL created = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &device, &created, &context);
        if (FAILED(hr)) {
            std::wcerr << L"D3D11CreateDevice failed: " << HResultText(hr) << L'\n';
            return false;
        }
        adapterName = desc.Description;
        return true;
    }
    std::wcerr << L"No NVIDIA hardware adapter was found.\n";
    return false;
}

bool ReadBmp32(const std::filesystem::path& path, Image32& image)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    file.read(reinterpret_cast<char*>(&fh), sizeof(fh));
    file.read(reinterpret_cast<char*>(&ih), sizeof(ih));
    if (!file || fh.bfType != 0x4D42 || ih.biSize < sizeof(BITMAPINFOHEADER) ||
        ih.biWidth <= 0 || ih.biHeight == 0 || ih.biPlanes != 1 ||
        ih.biBitCount != 32 || ih.biCompression != BI_RGB) {
        return false;
    }
    image.width = static_cast<uint32_t>(ih.biWidth);
    const bool topDown = ih.biHeight < 0;
    image.height = static_cast<uint32_t>(topDown ? -static_cast<int64_t>(ih.biHeight)
                                                 : static_cast<int64_t>(ih.biHeight));
    const size_t rowBytes = static_cast<size_t>(image.width) * 4u;
    image.bgra.resize(rowBytes * image.height);
    file.seekg(static_cast<std::streamoff>(fh.bfOffBits), std::ios::beg);
    std::vector<uint8_t> row(rowBytes);
    for (uint32_t sy = 0; sy < image.height; ++sy) {
        file.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(row.size()));
        if (!file) return false;
        const uint32_t dy = topDown ? sy : image.height - 1u - sy;
        std::copy(row.begin(), row.end(), image.bgra.begin() + static_cast<size_t>(dy) * rowBytes);
    }
    return true;
}

bool CreateInputTexture(ID3D11Device* device, const Image32& image,
                        ComPtr<ID3D11Texture2D>& texture)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = image.bgra.data();
    init.SysMemPitch = image.width * 4u;
    return SUCCEEDED(device->CreateTexture2D(&desc, &init, &texture));
}

bool CreateOutputSurface(ID3D11Device* device, uint32_t width, uint32_t height,
                         DXGI_FORMAT format, OutputSurface& surface)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (format == DXGI_FORMAT_R16G16_SINT) desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &surface.gpu);
    if (FAILED(hr)) return false;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &surface.staging));
}

bool QueryFormats(const nvof::D3D11FunctionList& api, nvof::Handle session,
                  nvof::BufferUsage usage, std::vector<DXGI_FORMAT>& formats)
{
    uint32_t count = 0;
    if (!api.getSurfaceFormatCountD3D11 || !api.getSurfaceFormatD3D11 ||
        api.getSurfaceFormatCountD3D11(session, usage, nvof::ModeOpticalFlow, &count) != nvof::Success) {
        return false;
    }
    formats.assign(count, DXGI_FORMAT_UNKNOWN);
    return !count || api.getSurfaceFormatD3D11(session, usage, nvof::ModeOpticalFlow,
                                                formats.data()) == nvof::Success;
}

bool HasFormat(const std::vector<DXGI_FORMAT>& formats, DXGI_FORMAT format)
{
    return std::find(formats.begin(), formats.end(), format) != formats.end();
}

bool RegisterResource(const nvof::D3D11FunctionList& api, nvof::Handle session,
                      ID3D11Resource* resource, ResourceGuard& guard)
{
    guard.unregisterResource = api.unregisterResourceD3D11;
    return api.registerResourceD3D11(session, resource, &guard.handle) == nvof::Success;
}

bool ReadSurfaceBytes(ID3D11DeviceContext* context, OutputSurface& surface,
                      uint32_t width, uint32_t height, uint32_t bytesPerPixel,
                      std::vector<uint8_t>& bytes)
{
    context->CopyResource(surface.staging.Get(), surface.gpu.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(surface.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const size_t rowBytes = static_cast<size_t>(width) * bytesPerPixel;
    bytes.resize(rowBytes * height);
    for (uint32_t y = 0; y < height; ++y) {
        const auto* src = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        std::copy_n(src, rowBytes, bytes.data() + static_cast<size_t>(y) * rowBytes);
    }
    context->Unmap(surface.staging.Get(), 0);
    return true;
}

ReplayResult RunReplay(ID3D11Device* device, ID3D11DeviceContext* context,
                       const nvof::D3D11FunctionList& api,
                       const Image32& first, const Image32& second,
                       DXGI_FORMAT costFormat, uint32_t costBytesPerCell)
{
    ReplayResult result;
    result.costFormat = costFormat;
    result.costBytesPerCell = costBytesPerCell;

    SessionGuard session;
    session.destroy = api.destroy;
    nvof::Status status = api.createOpticalFlowD3D11(device, context, &session.handle);
    if (status != nvof::Success || !session.handle) {
        result.failure = NvofFailure(L"createOpticalFlowD3D11", status, api, session.handle);
        return result;
    }

    std::vector<DXGI_FORMAT> inputFormats, outputFormats, costFormats;
    if (!QueryFormats(api, session.handle, nvof::BufferUsageInput, inputFormats) ||
        !QueryFormats(api, session.handle, nvof::BufferUsageOutput, outputFormats) ||
        !QueryFormats(api, session.handle, nvof::BufferUsageCost, costFormats)) {
        result.failure = L"surface-format query failed";
        return result;
    }
    if (!HasFormat(inputFormats, DXGI_FORMAT_B8G8R8A8_UNORM) ||
        !HasFormat(outputFormats, DXGI_FORMAT_R16G16_SINT) || !HasFormat(costFormats, costFormat)) {
        result.failure = L"requested cost format is not advertised by the driver";
        return result;
    }
    result.supported = true;

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
        result.failure = NvofFailure(L"NvOFInit(cost-enabled)", status, api, session.handle);
        return result;
    }

    const uint32_t gw = (first.width + GridSize - 1u) / GridSize;
    const uint32_t gh = (first.height + GridSize - 1u) / GridSize;
    ComPtr<ID3D11Texture2D> aTex, bTex;
    if (!CreateInputTexture(device, first, aTex) || !CreateInputTexture(device, second, bTex)) {
        result.failure = L"input texture creation failed";
        return result;
    }
    ResourceGuard aReg, bReg;
    if (!RegisterResource(api, session.handle, aTex.Get(), aReg) ||
        !RegisterResource(api, session.handle, bTex.Get(), bReg)) {
        result.failure = L"input registration failed";
        return result;
    }

    OutputSurface ff, bf, fc, bc;
    if (!CreateOutputSurface(device, gw, gh, DXGI_FORMAT_R16G16_SINT, ff) ||
        !CreateOutputSurface(device, gw, gh, DXGI_FORMAT_R16G16_SINT, bf) ||
        !CreateOutputSurface(device, gw, gh, costFormat, fc) ||
        !CreateOutputSurface(device, gw, gh, costFormat, bc)) {
        result.failure = L"output texture creation failed";
        return result;
    }
    if (!RegisterResource(api, session.handle, ff.gpu.Get(), ff.registered) ||
        !RegisterResource(api, session.handle, bf.gpu.Get(), bf.registered) ||
        !RegisterResource(api, session.handle, fc.gpu.Get(), fc.registered) ||
        !RegisterResource(api, session.handle, bc.gpu.Get(), bc.registered)) {
        result.failure = L"output registration failed";
        return result;
    }

    nvof::ExecuteInputParams in = {};
    in.inputFrame = bReg.handle;
    in.referenceFrame = aReg.handle;
    in.disableTemporalHints = nvof::True;
    nvof::ExecuteOutputParams out = {};
    out.outputBuffer = ff.registered.handle;
    out.outputCostBuffer = fc.registered.handle;
    out.backwardOutputBuffer = bf.registered.handle;
    out.backwardOutputCostBuffer = bc.registered.handle;

    const auto started = std::chrono::steady_clock::now();
    status = api.execute(session.handle, &in, &out);
    if (status != nvof::Success) {
        result.failure = NvofFailure(L"NvOFExecute", status, api, session.handle);
        return result;
    }
    context->Flush();
    if (!ReadSurfaceBytes(context, ff, gw, gh, 4, result.forwardFlow) ||
        !ReadSurfaceBytes(context, bf, gw, gh, 4, result.backwardFlow) ||
        !ReadSurfaceBytes(context, fc, gw, gh, costBytesPerCell, result.forwardCost) ||
        !ReadSurfaceBytes(context, bc, gw, gh, costBytesPerCell, result.backwardCost)) {
        result.failure = L"blocking readback failed";
        return result;
    }
    result.elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    result.succeeded = true;
    return result;
}

bool WriteBinary(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

std::filesystem::path MakeOutputDirectory(const std::filesystem::path& capture)
{
    SYSTEMTIME t = {};
    GetLocalTime(&t);
    wchar_t name[96] = {};
    swprintf_s(name, L"nvof-cost-replay-%04u%02u%02u-%02u%02u%02u",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const auto out = capture / name;
    std::filesystem::create_directories(out);
    return out;
}

template<class T> std::vector<T> AsValues(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() % sizeof(T)) return {};
    std::vector<T> values(bytes.size() / sizeof(T));
    std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}

template<class T> double Mean(const std::vector<T>& values)
{
    if (values.empty()) return 0.0;
    long double total = 0.0;
    for (T value : values) total += static_cast<long double>(value);
    return static_cast<double>(total / static_cast<long double>(values.size()));
}

template<class T> T Percentile(std::vector<T> values, double fraction)
{
    if (values.empty()) return T{};
    const size_t index = std::min(values.size() - 1u,
        static_cast<size_t>(std::floor(fraction * static_cast<double>(values.size() - 1u))));
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}

bool CompareFlowBytes(const std::vector<uint8_t>& aBytes, const std::vector<uint8_t>& bBytes,
                      double& meanAbsPixels, double& p99AbsPixels)
{
    if (aBytes.size() != bBytes.size() || aBytes.size() % sizeof(int16_t)) return false;
    const auto* a = reinterpret_cast<const int16_t*>(aBytes.data());
    const auto* b = reinterpret_cast<const int16_t*>(bBytes.data());
    const size_t count = aBytes.size() / sizeof(int16_t);
    std::vector<double> errors;
    errors.reserve(count);
    double total = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double e = std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])) / 32.0;
        errors.push_back(e);
        total += e;
    }
    meanAbsPixels = count ? total / static_cast<double>(count) : 0.0;
    if (errors.empty()) { p99AbsPixels = 0.0; return true; }
    const size_t index = static_cast<size_t>(0.99 * static_cast<double>(errors.size() - 1u));
    std::nth_element(errors.begin(), errors.begin() + index, errors.end());
    p99AbsPixels = errors[index];
    return true;
}

bool CompareFlowFile(const std::filesystem::path& path, const std::vector<uint8_t>& replay,
                     double& meanAbsPixels, double& p99AbsPixels)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto length = file.tellg();
    if (length < 0 || static_cast<size_t>(length) != replay.size()) return false;
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> original(replay.size());
    file.read(reinterpret_cast<char*>(original.data()), static_cast<std::streamsize>(original.size()));
    return file.good() && CompareFlowBytes(original, replay, meanAbsPixels, p99AbsPixels);
}

struct CostRelation {
    double correlation = 0.0;
    double slope = 0.0;
    double intercept = 0.0;
    double medianNonzeroRatio = 0.0;
    uint32_t bestShift = 0;
    double bestShiftExactPct = 0.0;
    double bestShiftMae = 0.0;
};

CostRelation AnalyzeRelation(const std::vector<uint8_t>& r8,
                             const std::vector<uint32_t>& r32)
{
    CostRelation result;
    if (r8.empty() || r8.size() != r32.size()) return result;
    const size_t n = r8.size();
    long double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    std::vector<double> ratios;
    ratios.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const long double x = r8[i], y = r32[i];
        sx += x; sy += y; sxx += x*x; syy += y*y; sxy += x*y;
        if (r8[i]) ratios.push_back(static_cast<double>(r32[i]) / static_cast<double>(r8[i]));
    }
    const long double dn = static_cast<long double>(n);
    const long double vx = dn*sxx - sx*sx;
    const long double vy = dn*syy - sy*sy;
    const long double cov = dn*sxy - sx*sy;
    if (vx > 0 && vy > 0) result.correlation = static_cast<double>(cov / std::sqrt(vx*vy));
    if (vx > 0) {
        result.slope = static_cast<double>(cov / vx);
        result.intercept = static_cast<double>((sy - result.slope*sx) / dn);
    }
    if (!ratios.empty()) {
        const size_t mid = ratios.size()/2;
        std::nth_element(ratios.begin(), ratios.begin()+mid, ratios.end());
        result.medianNonzeroRatio = ratios[mid];
    }
    double bestMae = std::numeric_limits<double>::infinity();
    for (uint32_t shift = 0; shift <= 24; ++shift) {
        uint64_t exact = 0;
        long double error = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t q = static_cast<uint8_t>(std::min<uint32_t>(r32[i] >> shift, 255u));
            if (q == r8[i]) ++exact;
            error += std::abs(static_cast<int>(q) - static_cast<int>(r8[i]));
        }
        const double mae = static_cast<double>(error / static_cast<long double>(n));
        if (mae < bestMae) {
            bestMae = mae;
            result.bestShift = shift;
            result.bestShiftMae = mae;
            result.bestShiftExactPct = 100.0 * static_cast<double>(exact) / static_cast<double>(n);
        }
    }
    return result;
}

void WriteReplayFiles(const std::filesystem::path& out, const wchar_t* suffix,
                      const ReplayResult& replay)
{
    if (!replay.succeeded) return;
    WriteBinary(out / (std::wstring(L"flow-forward-B-to-A-") + suffix + L"-s10.5.bin"), replay.forwardFlow);
    WriteBinary(out / (std::wstring(L"flow-backward-A-to-B-") + suffix + L"-s10.5.bin"), replay.backwardFlow);
    WriteBinary(out / (std::wstring(L"cost-forward-B-to-A-") + suffix + L".bin"), replay.forwardCost);
    WriteBinary(out / (std::wstring(L"cost-backward-A-to-B-") + suffix + L".bin"), replay.backwardCost);
}

void WriteFlowComparison(std::ofstream& manifest, const char* prefix,
                         const std::filesystem::path& capture,
                         const ReplayResult& replay)
{
    if (!replay.succeeded) return;
    double fMean=0, fP99=0, bMean=0, bP99=0;
    if (CompareFlowFile(capture / L"flow-forward-B-to-A-s10.5.bin", replay.forwardFlow, fMean, fP99)) {
        manifest << prefix << "_captured_forward_flow_mean_abs_component_px=" << fMean << '\n';
        manifest << prefix << "_captured_forward_flow_p99_abs_component_px=" << fP99 << '\n';
    }
    if (CompareFlowFile(capture / L"flow-backward-A-to-B-s10.5.bin", replay.backwardFlow, bMean, bP99)) {
        manifest << prefix << "_captured_backward_flow_mean_abs_component_px=" << bMean << '\n';
        manifest << prefix << "_captured_backward_flow_p99_abs_component_px=" << bP99 << '\n';
    }
}

void WriteCostStats(std::ofstream& manifest, const char* prefix, const ReplayResult& replay)
{
    if (!replay.succeeded) return;
    if (replay.costBytesPerCell == 1) {
        const auto f = AsValues<uint8_t>(replay.forwardCost), b = AsValues<uint8_t>(replay.backwardCost);
        manifest << prefix << "_forward_cost_mean=" << Mean(f) << '\n';
        manifest << prefix << "_forward_cost_p50=" << static_cast<unsigned>(Percentile(f,.50)) << '\n';
        manifest << prefix << "_forward_cost_p90=" << static_cast<unsigned>(Percentile(f,.90)) << '\n';
        manifest << prefix << "_forward_cost_p99=" << static_cast<unsigned>(Percentile(f,.99)) << '\n';
        manifest << prefix << "_backward_cost_mean=" << Mean(b) << '\n';
        manifest << prefix << "_backward_cost_p50=" << static_cast<unsigned>(Percentile(b,.50)) << '\n';
        manifest << prefix << "_backward_cost_p90=" << static_cast<unsigned>(Percentile(b,.90)) << '\n';
        manifest << prefix << "_backward_cost_p99=" << static_cast<unsigned>(Percentile(b,.99)) << '\n';
    } else {
        const auto f = AsValues<uint32_t>(replay.forwardCost), b = AsValues<uint32_t>(replay.backwardCost);
        manifest << prefix << "_forward_cost_mean=" << Mean(f) << '\n';
        manifest << prefix << "_forward_cost_p50=" << Percentile(f,.50) << '\n';
        manifest << prefix << "_forward_cost_p90=" << Percentile(f,.90) << '\n';
        manifest << prefix << "_forward_cost_p99=" << Percentile(f,.99) << '\n';
        manifest << prefix << "_backward_cost_mean=" << Mean(b) << '\n';
        manifest << prefix << "_backward_cost_p50=" << Percentile(b,.50) << '\n';
        manifest << prefix << "_backward_cost_p90=" << Percentile(b,.90) << '\n';
        manifest << prefix << "_backward_cost_p99=" << Percentile(b,.99) << '\n';
    }
}

void WriteRelation(std::ofstream& manifest, const char* direction,
                   const std::vector<uint8_t>& r8bytes,
                   const std::vector<uint8_t>& r32bytes)
{
    const auto r8 = AsValues<uint8_t>(r8bytes);
    const auto r32 = AsValues<uint32_t>(r32bytes);
    const CostRelation rel = AnalyzeRelation(r8, r32);
    manifest << direction << "_r8_r32_pearson=" << rel.correlation << '\n';
    manifest << direction << "_r32_from_r8_slope=" << rel.slope << '\n';
    manifest << direction << "_r32_from_r8_intercept=" << rel.intercept << '\n';
    manifest << direction << "_r32_r8_median_nonzero_ratio=" << rel.medianNonzeroRatio << '\n';
    manifest << direction << "_best_shift=" << rel.bestShift << '\n';
    manifest << direction << "_best_shift_exact_pct=" << rel.bestShiftExactPct << '\n';
    manifest << direction << "_best_shift_mae_r8_units=" << rel.bestShiftMae << '\n';
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
#ifndef _WIN64
    std::wcerr << L"NativeNvofCostReplayDual must be built as 64-bit.\n";
    return 2;
#else
    if (argc != 2) {
        std::wcerr << L"Usage: NativeNvofCostReplay.exe <capture-directory>\n";
        return 2;
    }
    const std::filesystem::path capture = std::filesystem::absolute(argv[1]);
    Image32 first, second;
    if (!ReadBmp32(capture/L"frame-A.bmp", first) || !ReadBmp32(capture/L"frame-B.bmp", second) ||
        first.width != second.width || first.height != second.height) {
        std::wcerr << L"Could not load matching 32-bit frame-A.bmp / frame-B.bmp.\n";
        return 3;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::wstring adapterName;
    if (!CreateDevice(device, context, adapterName)) return 4;
    ModuleGuard module{LoadDriverModule()};
    if (!module.module) return 5;
    const auto getVersion = reinterpret_cast<nvof::GetMaxSupportedApiVersionFn>(
        GetProcAddress(module.module, "NvOFGetMaxSupportedApiVersion"));
    const auto createInstance = reinterpret_cast<nvof::CreateInstanceD3D11Fn>(
        GetProcAddress(module.module, "NvOFAPICreateInstanceD3D11"));
    if (!getVersion || !createInstance) return 6;
    uint32_t driverVersion = 0;
    if (getVersion(&driverVersion) != nvof::Success || driverVersion < nvof::ApiVersion50) return 7;
    nvof::D3D11FunctionList api = {};
    if (createInstance(nvof::ApiVersion50, &api) != nvof::Success ||
        !api.createOpticalFlowD3D11 || !api.initialize || !api.getSurfaceFormatCountD3D11 ||
        !api.getSurfaceFormatD3D11 || !api.registerResourceD3D11 ||
        !api.unregisterResourceD3D11 || !api.execute || !api.destroy) return 8;

    std::wcout << L"Offline NVOF dual-cost replay\n"
               << L"Capture: " << capture.wstring() << L'\n'
               << L"Adapter: " << adapterName << L'\n'
               << L"Frame: " << first.width << L'x' << first.height << L'\n';

    ReplayResult r8 = RunReplay(device.Get(), context.Get(), api, first, second,
                                DXGI_FORMAT_R8_UINT, 1);
    ReplayResult r32 = RunReplay(device.Get(), context.Get(), api, first, second,
                                 DXGI_FORMAT_R32_UINT, 4);
    std::wcout << L"R8: " << (r8.succeeded ? L"success" : (r8.supported ? L"failed" : L"unsupported"));
    if (!r8.succeeded && !r8.failure.empty()) std::wcout << L" (" << r8.failure << L")";
    std::wcout << L'\n';
    std::wcout << L"R32: " << (r32.succeeded ? L"success" : (r32.supported ? L"failed" : L"unsupported"));
    if (!r32.succeeded && !r32.failure.empty()) std::wcout << L" (" << r32.failure << L")";
    std::wcout << L'\n';
    if (!r8.succeeded && !r32.succeeded) return 9;

    const auto out = MakeOutputDirectory(capture);
    WriteReplayFiles(out, L"r8", r8);
    WriteReplayFiles(out, L"r32", r32);
    const ReplayResult& canonical = r8.succeeded ? r8 : r32;
    WriteBinary(out/L"flow-forward-B-to-A-s10.5.bin", canonical.forwardFlow);
    WriteBinary(out/L"flow-backward-A-to-B-s10.5.bin", canonical.backwardFlow);

    std::ofstream manifest(out/L"replay-summary.txt");
    if (!manifest) return 10;
    manifest << "NVOF dual hardware-cost offline replay\n";
    manifest << "adapter=" << std::filesystem::path(adapterName).string() << '\n';
    manifest << "api=" << (driverVersion>>4) << '.' << (driverVersion&0x0f) << '\n';
    manifest << "frame=" << first.width << 'x' << first.height << '\n';
    manifest << "grid=" << ((first.width+3)/4) << 'x' << ((first.height+3)/4) << '\n';
    manifest << "r8_supported=" << (r8.supported?1:0) << '\n';
    manifest << "r8_success=" << (r8.succeeded?1:0) << '\n';
    manifest << "r32_supported=" << (r32.supported?1:0) << '\n';
    manifest << "r32_success=" << (r32.succeeded?1:0) << '\n';
    manifest << std::fixed << std::setprecision(6);
    if (r8.succeeded) manifest << "r8_execute_plus_readback_ms=" << r8.elapsedMs << '\n';
    if (r32.succeeded) manifest << "r32_execute_plus_readback_ms=" << r32.elapsedMs << '\n';
    WriteCostStats(manifest, "r8", r8);
    WriteCostStats(manifest, "r32", r32);
    WriteFlowComparison(manifest, "r8", capture, r8);
    WriteFlowComparison(manifest, "r32", capture, r32);
    if (r8.succeeded && r32.succeeded) {
        double fm=0,fp=0,bm=0,bp=0;
        if (CompareFlowBytes(r8.forwardFlow,r32.forwardFlow,fm,fp)) {
            manifest << "r8_r32_forward_flow_mean_abs_component_px=" << fm << '\n';
            manifest << "r8_r32_forward_flow_p99_abs_component_px=" << fp << '\n';
        }
        if (CompareFlowBytes(r8.backwardFlow,r32.backwardFlow,bm,bp)) {
            manifest << "r8_r32_backward_flow_mean_abs_component_px=" << bm << '\n';
            manifest << "r8_r32_backward_flow_p99_abs_component_px=" << bp << '\n';
        }
        WriteRelation(manifest, "forward", r8.forwardCost, r32.forwardCost);
        WriteRelation(manifest, "backward", r8.backwardCost, r32.backwardCost);
    }
    manifest.close();
    std::wcout << L"Output: " << out.wstring() << L'\n';
    return 0;
#endif
}
