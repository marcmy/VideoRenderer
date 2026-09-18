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
#include <chrono>
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
#include <vector>

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
    UINT contentWidth = 0;
    UINT contentHeight = 0;
    int gpu = RIFE_GPU_Auto;
    int contexts = RIFE_GPU_THREADS_DEF;
    bool performanceBoost = false;

    bool operator==(const RuntimeKey& other) const noexcept
    {
        return device == other.device && width == other.width && height == other.height
            && contentWidth == other.contentWidth && contentHeight == other.contentHeight
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
        UINT contentWidth = 0;
        UINT contentHeight = 0;
        uint64_t presenterGeneration = 0;
        uint64_t resetSerial = 0;
        Settings_t settings;
        FrameRate displayRate;
        uint32_t maxMultiplierMilli = 0;
        uint32_t maxOutputFpsMilli = 0;
        CComPtr<IReferenceClock> clock;
        REFERENCE_TIME graphStart = 0;
    };

    using SourceFramePtr = std::shared_ptr<SourceFrame>;

    struct TargetResult {
        FrameInterpolationTarget target;
        CComPtr<ID3D11Texture2D> generated;
        bool inferenceFailed = false;
        bool skippedLate = false;
    };

    struct PairJob {
        uint64_t sequence = 0;
        uint32_t contextIndex = 0;
        SourceFramePtr first;
        SourceFramePtr second;
        std::shared_ptr<CRifeFrameInterpolation> runtime;
        std::vector<FrameInterpolationTarget> targets;
        std::vector<TargetResult> results;
        UINT width = 0;
        UINT height = 0;
        bool sceneCut = false;
        std::atomic_bool done = false;
    };

    struct InferenceWorkerState {
        uint32_t index = 0;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::shared_ptr<PairJob>> queue;
        std::thread thread;
        bool stop = false;
        CNvidiaSceneChangeDetector nvofDetector;
        ImageCutDetector imageDetector;
        CComPtr<ID3D11Texture2D> inferenceFirst;
        CComPtr<ID3D11Texture2D> inferenceSecond;
        std::vector<CComPtr<ID3D11Texture2D>> inferenceOutputs;
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
    {
        for (uint32_t i = 0; i < inferenceWorkers.size(); ++i) {
            auto state = std::make_unique<InferenceWorkerState>();
            state->index = i;
            state->thread = std::thread(&Impl::InferenceWorkerMain, this, state.get());
            inferenceWorkers[i] = std::move(state);
        }
        worker = std::thread(&Impl::WorkerMain, this);
    }

    ~Impl()
    {
        stop.store(true);
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        for (auto& state : inferenceWorkers) {
            if (!state) continue;
            {
                std::lock_guard lock(state->mutex);
                state->stop = true;
            }
            state->cv.notify_all();
        }
        for (auto& state : inferenceWorkers) {
            if (state && state->thread.joinable()) {
                state->thread.join();
            }
        }
        ClearQueuedFrames();
    }

    CMpcVideoRenderer* owner = nullptr;
    std::array<SourceSlot, kSourcePoolSize> sourcePool;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<SourceFrame> queue;
    std::thread worker;
    std::array<std::unique_ptr<InferenceWorkerState>, RIFE_GPU_THREADS_MAX> inferenceWorkers;
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

    CRifeSceneBlender sceneBlender;
    CComPtr<ID3D11Texture2D> outputTexture;
    CComPtr<ID3D11Device> outputDevice;
    UINT outputWidth = 0;
    UINT outputHeight = 0;

    std::shared_ptr<RuntimeBuildState> runtimeBuild;
    std::optional<RuntimeKey> runtimeKey;
    std::deque<std::pair<RuntimeKey, std::shared_ptr<RuntimeBuildState>>> runtimeCache;
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
    std::atomic_uint64_t lastInputCopySubmitUs = 0;
    std::atomic_uint64_t lastRuntimeWallUs = 0;
    std::atomic_uint64_t lastInputMapUs = 0;
    std::atomic_uint64_t lastInputPackUs = 0;
    std::atomic_uint64_t lastInputUnmapUs = 0;
    std::atomic_uint64_t lastOutputMapUs = 0;
    std::atomic_uint64_t lastTensorRtUs = 0;
    std::atomic_uint64_t lastOutputWriteUs = 0;
    std::atomic_uint64_t lastOutputUnmapUs = 0;
    std::atomic_uint64_t lastContextLockWaitUs = 0;
    std::atomic_uint64_t lastCudaSetDeviceUs = 0;
    std::atomic_uint64_t lastRegistrationUs = 0;
    std::atomic_uint64_t lastInputPackLockWaitUs = 0;
    std::atomic_uint64_t lastRuntimeInternalUs = 0;
    std::atomic_uint64_t presentationSurfaceWaitUs = 0;
    std::atomic_uint64_t presentationSurfaceWaitCount = 0;
    std::atomic_uint64_t presentationSurfaceWaitMaxUs = 0;
    std::atomic_uint32_t activeInferences = 0;
    std::atomic_uint32_t maxConcurrentInferences = 0;
    std::atomic_uint32_t configuredInferenceContexts = RIFE_GPU_THREADS_DEF;
    std::atomic_bool tensorIoLinearValidated = false;
    std::atomic_int activeRule = -1;
    std::atomic_bool ruleBypass = false;
    std::atomic_uint32_t ruleMaxMultiplierMilli = 0;
    std::atomic_uint32_t ruleMaxOutputFpsMilli = 0;

    static uint64_t MsToUs(const double ms)
    {
        return static_cast<uint64_t>(std::max(0.0, ms) * 1000.0);
    }

    static void UpdateMax(std::atomic_uint64_t& target, const uint64_t value)
    {
        uint64_t observed = target.load(std::memory_order_relaxed);
        while (observed < value
                && !target.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
        }
    }

    void RecordPresentationSurfaceWait(const std::chrono::steady_clock::time_point start)
    {
        const auto us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
        presentationSurfaceWaitUs.fetch_add(us, std::memory_order_relaxed);
        presentationSurfaceWaitCount.fetch_add(1, std::memory_order_relaxed);
        UpdateMax(presentationSurfaceWaitMaxUs, us);
    }

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
        frame.contentWidth = static_cast<UINT>(contentSize.cx);
        frame.contentHeight = static_cast<UINT>(contentSize.cy);
        frame.presenterGeneration = presenterGeneration;
        frame.resetSerial = resetSerial.load(std::memory_order_acquire);
        frame.settings = settings;
        frame.displayRate = displayRate;
        frame.maxMultiplierMilli = maxMultiplierMilli;
        frame.maxOutputFpsMilli = maxOutputFpsMilli;
        configuredInferenceContexts.store(static_cast<uint32_t>(std::clamp(
            settings.iRifeGpuThreads, RIFE_GPU_THREADS_MIN, RIFE_GPU_THREADS_MAX)),
            std::memory_order_relaxed);
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
        const auto waitStartPrecise = std::chrono::steady_clock::now();
        bool waitedForSurface = false;
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
                if (waitedForSurface) {
                    RecordPresentationSurfaceWait(waitStartPrecise);
                }
                if (owner->QueueFrameInterpolationSource(
                        handle, time, synthetic, frame.presenterGeneration)) {
                    return true;
                }
                frame.processor->ReleaseFrameInterpolationSource(handle);
                return false;
            }
            waitedForSurface = true;

            const bool waitExpired = GetTickCount64() - waitStart >= kPresentationCapacityWaitMs;
            if (synthetic && waitExpired) {
                RecordPresentationSurfaceWait(waitStartPrecise);
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
                RecordPresentationSurfaceWait(waitStartPrecise);
                presentationDrops.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            Sleep(1);
        }
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
        key.contentWidth = frame.contentWidth;
        key.contentHeight = frame.contentHeight;
        key.gpu = frame.settings.iRifeGPU;
        key.contexts = std::clamp(frame.settings.iRifeGpuThreads, RIFE_GPU_THREADS_MIN, RIFE_GPU_THREADS_MAX);
        key.performanceBoost = frame.settings.bRifePerformanceBoost;

        const bool sameLiveGeometry = runtimeKey
            && runtimeKey->device == key.device
            && runtimeKey->width == key.width
            && runtimeKey->height == key.height;
        if (sameLiveGeometry && !(*runtimeKey == key)) {
            // CUDA/D3D11 registrations belong to a specific runtime instance.
            // Retaining two runtimes for the same live texture geometry can
            // leave the old instance holding registrations that make the new
            // instance's first Interpolate() fail. This is most visible when
            // Performance Boost is toggled during playback because the source
            // and presentation textures themselves do not change.
            //
            // TensorRT plans remain cached on disk, so discarding same-shape
            // runtime objects here preserves the expensive engine cache while
            // guaranteeing that only one runtime can own registrations for the
            // current D3D11 texture set. A normal video-size change must keep
            // the destination geometry cached so switching back to a clip can
            // immediately reuse its already-ready runtime.
            runtimeBuild.reset();
            for (auto it = runtimeCache.begin(); it != runtimeCache.end();) {
                if (it->first.device == key.device
                        && it->first.width == key.width
                        && it->first.height == key.height) {
                    it = runtimeCache.erase(it);
                } else {
                    ++it;
                }
            }
        }

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
                if (cached->done.load(std::memory_order_acquire)
                        && cached->success.load(std::memory_order_acquire)) {
                    tensorIoLinearValidated.store(true, std::memory_order_relaxed);
                }
                return;
            }

            // A transient initialization failure must not leave this key stuck
            // in runtime-wait forever. Drop the failed entry after a short
            // cooldown and let the normal build path retry it.
            runtimeCache.erase(it);
            break;
        }

        runtimeKey = key;
        tensorIoLinearValidated.store(false, std::memory_order_relaxed);

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
                L"", device, width, height, key.contentWidth, key.contentHeight, gpuIndex,
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

    bool DetectImageSceneCut(ImageCutDetector& detector, const SourceFrame& first, const SourceFrame& second)
    {
        ID3D11Device* device = second.processor ? second.processor->GetRifeDevice() : nullptr;
        if (!device) {
            return false;
        }

        bool cut = false;
        return detector.Analyze(device, first.texture, second.texture, cut) && cut;
    }

    bool PrepareInferenceInputs(
        InferenceWorkerState& workerState,
        ID3D11Device* device,
        ID3D11Texture2D* first,
        ID3D11Texture2D* second,
        const UINT width,
        const UINT height)
    {
        if (!device || !first || !second || !width || !height) {
            return false;
        }

        const bool recreate = !SameTextureShape(workerState.inferenceFirst, device, width, height)
            || !SameTextureShape(workerState.inferenceSecond, device, width, height);
        if (recreate) {
            workerState.inferenceFirst.Release();
            workerState.inferenceSecond.Release();
            workerState.inferenceOutputs.clear();
            if (FAILED(CreateBgraTexture(device, width, height, &workerState.inferenceFirst))
                    || FAILED(CreateBgraTexture(device, width, height, &workerState.inferenceSecond))) {
                workerState.inferenceFirst.Release();
                workerState.inferenceSecond.Release();
                return false;
            }
        }

        CComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        if (!context) {
            return false;
        }
        CComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&multithread))) && multithread) {
            multithread->SetMultithreadProtected(TRUE);
        }

        // Source-pool textures are shared by adjacent jobs (A/B, B/C). Copy
        // them into context-owned textures before CUDA interop so no worker
        // maps a source texture while another worker's scene detector reads it.
        context->CopyResource(workerState.inferenceFirst, first);
        context->CopyResource(workerState.inferenceSecond, second);
        return true;
    }

    ID3D11Texture2D* AcquireInferenceOutput(
        InferenceWorkerState& workerState,
        ID3D11Device* device,
        const UINT width,
        const UINT height,
        const size_t index)
    {
        if (!device || !width || !height) {
            return nullptr;
        }
        if (workerState.inferenceOutputs.size() <= index) {
            workerState.inferenceOutputs.resize(index + 1);
        }
        auto& output = workerState.inferenceOutputs[index];
        if (!SameTextureShape(output, device, width, height)) {
            output.Release();
            if (FAILED(CreateBgraTexture(device, width, height, &output))) {
                return nullptr;
            }
        }
        return output;
    }

    bool GenerateRife(
        const std::shared_ptr<CRifeFrameInterpolation>& runtime,
        const uint32_t contextIndex,
        ID3D11Texture2D* first,
        ID3D11Texture2D* second,
        float timestep,
        ID3D11Texture2D* output)
    {
        if (!runtime || !first || !second || !output) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        second->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC outputDesc = {};
        output->GetDesc(&outputDesc);
        if (outputDesc.Width != desc.Width
                || outputDesc.Height != desc.Height
                || outputDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                || outputDesc.SampleDesc.Count != 1) {
            return false;
        }

        MpcvrRifeStats stats = {};
        const uint32_t active = activeInferences.fetch_add(1, std::memory_order_acq_rel) + 1;
        uint32_t observedMax = maxConcurrentInferences.load(std::memory_order_relaxed);
        while (observedMax < active
                && !maxConcurrentInferences.compare_exchange_weak(
                    observedMax, active, std::memory_order_relaxed)) {
        }
        const auto runtimeStart = std::chrono::steady_clock::now();
        const bool ok = runtime->Interpolate(contextIndex, first, second,
            output, timestep, stats);
        const auto runtimeUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - runtimeStart).count());
        lastRuntimeWallUs.store(runtimeUs, std::memory_order_relaxed);
        lastInputMapUs.store(MsToUs(stats.inputMapMs), std::memory_order_relaxed);
        lastInputPackUs.store(MsToUs(stats.inputPackMs), std::memory_order_relaxed);
        lastInputUnmapUs.store(MsToUs(stats.inputUnmapMs), std::memory_order_relaxed);
        lastOutputMapUs.store(MsToUs(stats.outputMapMs), std::memory_order_relaxed);
        lastTensorRtUs.store(MsToUs(stats.tensorRtMs), std::memory_order_relaxed);
        lastOutputWriteUs.store(MsToUs(stats.outputWriteMs), std::memory_order_relaxed);
        lastOutputUnmapUs.store(MsToUs(stats.outputUnmapMs), std::memory_order_relaxed);
        lastContextLockWaitUs.store(MsToUs(stats.contextLockWaitMs), std::memory_order_relaxed);
        lastCudaSetDeviceUs.store(MsToUs(stats.cudaSetDeviceMs), std::memory_order_relaxed);
        lastRegistrationUs.store(MsToUs(stats.registrationMs), std::memory_order_relaxed);
        lastInputPackLockWaitUs.store(MsToUs(stats.inputPackLockWaitMs), std::memory_order_relaxed);
        lastRuntimeInternalUs.store(MsToUs(stats.totalRuntimeMs), std::memory_order_relaxed);
        activeInferences.fetch_sub(1, std::memory_order_acq_rel);
        if (!ok) {
            return false;
        }
        lastInferenceUs.store(MsToUs(stats.inferenceMs), std::memory_order_relaxed);
        return true;
    }

    void ProcessPairJob(InferenceWorkerState& workerState, PairJob& job)
    {
        if (!job.first || !job.second || !job.runtime || !IsCurrent(*job.second)) {
            return;
        }
        auto& first = *job.first;
        auto& second = *job.second;
        ID3D11Device* device = second.processor ? second.processor->GetRifeDevice() : nullptr;
        if (!device || !job.width || !job.height) {
            return;
        }
        const auto inputCopyStart = std::chrono::steady_clock::now();
        const bool inputsReady = PrepareInferenceInputs(workerState, device, first.texture, second.texture,
            job.width, job.height);
        lastInputCopySubmitUs.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - inputCopyStart).count()), std::memory_order_relaxed);
        if (!inputsReady) {
            for (const auto& target : job.targets) {
                TargetResult result;
                result.target = target;
                result.inferenceFailed = !target.exactSource;
                if (result.inferenceFailed) {
                    inferenceFallbackFrames.fetch_add(1, std::memory_order_relaxed);
                }
                job.results.push_back(std::move(result));
            }
            return;
        }

        bool hasTimelySyntheticTarget = false;
        for (const auto& target : job.targets) {
            if (!target.exactSource && !IsLate(second, target.presentationTime)) {
                hasTimelySyntheticTarget = true;
                break;
            }
        }

        bool sceneDecisionReady = second.settings.iRifeSceneDetection != RIFE_SCENE_NVOF;
        bool sceneCut = hasTimelySyntheticTarget
            && second.settings.iRifeSceneDetection == RIFE_SCENE_Image
            && DetectImageSceneCut(workerState.imageDetector, first, second);

        job.results.reserve(job.targets.size());
        size_t outputIndex = 0;
        for (const auto& target : job.targets) {
            TargetResult result;
            result.target = target;

            if (target.exactSource) {
                job.results.push_back(std::move(result));
                continue;
            }
            if (!IsCurrent(second) || IsLate(second, target.presentationTime)) {
                result.skippedLate = true;
                lateSyntheticDrops.fetch_add(1, std::memory_order_relaxed);
                job.results.push_back(std::move(result));
                continue;
            }
            if (sceneDecisionReady && sceneCut) {
                job.results.push_back(std::move(result));
                continue;
            }

            ID3D11Texture2D* generated = AcquireInferenceOutput(
                workerState, device, job.width, job.height, outputIndex++);
            if (!generated) {
                result.inferenceFailed = true;
                inferenceFallbackFrames.fetch_add(1, std::memory_order_relaxed);
                job.results.push_back(std::move(result));
                continue;
            }

            bool nvofStarted = false;
            if (!sceneDecisionReady) {
                const bool nvofReady = workerState.nvofDetector.Initialize(device, job.width, job.height);
                nvofStarted = nvofReady
                    && workerState.nvofDetector.BeginAnalyze(first.texture, second.texture);
                if (!nvofStarted) {
                    sceneCut = DetectImageSceneCut(workerState.imageDetector, first, second);
                    sceneDecisionReady = true;
                    if (sceneCut) {
                        job.results.push_back(std::move(result));
                        continue;
                    }
                }
            }

            const bool generatedOk = GenerateRife(job.runtime, job.contextIndex,
                workerState.inferenceFirst, workerState.inferenceSecond,
                static_cast<float>(target.timestep), generated);

            if (nvofStarted) {
                CNvidiaSceneChangeDetector::Metrics metrics;
                if (workerState.nvofDetector.FinishAnalyze(metrics) && metrics.valid) {
                    sceneCut = metrics.likelyCut;
                } else {
                    sceneCut = DetectImageSceneCut(workerState.imageDetector, first, second);
                }
                sceneDecisionReady = true;
                if (sceneCut) {
                    job.results.push_back(std::move(result));
                    continue;
                }
            }

            if (generatedOk) {
                result.generated = generated;
            } else {
                result.inferenceFailed = true;
                inferenceFallbackFrames.fetch_add(1, std::memory_order_relaxed);
            }
            job.results.push_back(std::move(result));
        }
        job.sceneCut = sceneCut;
    }

    void InferenceWorkerMain(InferenceWorkerState* state)
    {
        if (!state) return;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        for (;;) {
            std::shared_ptr<PairJob> job;
            {
                std::unique_lock lock(state->mutex);
                state->cv.wait(lock, [&] { return state->stop || !state->queue.empty(); });
                if (state->stop && state->queue.empty()) {
                    break;
                }
                job = std::move(state->queue.front());
                state->queue.pop_front();
            }
            if (job) {
                ProcessPairJob(*state, *job);
                job->done.store(true, std::memory_order_release);
                cv.notify_all();
            }
        }
    }

    void DispatchPairJob(const std::shared_ptr<PairJob>& job)
    {
        if (!job || job->contextIndex >= inferenceWorkers.size() || !inferenceWorkers[job->contextIndex]) {
            if (job) job->done.store(true, std::memory_order_release);
            cv.notify_all();
            return;
        }
        auto& state = *inferenceWorkers[job->contextIndex];
        {
            std::lock_guard lock(state.mutex);
            state.queue.push_back(job);
        }
        state.cv.notify_one();
    }

    bool PresentPairJob(PairJob& job)
    {
        if (!job.first || !job.second || !IsCurrent(*job.second)) {
            return false;
        }
        auto& first = *job.first;
        auto& second = *job.second;
        ID3D11Device* device = second.processor ? second.processor->GetRifeDevice() : nullptr;
        bool queuedOutput = false;

        const auto queueSceneCutTarget = [&](const FrameInterpolationTarget& target) {
            if (second.settings.iRifeSceneProcessing == RIFE_SCENE_PROCESS_Blend
                    && device && job.width && job.height
                    && EnsureOutputTexture(device, job.width, job.height)
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

        for (auto& result : job.results) {
            const auto& target = result.target;
            if (!IsCurrent(second)) {
                return queuedOutput;
            }
            if (target.exactSource) {
                queuedOutput |= QueueTexture(second, second.texture, target.presentationTime, false);
                continue;
            }
            if (result.skippedLate) {
                continue;
            }
            if (job.sceneCut) {
                queuedOutput |= queueSceneCutTarget(target);
                continue;
            }
            if (result.generated) {
                generatedFrames.fetch_add(1, std::memory_order_relaxed);
                queuedOutput |= QueueTexture(
                    second, result.generated, target.presentationTime, true, false);
            } else {
                ID3D11Texture2D* fallback = target.timestep < 0.5
                    ? first.texture.p : second.texture.p;
                queuedOutput |= QueueTexture(second, fallback, target.presentationTime, true);
            }
        }
        if (!job.targets.empty() && !queuedOutput && IsCurrent(second)) {
            if (QueueTexture(second, second.texture, second.time, false)) {
                sourceContinuityFrames.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return queuedOutput;
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
        diagnostics += std::format(L"\nRIFE parallel: active {}, max {}, contexts {}, TensorRT I/O {}",
            activeInferences.load(std::memory_order_relaxed),
            maxConcurrentInferences.load(std::memory_order_relaxed),
            configuredInferenceContexts.load(std::memory_order_relaxed),
            tensorIoLinearValidated.load(std::memory_order_relaxed) ? L"LINEAR" : L"pending");
        const auto gpuUs = lastInferenceUs.load(std::memory_order_relaxed);
        const auto packUs = lastInputPackUs.load(std::memory_order_relaxed);
        const auto trtUs = lastTensorRtUs.load(std::memory_order_relaxed);
        const auto writeUs = lastOutputWriteUs.load(std::memory_order_relaxed);
        const auto knownGpuUs = packUs + trtUs + writeUs;
        const auto gpuGapUs = gpuUs > knownGpuUs ? gpuUs - knownGpuUs : 0;
        diagnostics += std::format(
            L"\nRIFE timing  : wall {:.2f} ms, copy-submit {:.2f}, GPU {:.2f} [pack {:.2f}, TRT {:.2f}, write {:.2f}, gap {:.2f}]",
            lastRuntimeWallUs.load(std::memory_order_relaxed) / 1000.0,
            lastInputCopySubmitUs.load(std::memory_order_relaxed) / 1000.0,
            gpuUs / 1000.0, packUs / 1000.0, trtUs / 1000.0, writeUs / 1000.0, gpuGapUs / 1000.0);
        const auto surfaceWaitCount = presentationSurfaceWaitCount.load(std::memory_order_relaxed);
        const auto surfaceWaitUs = presentationSurfaceWaitUs.load(std::memory_order_relaxed);
        diagnostics += std::format(
            L"\nRIFE interop : in map {:.2f}/unmap {:.2f} ms, out map {:.2f}/unmap {:.2f}, surface-wait avg {:.2f}/max {:.2f} ms ({})",
            lastInputMapUs.load(std::memory_order_relaxed) / 1000.0,
            lastInputUnmapUs.load(std::memory_order_relaxed) / 1000.0,
            lastOutputMapUs.load(std::memory_order_relaxed) / 1000.0,
            lastOutputUnmapUs.load(std::memory_order_relaxed) / 1000.0,
            surfaceWaitCount ? (surfaceWaitUs / static_cast<double>(surfaceWaitCount)) / 1000.0 : 0.0,
            presentationSurfaceWaitMaxUs.load(std::memory_order_relaxed) / 1000.0,
            surfaceWaitCount);
        diagnostics += std::format(
            L"\nRIFE host    : internal {:.2f} ms, ctx-lock {:.2f}, set-device {:.2f}, register {:.2f}, pack-lock {:.2f}",
            lastRuntimeInternalUs.load(std::memory_order_relaxed) / 1000.0,
            lastContextLockWaitUs.load(std::memory_order_relaxed) / 1000.0,
            lastCudaSetDeviceUs.load(std::memory_order_relaxed) / 1000.0,
            lastRegistrationUs.load(std::memory_order_relaxed) / 1000.0,
            lastInputPackLockWaitUs.load(std::memory_order_relaxed) / 1000.0);
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

    SourceFramePtr AdoptFrame(SourceFrame&& frame)
    {
        return SourceFramePtr(new SourceFrame(std::move(frame)), [this](SourceFrame* owned) {
            if (!owned) return;
            ReleaseFrame(*owned);
            delete owned;
        });
    }

    void WorkerMain()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        SourceFramePtr previous;
        std::deque<std::shared_ptr<PairJob>> pendingPairs;
        uint64_t activeSerial = resetSerial.load(std::memory_order_acquire);
        uint64_t nextSequence = 1;
        uint32_t nextWorker = 0;

        const auto presentCompletedFront = [&]() -> bool {
            if (pendingPairs.empty()
                    || !pendingPairs.front()->done.load(std::memory_order_acquire)) {
                return false;
            }
            auto job = std::move(pendingPairs.front());
            pendingPairs.pop_front();
            if (!stop.load(std::memory_order_acquire) && job && job->second && IsCurrent(*job->second)) {
                PresentPairJob(*job);
            }
            return true;
        };

        const auto waitForFront = [&](const bool present) {
            if (pendingPairs.empty()) return;
            auto job = pendingPairs.front();
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] {
                    return job->done.load(std::memory_order_acquire);
                });
            }
            pendingPairs.pop_front();
            if (present && !stop.load(std::memory_order_acquire)
                    && job->second && IsCurrent(*job->second)) {
                PresentPairJob(*job);
            }
        };

        const auto drainPending = [&](const bool present) {
            while (!pendingPairs.empty()) {
                waitForFront(present);
            }
        };

        while (!stop.load(std::memory_order_acquire)) {
            while (presentCompletedFront()) {
            }

            const uint64_t currentSerial = resetSerial.load(std::memory_order_acquire);
            if (currentSerial != activeSerial) {
                drainPending(false);
                previous.reset();
                activeSerial = currentSerial;
                nextWorker = 0;
                ResetSequenceState();
                continue;
            }

            const uint32_t configuredContexts = previous
                ? static_cast<uint32_t>(std::clamp(previous->settings.iRifeGpuThreads,
                    RIFE_GPU_THREADS_MIN, RIFE_GPU_THREADS_MAX))
                : static_cast<uint32_t>(RIFE_GPU_THREADS_MAX);
            const bool canConsumeSource = !previous || pendingPairs.size() < configuredContexts;
            SourceFrame current;
            bool haveCurrent = false;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] {
                    return stop.load(std::memory_order_acquire)
                        || resetSerial.load(std::memory_order_acquire) != activeSerial
                        || (!pendingPairs.empty()
                            && pendingPairs.front()->done.load(std::memory_order_acquire))
                        || (canConsumeSource && !queue.empty());
                });
                if (stop.load(std::memory_order_acquire)) {
                    break;
                }
                if (resetSerial.load(std::memory_order_acquire) != activeSerial
                        || (!pendingPairs.empty()
                            && pendingPairs.front()->done.load(std::memory_order_acquire))) {
                    continue;
                }
                if (canConsumeSource && !queue.empty()) {
                    current = std::move(queue.front());
                    queue.pop_front();
                    haveCurrent = true;
                }
            }
            if (!haveCurrent) continue;

            if (!IsCurrent(current)) {
                ReleaseFrame(current);
                continue;
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

            auto currentFrame = AdoptFrame(std::move(current));
            if (!previous) {
                // The scheduler anchors its target grid at this real frame.
                ConfigureScheduler(*currentFrame);
                QueueTexture(*currentFrame, currentFrame->texture, currentFrame->time, false);
                previous = std::move(currentFrame);
                continue;
            }

            if (!RifeFramesCompatible(*previous, *currentFrame)) {
                drainPending(true);
                sourceResyncs.fetch_add(1, std::memory_order_relaxed);
                ResetSequenceState();
                ConfigureScheduler(*currentFrame);
                QueueTexture(*currentFrame, currentFrame->texture, currentFrame->time, false);
                previous = std::move(currentFrame);
                continue;
            }

            ConfigureScheduler(*currentFrame);
            const FrameRate sourceRate = SourceRateFromDuration(
                currentFrame->frameDuration > 0
                    ? currentFrame->frameDuration : currentFrame->time - previous->time);
            const auto targets = scheduler.Schedule(previous->time, currentFrame->time, sourceRate);

            ID3D11Device* device = currentFrame->processor
                ? currentFrame->processor->GetRifeDevice() : nullptr;
            D3D11_TEXTURE2D_DESC desc = {};
            if (currentFrame->texture) currentFrame->texture->GetDesc(&desc);
            if (!device || !desc.Width || !desc.Height) {
                drainPending(true);
                QueueTexture(*currentFrame, currentFrame->texture, currentFrame->time, false);
                previous = std::move(currentFrame);
                continue;
            }

            const int contextCount = std::clamp(currentFrame->settings.iRifeGpuThreads,
                RIFE_GPU_THREADS_MIN, RIFE_GPU_THREADS_MAX);
            const bool runtimeSettingsChanged = runtimeKey
                && (runtimeKey->device != device
                    || runtimeKey->width != desc.Width
                    || runtimeKey->height != desc.Height
                    || runtimeKey->contentWidth != currentFrame->contentWidth
                    || runtimeKey->contentHeight != currentFrame->contentHeight
                    || runtimeKey->gpu != currentFrame->settings.iRifeGPU
                    || runtimeKey->contexts != contextCount
                    || runtimeKey->performanceBoost != currentFrame->settings.bRifePerformanceBoost);
            if (runtimeSettingsChanged) {
                // Retire all work using the previous runtime before switching a
                // setting that can create another registration cache for the
                // same D3D11 source pool.
                drainPending(true);
                nextWorker = 0;
            }

            EnsureRuntimeBuild(*currentFrame, device, desc.Width, desc.Height);
            auto runtime = ReadyRuntime();
            if (!runtime) {
                drainPending(true);
                runtimeWaitPairs.fetch_add(1, std::memory_order_relaxed);
                QueueTexture(*currentFrame, currentFrame->texture, currentFrame->time, false);
                previous = std::move(currentFrame);
                continue;
            }
            tensorIoLinearValidated.store(true, std::memory_order_relaxed);

            while (pendingPairs.size() >= static_cast<size_t>(contextCount)) {
                waitForFront(true);
            }

            auto job = std::make_shared<PairJob>();
            job->sequence = nextSequence++;
            job->contextIndex = nextWorker++ % static_cast<uint32_t>(contextCount);
            job->first = previous;
            job->second = currentFrame;
            job->runtime = std::move(runtime);
            job->targets = targets;
            job->width = desc.Width;
            job->height = desc.Height;
            pendingPairs.push_back(job);
            DispatchPairJob(job);
            previous = std::move(currentFrame);
        }

        drainPending(false);
        previous.reset();
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
