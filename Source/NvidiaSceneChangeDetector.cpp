#include "stdafx.h"
#include "NvidiaSceneChangeDetector.h"

#include <d3d11_4.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <vector>

namespace nvof_scene {

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

enum Bool : int { False = 0, True = 1 };
enum PerfLevel : int { PerfUndefined = 0, PerfSlow = 5, PerfMedium = 10, PerfFast = 20 };
enum OutputGridSize : int { OutputGridUndefined = 0, OutputGrid1 = 1, OutputGrid2 = 2, OutputGrid4 = 4 };
enum HintGridSize : int { HintGridUndefined = 0, HintGrid1 = 1, HintGrid2 = 2, HintGrid4 = 4, HintGrid8 = 8 };
enum Mode : int { ModeUndefined = 0, ModeOpticalFlow, ModeStereoDisparity };
enum BufferUsage : int { BufferUsageUndefined = 0, BufferUsageInput, BufferUsageOutput, BufferUsageHint, BufferUsageCost, BufferUsageGlobalFlow };
enum BufferFormat : int { BufferFormatUndefined = 0, BufferFormatGrayscale8, BufferFormatNv12, BufferFormatAbgr8, BufferFormatShort, BufferFormatShort2, BufferFormatUint, BufferFormatUint8 };
enum StereoDisparityRange : int { StereoRangeUndefined = 0, StereoRange128 = 128, StereoRange256 = 256 };
enum PredictionDirection : int { PredictionForward = 0, PredictionBoth = 2 };

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

struct RoiRect { uint32_t startX, startY, width, height; };

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
using CreateOpticalFlowD3D11Fn = Status (WINAPI*)(ID3D11Device*, ID3D11DeviceContext*, Handle*);
using InitFn = Status (WINAPI*)(Handle, const InitParams*);
using GetSurfaceFormatCountD3D11Fn = Status (WINAPI*)(Handle, BufferUsage, Mode, uint32_t*);
using GetSurfaceFormatD3D11Fn = Status (WINAPI*)(Handle, BufferUsage, Mode, DXGI_FORMAT*);
using RegisterResourceD3D11Fn = Status (WINAPI*)(Handle, ID3D11Resource*, GpuBufferHandle*);
using UnregisterResourceD3D11Fn = Status (WINAPI*)(GpuBufferHandle);
using ExecuteFn = Status (WINAPI*)(Handle, const ExecuteInputParams*, ExecuteOutputParams*);
using DestroyFn = Status (WINAPI*)(Handle);
using GetLastErrorFn = Status (WINAPI*)(Handle, char[], uint32_t*);
using GetCapsFn = Status (WINAPI*)(Handle, int, uint32_t*, uint32_t*);

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
using CreateInstanceD3D11Fn = Status (WINAPI*)(uint32_t, D3D11FunctionList*);

} // namespace nvof_scene

namespace {

#ifdef _WIN64
constexpr wchar_t NvofModuleName[] = L"nvofapi64.dll";
#else
constexpr wchar_t NvofModuleName[] = L"nvofapi.dll";
#endif

constexpr UINT FlowGridSize = 4;
constexpr int CapsSupportedOutputGridSizes = 0;
constexpr float FlowFixedPointScale = 1.0f / 32.0f; // signed S10.5 vectors

const wchar_t* StatusName(nvof_scene::Status status)
{
    using namespace nvof_scene;
    switch (status) {
    case Success: return L"success";
    case OpticalFlowNotAvailable: return L"optical flow unavailable";
    case UnsupportedDevice: return L"unsupported device";
    case DeviceDoesNotExist: return L"device no longer exists";
    case InvalidPointer: return L"invalid pointer";
    case InvalidParameter: return L"invalid parameter";
    case InvalidCall: return L"invalid call sequence";
    case InvalidVersion: return L"invalid API version";
    case OutOfMemory: return L"out of memory";
    case NotInitialized: return L"not initialized";
    case UnsupportedFeature: return L"unsupported feature";
    case GenericError: return L"generic driver error";
    default: return L"unknown status";
    }
}

HMODULE LoadSystemNvofModule()
{
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
    if (!length || length >= std::size(systemDirectory)) {
        return nullptr;
    }
    return LoadLibraryW((std::filesystem::path(systemDirectory) / NvofModuleName).c_str());
}

struct FlowVector { int16_t x; int16_t y; };
static_assert(sizeof(FlowVector) == 4);

} // namespace

struct CNvidiaSceneChangeDetector::Impl
{
    struct RegisteredInput {
        CComPtr<ID3D11Texture2D> texture;
        nvof_scene::GpuBufferHandle handle = nullptr;
    };

    struct FlowSurface {
        CComPtr<ID3D11Texture2D> texture;
        CComPtr<ID3D11Texture2D> staging;
        nvof_scene::GpuBufferHandle handle = nullptr;
    };

    std::mutex mutex;
    HMODULE module = nullptr;
    nvof_scene::D3D11FunctionList api = {};
    nvof_scene::Handle session = nullptr;
    CComPtr<ID3D11Device> device;
    CComPtr<ID3D11DeviceContext> context;
    CComPtr<ID3D11Multithread> multithread;
    RegisteredInput firstInput;
    RegisteredInput secondInput;
    FlowSurface forward;
    FlowSurface backward;
    UINT width = 0;
    UINT height = 0;
    UINT flowWidth = 0;
    UINT flowHeight = 0;
    std::wstring status = L"Disabled";

    std::wstring DriverError(nvof_scene::Status code) const
    {
        std::wstring result = std::format(L"{} ({})", StatusName(code), static_cast<int>(code));
        if (api.getLastError && session) {
            char message[512] = {};
            uint32_t size = static_cast<uint32_t>(std::size(message));
            if (api.getLastError(session, message, &size) == nvof_scene::Success && size) {
                result.append(L": ");
                for (uint32_t i = 0; i < size && i < std::size(message) && message[i]; ++i) {
                    result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(message[i])));
                }
            }
        }
        return result;
    }

    void UnregisterInput(RegisteredInput& input)
    {
        if (input.handle && api.unregisterResourceD3D11) {
            api.unregisterResourceD3D11(input.handle);
        }
        input.handle = nullptr;
        input.texture.Release();
    }

    void ReleaseFlow(FlowSurface& flow)
    {
        if (flow.handle && api.unregisterResourceD3D11) {
            api.unregisterResourceD3D11(flow.handle);
        }
        flow.handle = nullptr;
        flow.staging.Release();
        flow.texture.Release();
    }

    void ResetUnlocked()
    {
        UnregisterInput(firstInput);
        UnregisterInput(secondInput);
        ReleaseFlow(forward);
        ReleaseFlow(backward);
        if (session && api.destroy) {
            api.destroy(session);
        }
        session = nullptr;
        multithread.Release();
        context.Release();
        device.Release();
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
        api = {};
        width = height = flowWidth = flowHeight = 0;
    }

    bool Fail(const std::wstring& message)
    {
        status = message;
        const std::wstring saved = status;
        ResetUnlocked();
        status = saved;
        return false;
    }

    bool CreateFlowSurface(FlowSurface& flow)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = flowWidth;
        desc.Height = flowHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16_SINT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &flow.texture);
        if (FAILED(hr)) {
            status = std::format(L"NVOF flow texture creation failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }
        const auto code = api.registerResourceD3D11(session, flow.texture, &flow.handle);
        if (code != nvof_scene::Success) {
            status = std::format(L"NVOF flow registration failed: {}", DriverError(code));
            return false;
        }

        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        hr = device->CreateTexture2D(&desc, nullptr, &flow.staging);
        if (FAILED(hr)) {
            status = std::format(L"NVOF staging texture creation failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }
        return true;
    }

    bool CreateInputSurface(RegisteredInput& input)
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

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &input.texture);
        if (FAILED(hr)) {
            status = std::format(L"NVOF input texture creation failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }
        const auto code = api.registerResourceD3D11(session, input.texture, &input.handle);
        if (code != nvof_scene::Success) {
            status = std::format(L"NVOF input registration failed: {}", DriverError(code));
            return false;
        }
        return true;
    }

    bool SupportsOutputGrid4()
    {
        uint32_t count = 0;
        if (api.getCaps(session, CapsSupportedOutputGridSizes, nullptr, &count) != nvof_scene::Success || !count) {
            return false;
        }
        std::vector<uint32_t> values(count);
        if (api.getCaps(session, CapsSupportedOutputGridSizes, values.data(), &count) != nvof_scene::Success) {
            return false;
        }
        values.resize(count);
        return std::find(values.begin(), values.end(), FlowGridSize) != values.end();
    }

    bool SupportsFormat(nvof_scene::BufferUsage usage, DXGI_FORMAT wanted)
    {
        uint32_t count = 0;
        if (api.getSurfaceFormatCountD3D11(session, usage, nvof_scene::ModeOpticalFlow, &count) != nvof_scene::Success || !count) {
            return false;
        }
        std::vector<DXGI_FORMAT> formats(count, DXGI_FORMAT_UNKNOWN);
        if (api.getSurfaceFormatD3D11(session, usage, nvof_scene::ModeOpticalFlow, formats.data()) != nvof_scene::Success) {
            return false;
        }
        return std::find(formats.begin(), formats.end(), wanted) != formats.end();
    }

    bool Initialize(ID3D11Device* requestedDevice, UINT requestedWidth, UINT requestedHeight)
    {
        std::scoped_lock lock(mutex);
        if (!requestedDevice || !requestedWidth || !requestedHeight) {
            status = L"Invalid NVOF scene-detector device or dimensions";
            return false;
        }
        if (session && device == requestedDevice && width == requestedWidth && height == requestedHeight) {
            return true;
        }

        ResetUnlocked();
        module = LoadSystemNvofModule();
        if (!module) {
            return Fail(std::format(L"{} is unavailable", NvofModuleName));
        }

        const auto getVersion = reinterpret_cast<nvof_scene::GetMaxSupportedApiVersionFn>(
            GetProcAddress(module, "NvOFGetMaxSupportedApiVersion"));
        const auto createInstance = reinterpret_cast<nvof_scene::CreateInstanceD3D11Fn>(
            GetProcAddress(module, "NvOFAPICreateInstanceD3D11"));
        if (!getVersion || !createInstance) {
            return Fail(L"NVOF D3D11 exports are missing from the display driver");
        }

        uint32_t version = 0;
        auto code = getVersion(&version);
        if (code != nvof_scene::Success || version < nvof_scene::ApiVersion50) {
            return Fail(std::format(L"NVOF API 5.0 unavailable: {}", StatusName(code)));
        }
        code = createInstance(nvof_scene::ApiVersion50, &api);
        if (code != nvof_scene::Success || !api.createOpticalFlowD3D11 || !api.initialize ||
            !api.getSurfaceFormatCountD3D11 || !api.getSurfaceFormatD3D11 || !api.registerResourceD3D11 ||
            !api.unregisterResourceD3D11 || !api.execute || !api.destroy || !api.getCaps) {
            return Fail(L"Could not create the NVOF D3D11 function table");
        }

        device = requestedDevice;
        device->GetImmediateContext(&context);
        if (!context) {
            return Fail(L"D3D11 immediate context unavailable to NVOF scene detector");
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
        if (code != nvof_scene::Success || !session) {
            return Fail(std::format(L"NvCreateOpticalFlowD3D11 failed: {}", DriverError(code)));
        }
        if (!SupportsOutputGrid4()) {
            return Fail(L"NVOF 4x4 flow is unavailable on this GPU/driver");
        }
        if (!SupportsFormat(nvof_scene::BufferUsageInput, DXGI_FORMAT_B8G8R8A8_UNORM) ||
            !SupportsFormat(nvof_scene::BufferUsageOutput, DXGI_FORMAT_R16G16_SINT)) {
            return Fail(L"Required NVOF BGRA8 input/R16G16_SINT output formats are unavailable");
        }

        nvof_scene::InitParams init = {};
        init.width = width;
        init.height = height;
        init.outputGridSize = nvof_scene::OutputGrid4;
        init.hintGridSize = nvof_scene::HintGridUndefined;
        init.mode = nvof_scene::ModeOpticalFlow;
        // Scene detection needs robust coarse motion, not the highest-quality
        // optical flow field. PerfSlow consumes too much of a 16.7 ms source
        // frame budget at 60 fps once RIFE inference is added afterward.
        init.performance = nvof_scene::PerfFast;
        init.enableExternalHints = nvof_scene::False;
        init.enableOutputCost = nvof_scene::False; // intentionally off on the Turing live path
        init.disparityRange = nvof_scene::StereoRangeUndefined;
        init.enableRoi = nvof_scene::False;
        init.predictionDirection = nvof_scene::PredictionBoth;
        init.enableGlobalFlow = nvof_scene::False;
        init.inputBufferFormat = nvof_scene::BufferFormatAbgr8;
        code = api.initialize(session, &init);
        if (code != nvof_scene::Success) {
            return Fail(std::format(L"NvOFInit(scene detector) failed: {}", DriverError(code)));
        }

        if (!CreateInputSurface(firstInput) || !CreateInputSurface(secondInput)
                || !CreateFlowSurface(forward) || !CreateFlowSurface(backward)) {
            const std::wstring saved = status;
            ResetUnlocked();
            status = saved;
            return false;
        }

        status = std::format(L"NVOF scene detector ready, {}x{}, fast bidirectional 4x4 flow", width, height);
        return true;
    }

    bool CopyInput(ID3D11Texture2D* source, RegisteredInput& input)
    {
        if (!source || !input.texture || !input.handle || !context) {
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        source->GetDesc(&desc);
        if (desc.Width != width || desc.Height != height || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            desc.SampleDesc.Count != 1) {
            status = L"NVOF scene detector requires single-sample BGRA8 source textures at the configured size";
            return false;
        }
        context->CopyResource(input.texture, source);
        return true;
    }

    bool Analyze(ID3D11Texture2D* firstTexture, ID3D11Texture2D* secondTexture, CNvidiaSceneChangeDetector::Metrics& metrics)
    {
        std::scoped_lock lock(mutex);
        metrics = {};
        if (!session) {
            status = L"NVOF scene detector is not initialized";
            return false;
        }

        if (!CopyInput(firstTexture, firstInput) || !CopyInput(secondTexture, secondInput)) {
            return false;
        }

        nvof_scene::ExecuteInputParams input = {};
        input.inputFrame = secondInput.handle;
        input.referenceFrame = firstInput.handle;
        input.disableTemporalHints = nvof_scene::True;
        nvof_scene::ExecuteOutputParams output = {};
        output.outputBuffer = forward.handle;
        output.backwardOutputBuffer = backward.handle;

        const auto code = api.execute(session, &input, &output);
        if (code != nvof_scene::Success) {
            status = std::format(L"NvOFExecute(scene detector) failed: {}", DriverError(code));
            return false;
        }

        context->CopyResource(forward.staging, forward.texture);
        context->CopyResource(backward.staging, backward.texture);

        D3D11_MAPPED_SUBRESOURCE fwd = {};
        D3D11_MAPPED_SUBRESOURCE bwd = {};
        HRESULT hr = context->Map(forward.staging, 0, D3D11_MAP_READ, 0, &fwd);
        if (FAILED(hr)) {
            status = std::format(L"Map(NVOF forward flow) failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }
        hr = context->Map(backward.staging, 0, D3D11_MAP_READ, 0, &bwd);
        if (FAILED(hr)) {
            context->Unmap(forward.staging, 0);
            status = std::format(L"Map(NVOF backward flow) failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }

        std::vector<float> magnitudes;
        magnitudes.reserve(static_cast<size_t>(flowWidth) * flowHeight);
        double sumMotion = 0.0;
        double sumResidual = 0.0;
        double sumForwardX = 0.0;
        double sumForwardY = 0.0;
        size_t incoherent = 0;
        size_t count = 0;

        for (UINT y = 0; y < flowHeight; ++y) {
            const auto* frow = reinterpret_cast<const FlowVector*>(static_cast<const uint8_t*>(fwd.pData) + static_cast<size_t>(y) * fwd.RowPitch);
            const auto* brow = reinterpret_cast<const FlowVector*>(static_cast<const uint8_t*>(bwd.pData) + static_cast<size_t>(y) * bwd.RowPitch);
            for (UINT x = 0; x < flowWidth; ++x) {
                const float fx = frow[x].x * FlowFixedPointScale;
                const float fy = frow[x].y * FlowFixedPointScale;
                const float bx = brow[x].x * FlowFixedPointScale;
                const float by = brow[x].y * FlowFixedPointScale;
                const float forwardMag = std::hypot(fx, fy);
                const float backwardMag = std::hypot(bx, by);
                const float motion = 0.5f * (forwardMag + backwardMag);
                const float residual = std::hypot(fx + bx, fy + by);
                magnitudes.push_back(motion);
                sumMotion += motion;
                sumResidual += residual;
                sumForwardX += fx;
                sumForwardY += fy;
                if (residual > 4.0f + 0.35f * (forwardMag + backwardMag)) {
                    ++incoherent;
                }
                ++count;
            }
        }

        context->Unmap(backward.staging, 0);
        context->Unmap(forward.staging, 0);

        if (!count) {
            status = L"NVOF scene detector returned an empty flow field";
            return false;
        }

        const float meanMotion = static_cast<float>(sumMotion / count);
        const float meanResidual = static_cast<float>(sumResidual / count);
        const size_t p90Index = std::min(magnitudes.size() - 1,
            static_cast<size_t>(0.90 * static_cast<double>(magnitudes.size() - 1)));
        std::nth_element(magnitudes.begin(), magnitudes.begin() + p90Index, magnitudes.end());
        const float p90 = magnitudes[p90Index];
        const float residualRatio = meanResidual / std::max(1.0f, meanMotion);
        const float incoherentFraction = static_cast<float>(incoherent) / static_cast<float>(count);
        const float meanVectorLength = std::hypot(static_cast<float>(sumForwardX / count), static_cast<float>(sumForwardY / count));
        const float directionCoherence = meanVectorLength / std::max(1.0f, meanMotion);

        metrics.valid = true;
        metrics.meanMotionPixels = meanMotion;
        metrics.p90MotionPixels = p90;
        metrics.meanBidirectionalResidualPixels = meanResidual;
        metrics.residualRatio = residualRatio;
        metrics.incoherentFraction = incoherentFraction;
        metrics.directionCoherence = directionCoherence;

        // Deliberately conservative without the hardware cost map.  This is
        // intended to reject obvious cross-cut flow, not classify energetic
        // same-shot motion as a cut.  Image-comparison mode remains available
        // independently and can later be used as corroboration.
        metrics.likelyCut = p90 > 24.0f
            && meanMotion > 6.0f
            && residualRatio > 0.85f
            && incoherentFraction > 0.65f
            && directionCoherence < 0.18f;

        status = std::format(L"NVOF scene analysis: motion {:.1f}px (p90 {:.1f}), residual {:.2f}, incoherent {:.0f}%{}",
            meanMotion, p90, residualRatio, incoherentFraction * 100.0f,
            metrics.likelyCut ? L"; cut" : L"");
        return true;
    }
};

CNvidiaSceneChangeDetector::CNvidiaSceneChangeDetector()
    : m_impl(std::make_unique<Impl>())
{
}

CNvidiaSceneChangeDetector::~CNvidiaSceneChangeDetector()
{
    Reset();
}

bool CNvidiaSceneChangeDetector::Initialize(ID3D11Device* device, UINT width, UINT height)
{
    return m_impl->Initialize(device, width, height);
}

bool CNvidiaSceneChangeDetector::Analyze(ID3D11Texture2D* first, ID3D11Texture2D* second, Metrics& metrics)
{
    return m_impl->Analyze(first, second, metrics);
}

void CNvidiaSceneChangeDetector::Reset()
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->ResetUnlocked();
    m_impl->status = L"Disabled";
}

const std::wstring& CNvidiaSceneChangeDetector::GetStatus() const
{
    return m_impl->status;
}
