#include "stdafx.h"

#include "RifePlaybackPipeline.h"

#include "DX11VideoProcessor.h"
#include "NvidiaSceneChangeDetector.h"
#include "RifeFrameInterpolation.h"
#include "RifeSceneBlender.h"
#include "VideoRenderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr size_t kSourcePoolSize = 12;
constexpr REFERENCE_TIME kLateTolerance = 10'000; // 1 ms in DirectShow ticks.
constexpr ULONGLONG kPresentationCapacityWaitMs = 100;
constexpr size_t kRuntimeCacheSize = 4;
constexpr ULONGLONG kRuntimeRetryDelayMs = 2'000;

FrameInterpolationRateMode ToSchedulerMode(const int mode)
{
    switch (mode) {
    case RIFE_MODE_ToScreen:  return FrameInterpolationRateMode::ToScreen;
    case RIFE_MODE_Movie2x:   return FrameInterpolationRateMode::Movie2x;
    case RIFE_MODE_Movie2_5x: return FrameInterpolationRateMode::Movie2_5x;
    case RIFE_MODE_Movie3x:   return FrameInterpolationRateMode::Movie3x;
    case RIFE_MODE_Movie4x:   return FrameInterpolationRateMode::Movie4x;
    case RIFE_MODE_Movie5x:   return FrameInterpolationRateMode::Movie5x;
    case RIFE_MODE_Fixed60:   return FrameInterpolationRateMode::Fixed60;
    case RIFE_MODE_Fixed72:   return FrameInterpolationRateMode::Fixed72;
    case RIFE_MODE_Fixed90:   return FrameInterpolationRateMode::Fixed90;
    case RIFE_MODE_Fixed120:  return FrameInterpolationRateMode::Fixed120;
    case RIFE_MODE_Custom:    return FrameInterpolationRateMode::Custom;
    default:                  return FrameInterpolationRateMode::Disabled;
    }
}

FrameRate SourceRateFromDuration(const REFERENCE_TIME duration)
{
    if (duration <= 0 || duration > static_cast<REFERENCE_TIME>(UINT32_MAX)) {
        return {};
    }
    return {10'000'000u, static_cast<uint32_t>(duration)};
}

std::filesystem::path LocalAppDataRoot()
{
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!required) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
    if (!written || written >= required) {
        return {};
    }
    value.resize(written);
    return std::filesystem::path(value) / L"MPCVideoRenderer" / L"RIFE";
}

bool SameTextureShape(ID3D11Texture2D* texture, ID3D11Device* device, UINT width, UINT height)
{
    if (!texture || !device) {
        return false;
    }
    CComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    if (textureDevice != device) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    return desc.Width == width && desc.Height == height
        && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
        && desc.SampleDesc.Count == 1;
}

HRESULT CreateBgraTexture(ID3D11Device* device, UINT width, UINT height, ID3D11Texture2D** texture)
{
    if (!device || !width || !height || !texture) {
        return E_INVALIDARG;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return device->CreateTexture2D(&desc, nullptr, texture);
}

struct RuntimeBuildState {
    std::mutex mutex;
    std::shared_ptr<CRifeFrameInterpolation> runtime;
    std::wstring status = L"Not started";
    std::atomic_bool done = false;
    std::atomic_bool success = false;
    std::atomic_uint64_t retryAfterTick = 0;
};

struct RuntimeKey {
    ID3D11Device* device = nullptr;
    UINT width = 0;
    UINT height = 0;
    int gpu = RIFE_GPU_Auto;
    int contexts = RIFE_GPU_THREADS_DEF;
    bool performanceBoost = false;

    bool operator==(const RuntimeKey& other) const noexcept
    {
        return device == other.device && width == other.width && height == other.height
            && gpu == other.gpu && contexts == other.contexts
            && performanceBoost == other.performanceBoost;
    }
};

class ImageCutDetector
{
public:
    bool Analyze(ID3D11Device* device, ID3D11Texture2D* first, ID3D11Texture2D* second, bool& likelyCut)
    {
        likelyCut = false;
        if (!device || !first || !second) {
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        first->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC secondDesc = {};
        second->GetDesc(&secondDesc);
        if (desc.Width != secondDesc.Width || desc.Height != secondDesc.Height
                || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                || secondDesc.Format != desc.Format) {
            return false;
        }

        if (!EnsureResources(device, desc.Width, desc.Height)) {
            return false;
        }

        m_context->CopyResource(m_firstStaging, first);
        m_context->CopyResource(m_secondStaging, second);
        m_context->Flush();

        D3D11_MAPPED_SUBRESOURCE a = {};
        D3D11_MAPPED_SUBRESOURCE b = {};
        if (FAILED(m_context->Map(m_firstStaging, 0, D3D11_MAP_READ, 0, &a))) {
            return false;
        }
        if (FAILED(m_context->Map(m_secondStaging, 0, D3D11_MAP_READ, 0, &b))) {
            m_context->Unmap(m_firstStaging, 0);
            return false;
        }

        const UINT stepX = std::max<UINT>(1, desc.Width / 64);
        const UINT stepY = std::max<UINT>(1, desc.Height / 36);
        double sumA = 0.0;
        double sumB = 0.0;
        double sumAA = 0.0;
        double sumBB = 0.0;
        double sumAB = 0.0;
        double mad = 0.0;
        uint64_t samples = 0;

        for (UINT y = stepY / 2; y < desc.Height; y += stepY) {
            const auto* rowA = static_cast<const uint8_t*>(a.pData) + static_cast<size_t>(y) * a.RowPitch;
            const auto* rowB = static_cast<const uint8_t*>(b.pData) + static_cast<size_t>(y) * b.RowPitch;
            for (UINT x = stepX / 2; x < desc.Width; x += stepX) {
                const auto* pa = rowA + static_cast<size_t>(x) * 4;
                const auto* pb = rowB + static_cast<size_t>(x) * 4;
                const double ya = (0.0722 * pa[0] + 0.7152 * pa[1] + 0.2126 * pa[2]) / 255.0;
                const double yb = (0.0722 * pb[0] + 0.7152 * pb[1] + 0.2126 * pb[2]) / 255.0;
                sumA += ya;
                sumB += yb;
                sumAA += ya * ya;
                sumBB += yb * yb;
                sumAB += ya * yb;
                mad += std::abs(ya - yb);
                ++samples;
            }
        }

        m_context->Unmap(m_secondStaging, 0);
        m_context->Unmap(m_firstStaging, 0);
        if (samples < 16) {
            return false;
        }

        const double n = static_cast<double>(samples);
        const double meanA = sumA / n;
        const double meanB = sumB / n;
        const double varA = std::max(0.0, sumAA / n - meanA * meanA);
        const double varB = std::max(0.0, sumBB / n - meanB * meanB);
        const double covariance = sumAB / n - meanA * meanB;
        double correlation = 1.0;
        const double denom = std::sqrt(varA * varB);
        if (denom > 1.0e-8) {
            correlation = std::clamp(covariance / denom, -1.0, 1.0);
        } else if (std::abs(meanA - meanB) > 0.02) {
            correlation = 0.0;
        }

        const double normalizedMad = mad / n;
        likelyCut = correlation < 0.15 && normalizedMad > 0.055;
        return true;
    }

    void Reset()
    {
        m_secondStaging.Release();
        m_firstStaging.Release();
        m_context.Release();
        m_device.Release();
        m_width = m_height = 0;
    }

private:
    bool EnsureResources(ID3D11Device* device, UINT width, UINT height)
    {
        if (m_device == device && m_width == width && m_height == height
                && m_firstStaging && m_secondStaging && m_context) {
            return true;
        }
        Reset();
        m_device = device;
        m_width = width;
        m_height = height;
        device->GetImmediateContext(&m_context);
        if (!m_context) {
            return false;
        }
        CComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(m_context->QueryInterface(IID_PPV_ARGS(&multithread))) && multithread) {
            multithread->SetMultithreadProtected(TRUE);
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &m_firstStaging))
            && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &m_secondStaging));
    }

    CComPtr<ID3D11Device> m_device;
    CComPtr<ID3D11DeviceContext> m_context;
    CComPtr<ID3D11Texture2D> m_firstStaging;
    CComPtr<ID3D11Texture2D> m_secondStaging;
    UINT m_width = 0;
    UINT m_height = 0;
};

} // namespace

struct CRifePlaybackPipeline::Impl
{
    struct SourceSlot {
        CComPtr<ID3D11Texture2D> texture;
        bool inUse = false;
    };

    struct SourceFrame {
        size_t slot = SIZE_MAX;
        CComPtr<ID3D11Texture2D> texture;
        CDX11VideoProcessor* processor = nullptr;
        REFERENCE_TIME time = INVALID_TIME;
        REFERENCE_TIME frameDuration = 0;
        uint64_t presenterGeneration = 0;
        uint64_t resetSerial = 0;
        Settings_t settings;
        FrameRate displayRate;
        uint32_t maxMultiplierMilli = 0;
        uint32_t maxOutputFpsMilli = 0;
        CComPtr<IReferenceClock> clock;
        REFERENCE_TIME graphStart = 0;
    };

    static bool RifeFramesCompatible(const SourceFrame& first, const SourceFrame& second)
    {
        if (!first.texture || !second.texture || !first.processor || first.processor != second.processor) {
            return false;
        }

        CComPtr<ID3D11Device> firstDevice;
        CComPtr<ID3D11Device> secondDevice;
        first.texture->GetDevice(&firstDevice);
        second.texture->GetDevice(&secondDevice);
        if (!firstDevice || firstDevice != secondDevice) {
            return false;
        }

        D3D11_TEXTURE2D_DESC firstDesc = {};
        D3D11_TEXTURE2D_DESC secondDesc = {};
        first.texture->GetDesc(&firstDesc);
        second.texture->GetDesc(&secondDesc);
        return firstDesc.Width == secondDesc.Width
            && firstDesc.Height == secondDesc.Height
            && firstDesc.Format == secondDesc.Format
            && firstDesc.SampleDesc.Count == secondDesc.SampleDesc.Count
            && firstDesc.SampleDesc.Quality == secondDesc.SampleDesc.Quality;
    }

    explicit Impl(CMpcVideoRenderer* renderer)
        : owner(renderer)
        , worker(&Impl::WorkerMain, this)
    {
    }

    ~Impl()
    {
        stop.store(true);
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        ClearQueuedFrames();
    }

    CMpcVideoRenderer* owner = nullptr;
    std::array<SourceSlot, kSourcePoolSize> sourcePool;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<SourceFrame> queue;
    std::thread worker;
    std::atomic_bool stop = false;
    std::atomic_uint64_t resetSerial = 1;
    uint64_t lastSubmittedGeneration = UINT64_MAX;

    CFrameInterpolationScheduler scheduler;
    bool schedulerConfigured = false;
    int schedulerMode = RIFE_MODE_Disabled;
    int schedulerCustomFps = 0;
    FrameRate schedulerDisplayRate = {};
    uint64_t schedulerSerial = 0;
    uint32_t schedulerMaxMultiplierMilli = 0;
    uint32_t schedulerMaxOutputFpsMilli = 0;

    CNvidiaSceneChangeDetector nvofDetector;
    ImageCutDetector imageDetector;
    CRifeSceneBlender sceneBlender;
    CComPtr<ID3D11Texture2D> outputTexture;
    CComPtr<ID3D11Device> outputDevice;
    UINT outputWidth = 0;
    UINT outputHeight = 0;

    std::shared_ptr<RuntimeBuildState> runtimeBuild;
    std::optional<RuntimeKey> runtimeKey;
    std::deque<std::pair<RuntimeKey, std::shared_ptr<RuntimeBuildState>>> runtimeCache;
    uint32_t nextContext = 0;
    bool removeEveryOtherToggle = false;

    std::atomic_uint64_t generatedFrames = 0;
    std::atomic_uint64_t sceneRepeatFrames = 0;
    std::atomic_uint64_t inferenceFallbackFrames = 0;
    std::atomic_uint64_t runtimeWaitPairs = 0;
    std::atomic_uint64_t lateSyntheticDrops = 0;
    std::atomic_uint64_t sourceContinuityFrames = 0;
    std::atomic_uint64_t presentationDrops = 0;
    std::atomic_uint64_t presentationReclaims = 0;
    std::atomic_uint64_t sourceResyncs = 0;
    std::atomic_uint64_t lastInferenceUs = 0;
    std::atomic_int activeRule = -1;
    std::atomic_bool ruleBypass = false;
    std::atomic_uint32_t ruleMaxMultiplierMilli = 0;
    std::atomic_uint32_t ruleMaxOutputFpsMilli = 0;

    void ClearQueuedFrames()
    {
        std::deque<SourceFrame> stale;
        {
            std::lock_guard lock(mutex);
            stale.swap(queue);
            for (const auto& frame : stale) {
                if (frame.slot < sourcePool.size()) {
                    sourcePool[frame.slot].inUse = false;
                }
            }
        }
    }

    void ResetNonBlocking()
    {
        resetSerial.fetch_add(1, std::memory_order_acq_rel);
        ClearQueuedFrames();
        cv.notify_all();
    }

    void ResetSequenceState()
    {
        scheduler.Reset();
        schedulerConfigured = false;
        nvofDetector.Reset();
        imageDetector.Reset();
        sceneBlender.Reset();
        removeEveryOtherToggle = false;
    }

    bool AcquireSourceSlot(ID3D11Device* device, UINT width, UINT height, size_t& index, ID3D11Texture2D** texture)
    {
        index = SIZE_MAX;
        if (!device || !width || !height || !texture) {
            return false;
        }

        std::lock_guard lock(mutex);
        for (size_t i = 0; i < sourcePool.size(); ++i) {
            auto& slot = sourcePool[i];
            if (slot.inUse) {
                continue;
            }
            if (!SameTextureShape(slot.texture, device, width, height)) {
                slot.texture.Release();
                if (FAILED(CreateBgraTexture(device, width, height, &slot.texture))) {
                    continue;
                }
            }
            slot.inUse = true;
            index = i;
            *texture = slot.texture;
            (*texture)->AddRef();
            return true;
        }
        return false;
    }

    void ReleaseSourceSlot(size_t index)
    {
        if (index >= sourcePool.size()) {
            return;
        }
        std::lock_guard lock(mutex);
        sourcePool[index].inUse = false;
    }

    bool Submit(
        CDX11VideoProcessor* processor,
        IMediaSample* sample,
        const Settings_t& settings,
        uint64_t presenterGeneration,
        FrameRate displayRate,
        REFERENCE_TIME frameDuration)
    {
        if (!owner || !processor || !sample || settings.iRifeMode == RIFE_MODE_Disabled) {
            return false;
        }

        // Match the visible source geometry, before TensorRT padding, window
        // scaling, texture allocation, or any engine work. Returning false
        // uses Receive's existing normally paced source-video path.
        const CSize contentSize = processor->GetRifeContentSize();
        const int ruleIndex = MatchRifeRateRule(settings.rifeRules,
            static_cast<uint32_t>(std::max<LONG>(0, contentSize.cx)),
            static_cast<uint32_t>(std::max<LONG>(0, contentSize.cy)), frameDuration);
        uint32_t maxMultiplierMilli = 0;
        uint32_t maxOutputFpsMilli = 0;
        bool bypass = false;
        if (ruleIndex >= 0) {
            const auto& rule = settings.rifeRules.rules[ruleIndex];
            maxMultiplierMilli = rule.maxMultiplierMilli;
            maxOutputFpsMilli = rule.maxOutputFpsMilli;
            bypass = rule.off;
            if (!bypass && (maxMultiplierMilli || maxOutputFpsMilli)) {
                CFrameInterpolationScheduler capped;
                capped.Configure(ToSchedulerMode(settings.iRifeMode),
                    {static_cast<uint32_t>(settings.iRifeCustomFps), 1}, displayRate,
                    maxMultiplierMilli, maxOutputFpsMilli);
                const FrameRate sourceRate = SourceRateFromDuration(frameDuration);
                const FrameRate targetRate = capped.ResolveTargetRate(sourceRate);
                // A cap at/below source FPS means no interpolation, never
                // decimate the original video to satisfy a workload limit.
                bypass = sourceRate.IsValid() && (!targetRate.IsValid()
                    || static_cast<uint64_t>(targetRate.numerator) * sourceRate.denominator
                        <= static_cast<uint64_t>(sourceRate.numerator) * targetRate.denominator
                    || maxMultiplierMilli == 1000);
            }
        }
        activeRule.store(ruleIndex, std::memory_order_relaxed);
        ruleBypass.store(bypass, std::memory_order_relaxed);
        ruleMaxMultiplierMilli.store(maxMultiplierMilli, std::memory_order_relaxed);
        ruleMaxOutputFpsMilli.store(maxOutputFpsMilli, std::memory_order_relaxed);
        if (bypass) {
            return false;
        }

        if (lastSubmittedGeneration != presenterGeneration) {
            lastSubmittedGeneration = presenterGeneration;
            ResetNonBlocking();
        }

        ID3D11Device* device = processor->GetRifeDevice();
        const CSize size = processor->GetRifeFrameSize();
        if (!device || size.cx <= 0 || size.cy <= 0) {
            return false;
        }

        size_t slot = SIZE_MAX;
        CComPtr<ID3D11Texture2D> texture;
        if (!AcquireSourceSlot(device, static_cast<UINT>(size.cx), static_cast<UINT>(size.cy), slot, &texture)) {
            return false;
        }

        REFERENCE_TIME sourceTime = INVALID_TIME;
        if (!processor->PrepareRifeSource(sample, texture, sourceTime) || sourceTime == INVALID_TIME) {
            ReleaseSourceSlot(slot);
            return false;
        }

        SourceFrame frame;
        frame.slot = slot;
        frame.texture = texture;
        frame.processor = processor;
        frame.time = sourceTime;
        frame.frameDuration = frameDuration;
        frame.presenterGeneration = presenterGeneration;
        frame.resetSerial = resetSerial.load(std::memory_order_acquire);
        frame.settings = settings;
        frame.displayRate = displayRate;
        frame.maxMultiplierMilli = maxMultiplierMilli;
        frame.maxOutputFpsMilli = maxOutputFpsMilli;
        frame.clock = owner->m_pClock;
        frame.graphStart = static_cast<REFERENCE_TIME>(owner->m_tStart);

        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(frame));
        }
        cv.notify_one();
        return true;
    }

    bool IsCurrent(const SourceFrame& frame) const
    {
        return owner && !stop.load(std::memory_order_acquire)
            && frame.resetSerial == resetSerial.load(std::memory_order_acquire)
            && frame.presenterGeneration == owner->m_FrameInterpolationPresenterGeneration.load(std::memory_order_acquire);
    }

    bool IsLate(const SourceFrame& frame, REFERENCE_TIME presentationTime) const
    {
        if (!frame.clock) {
            return false;
        }
        REFERENCE_TIME now = 0;
        if (FAILED(frame.clock->GetTime(&now))) {
            return false;
        }
        return now > frame.graphStart + presentationTime + kLateTolerance;
    }

    bool QueueTexture(const SourceFrame& frame, ID3D11Texture2D* texture, REFERENCE_TIME time,
        bool synthetic, bool dropIfLate = true)
    {
        if (!texture || !frame.processor) {
            return false;
        }

        const ULONGLONG waitStart = GetTickCount64();
        for (;;) {
            if (!IsCurrent(frame)) {
                return false;
            }

            const bool late = IsLate(frame, time);
            if (synthetic && dropIfLate && late) {
                lateSyntheticDrops.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            UINT handle = UINT_MAX;
            if (frame.processor->ReserveRifePresentationSurface(texture, handle)) {
                if (owner->QueueFrameInterpolationSource(
                        handle, time, synthetic, frame.presenterGeneration)) {
                    return true;
                }
                frame.processor->ReleaseFrameInterpolationSource(handle);
                return false;
            }

            const bool waitExpired = GetTickCount64() - waitStart >= kPresentationCapacityWaitMs;
            if (synthetic && waitExpired) {
                presentationDrops.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            // Backpressure the worker until the presenter retires a surface.
            // This keeps fast RIFE/image-comparison paths from filling all four
            // presentation surfaces and then evicting their own queued output.
            // A real source frame may reclaim only after its presentation time
            // has passed (or the bounded fallback wait expires).
            if (!synthetic && (late || waitExpired)) {
                if (owner->ReclaimFrameInterpolationPresentationSource()) {
                    presentationReclaims.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                presentationDrops.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            Sleep(1);
        }
    }

    bool AcquireRifePresentationSurfaceWithBackpressure(const SourceFrame& frame, UINT width, UINT height,
        REFERENCE_TIME time, ID3D11Texture2D** target, UINT& handle)
    {
        if (!frame.processor || !target) {
            return false;
        }

        const ULONGLONG waitStart = GetTickCount64();
        for (;;) {
            if (!IsCurrent(frame)) {
                return false;
            }
            if (IsLate(frame, time)) {
                lateSyntheticDrops.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (frame.processor->AcquireRifePresentationSurface(width, height, target, handle)) {
                return true;
            }
            if (GetTickCount64() - waitStart >= kPresentationCapacityWaitMs) {
                presentationDrops.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            Sleep(1);
        }
    }

    bool QueueReservedSurface(const SourceFrame& frame, UINT handle, REFERENCE_TIME time,
        bool synthetic, bool dropIfLate = true)
    {
        if (handle == UINT_MAX || !frame.processor) {
            return false;
        }

        const auto release = [&]() {
            frame.processor->ReleaseFrameInterpolationSource(handle);
        };

        if (!IsCurrent(frame)) {
            release();
            return false;
        }
        if (synthetic && dropIfLate && IsLate(frame, time)) {
            lateSyntheticDrops.fetch_add(1, std::memory_order_relaxed);
            release();
            return false;
        }
        if (owner->QueueFrameInterpolationSource(
                handle, time, synthetic, frame.presenterGeneration)) {
            return true;
        }

        presentationDrops.fetch_add(1, std::memory_order_relaxed);
        release();
        return false;
    }

    void ConfigureScheduler(const SourceFrame& frame)
    {
        const bool changed = !schedulerConfigured
            || schedulerSerial != frame.resetSerial
            || schedulerMode != frame.settings.iRifeMode
            || schedulerCustomFps != frame.settings.iRifeCustomFps
            || schedulerMaxMultiplierMilli != frame.maxMultiplierMilli
            || schedulerMaxOutputFpsMilli != frame.maxOutputFpsMilli
            || schedulerDisplayRate.numerator != frame.displayRate.numerator
            || schedulerDisplayRate.denominator != frame.displayRate.denominator;
        if (!changed) {
            return;
        }

        schedulerMode = frame.settings.iRifeMode;
        schedulerCustomFps = frame.settings.iRifeCustomFps;
        schedulerMaxMultiplierMilli = frame.maxMultiplierMilli;
        schedulerMaxOutputFpsMilli = frame.maxOutputFpsMilli;
        schedulerDisplayRate = frame.displayRate;
        schedulerSerial = frame.resetSerial;
        scheduler.Configure(
            ToSchedulerMode(frame.settings.iRifeMode),
            {static_cast<uint32_t>(std::clamp(frame.settings.iRifeCustomFps,
                RIFE_CUSTOM_FPS_MIN, RIFE_CUSTOM_FPS_MAX)), 1},
            frame.displayRate, frame.maxMultiplierMilli, frame.maxOutputFpsMilli);
        schedulerConfigured = true;
    }

    bool EnsureOutputTexture(ID3D11Device* device, UINT width, UINT height)
    {
        if (outputDevice == device && outputWidth == width && outputHeight == height
                && SameTextureShape(outputTexture, device, width, height)) {
            return true;
        }
        outputTexture.Release();
        outputDevice = device;
        outputWidth = width;
        outputHeight = height;
        return SUCCEEDED(CreateBgraTexture(device, width, height, &outputTexture));
    }

    std::shared_ptr<CRifeFrameInterpolation> ReadyRuntime() const
    {
        const auto build = runtimeBuild;
        if (!build || !build->done.load(std::memory_order_acquire)
                || !build->success.load(std::memory_order_acquire)) {
            return {};
        }
        std::lock_guard lock(build->mutex);
        return build->runtime;
    }

    void EnsureRuntimeBuild(const SourceFrame& frame, ID3D11Device* device, UINT width, UINT height)
    {
        RuntimeKey key;
        key.device = device;
        key.width = width;
        key.height = height;
        key.gpu = frame.settings.iRifeGPU;
        key.contexts = std::clamp(frame.settings.iRifeGpuThreads, RIFE_GPU_THREADS_MIN, RIFE_GPU_THREADS_MAX);
        key.performanceBoost = frame.settings.bRifePerformanceBoost;

        for (auto it = runtimeCache.begin(); it != runtimeCache.end(); ++it) {
            if (!(it->first == key)) {
                continue;
            }

            const auto cached = it->second;
            const bool failed = cached->done.load(std::memory_order_acquire)
                && !cached->success.load(std::memory_order_acquire);
            if (!failed || GetTickCount64() < cached->retryAfterTick.load(std::memory_order_acquire)) {
                runtimeKey = key;
                runtimeBuild = cached;
                return;
            }

            // A transient initialization failure must not leave this key stuck
            // in runtime-wait forever. Drop the failed entry after a short
            // cooldown and let the normal build path retry it.
            runtimeCache.erase(it);
            break;
        }

        runtimeKey = key;
        nextContext = 0;

        auto state = std::make_shared<RuntimeBuildState>();
        runtimeBuild = state;
        if (runtimeCache.size() >= kRuntimeCacheSize) {
            runtimeCache.pop_front();
        }
        runtimeCache.emplace_back(key, state);

        const auto root = LocalAppDataRoot();
        const auto model = root / L"models" / L"rife_v4.6.onnx";
        const auto cache = root / L"cache";
        if (root.empty() || !std::filesystem::exists(model)) {
            state->status = L"RIFE 4.6 model is not installed";
            state->retryAfterTick.store(GetTickCount64() + kRuntimeRetryDelayMs,
                std::memory_order_release);
            state->done.store(true, std::memory_order_release);
            return;
        }

        device->AddRef();
        const uint32_t gpuIndex = key.gpu == RIFE_GPU_Auto
            ? UINT32_MAX : static_cast<uint32_t>(key.gpu);
        std::thread([state, device, width, height, gpuIndex, key, model, cache]() {
            auto runtime = std::make_shared<CRifeFrameInterpolation>();
            const bool ok = runtime->Initialize(
                L"", device, width, height, gpuIndex,
                static_cast<uint32_t>(key.contexts), key.performanceBoost,
                model.wstring(), cache.wstring());
            device->Release();

            {
                std::lock_guard lock(state->mutex);
                state->status = runtime->GetStatus();
                if (ok) {
                    state->runtime = std::move(runtime);
                }
            }
            if (!ok) {
                state->retryAfterTick.store(GetTickCount64() + kRuntimeRetryDelayMs,
                    std::memory_order_release);
            }
            state->success.store(ok, std::memory_order_release);
            state->done.store(true, std::memory_order_release);
        }).detach();
    }

    bool DetectImageSceneCut(const SourceFrame& first, const SourceFrame& second)
    {
        ID3D11Device* device = second.processor ? second.processor->GetRifeDevice() : nullptr;
        if (!device) {
            return false;
        }

        bool cut = false;
        return imageDetector.Analyze(device, first.texture, second.texture, cut) && cut;
    }

    bool GenerateRife(
        const SourceFrame& first,
        const SourceFrame& second,
        float timestep,
        ID3D11Texture2D* output)
    {
        if (!output || !second.processor) {
            return false;
        }
        ID3D11Device* device = second.processor->GetRifeDevice();
        if (!device) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        second.texture->GetDesc(&desc);
        EnsureRuntimeBuild(second, device, desc.Width, desc.Height);
        auto runtime = ReadyRuntime();
        D3D11_TEXTURE2D_DESC outputDesc = {};
        output->GetDesc(&outputDesc);
        if (!runtime
                || outputDesc.Width != desc.Width
                || outputDesc.Height != desc.Height
                || outputDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                || outputDesc.SampleDesc.Count != 1) {
            return false;
        }

        MpcvrRifeStats stats = {};
        const uint32_t contextCount = static_cast<uint32_t>(std::clamp(
            second.settings.iRifeGpuThreads, RIFE_GPU_THREADS_MIN, RIFE_GPU_THREADS_MAX));
        const uint32_t context = nextContext++ % std::max(1u, contextCount);
        if (!runtime->Interpolate(context, first.texture, second.texture,
                output, timestep, stats)) {
            return false;
        }
        lastInferenceUs.store(static_cast<uint64_t>(std::max(0.0, stats.inferenceMs) * 1000.0),
            std::memory_order_relaxed);
        return true;
    }

    void ProcessPair(SourceFrame& first, SourceFrame& second)
    {
        if (!IsCurrent(second) || second.time <= first.time) {
            return;
        }

        ConfigureScheduler(second);
        const FrameRate sourceRate = SourceRateFromDuration(
            second.frameDuration > 0 ? second.frameDuration : second.time - first.time);
        const auto targets = scheduler.Schedule(first.time, second.time, sourceRate);

        ID3D11Device* device = second.processor ? second.processor->GetRifeDevice() : nullptr;
        D3D11_TEXTURE2D_DESC desc = {};
        if (second.texture) {
            second.texture->GetDesc(&desc);
        }
        if (device && desc.Width && desc.Height) {
            EnsureRuntimeBuild(second, device, desc.Width, desc.Height);
        }
        const bool runtimeReady = static_cast<bool>(ReadyRuntime());

        // During first-run TensorRT optimization or when the optional runtime
        // is absent, preserve ordinary video playback rather than holding B.
        if (!runtimeReady) {
            runtimeWaitPairs.fetch_add(1, std::memory_order_relaxed);
            QueueTexture(second, second.texture, second.time, false);
            return;
        }

        bool hasTimelySyntheticTarget = false;
        for (const auto& target : targets) {
            if (!target.exactSource && !IsLate(second, target.presentationTime)) {
                hasTimelySyntheticTarget = true;
                break;
            }
        }

        bool queuedOutput = false;
        bool sceneDecisionReady = second.settings.iRifeSceneDetection != RIFE_SCENE_NVOF;
        bool sceneCut = hasTimelySyntheticTarget
            && second.settings.iRifeSceneDetection == RIFE_SCENE_Image
            && DetectImageSceneCut(first, second);

        const auto queueSceneCutTarget = [&](const FrameInterpolationTarget& target) {
            if (second.settings.iRifeSceneProcessing == RIFE_SCENE_PROCESS_Blend
                    && device && desc.Width && desc.Height
                    && EnsureOutputTexture(device, desc.Width, desc.Height)
                    && sceneBlender.Blend(device, first.texture, second.texture,
                        outputTexture, static_cast<float>(target.timestep))) {
                return QueueTexture(second, outputTexture, target.presentationTime, true);
            }

            // Repeat is the default and also the safe fallback if the GPU
            // blend path cannot produce a frame.
            ID3D11Texture2D* repeated = target.timestep < 0.5
                ? first.texture.p : second.texture.p;
            sceneRepeatFrames.fetch_add(1, std::memory_order_relaxed);
            return QueueTexture(second, repeated, target.presentationTime, true);
        };

        for (const auto& target : targets) {
            if (!IsCurrent(second)) {
                return;
            }

            if (target.exactSource) {
                queuedOutput |= QueueTexture(second, second.texture, target.presentationTime, false);
                continue;
            }

            if (IsLate(second, target.presentationTime)) {
                lateSyntheticDrops.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (sceneDecisionReady && sceneCut) {
                queuedOutput |= queueSceneCutTarget(target);
                continue;
            }

            UINT generatedSurface = UINT_MAX;
            CComPtr<ID3D11Texture2D> generated;
            if (!AcquireRifePresentationSurfaceWithBackpressure(second, desc.Width, desc.Height,
                    target.presentationTime, &generated, generatedSurface)) {
                continue;
            }

            bool nvofStarted = false;
            if (!sceneDecisionReady && device && desc.Width && desc.Height) {
                const bool nvofReady = nvofDetector.Initialize(device, desc.Width, desc.Height);
                if (nvofReady && IsLate(second, target.presentationTime)) {
                    second.processor->ReleaseFrameInterpolationSource(generatedSurface);
                    lateSyntheticDrops.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                nvofStarted = nvofReady
                    && nvofDetector.BeginAnalyze(first.texture, second.texture);
                if (!nvofStarted) {
                    // Driver/API failures retain cut protection through the
                    // existing image-comparison path.
                    sceneCut = DetectImageSceneCut(first, second);
                    sceneDecisionReady = true;
                    if (sceneCut) {
                        second.processor->ReleaseFrameInterpolationSource(generatedSurface);
                        queuedOutput |= queueSceneCutTarget(target);
                        continue;
                    }
                }
            }

            const bool generatedOk = GenerateRife(
                first, second, static_cast<float>(target.timestep), generated);

            if (nvofStarted) {
                CNvidiaSceneChangeDetector::Metrics metrics;
                if (nvofDetector.FinishAnalyze(metrics) && metrics.valid) {
                    sceneCut = metrics.likelyCut;
                } else {
                    sceneCut = DetectImageSceneCut(first, second);
                }
                sceneDecisionReady = true;
                if (sceneCut) {
                    second.processor->ReleaseFrameInterpolationSource(generatedSurface);
                    queuedOutput |= queueSceneCutTarget(target);
                    continue;
                }
            }

            if (generatedOk) {
                generatedFrames.fetch_add(1, std::memory_order_relaxed);
                // CUDA writes directly into a presentation surface whose D3D11
                // retirement query controls reuse. This avoids re-mapping one
                // shared CUDA output while an asynchronous D3D11 copy from the
                // preceding inference may still be reading it.
                queuedOutput |= QueueReservedSurface(
                    second, generatedSurface, target.presentationTime, true, false);
            } else {
                second.processor->ReleaseFrameInterpolationSource(generatedSurface);
                // A transient inference failure must degrade to a real frame,
                // never stall the graph or audio clock.
                ID3D11Texture2D* fallback = target.timestep < 0.5
                    ? first.texture.p : second.texture.p;
                inferenceFallbackFrames.fetch_add(1, std::memory_order_relaxed);
                queuedOutput |= QueueTexture(second, fallback, target.presentationTime, true);
            }
        }
        // Rounded source timestamps need not coincide with the output grid.
        // Keep source video moving if every scheduled output was discarded.
        if (!targets.empty() && !queuedOutput && IsCurrent(second)) {
            if (QueueTexture(second, second.texture, second.time, false)) {
                sourceContinuityFrames.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    std::wstring Diagnostics() const
    {
        std::wstring diagnostics = std::format(
            L"generated {}, scene-repeat {}, infer-fallback {}, runtime-wait {}, late-drop {}, source-fallback {}, present-drop {}, present-reclaim {}, source-resync {}, last infer {:.2f} ms",
            generatedFrames.load(std::memory_order_relaxed),
            sceneRepeatFrames.load(std::memory_order_relaxed),
            inferenceFallbackFrames.load(std::memory_order_relaxed),
            runtimeWaitPairs.load(std::memory_order_relaxed),
            lateSyntheticDrops.load(std::memory_order_relaxed),
            sourceContinuityFrames.load(std::memory_order_relaxed),
            presentationDrops.load(std::memory_order_relaxed),
            presentationReclaims.load(std::memory_order_relaxed),
            sourceResyncs.load(std::memory_order_relaxed),
            lastInferenceUs.load(std::memory_order_relaxed) / 1000.0);
        const int ruleIndex = activeRule.load(std::memory_order_relaxed);
        if (ruleIndex >= 0) {
            diagnostics += std::format(L"\nRIFE rule    : #{}", ruleIndex + 1);
            if (ruleBypass.load(std::memory_order_relaxed)) {
                diagnostics += L" - off (source playback)";
            } else {
                const auto multiplier = ruleMaxMultiplierMilli.load(std::memory_order_relaxed);
                const auto fps = ruleMaxOutputFpsMilli.load(std::memory_order_relaxed);
                if (multiplier) diagnostics += std::format(L" - max {:.3g}x", multiplier / 1000.0);
                if (fps) diagnostics += std::format(L" - max {:.3f} fps", fps / 1000.0);
                if (!multiplier && !fps) diagnostics += L" - selected rate";
            }
        }
        return diagnostics;
    }

    void ReleaseFrame(SourceFrame& frame)
    {
        const size_t slot = frame.slot;
        frame.texture.Release();
        frame.slot = SIZE_MAX;
        ReleaseSourceSlot(slot);
    }

    void WorkerMain()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        std::optional<SourceFrame> previous;
        uint64_t activeSerial = 0;

        while (!stop.load(std::memory_order_acquire)) {
            SourceFrame current;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] {
                    return stop.load(std::memory_order_acquire) || !queue.empty();
                });
                if (stop.load(std::memory_order_acquire)) {
                    break;
                }
                current = std::move(queue.front());
                queue.pop_front();
            }

            if (!IsCurrent(current)) {
                ReleaseFrame(current);
                continue;
            }

            if (activeSerial != current.resetSerial) {
                if (previous) {
                    ReleaseFrame(*previous);
                    previous.reset();
                }
                activeSerial = current.resetSerial;
                ResetSequenceState();
            }

            if (current.settings.iRifeDuplicateRemoval == RIFE_DUPLICATES_RemoveEveryOther) {
                removeEveryOtherToggle = !removeEveryOtherToggle;
                if (!removeEveryOtherToggle) {
                    ReleaseFrame(current);
                    continue;
                }
            } else {
                removeEveryOtherToggle = false;
            }

            if (!previous) {
                // The scheduler anchors its target grid at this real frame.
                ConfigureScheduler(current);
                QueueTexture(current, current.texture, current.time, false);
                previous = std::move(current);
                continue;
            }

            if (!RifeFramesCompatible(*previous, current)) {
                sourceResyncs.fetch_add(1, std::memory_order_relaxed);
                ResetSequenceState();
                ConfigureScheduler(current);
                QueueTexture(current, current.texture, current.time, false);
                ReleaseFrame(*previous);
                previous = std::move(current);
                continue;
            }

            ProcessPair(*previous, current);
            ReleaseFrame(*previous);
            previous = std::move(current);
        }

        if (previous) {
            ReleaseFrame(*previous);
        }
    }
};

CRifePlaybackPipeline::CRifePlaybackPipeline(CMpcVideoRenderer* owner)
    : m_impl(std::make_unique<Impl>(owner))
{
}

CRifePlaybackPipeline::~CRifePlaybackPipeline() = default;

bool CRifePlaybackPipeline::SubmitSample(
    CDX11VideoProcessor* processor,
    IMediaSample* sample,
    const Settings_t& settings,
    uint64_t presenterGeneration,
    FrameRate displayRate,
    REFERENCE_TIME frameDuration)
{
    return m_impl && m_impl->Submit(
        processor, sample, settings, presenterGeneration, displayRate, frameDuration);
}

void CRifePlaybackPipeline::Reset() noexcept
{
    if (m_impl) {
        m_impl->ResetNonBlocking();
    }
}

std::wstring CRifePlaybackPipeline::GetDiagnostics() const
{
    return m_impl ? m_impl->Diagnostics() : std::wstring();
}
