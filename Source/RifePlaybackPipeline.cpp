#include "stdafx.h"

#include "RifePlaybackPipeline.h"
#include "RifePrecision.h"

#include "DX11VideoProcessor.h"
#include "NvidiaSceneChangeDetector.h"
#include "RifeFrameInterpolation.h"
#include "RifeInferenceTiming.h"
#include "RifeImageSceneDetector.h"
#include "RifeRuntimeCachePolicy.h"
#include "RifeSceneBlender.h"
#include "SvpSceneDetector.h"
#include "RollingTimingWindow.h"
#include "Shaders.h"
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
#include <limits>
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
// Never use texture addresses or source timestamps as content generations.
// Pool reuse, seeking and repeat playback can reproduce both.
std::atomic_uint64_t nextInputPairId = 1;

std::wstring FormatPressureReasons(const uint32_t reasons)
{
    std::wstring text;
    const auto append = [&](const wchar_t* reason) {
        if (!text.empty()) text += L"+";
        text += reason;
    };
    if (reasons & RIFE_PRESSURE_SOURCE_POOL_MISS) append(L"pool");
    if (reasons & RIFE_PRESSURE_LATE_SYNTHETIC_DROP) append(L"late");
    if (reasons & RIFE_PRESSURE_PRESENTATION_DROP) append(L"present-drop");
    if (reasons & RIFE_PRESSURE_PRESENTATION_RECLAIM) append(L"reclaim");
    if (reasons & RIFE_PRESSURE_BACKLOG) append(L"backlog");
    if (reasons & RIFE_PRESSURE_PRESENTATION_SURFACE_WAIT) append(L"surface-wait");
    if (reasons & RIFE_PRESSURE_PRESENTER_STALE_DROP) append(L"stale-drop");
    if (reasons & RIFE_PRESSURE_DELIVERY_SHORTFALL) append(L"output-shortfall");
    return text.empty() ? std::wstring(L"none") : text;
}

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

REFERENCE_TIME DurationFromSourceRate(const FrameRate rate)
{
    if (!rate.IsValid()) {
        return 0;
    }
    return static_cast<REFERENCE_TIME>((10'000'000ULL * rate.denominator
        + rate.numerator / 2) / rate.numerator);
}

uint32_t FpsMilli(const FrameRate rate)
{
    return rate.IsValid() ? static_cast<uint32_t>(
        (static_cast<uint64_t>(rate.numerator) * 1000 + rate.denominator / 2)
        / rate.denominator) : 0;
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

const wchar_t* RifeModelFileName(const int model) noexcept
{
    switch (model) {
    case RIFE_MODEL_44:       return L"rife_v4.4.onnx";
    case RIFE_MODEL_415_LITE: return L"rife_v4.15_lite.onnx";
    case RIFE_MODEL_425:      return L"rife_v4.25.onnx";
    case RIFE_MODEL_425_LITE: return L"rife_v4.25_lite.onnx";
    case RIFE_MODEL_46:
    default:                  return L"rife_v4.6.onnx";
    }
}

const wchar_t* RifeModelDisplayName(const int model) noexcept
{
    switch (model) {
    case RIFE_MODEL_44:       return L"4.4";
    case RIFE_MODEL_415_LITE: return L"4.15 Lite";
    case RIFE_MODEL_425:      return L"4.25";
    case RIFE_MODEL_425_LITE: return L"4.25 Lite";
    case RIFE_MODEL_46:
    default:                  return L"4.6";
    }
}

UINT RifeModelAlignment(const int model) noexcept
{
    return model == RIFE_MODEL_425_LITE ? 128u : 32u;
}

DXGI_FORMAT TextureFormat(ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    if (texture) texture->GetDesc(&desc);
    return desc.Format;
}

bool SameTextureShape(ID3D11Texture2D* texture, ID3D11Device* device, UINT width, UINT height,
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM)
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
        && desc.Format == format
        && desc.SampleDesc.Count == 1;
}

HRESULT CreateBgraTexture(ID3D11Device* device, UINT width, UINT height, ID3D11Texture2D** texture,
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM)
{
    if (!device || !width || !height || !texture) {
        return E_INVALIDARG;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
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
    int model = RIFE_MODEL_425;
    bool performanceBoost = false;
    bool featureReuse = false;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;

    bool operator==(const RuntimeKey& other) const noexcept
    {
        return device == other.device && width == other.width && height == other.height
            && contentWidth == other.contentWidth && contentHeight == other.contentHeight
            && gpu == other.gpu && contexts == other.contexts && model == other.model
            && performanceBoost == other.performanceBoost && featureReuse == other.featureReuse
            && format == other.format;
    }
};


} // namespace

struct CRifePlaybackPipeline::Impl
{
    struct SourceSlot {
        CComPtr<ID3D11Texture2D> texture;
        CComPtr<ID3D11Texture2D> inferenceAsFirst;
        CComPtr<ID3D11Texture2D> inferenceAsSecond;
        CComPtr<ID3D11Query> inputCopyAsFirstReadyQuery;
        CComPtr<ID3D11Query> inputCopyAsSecondReadyQuery;
        bool inUse = false;
    };

    struct SourceFrame {
        size_t slot = SIZE_MAX;
        CComPtr<ID3D11Texture2D> texture;
        CComPtr<ID3D11Texture2D> inferenceAsFirst;
        CComPtr<ID3D11Texture2D> inferenceAsSecond;
        CComPtr<ID3D11Query> inputCopyAsFirstReadyQuery;
        CComPtr<ID3D11Query> inputCopyAsSecondReadyQuery;
        CDX11VideoProcessor* processor = nullptr;
        REFERENCE_TIME time = INVALID_TIME;
        REFERENCE_TIME frameDuration = 0;
        bool variableTiming = false;
        UINT contentWidth = 0;
        UINT contentHeight = 0;
        bool cudaOutputEligible = false;
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
        std::shared_ptr<RifeCudaOutputLease> cudaOutput;
        bool inferenceFailed = false;
        bool skippedLate = false;
        bool sceneCut = false;
    };

    struct PairJob {
        uint64_t sequence = 0;
        uint64_t inputPairId = nextInputPairId.fetch_add(1, std::memory_order_relaxed);
        uint32_t contextIndex = 0;
        SourceFramePtr first;
        SourceFramePtr second;
        std::shared_ptr<CRifeFrameInterpolation> runtime;
        std::vector<FrameInterpolationTarget> targets;
        std::vector<TargetResult> results;
        std::atomic_size_t readyResults = 0;
        size_t presentedResults = 0;
        bool queuedOutput = false;
        bool presentationFinalized = false;
        UINT width = 0;
        UINT height = 0;
        std::atomic_bool done = false;
    };

    struct InferenceWorkerState {
        uint32_t index = 0;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::shared_ptr<PairJob>> queue;
        std::thread thread;
        bool stop = false;
        CRifeImageSceneDetector imageDetector{CompileShader};
        CSvpSceneDetector svpDetector{CompileShader};
        std::vector<CComPtr<ID3D11Texture2D>> inferenceOutputs;
        SourceFramePtr retainedInputFirst;
        SourceFramePtr retainedInputSecond;
        std::shared_ptr<CRifeFrameInterpolation> retainedInputRuntime;
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
            && first.contentWidth == second.contentWidth
            && first.contentHeight == second.contentHeight
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
        DrainAllRetainedInputs();
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
    std::atomic_bool preserveAdaptiveForResume = false;
    // A hard reset must survive any later seek/flush before the worker handles it.
    std::atomic_bool hardResetPending = false;
    uint64_t lastSubmittedGeneration = UINT64_MAX;
    CSize lastSubmittedContentSize = CSize(0, 0); // Submit holds the renderer lock.
    std::mutex sourceRateMutex;
    CFrameInterpolationSourceRateStabilizer sourceRateStabilizer;

    CFrameInterpolationScheduler scheduler;
    bool schedulerConfigured = false;
    int schedulerMode = RIFE_MODE_Disabled;
    int schedulerCustomFps = 0;
    FrameRate schedulerDisplayRate = {};
    uint64_t schedulerSerial = 0;
    uint32_t schedulerMaxMultiplierMilli = 0;
    uint32_t schedulerMaxOutputFpsMilli = 0;
    bool schedulerPerformanceBoost = false;
    bool schedulerVariableTiming = false;
    uint32_t schedulerSourceFrameStride = 1;
    CRifeInferenceTimingWindow<16> adaptiveInferenceTiming;
    CFrameInterpolationPressureController adaptivePressureController;
    uint32_t adaptiveAppliedCapFpsMilli = 0;
    bool adaptiveResumePending = false;
    std::atomic_uint32_t adaptiveRequestedOutputFpsMilli = 0;
    std::atomic_uint32_t adaptiveSyntheticCapacityFpsMilli = 0;
    std::atomic_uint32_t adaptiveMeasuredParallelismPermille = 0;
    std::atomic_bool adaptiveInterpolationReady = false;
    std::atomic_uint32_t adaptiveOutputCapFpsMilli = 0;
    std::atomic_uint32_t adaptiveLoadSamples = 0;
    std::atomic_uint32_t adaptiveHeadroomPermille = 970;
    std::atomic_bool adaptivePressureDetected = false;
    std::atomic_uint32_t adaptivePressurePhase =
        static_cast<uint32_t>(FrameInterpolationPressurePhase::Open);
    std::atomic_uint32_t adaptivePressureGoodFpsMilli = 0;
    std::atomic_uint32_t adaptivePressureBadFpsMilli = 0;
    std::atomic_uint32_t adaptivePressureBackoffFpsMilli = 0;
    std::atomic_uint32_t adaptiveMeasuredOutputFpsMilli = 0;
    std::atomic_bool adaptiveMeasuredOutputReady = false;
    std::atomic_bool adaptiveMeasuredOutputHealthy = false;
    std::atomic_bool adaptiveRecoveryWaitingForHeadroom = false;
    std::atomic_uint32_t adaptivePressureObservedReasons = RIFE_PRESSURE_NONE;
    std::atomic_uint32_t adaptivePressureConfirmedReasons = RIFE_PRESSURE_NONE;
    std::atomic_uint32_t adaptiveLastPressureCapFpsMilli = 0;
    std::atomic_uint32_t adaptiveLastPressureReasons = RIFE_PRESSURE_NONE;
    std::atomic_uint32_t adaptiveSourceQueueDepth = 0;
    std::atomic_uint32_t adaptivePendingPairDepth = 0;
    std::atomic_uint64_t sourcePoolMisses = 0;

    // The NVIDIA Optical Flow engine is a device-wide resource. Running one
    // D3D11 NVOF session per parallel TensorRT worker can make two workers
    // execute/read back OFA work concurrently on the same immediate context,
    // which has caused long driver stalls on the live Turing path. Keep one
    // detector/session and serialize NVOF itself while leaving TensorRT jobs
    // free to run in parallel.
    std::mutex nvofMutex;
    CNvidiaSceneChangeDetector nvofDetector;
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
    std::atomic_uint64_t sceneBlendFrames = 0;
    std::atomic_uint64_t alternateSkippedFrames = 0;
    std::atomic_uint32_t sourceFpsMilli = 0;
    std::atomic_uint32_t retainedFpsMilli = 0;
    std::atomic_bool variableTimingDetected = false;
    std::atomic_uint64_t inferenceFallbackFrames = 0;
    std::atomic_uint64_t runtimeWaitPairs = 0;
    std::atomic_uint64_t lateSyntheticDrops = 0;
    std::atomic_uint64_t sourceContinuityFrames = 0;
    std::atomic_uint64_t presentationDrops = 0;
    std::atomic_uint64_t presentationReclaims = 0;
    std::atomic_uint64_t sourceResyncs = 0;
    // Publish/read a complete inference sample; workers finish independently.
    mutable std::mutex timingMutex;
    MpcvrRifeStats lastHostStats{};
    uint32_t lastTimingContext = 0;
    std::atomic_uint64_t lastInferenceUs = 0;
    std::atomic_uint64_t lastInputCopySubmitUs = 0;
    std::atomic_uint64_t lastRuntimeWallUs = 0;
    std::atomic_uint64_t lastInputMapUs = 0;
    std::atomic_uint64_t lastInputPackUs = 0;
    std::atomic_uint64_t inputPairPacks = 0;
    std::atomic_uint64_t inputPairReuses = 0;
    std::atomic_bool inputPairReuseAvailable = false;
    std::atomic_bool encoderFeaturesActive = false;
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
    std::atomic_uint64_t lastTensorRtSubmitUs = 0;
    std::atomic_bool tensorRtGraphUsed = false;
    CRollingTimingWindow<256> rollingWallTiming;
    CRollingTimingWindow<256> rollingInternalTiming;
    CRollingTimingWindow<256> rollingGpuTiming;
    CRollingTimingWindow<256> rollingTensorRtTiming;
    CRollingTimingWindow<256> rollingHandoffStartWaitTiming;
    CRollingTimingWindow<256> rollingHandoffEndWaitTiming;
    CRollingTimingWindow<256> rollingHandoffPreMapWaitTiming;
    CRollingTimingWindow<256> rollingHandoffMapWaitTiming;
    CRollingTimingWindow<256> rollingHandoffStartWaitCopiesReadyTiming;
    CRollingTimingWindow<256> rollingHandoffStartWaitCopiesPendingTiming;
    std::atomic_uint64_t inputCopyReadyChecks = 0;
    std::atomic_uint64_t inputCopyReadyAtWorkerStart = 0;
    std::atomic_uint64_t inputCopyFirstReadyAtWorkerStart = 0;
    std::atomic_uint64_t inputCopySecondReadyAtWorkerStart = 0;
    std::atomic_bool lastInputCopyFirstReady = false;
    std::atomic_bool lastInputCopySecondReady = false;
    std::atomic_uint64_t presentationSurfaceWaitUs = 0;
    std::atomic_uint64_t presentationSurfaceWaitCount = 0;
    std::atomic_uint64_t presentationSurfaceWaitMaxUs = 0;
    std::atomic_uint32_t activeInferences = 0;
    std::atomic_uint32_t maxConcurrentInferences = 0;
    std::atomic_uint32_t configuredInferenceContexts = RIFE_GPU_THREADS_DEF;
    std::atomic_bool tensorIoLinearValidated = false;
    enum SceneDiagnosticMode : int {
        SceneDiagDisabled = 0,
        SceneDiagImage,
        SceneDiagNvofOverlap,
        SceneDiagNvofSerialized,
        SceneDiagImageFallback,
        SceneDiagSvp,
        SceneDiagSvpFallback,
    };
    std::atomic_int lastSceneDiagnosticMode = SceneDiagDisabled;
    std::atomic_uint64_t nvofSuccessfulAnalyses = 0;
    std::atomic_uint64_t nvofAnalysisDimensions = 0;
    std::atomic_uint64_t nvofInitFailures = 0;
    std::atomic_uint64_t nvofBeginFailures = 0;
    std::atomic_uint64_t nvofFinishFailures = 0;
    std::atomic_uint64_t nvofInvalidMetrics = 0;
    std::atomic_uint64_t nvofMutexContentions = 0;
    std::atomic_uint64_t nvofCallUs = 0;
    std::atomic_uint64_t nvofCallCount = 0;
    std::atomic_uint64_t nvofMutexWaitUs = 0;
    std::atomic_uint64_t nvofMutexWaitCount = 0;
    std::atomic_uint64_t imageSceneAnalyses = 0;
    std::atomic_bool lastImageCompactReadback = false;
    std::atomic_uint64_t imageSceneFallbacks = 0;
    std::atomic_uint64_t imageSceneUs = 0;
    std::atomic_uint64_t svpSceneUs = 0;
    std::atomic_uint64_t svpSceneAnalyses = 0;
    std::atomic_uint64_t svpSceneFallbacks = 0;
    std::atomic_uint64_t svpAnalysisDimensions = 0;
    std::atomic_uint32_t svpBadCoverageMilli = 0;
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

    std::optional<bool> ProbeInputCopyQueries(const SourceFrame& first, const SourceFrame& second)
    {
        ID3D11Query* firstQuery = first.inputCopyAsFirstReadyQuery;
        ID3D11Query* secondQuery = second.inferenceAsSecond
            ? second.inputCopyAsSecondReadyQuery.p : second.inputCopyAsFirstReadyQuery.p;
        if (!firstQuery || !secondQuery || !second.processor) {
            return std::nullopt;
        }

        ID3D11Device* device = second.processor->GetRifeDevice();
        if (!device) {
            return std::nullopt;
        }
        CComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        if (!context) {
            return std::nullopt;
        }

        const auto queryReady = [&](ID3D11Query* query, const UINT flags) -> HRESULT {
            BOOL complete = FALSE;
            const HRESULT hr = context->GetData(query, &complete, sizeof(complete), flags);
            return hr == S_OK && complete ? S_OK : hr == S_OK ? S_FALSE : hr;
        };

        const bool firstReady = queryReady(firstQuery, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        const bool secondReady = queryReady(secondQuery, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        lastInputCopyFirstReady.store(firstReady, std::memory_order_relaxed);
        lastInputCopySecondReady.store(secondReady, std::memory_order_relaxed);
        inputCopyReadyChecks.fetch_add(1, std::memory_order_relaxed);
        if (firstReady) {
            inputCopyFirstReadyAtWorkerStart.fetch_add(1, std::memory_order_relaxed);
        }
        if (secondReady) {
            inputCopySecondReadyAtWorkerStart.fetch_add(1, std::memory_order_relaxed);
        }
        if (firstReady && secondReady) {
            inputCopyReadyAtWorkerStart.fetch_add(1, std::memory_order_relaxed);
        }
        return firstReady && secondReady;
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

    void ResetNonBlocking(const bool preserveAdaptive = false)
    {
        {
            std::lock_guard lock(sourceRateMutex);
            if (preserveAdaptive) {
                sourceRateStabilizer.ResetSegment();
            } else {
                sourceRateStabilizer.Reset();
            }
        }
        if (!preserveAdaptive) {
            hardResetPending.store(true, std::memory_order_release);
        }
        preserveAdaptiveForResume.store(preserveAdaptive, std::memory_order_release);
        resetSerial.fetch_add(1, std::memory_order_acq_rel);
        ClearQueuedFrames();
        cv.notify_all();
    }

    void ResetAdaptiveLoadState()
    {
        adaptiveInferenceTiming.Clear();
        adaptivePressureController.Reset();
        adaptiveAppliedCapFpsMilli = 0;
        adaptiveRequestedOutputFpsMilli.store(0, std::memory_order_relaxed);
        adaptiveSyntheticCapacityFpsMilli.store(0, std::memory_order_relaxed);
        adaptiveMeasuredParallelismPermille.store(0, std::memory_order_relaxed);
        adaptiveInterpolationReady.store(false, std::memory_order_relaxed);
        adaptiveOutputCapFpsMilli.store(0, std::memory_order_relaxed);
        adaptiveLoadSamples.store(0, std::memory_order_relaxed);
        adaptivePressureDetected.store(false, std::memory_order_relaxed);
        adaptivePressurePhase.store(
            static_cast<uint32_t>(FrameInterpolationPressurePhase::Open), std::memory_order_relaxed);
        adaptivePressureGoodFpsMilli.store(0, std::memory_order_relaxed);
        adaptivePressureBadFpsMilli.store(0, std::memory_order_relaxed);
        adaptivePressureBackoffFpsMilli.store(0, std::memory_order_relaxed);
        adaptiveMeasuredOutputFpsMilli.store(0, std::memory_order_relaxed);
        adaptiveMeasuredOutputReady.store(false, std::memory_order_relaxed);
        adaptiveMeasuredOutputHealthy.store(false, std::memory_order_relaxed);
        adaptiveRecoveryWaitingForHeadroom.store(false, std::memory_order_relaxed);
        adaptivePressureObservedReasons.store(RIFE_PRESSURE_NONE, std::memory_order_relaxed);
        adaptivePressureConfirmedReasons.store(RIFE_PRESSURE_NONE, std::memory_order_relaxed);
        adaptiveLastPressureCapFpsMilli.store(0, std::memory_order_relaxed);
        adaptiveLastPressureReasons.store(RIFE_PRESSURE_NONE, std::memory_order_relaxed);
        adaptiveSourceQueueDepth.store(0, std::memory_order_relaxed);
        adaptivePendingPairDepth.store(0, std::memory_order_relaxed);
        scheduler.SetRuntimeOutputFpsCap(0);
    }

    void ResetSequenceState(const bool preserveAdaptive = false)
    {
        scheduler.Reset();
        schedulerConfigured = false;
        adaptiveResumePending = preserveAdaptive;
        if (!preserveAdaptive) {
            ResetAdaptiveLoadState();
        }
        {
            std::lock_guard nvofLock(nvofMutex);
            nvofDetector.Reset();
        }
        sceneBlender.Reset();
        removeEveryOtherToggle = false;
    }

    bool AcquireSourceSlot(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, bool parallelInputs,
        size_t& index, ID3D11Texture2D** texture,
        ID3D11Texture2D** inferenceAsFirst, ID3D11Texture2D** inferenceAsSecond,
        ID3D11Query** inputCopyAsFirstReadyQuery, ID3D11Query** inputCopyAsSecondReadyQuery)
    {
        index = SIZE_MAX;
        if (!device || !width || !height || !texture || !inferenceAsFirst || !inferenceAsSecond
                || !inputCopyAsFirstReadyQuery || !inputCopyAsSecondReadyQuery) {
            return false;
        }
        *inferenceAsFirst = nullptr;
        *inferenceAsSecond = nullptr;
        *inputCopyAsFirstReadyQuery = nullptr;
        *inputCopyAsSecondReadyQuery = nullptr;

        std::lock_guard lock(mutex);
        for (size_t i = 0; i < sourcePool.size(); ++i) {
            auto& slot = sourcePool[i];
            if (slot.inUse) {
                continue;
            }
            if (!SameTextureShape(slot.texture, device, width, height, format)) {
                slot.texture.Release();
                slot.inputCopyAsFirstReadyQuery.Release();
                slot.inputCopyAsSecondReadyQuery.Release();
                if (FAILED(CreateBgraTexture(device, width, height, &slot.texture, format))) {
                    continue;
                }
            }
            if (!SameTextureShape(slot.inferenceAsFirst, device, width, height, format)) {
                slot.inferenceAsFirst.Release();
                if (FAILED(CreateBgraTexture(device, width, height, &slot.inferenceAsFirst, format))) {
                    continue;
                }
            }
            if (parallelInputs) {
                if (!SameTextureShape(slot.inferenceAsSecond, device, width, height, format)) {
                    slot.inferenceAsSecond.Release();
                    if (FAILED(CreateBgraTexture(device, width, height, &slot.inferenceAsSecond, format))) {
                        continue;
                    }
                }
            } else {
                slot.inferenceAsSecond.Release();
                slot.inputCopyAsSecondReadyQuery.Release();
            }
            if (!slot.inputCopyAsFirstReadyQuery) {
                D3D11_QUERY_DESC queryDesc = {};
                queryDesc.Query = D3D11_QUERY_EVENT;
                if (FAILED(device->CreateQuery(&queryDesc, &slot.inputCopyAsFirstReadyQuery))) {
                    slot.inputCopyAsFirstReadyQuery.Release();
                }
            }
            if (parallelInputs && !slot.inputCopyAsSecondReadyQuery) {
                D3D11_QUERY_DESC queryDesc = {};
                queryDesc.Query = D3D11_QUERY_EVENT;
                if (FAILED(device->CreateQuery(&queryDesc, &slot.inputCopyAsSecondReadyQuery))) {
                    slot.inputCopyAsSecondReadyQuery.Release();
                }
            }
            slot.inUse = true;
            index = i;
            *texture = slot.texture;
            (*texture)->AddRef();
            *inferenceAsFirst = slot.inferenceAsFirst;
            (*inferenceAsFirst)->AddRef();
            if (slot.inferenceAsSecond) {
                *inferenceAsSecond = slot.inferenceAsSecond;
                (*inferenceAsSecond)->AddRef();
            }
            if (slot.inputCopyAsFirstReadyQuery) {
                *inputCopyAsFirstReadyQuery = slot.inputCopyAsFirstReadyQuery;
                (*inputCopyAsFirstReadyQuery)->AddRef();
            }
            if (slot.inputCopyAsSecondReadyQuery) {
                *inputCopyAsSecondReadyQuery = slot.inputCopyAsSecondReadyQuery;
                (*inputCopyAsSecondReadyQuery)->AddRef();
            }
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
        REFERENCE_TIME& frameDuration)
    {
        if (!owner || !processor || !sample || settings.iRifeMode == RIFE_MODE_Disabled) {
            return false;
        }

        if (lastSubmittedGeneration != presenterGeneration) {
            lastSubmittedGeneration = presenterGeneration;
            ResetNonBlocking(preserveAdaptiveForResume.load(std::memory_order_acquire));
        }

        REFERENCE_TIME sampleStart = 0, sampleEnd = 0;
        const bool hasSampleTime = SUCCEEDED(sample->GetTime(&sampleStart, &sampleEnd));
        bool variableTiming = false;
        {
            std::lock_guard lock(sourceRateMutex);
            const FrameRate stableRate = sourceRateStabilizer.Observe(
                SourceRateFromDuration(frameDuration), sampleStart, hasSampleTime);
            variableTiming = sourceRateStabilizer.HasVariableTiming();
            const REFERENCE_TIME stableDuration = DurationFromSourceRate(stableRate);
            if (stableDuration > 0) {
                frameDuration = stableDuration;
            }
            sourceFpsMilli.store(FpsMilli(stableRate), std::memory_order_relaxed);
            retainedFpsMilli.store(FpsMilli(RetainedSourceRate(stableRate,
                settings.iRifeDuplicateRemoval == RIFE_DUPLICATES_RemoveEveryOther ? 2u : 1u)),
                std::memory_order_relaxed);
            variableTimingDetected.store(variableTiming, std::memory_order_relaxed);
        }

        // Match the visible source geometry, before TensorRT padding, window
        // scaling, texture allocation, or any engine work. Returning false
        // uses Receive's existing normally paced source-video path.
        const CSize sourceSize = processor->GetRifeSourceContentSize();
        const int ruleIndex = MatchRifeRateRule(settings.rifeRules,
            static_cast<uint32_t>(std::max<LONG>(0, sourceSize.cx)),
            static_cast<uint32_t>(std::max<LONG>(0, sourceSize.cy)), frameDuration);
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
                    maxMultiplierMilli, maxOutputFpsMilli,
                    settings.iRifeDuplicateRemoval == RIFE_DUPLICATES_RemoveEveryOther ? 2u : 1u);
                capped.SetVariableSourceTiming(variableTiming);
                const FrameRate sourceRate = RetainedSourceRate(
                    SourceRateFromDuration(frameDuration),
                    settings.iRifeDuplicateRemoval == RIFE_DUPLICATES_RemoveEveryOther ? 2u : 1u);
                const FrameRate targetRate = capped.ResolveTargetRate(sourceRate);
                // A cap at/below source FPS means no interpolation, never
                // decimate the original video to satisfy a workload limit.
                // The explicit remove-every-other option must still pass
                // through the worker when interpolation is capped at 1x of
                // the retained cadence. Otherwise bypass silently keeps all
                // decoded frames and disables duplicate removal.
                bypass = settings.iRifeDuplicateRemoval != RIFE_DUPLICATES_RemoveEveryOther
                    && sourceRate.IsValid() && (!targetRate.IsValid()
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

        ID3D11Device* device = processor->GetRifeDevice();
        const CSize contentSize = processor->GetRifeContentSize();
        if (contentSize != lastSubmittedContentSize) {
            if (lastSubmittedContentSize.cx > 0 && lastSubmittedContentSize.cy > 0) {
                // Explicit working geometry changes invalidate queued crops,
                // source pairs, retained CUDA inputs and measured capacity.
                // Full-source mode is independent of player window size.
                owner->ResetFrameInterpolationPresenterQueue();
                presenterGeneration = owner->m_FrameInterpolationPresenterGeneration.load(std::memory_order_acquire);
                lastSubmittedGeneration = presenterGeneration;
                ResetNonBlocking();
            }
            lastSubmittedContentSize = contentSize;
        }
        const CSize size = processor->GetRifeFrameSize(
            RifeModelAlignment(settings.iRifeModel));
        if (!device || size.cx <= 0 || size.cy <= 0) {
            return false;
        }

        const uint32_t inferenceContextCount = static_cast<uint32_t>(std::clamp(
            settings.iRifeGpuThreads, RIFE_GPU_THREADS_MIN, RIFE_GPU_THREADS_MAX));
        size_t slot = SIZE_MAX;
        CComPtr<ID3D11Texture2D> texture;
        CComPtr<ID3D11Texture2D> inferenceAsFirst;
        CComPtr<ID3D11Texture2D> inferenceAsSecond;
        CComPtr<ID3D11Query> inputCopyAsFirstReadyQuery;
        CComPtr<ID3D11Query> inputCopyAsSecondReadyQuery;
        if (!AcquireSourceSlot(device, static_cast<UINT>(size.cx), static_cast<UINT>(size.cy),
                processor->GetRifeSurfaceFormat(), inferenceContextCount > 1, slot, &texture, &inferenceAsFirst, &inferenceAsSecond,
                &inputCopyAsFirstReadyQuery, &inputCopyAsSecondReadyQuery)) {
            sourcePoolMisses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        REFERENCE_TIME sourceTime = INVALID_TIME;
        if (!processor->PrepareRifeSource(
                sample, texture, sourceTime, RifeModelAlignment(settings.iRifeModel))
                || sourceTime == INVALID_TIME) {
            ReleaseSourceSlot(slot);
            return false;
        }

        // Stage CUDA-only copies immediately after the source image is produced,
        // while its D3D work is still near the front of the queue. With parallel
        // contexts, keep separate copies for this frame's two possible roles:
        // second input of A/B and first input of B/C. Adjacent pair jobs therefore
        // never contend for ownership of the same CUDA graphics resource.
        const auto inputCopyStart = std::chrono::steady_clock::now();
        CComPtr<ID3D11DeviceContext> inputCopyContext;
        device->GetImmediateContext(&inputCopyContext);
        if (!inputCopyContext) {
            ReleaseSourceSlot(slot);
            return false;
        }
        CComPtr<ID3D11Multithread> inputCopyMultithread;
        if (SUCCEEDED(inputCopyContext->QueryInterface(IID_PPV_ARGS(&inputCopyMultithread)))
                && inputCopyMultithread) {
            inputCopyMultithread->SetMultithreadProtected(TRUE);
        }
        // With parallel A/B and B/C jobs, the newly submitted frame is needed
        // immediately as the second input of A/B. Queue that role first. Its
        // first-input copy is only needed by the future B/C pair one source
        // interval later, so it can safely trail the latency-critical copy.
        if (inferenceAsSecond) {
            inputCopyContext->CopyResource(inferenceAsSecond, texture);
            if (inputCopyAsSecondReadyQuery) {
                inputCopyContext->End(inputCopyAsSecondReadyQuery);
            }
        }
        inputCopyContext->CopyResource(inferenceAsFirst, texture);
        if (inputCopyAsFirstReadyQuery) {
            inputCopyContext->End(inputCopyAsFirstReadyQuery);
        }
        lastInputCopySubmitUs.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - inputCopyStart).count()), std::memory_order_relaxed);

        SourceFrame frame;
        frame.slot = slot;
        frame.texture = texture;
        frame.inferenceAsFirst = inferenceAsFirst;
        frame.inferenceAsSecond = inferenceAsSecond;
        frame.inputCopyAsFirstReadyQuery = inputCopyAsFirstReadyQuery;
        frame.inputCopyAsSecondReadyQuery = inputCopyAsSecondReadyQuery;
        frame.processor = processor;
        frame.time = sourceTime;
        frame.frameDuration = frameDuration;
        frame.variableTiming = variableTiming;
        frame.contentWidth = static_cast<UINT>(contentSize.cx);
        frame.contentHeight = static_cast<UINT>(contentSize.cy);
        frame.cudaOutputEligible = processor->CanUseRifeCudaOutput();
        frame.presenterGeneration = presenterGeneration;
        frame.resetSerial = resetSerial.load(std::memory_order_acquire);
        frame.settings = settings;
        frame.displayRate = displayRate;
        frame.maxMultiplierMilli = maxMultiplierMilli;
        frame.maxOutputFpsMilli = maxOutputFpsMilli;
        configuredInferenceContexts.store(inferenceContextCount, std::memory_order_relaxed);
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
        bool synthetic, bool dropIfLate = true, const std::shared_ptr<RifeCudaOutputLease>& cudaOutput = {})
    {
        if ((!texture && !cudaOutput) || !frame.processor) {
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
            const CSize contentSize(static_cast<int>(frame.contentWidth), static_cast<int>(frame.contentHeight));
            const bool reserved = cudaOutput
                ? frame.processor->ReserveRifeCudaPresentationSurface(cudaOutput, handle, contentSize)
                : frame.processor->ReserveRifePresentationSurface(texture, handle, contentSize);
            if (reserved) {
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
        const uint32_t frameStride = frame.settings.iRifeDuplicateRemoval == RIFE_DUPLICATES_RemoveEveryOther ? 2u : 1u;
        const bool sameRateConfiguration = schedulerSourceFrameStride == frameStride
            && schedulerMode == frame.settings.iRifeMode
            && schedulerCustomFps == frame.settings.iRifeCustomFps
            && schedulerMaxMultiplierMilli == frame.maxMultiplierMilli
            && schedulerMaxOutputFpsMilli == frame.maxOutputFpsMilli
            && schedulerPerformanceBoost == frame.settings.bRifePerformanceBoost
            && schedulerVariableTiming == frame.variableTiming
            && schedulerDisplayRate.numerator == frame.displayRate.numerator
            && schedulerDisplayRate.denominator == frame.displayRate.denominator;
        const bool changed = !schedulerConfigured
            || schedulerSerial != frame.resetSerial
            || !sameRateConfiguration;
        if (!changed) {
            return;
        }

        schedulerSourceFrameStride = frameStride;
        schedulerMode = frame.settings.iRifeMode;
        schedulerCustomFps = frame.settings.iRifeCustomFps;
        schedulerMaxMultiplierMilli = frame.maxMultiplierMilli;
        schedulerMaxOutputFpsMilli = frame.maxOutputFpsMilli;
        schedulerPerformanceBoost = frame.settings.bRifePerformanceBoost;
        schedulerVariableTiming = frame.variableTiming;
        schedulerDisplayRate = frame.displayRate;
        schedulerSerial = frame.resetSerial;
        scheduler.Configure(
            ToSchedulerMode(frame.settings.iRifeMode),
            {static_cast<uint32_t>(std::clamp(frame.settings.iRifeCustomFps,
                RIFE_CUSTOM_FPS_MIN, RIFE_CUSTOM_FPS_MAX)), 1},
            frame.displayRate, frame.maxMultiplierMilli, frame.maxOutputFpsMilli, frameStride);
        scheduler.SetVariableSourceTiming(frame.variableTiming);
        if (adaptiveResumePending && sameRateConfiguration) {
            adaptivePressureController.Resume(GetTickCount64());
            scheduler.SetRuntimeOutputFpsCap(adaptiveAppliedCapFpsMilli);
        } else {
            ResetAdaptiveLoadState();
        }
        adaptiveResumePending = false;
        preserveAdaptiveForResume.store(false, std::memory_order_release);
        schedulerConfigured = true;
    }

    void UpdateAdaptiveLoadLimit(const SourceFrame& frame, const FrameRate sourceRate,
        const size_t pendingPairDepth, const bool interpolationReady)
    {
        const FrameRate requestedRate = scheduler.ResolveConfiguredTargetRate(sourceRate);
        const auto timing = adaptiveInferenceTiming.GetSummary();
        const uint32_t samples = static_cast<uint32_t>(timing.count);
        const uint32_t contexts = static_cast<uint32_t>(std::clamp(
            frame.settings.iRifeGpuThreads, RIFE_GPU_THREADS_MIN, RIFE_GPU_THREADS_MAX));

        // Actual delivery and congestion decide backoff. Inference capacity
        // can authorize a recovery probe only with fresh presenter measurements
        // and sustained spare capacity; it never lowers a verified output rate.
        const uint32_t headroom = frame.settings.bRifePerformanceBoost ? 990u : 970u;
        const auto estimate = EstimateFrameInterpolationLoad(
            sourceRate, requestedRate, timing.parallelismPermille, timing.averageWallUs, samples, headroom,
            frame.variableTiming);
        adaptiveRequestedOutputFpsMilli.store(estimate.requestedOutputFpsMilli, std::memory_order_relaxed);
        adaptiveSyntheticCapacityFpsMilli.store(
            estimate.sustainableSyntheticFpsMilli, std::memory_order_relaxed);
        adaptiveMeasuredParallelismPermille.store(timing.parallelismPermille, std::memory_order_relaxed);
        adaptiveInterpolationReady.store(interpolationReady, std::memory_order_relaxed);
        adaptiveLoadSamples.store(samples, std::memory_order_relaxed);
        adaptiveHeadroomPermille.store(headroom, std::memory_order_relaxed);

        uint32_t sourceQueueDepth = 0;
        {
            std::lock_guard lock(mutex);
            sourceQueueDepth = static_cast<uint32_t>(std::min<size_t>(
                queue.size(), std::numeric_limits<uint32_t>::max()));
        }
        const auto renderTiming = adaptiveAppliedCapFpsMilli
            ? owner->m_FrameInterpolationPresenterRenderTiming.GetSummary()
            : CRollingTimingWindow<256>::Summary{};
        const FrameInterpolationPressureSnapshot pressureSnapshot = {
            sourcePoolMisses.load(std::memory_order_relaxed),
            lateSyntheticDrops.load(std::memory_order_relaxed),
            presentationDrops.load(std::memory_order_relaxed),
            presentationReclaims.load(std::memory_order_relaxed),
            presentationSurfaceWaitUs.load(std::memory_order_relaxed),
            presentationSurfaceWaitCount.load(std::memory_order_relaxed),
            sourceQueueDepth,
            static_cast<uint32_t>(std::min<size_t>(
                pendingPairDepth, std::numeric_limits<uint32_t>::max())),
            contexts,
            owner->m_FrameInterpolationPresenterStaleDrops.load(std::memory_order_relaxed),
            true,
            owner->m_FrameInterpolationPresenterRenderedFrames.load(std::memory_order_relaxed),
            interpolationReady,
            estimate.sustainableSyntheticFpsMilli,
            samples,
            timing.completedCalls,
            static_cast<uint64_t>(renderTiming.averageMs * 1000.0),
            static_cast<uint32_t>(renderTiming.count),
        };
        const uint32_t capBeforePressureUpdate = adaptiveAppliedCapFpsMilli
            ? adaptiveAppliedCapFpsMilli : estimate.requestedOutputFpsMilli;
        const auto pressureDecision = adaptivePressureController.Update(
            estimate.requestedOutputFpsMilli, sourceRate, GetTickCount64(), pressureSnapshot,
            frame.variableTiming);
        adaptiveAppliedCapFpsMilli = pressureDecision.outputCapFpsMilli;
        adaptivePressureDetected.store(pressureDecision.pressureDetected, std::memory_order_relaxed);
        adaptivePressurePhase.store(
            static_cast<uint32_t>(pressureDecision.phase), std::memory_order_relaxed);
        adaptivePressureGoodFpsMilli.store(
            pressureDecision.lastKnownGoodFpsMilli, std::memory_order_relaxed);
        adaptivePressureBadFpsMilli.store(
            pressureDecision.lastKnownBadFpsMilli, std::memory_order_relaxed);
        adaptivePressureBackoffFpsMilli.store(
            pressureDecision.backoffStepFpsMilli, std::memory_order_relaxed);
        adaptiveMeasuredOutputFpsMilli.store(
            pressureDecision.measuredOutputFpsMilli, std::memory_order_relaxed);
        adaptiveMeasuredOutputReady.store(
            pressureDecision.measuredOutputReady, std::memory_order_relaxed);
        adaptiveMeasuredOutputHealthy.store(
            pressureDecision.measuredOutputHealthy, std::memory_order_relaxed);
        adaptiveRecoveryWaitingForHeadroom.store(
            pressureDecision.recoveryWaitingForHeadroom, std::memory_order_relaxed);
        adaptivePressureObservedReasons.store(
            pressureDecision.observedPressureReasons, std::memory_order_relaxed);
        adaptivePressureConfirmedReasons.store(
            pressureDecision.confirmedPressureReasons, std::memory_order_relaxed);
        if (pressureDecision.pressureDetected) {
            adaptiveLastPressureCapFpsMilli.store(
                capBeforePressureUpdate, std::memory_order_relaxed);
            adaptiveLastPressureReasons.store(
                pressureDecision.confirmedPressureReasons, std::memory_order_relaxed);
        }
        adaptiveSourceQueueDepth.store(sourceQueueDepth, std::memory_order_relaxed);
        adaptivePendingPairDepth.store(
            pressureSnapshot.pendingPairDepth, std::memory_order_relaxed);
        scheduler.SetRuntimeOutputFpsCap(adaptiveAppliedCapFpsMilli);
        adaptiveOutputCapFpsMilli.store(adaptiveAppliedCapFpsMilli, std::memory_order_relaxed);
    }

    bool EnsureOutputTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format)
    {
        if (outputDevice == device && outputWidth == width && outputHeight == height
                && SameTextureShape(outputTexture, device, width, height, format)) {
            return true;
        }
        outputTexture.Release();
        outputDevice = device;
        outputWidth = width;
        outputHeight = height;
        return SUCCEEDED(CreateBgraTexture(device, width, height, &outputTexture, format));
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
        key.model = frame.settings.iRifeModel;
        key.performanceBoost = frame.settings.bRifePerformanceBoost;
        key.format = TextureFormat(frame.texture);
        key.featureReuse = frame.settings.bRifeFeatureReuse
            && (key.model == RIFE_MODEL_415_LITE || key.model == RIFE_MODEL_425);
        if (!runtimeKey || !(*runtimeKey == key)) encoderFeaturesActive.store(false, std::memory_order_relaxed);

        if (runtimeKey && RifeRuntimeCacheConflicts(*runtimeKey, key)) {
            runtimeBuild.reset();
        }
        // 4.25 Lite changes 32-pixel padding to 128-pixel padding. The active
        // runtime can consequently have a different size while an inactive
        // cached model still owns textures the source/output pools will reuse.
        // Retire every conflicting registration owner before selecting/building
        // the destination runtime. Disk engine plans and exact-key hits remain.
        for (auto it = runtimeCache.begin(); it != runtimeCache.end();) {
            if (RifeRuntimeCacheConflicts(it->first, key)) {
                it = runtimeCache.erase(it);
            } else {
                ++it;
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
        const auto model = root / L"models" / RifeModelFileName(key.model);
        const auto cache = root / L"cache";
        if (root.empty() || !std::filesystem::exists(model)) {
            state->status = std::format(L"RIFE {} model is not installed", RifeModelDisplayName(key.model));
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
                model.wstring(), cache.wstring(), key.format == DXGI_FORMAT_R16G16B16A16_FLOAT, key.featureReuse);
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

    static uint64_t ElapsedMicroseconds(const std::chrono::steady_clock::time_point start)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
    }

    void RecordNvofCall(const std::chrono::steady_clock::time_point start)
    {
        nvofCallUs.fetch_add(ElapsedMicroseconds(start), std::memory_order_relaxed);
        nvofCallCount.fetch_add(1, std::memory_order_relaxed);
    }

    bool DetectImageSceneCut(CRifeImageSceneDetector& detector, const SourceFrame& first, const SourceFrame& second,
        const bool fallback = false)
    {
        if (fallback) {
            imageSceneFallbacks.fetch_add(1, std::memory_order_relaxed);
            lastSceneDiagnosticMode.store(SceneDiagImageFallback, std::memory_order_relaxed);
        } else {
            lastSceneDiagnosticMode.store(SceneDiagImage, std::memory_order_relaxed);
        }
        const auto start = std::chrono::steady_clock::now();
        ID3D11Device* device = second.processor ? second.processor->GetRifeDevice() : nullptr;
        if (!device) {
            lastImageCompactReadback.store(false, std::memory_order_relaxed);
            imageSceneUs.fetch_add(ElapsedMicroseconds(start), std::memory_order_relaxed);
            imageSceneAnalyses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        bool cut = false;
        const bool analyzed = detector.Analyze(device, first.texture, second.texture, cut,
            std::clamp(second.settings.iRifeSceneThreshold, 0, 100) / 100.0,
            second.contentWidth, second.contentHeight);
        lastImageCompactReadback.store(detector.UsedCompactReadback(), std::memory_order_relaxed);
        imageSceneUs.fetch_add(ElapsedMicroseconds(start), std::memory_order_relaxed);
        imageSceneAnalyses.fetch_add(1, std::memory_order_relaxed);
        return analyzed && cut;
    }

    bool DetectSvpSceneCut(InferenceWorkerState& workerState, const SourceFrame& first, const SourceFrame& second)
    {
        const auto start = std::chrono::steady_clock::now();
        auto* device = second.processor ? second.processor->GetRifeDevice() : nullptr;
        SvpSceneResult result;
        const bool analyzed = workerState.svpDetector.Analyze(device, first.texture, second.texture,
            second.contentWidth, second.contentHeight, result);
        svpSceneUs.fetch_add(ElapsedMicroseconds(start), std::memory_order_relaxed);
        svpSceneAnalyses.fetch_add(1, std::memory_order_relaxed);
        if (analyzed) {
            const auto size = workerState.svpDetector.Size();
            svpAnalysisDimensions.store((uint64_t(size.width) << 32) | size.height, std::memory_order_relaxed);
            svpBadCoverageMilli.store(static_cast<uint32_t>(result.badFraction * 100000), std::memory_order_relaxed);
            lastSceneDiagnosticMode.store(SceneDiagSvp, std::memory_order_relaxed);
            return result.cut;
        }
        svpSceneFallbacks.fetch_add(1, std::memory_order_relaxed);
        const bool cut = DetectImageSceneCut(workerState.imageDetector, first, second);
        lastSceneDiagnosticMode.store(SceneDiagSvpFallback, std::memory_order_relaxed);
        return cut;
    }

    ID3D11Texture2D* AcquireInferenceOutput(
        InferenceWorkerState& workerState,
        ID3D11Device* device,
        const UINT width,
        const UINT height,
        const size_t index, DXGI_FORMAT format)
    {
        if (!device || !width || !height) {
            return nullptr;
        }
        if (workerState.inferenceOutputs.size() <= index) {
            workerState.inferenceOutputs.resize(index + 1);
        }
        auto& output = workerState.inferenceOutputs[index];
        if (!SameTextureShape(output, device, width, height, format)) {
            output.Release();
            if (FAILED(CreateBgraTexture(device, width, height, &output, format))) {
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
        ID3D11Texture2D* output,
        const std::optional<bool> copiesReadyAtWorkerStart,
        const uint64_t inputPairId,
        std::shared_ptr<RifeCudaOutputLease>& cudaOutput)
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
                || outputDesc.Format != desc.Format || !RifeSupportedSurfaceFormat(desc.Format)
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
        bool ok = false;
        if (cudaOutput) {
            ok = cudaOutput->Interpolate(contextIndex, first, second, timestep, stats, inputPairId);
            if (!ok) cudaOutput.reset();
        }
        if (!cudaOutput) {
            ok = runtime->Interpolate(contextIndex, first, second, output, timestep, stats, inputPairId);
        }
        const auto runtimeEnd = std::chrono::steady_clock::now();
        const auto runtimeUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            runtimeEnd - runtimeStart).count());
        activeInferences.fetch_sub(1, std::memory_order_acq_rel);
        if (!ok) {
            return false;
        }
        inputPairReuseAvailable.store(stats.inputPairReuse != 0, std::memory_order_relaxed);
        encoderFeaturesActive.store(stats.featureReuse != 0, std::memory_order_relaxed);
        if (stats.inputPairReuse == 1) inputPairPacks.fetch_add(1, std::memory_order_relaxed);
        if (stats.inputPairReuse == 2) inputPairReuses.fetch_add(1, std::memory_order_relaxed);
        adaptiveInferenceTiming.AddMicroseconds(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                runtimeStart.time_since_epoch()).count()),
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                runtimeEnd.time_since_epoch()).count()));
        std::lock_guard timingLock(timingMutex);
        lastHostStats = stats;
        lastTimingContext = contextIndex;
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
        lastTensorRtSubmitUs.store(MsToUs(stats.tensorRtSubmitMs), std::memory_order_relaxed);
        tensorRtGraphUsed.store(stats.tensorRtGraphUsed != 0, std::memory_order_relaxed);
        lastInferenceUs.store(MsToUs(stats.inferenceMs), std::memory_order_relaxed);
        rollingWallTiming.AddMicroseconds(runtimeUs);
        rollingInternalTiming.AddMicroseconds(MsToUs(stats.totalRuntimeMs));
        rollingGpuTiming.AddMicroseconds(MsToUs(stats.inferenceMs));
        rollingTensorRtTiming.AddMicroseconds(MsToUs(stats.tensorRtMs));
        rollingHandoffStartWaitTiming.AddMicroseconds(MsToUs(stats.handoffStartWaitMs));
        rollingHandoffEndWaitTiming.AddMicroseconds(MsToUs(stats.handoffEndWaitMs));
        rollingHandoffPreMapWaitTiming.AddMicroseconds(MsToUs(stats.handoffPreMapWaitMs));
        rollingHandoffMapWaitTiming.AddMicroseconds(MsToUs(stats.handoffMapWaitMs));
        if (copiesReadyAtWorkerStart.has_value()) {
            auto& correlatedTiming = *copiesReadyAtWorkerStart
                ? rollingHandoffStartWaitCopiesReadyTiming
                : rollingHandoffStartWaitCopiesPendingTiming;
            correlatedTiming.AddMicroseconds(MsToUs(stats.handoffStartWaitMs));
        }
        return true;
    }

    bool DrainRetainedInputs(InferenceWorkerState& workerState)
    {
        if (workerState.retainedInputRuntime
                && !workerState.retainedInputRuntime->DrainContext(workerState.index)) {
            return false;
        }
        workerState.retainedInputFirst.reset();
        workerState.retainedInputSecond.reset();
        workerState.retainedInputRuntime.reset();
        return true;
    }

    void DrainAllRetainedInputs()
    {
        for (auto& workerState : inferenceWorkers) {
            if (workerState) {
                DrainRetainedInputs(*workerState);
            }
        }
    }

    void PublishTargetResult(PairJob& job, const size_t index, TargetResult&& result)
    {
        if (index >= job.results.size()) {
            return;
        }
        job.results[index] = std::move(result);
        job.readyResults.store(index + 1, std::memory_order_release);
        cv.notify_all();
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
        if (workerState.retainedInputRuntime
                && workerState.retainedInputRuntime != job.runtime
                && !DrainRetainedInputs(workerState)) {
            for (size_t i = 0; i < job.targets.size(); ++i) {
                const auto& target = job.targets[i];
                TargetResult result;
                result.target = target;
                result.inferenceFailed = !target.exactSource;
                if (result.inferenceFailed) {
                    inferenceFallbackFrames.fetch_add(1, std::memory_order_relaxed);
                }
                PublishTargetResult(job, i, std::move(result));
            }
            return;
        }
        ID3D11Texture2D* firstInference = first.inferenceAsFirst;
        ID3D11Texture2D* secondInference = second.inferenceAsSecond
            ? second.inferenceAsSecond.p : second.inferenceAsFirst.p;
        if (!firstInference || !secondInference) {
            for (size_t i = 0; i < job.targets.size(); ++i) {
                const auto& target = job.targets[i];
                TargetResult result;
                result.target = target;
                result.inferenceFailed = !target.exactSource;
                if (result.inferenceFailed) {
                    inferenceFallbackFrames.fetch_add(1, std::memory_order_relaxed);
                }
                PublishTargetResult(job, i, std::move(result));
            }
            return;
        }

        // CUDA/D3D11 interop performs the ownership synchronization when the
        // runtime maps these staged inputs. Keep the D3D11 event queries as a
        // readiness probe only; blocking here serializes the CPU before CUDA
        // work has even been queued and duplicates part of the interop wait.
        const auto copiesReadyAtWorkerStart = ProbeInputCopyQueries(first, second);

        bool hasTimelySyntheticTarget = false;
        for (const auto& target : job.targets) {
            if (!target.exactSource && !IsLate(second, target.presentationTime)) {
                hasTimelySyntheticTarget = true;
                break;
            }
        }

        if (second.settings.iRifeSceneDetection == RIFE_SCENE_Disabled) {
            lastSceneDiagnosticMode.store(SceneDiagDisabled, std::memory_order_relaxed);
        } else if (second.settings.iRifeSceneDetection == RIFE_SCENE_Image) {
            lastSceneDiagnosticMode.store(SceneDiagImage, std::memory_order_relaxed);
        }
        bool sceneDecisionReady = second.settings.iRifeSceneDetection != RIFE_SCENE_NVOF;
        bool sceneCut = hasTimelySyntheticTarget
            && second.settings.iRifeSceneDetection == RIFE_SCENE_Image
            && DetectImageSceneCut(workerState.imageDetector, first, second);
        if (hasTimelySyntheticTarget && second.settings.iRifeSceneDetection == RIFE_SCENE_SVPflow1) {
            sceneCut = DetectSvpSceneCut(workerState, first, second);
        }
        bool advancedDeferredInputs = false;
        const auto retainDeferredInputs = [&]() {
            if (advancedDeferredInputs) return;
            workerState.retainedInputFirst = job.first;
            workerState.retainedInputSecond = job.second;
            workerState.retainedInputRuntime = job.runtime;
            advancedDeferredInputs = true;
        };

        size_t outputIndex = 0;
        for (size_t targetIndex = 0; targetIndex < job.targets.size(); ++targetIndex) {
            const auto& target = job.targets[targetIndex];
            TargetResult result;
            result.target = target;
            result.sceneCut = sceneCut;

            if (target.exactSource) {
                PublishTargetResult(job, targetIndex, std::move(result));
                continue;
            }
            if (!IsCurrent(second)) {
                PublishTargetResult(job, targetIndex, std::move(result));
                continue;
            }
            if (IsLate(second, target.presentationTime)) {
                result.skippedLate = true;
                lateSyntheticDrops.fetch_add(1, std::memory_order_relaxed);
                PublishTargetResult(job, targetIndex, std::move(result));
                continue;
            }
            if (sceneDecisionReady && sceneCut) {
                PublishTargetResult(job, targetIndex, std::move(result));
                continue;
            }

            ID3D11Texture2D* generated = AcquireInferenceOutput(
                workerState, device, job.width, job.height, outputIndex++, TextureFormat(second.texture));
            if (!generated) {
                result.inferenceFailed = true;
                inferenceFallbackFrames.fetch_add(1, std::memory_order_relaxed);
                PublishTargetResult(job, targetIndex, std::move(result));
                continue;
            }

            bool nvofStarted = false;
            std::unique_lock<std::mutex> nvofLock;
            if (!sceneDecisionReady) {
                // Do not block a second TensorRT worker behind OFA before it
                // has submitted its own inference. The worker that wins the
                // NVOF lock keeps the original overlap (Begin -> RIFE ->
                // Finish). A contending worker runs RIFE first, then performs
                // its serialized scene analysis afterward.
                nvofLock = std::unique_lock<std::mutex>(nvofMutex, std::try_to_lock);
                if (nvofLock.owns_lock()) {
                    const auto nvofStart = std::chrono::steady_clock::now();
                    const bool nvofReady = nvofDetector.Initialize(device, job.width, job.height, second.contentWidth, second.contentHeight);
                    if (!nvofReady) {
                        nvofInitFailures.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        nvofStarted = nvofDetector.BeginAnalyze(first.texture, second.texture);
                        if (!nvofStarted) {
                            nvofBeginFailures.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    RecordNvofCall(nvofStart);
                    if (nvofStarted) {
                        lastSceneDiagnosticMode.store(SceneDiagNvofOverlap, std::memory_order_relaxed);
                    }
                } else {
                    nvofMutexContentions.fetch_add(1, std::memory_order_relaxed);
                }
                if (nvofLock.owns_lock() && !nvofStarted) {
                    nvofLock.unlock();
                    sceneCut = DetectImageSceneCut(workerState.imageDetector, first, second, true);
                    sceneDecisionReady = true;
                    if (sceneCut) {
                        result.sceneCut = true;
                        PublishTargetResult(job, targetIndex, std::move(result));
                        continue;
                    }
                }
            }

            auto cudaOutput = second.cudaOutputEligible ? job.runtime->AcquireCudaOutput() : nullptr;
            const bool generatedOk = GenerateRife(job.runtime, job.contextIndex,
                firstInference, secondInference,
                static_cast<float>(target.timestep), generated,
                copiesReadyAtWorkerStart, job.inputPairId, cudaOutput);
            // GenerateRife() first retires any deferred input release left by
            // the previous request on this same context. Only now is it safe to
            // let those old source-pool slots go; retain this pair until the
            // context advances again.
            retainDeferredInputs();

            if (nvofStarted) {
                CNvidiaSceneChangeDetector::Metrics metrics;
                const auto nvofStart = std::chrono::steady_clock::now();
                const bool finished = nvofDetector.FinishAnalyze(metrics);
                RecordNvofCall(nvofStart);
                if (finished && metrics.valid) {
                    nvofSuccessfulAnalyses.fetch_add(1, std::memory_order_relaxed);
                    nvofAnalysisDimensions.store((static_cast<uint64_t>(metrics.analysisWidth) << 32)
                        | metrics.analysisHeight, std::memory_order_relaxed);
                    lastSceneDiagnosticMode.store(SceneDiagNvofOverlap, std::memory_order_relaxed);
                    sceneCut = metrics.likelyCut;
                } else {
                    if (!finished) {
                        nvofFinishFailures.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        nvofInvalidMetrics.fetch_add(1, std::memory_order_relaxed);
                    }
                    sceneCut = DetectImageSceneCut(workerState.imageDetector, first, second, true);
                }
                nvofLock.unlock();
                sceneDecisionReady = true;
                if (sceneCut) {
                    result.sceneCut = true;
                    PublishTargetResult(job, targetIndex, std::move(result));
                    continue;
                }
            } else if (!sceneDecisionReady) {
                // Another worker owned NVOF while this inference was running.
                // Serialize only the OFA analysis now; TensorRT parallelism has
                // already been preserved for this target.
                const auto nvofMutexWaitStart = std::chrono::steady_clock::now();
                nvofLock = std::unique_lock<std::mutex>(nvofMutex);
                nvofMutexWaitUs.fetch_add(ElapsedMicroseconds(nvofMutexWaitStart), std::memory_order_relaxed);
                nvofMutexWaitCount.fetch_add(1, std::memory_order_relaxed);
                CNvidiaSceneChangeDetector::Metrics metrics;
                const auto nvofStart = std::chrono::steady_clock::now();
                const bool nvofReady = nvofDetector.Initialize(device, job.width, job.height, second.contentWidth, second.contentHeight);
                if (!nvofReady) {
                    nvofInitFailures.fetch_add(1, std::memory_order_relaxed);
                }
                const bool began = nvofReady
                    && nvofDetector.BeginAnalyze(first.texture, second.texture);
                if (nvofReady && !began) {
                    nvofBeginFailures.fetch_add(1, std::memory_order_relaxed);
                }
                const bool finished = began && nvofDetector.FinishAnalyze(metrics);
                if (began && !finished) {
                    nvofFinishFailures.fetch_add(1, std::memory_order_relaxed);
                } else if (finished && !metrics.valid) {
                    nvofInvalidMetrics.fetch_add(1, std::memory_order_relaxed);
                }
                RecordNvofCall(nvofStart);
                if (finished && metrics.valid) {
                    nvofSuccessfulAnalyses.fetch_add(1, std::memory_order_relaxed);
                    nvofAnalysisDimensions.store((static_cast<uint64_t>(metrics.analysisWidth) << 32)
                        | metrics.analysisHeight, std::memory_order_relaxed);
                    lastSceneDiagnosticMode.store(SceneDiagNvofSerialized, std::memory_order_relaxed);
                    sceneCut = metrics.likelyCut;
                } else {
                    sceneCut = DetectImageSceneCut(workerState.imageDetector, first, second, true);
                }
                nvofLock.unlock();
                sceneDecisionReady = true;
                if (sceneCut) {
                    result.sceneCut = true;
                    PublishTargetResult(job, targetIndex, std::move(result));
                    continue;
                }
            }

            if (generatedOk) {
                if (cudaOutput) result.cudaOutput = std::move(cudaOutput);
                else result.generated = generated;
            } else {
                result.inferenceFailed = true;
                inferenceFallbackFrames.fetch_add(1, std::memory_order_relaxed);
            }
            PublishTargetResult(job, targetIndex, std::move(result));
        }
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
                const size_t published = std::min(
                    job->readyResults.load(std::memory_order_acquire), job->results.size());
                for (size_t i = published; i < job->results.size(); ++i) {
                    TargetResult result;
                    result.target = job->targets[i];
                    result.inferenceFailed = !result.target.exactSource;
                    if (result.inferenceFailed && job->second && IsCurrent(*job->second)) {
                        inferenceFallbackFrames.fetch_add(1, std::memory_order_relaxed);
                    }
                    PublishTargetResult(*job, i, std::move(result));
                }
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
        bool progressed = false;

        const auto queueSceneCutTarget = [&](const FrameInterpolationTarget& target) {
            if (second.settings.iRifeSceneProcessing == RIFE_SCENE_PROCESS_Blend
                    && device && job.width && job.height
                    && EnsureOutputTexture(device, job.width, job.height, TextureFormat(second.texture))
                    && sceneBlender.Blend(device, first.texture, second.texture,
                        outputTexture, static_cast<float>(target.timestep))) {
                const bool queued = QueueTexture(second, outputTexture, target.presentationTime, true);
                if (queued) sceneBlendFrames.fetch_add(1, std::memory_order_relaxed);
                return queued;
            }

            // Repeat is the default and also the safe fallback if the GPU
            // blend path cannot produce a frame.
            // ChangeFPS-style hold: the next scene starts at its source timestamp.
            ID3D11Texture2D* repeated = first.texture.p;
            sceneRepeatFrames.fetch_add(1, std::memory_order_relaxed);
            return QueueTexture(second, repeated, target.presentationTime, true);
        };

        const size_t readyResults = std::min(
            job.readyResults.load(std::memory_order_acquire), job.results.size());
        while (job.presentedResults < readyResults) {
            auto& result = job.results[job.presentedResults++];
            progressed = true;
            const auto& target = result.target;
            if (!IsCurrent(second)) {
                return progressed;
            }
            if (target.exactSource) {
                job.queuedOutput |= QueueTexture(second, second.texture, target.presentationTime, false);
                continue;
            }
            if (result.skippedLate) {
                continue;
            }
            if (result.sceneCut) {
                job.queuedOutput |= queueSceneCutTarget(target);
                continue;
            }
            if (result.generated || result.cudaOutput) {
                generatedFrames.fetch_add(1, std::memory_order_relaxed);
                job.queuedOutput |= QueueTexture(
                    second, result.generated, target.presentationTime, true, false, result.cudaOutput);
                // The presentation surface owns a queued lease. Do not keep
                // another reference for the rest of a high-multiplier pair.
                result.cudaOutput.reset();
            } else {
                ID3D11Texture2D* fallback = target.timestep < 0.5
                    ? first.texture.p : second.texture.p;
                job.queuedOutput |= QueueTexture(second, fallback, target.presentationTime, true);
            }
        }

        if (job.done.load(std::memory_order_acquire)
                && job.presentedResults >= job.results.size()
                && !job.presentationFinalized) {
            job.presentationFinalized = true;
            progressed = true;
        }
        if (job.presentationFinalized && !job.targets.empty() && !job.queuedOutput && IsCurrent(second)) {
            if (QueueTexture(second, second.texture, second.time, false)) {
                sourceContinuityFrames.fetch_add(1, std::memory_order_relaxed);
                job.queuedOutput = true;
            }
        }
        return progressed;
    }

    std::wstring CompactDiagnostics() const
    {
        return Diagnostics(false);
    }

    std::wstring Diagnostics(const bool detailed = true) const
    {
        std::lock_guard timingLock(timingMutex);
        const int ruleIndex = activeRule.load(std::memory_order_relaxed);
        if (ruleBypass.load(std::memory_order_relaxed)) {
            return std::format(L"off by rule #{} (source playback)", ruleIndex + 1);
        }
        const bool ready = adaptiveInterpolationReady.load(std::memory_order_relaxed);
        std::wstring diagnostics = std::format(L"{}, {} GPU threads, boost {}{}",
            RifeModelDisplayName(owner->m_Sets.iRifeModel), owner->m_Sets.iRifeGpuThreads,
            owner->m_Sets.bRifePerformanceBoost ? L"On" : L"Off",
            ready ? L"" : L", waiting for engine");
        if (detailed) {
            diagnostics += std::format(L", contexts {}, I/O {}",
                configuredInferenceContexts.load(std::memory_order_relaxed),
                tensorIoLinearValidated.load(std::memory_order_relaxed) ? L"LINEAR" : L"pending");
            if (owner->m_Sets.bRifeFeatureReuse && owner->m_Sets.iRifeModel != RIFE_MODEL_44
                    && owner->m_Sets.iRifeModel != RIFE_MODEL_46) {
                diagnostics += encoderFeaturesActive.load(std::memory_order_relaxed) ? L", features On" : L", features inactive";
            }
        }
        const auto requested = adaptiveRequestedOutputFpsMilli.load(std::memory_order_relaxed);
        const auto cap = adaptiveOutputCapFpsMilli.load(std::memory_order_relaxed);
        diagnostics += std::format(L"\nRIFE rate     : request {:.3f}, target {:.3f} fps",
            requested / 1000.0, (cap ? cap : requested) / 1000.0);
        if (adaptiveRecoveryWaitingForHeadroom.load(std::memory_order_relaxed)) {
            diagnostics += L", holding for headroom";
        }
        const wchar_t* phase = L"open";
        switch (static_cast<FrameInterpolationPressurePhase>(adaptivePressurePhase.load(std::memory_order_relaxed))) {
        case FrameInterpolationPressurePhase::Settling: phase = L"settling"; break;
        case FrameInterpolationPressurePhase::Stable: phase = L"stable"; break;
        case FrameInterpolationPressurePhase::Probe: phase = L"probe"; break;
        case FrameInterpolationPressurePhase::VerifiedRecovery: phase = L"recovering"; break;
        default: break;
        }
        const bool deliveryReady = adaptiveMeasuredOutputReady.load(std::memory_order_relaxed);
        diagnostics += std::format(L"\nRIFE delivery : {}, {} ({})",
            deliveryReady ? std::format(L"{:.3f} fps", adaptiveMeasuredOutputFpsMilli.load(std::memory_order_relaxed) / 1000.0)
                : std::wstring(L"sampling"),
            deliveryReady ? (adaptiveMeasuredOutputHealthy.load(std::memory_order_relaxed) ? L"verified" : L"shortfall")
                : (ready ? L"warming up" : L"waiting for engine"), phase);
        const auto pressure = adaptivePressureConfirmedReasons.load(std::memory_order_relaxed);
        const auto lastTrip = adaptiveLastPressureCapFpsMilli.load(std::memory_order_relaxed);
        if (pressure) {
            diagnostics += std::format(L"\nRIFE limit    : pressure ({})", FormatPressureReasons(pressure));
        } else if (lastTrip && cap && cap < requested) {
            diagnostics += std::format(L"\nRIFE limit    : {:.3f} fps rejected ({})", lastTrip / 1000.0,
                FormatPressureReasons(adaptiveLastPressureReasons.load(std::memory_order_relaxed)));
        }
        if (detailed) {
            diagnostics += std::format(L"\nRIFE capacity : synth-cap {:.3f} fps (inference only), overlap {:.2f}",
                adaptiveSyntheticCapacityFpsMilli.load(std::memory_order_relaxed) / 1000.0,
                adaptiveMeasuredParallelismPermille.load(std::memory_order_relaxed) / 1000.0);
            if (cap < requested || pressure || adaptivePressureObservedReasons.load(std::memory_order_relaxed)) {
                diagnostics += std::format(L"\nRIFE pressure : observed {}, confirmed {}",
                    FormatPressureReasons(adaptivePressureObservedReasons.load(std::memory_order_relaxed)),
                    FormatPressureReasons(pressure));
            }
        }
        const auto wall = rollingWallTiming.GetSummary();
        const auto gpu = rollingGpuTiming.GetSummary();
        if (wall.count) {
            diagnostics += std::format(L"\nRIFE timing   : wall {:.2f} avg/{:.2f} p95, GPU {:.2f}/{:.2f} ms",
                wall.averageMs, wall.p95Ms, gpu.averageMs, gpu.p95Ms);
        }
        if (detailed) {
            const auto trt = rollingTensorRtTiming.GetSummary();
            const auto startWait = rollingHandoffStartWaitTiming.GetSummary();
            diagnostics += std::format(L"\nRIFE stages   : TRT {:.2f} avg/{:.2f} p95, input-wait {:.2f}/{:.2f} ms",
                trt.averageMs, trt.p95Ms, startWait.averageMs, startWait.p95Ms);
            const auto packed = inputPairPacks.load(std::memory_order_relaxed);
            const auto reused = inputPairReuses.load(std::memory_order_relaxed);
            if (inputPairReuseAvailable.load(std::memory_order_relaxed) && packed + reused) {
                diagnostics += std::format(L", pair reuse {:.0f}%", 100.0 * reused / (packed + reused));
            } else {
                diagnostics += L", pair reuse unavailable";
            }
            const auto waits = presentationSurfaceWaitCount.load(std::memory_order_relaxed);
            if (waits) {
                diagnostics += std::format(L"\nRIFE surfaces : wait {:.2f} avg/{:.2f} max ms",
                    presentationSurfaceWaitUs.load(std::memory_order_relaxed) / (1000.0 * waits),
                    presentationSurfaceWaitMaxUs.load(std::memory_order_relaxed) / 1000.0);
            }
        }
        diagnostics += std::format(L"\nRIFE totals   : generated {}, scene-repeat {}, late-drop {}",
            generatedFrames.load(std::memory_order_relaxed), sceneRepeatFrames.load(std::memory_order_relaxed),
            lateSyntheticDrops.load(std::memory_order_relaxed));
        const auto inferenceFallback = inferenceFallbackFrames.load(std::memory_order_relaxed);
        const auto sourceFallback = sourceContinuityFrames.load(std::memory_order_relaxed);
        const auto presentDrop = presentationDrops.load(std::memory_order_relaxed);
        const auto reclaim = presentationReclaims.load(std::memory_order_relaxed);
        const auto poolMiss = sourcePoolMisses.load(std::memory_order_relaxed);
        const auto resync = sourceResyncs.load(std::memory_order_relaxed);
        if (inferenceFallback || sourceFallback || presentDrop || reclaim || poolMiss || resync) {
            diagnostics += std::format(L"\nRIFE fallback : infer {}, source {}, present-drop {}, reclaim {}, pool-miss {}, resync {}",
                inferenceFallback, sourceFallback, presentDrop, reclaim, poolMiss, resync);
        }
        const wchar_t* scene = owner->m_Sets.iRifeSceneDetection == RIFE_SCENE_Disabled ? L"Disabled" : L"starting";
        switch (lastSceneDiagnosticMode.load(std::memory_order_relaxed)) {
        case SceneDiagImage: scene = L"Image comparison"; break;
        case SceneDiagNvofOverlap:
        case SceneDiagNvofSerialized: scene = L"NVOF"; break;
        case SceneDiagImageFallback: scene = L"Image comparison (NVOF fallback)"; break;
        case SceneDiagSvp: scene = L"SVPflow1 motion vectors"; break;
        case SceneDiagSvpFallback: scene = L"Image comparison (SVPflow1 fallback)"; break;
        default: break;
        }
        const bool imageMode = owner->m_Sets.iRifeSceneDetection == RIFE_SCENE_Image
            || lastSceneDiagnosticMode.load(std::memory_order_relaxed) == SceneDiagImageFallback
            || lastSceneDiagnosticMode.load(std::memory_order_relaxed) == SceneDiagSvpFallback;
        const std::wstring sceneLabel = imageMode
            ? std::format(L"{} ({}%)", scene, owner->m_Sets.iRifeSceneThreshold) : scene;
        diagnostics += std::format(L"\nRIFE scene    : {}, {}", sceneLabel,
            owner->m_Sets.iRifeSceneProcessing == RIFE_SCENE_PROCESS_Blend ? L"blend" : L"repeat");
        if (detailed && owner->m_Sets.iRifeSceneDetection == RIFE_SCENE_SVPflow1) {
            const auto calls = svpSceneAnalyses.load(std::memory_order_relaxed);
            const auto dimensions = svpAnalysisDimensions.load(std::memory_order_relaxed);
            diagnostics += std::format(L"\nRIFE analysis : SVPflow1 {}x{}, {:.2f} ms avg, bad area {:.1f}%, fallback {}",
                dimensions >> 32, dimensions & 0xffffffff,
                calls ? svpSceneUs.load(std::memory_order_relaxed) / (1000.0 * calls) : 0.0,
                svpBadCoverageMilli.load(std::memory_order_relaxed) / 1000.0,
                svpSceneFallbacks.load(std::memory_order_relaxed));
        } else if (detailed) {
            const auto calls = nvofCallCount.load(std::memory_order_relaxed);
            const auto images = imageSceneAnalyses.load(std::memory_order_relaxed);
            const auto dimensions = nvofAnalysisDimensions.load(std::memory_order_relaxed);
            const auto mode = lastSceneDiagnosticMode.load(std::memory_order_relaxed);
            const auto geometry = owner->m_Sets.iRifeSceneDetection == RIFE_SCENE_NVOF && dimensions
                    && (mode == SceneDiagNvofOverlap || mode == SceneDiagNvofSerialized)
                ? std::format(L" {}x{}", dimensions >> 32, dimensions & 0xffffffff) : L"";
            diagnostics += std::format(L"\nRIFE analysis : NVOF{} {:.2f}, image {:.2f} ms avg ({}), fallback {}",
                geometry,
                calls ? nvofCallUs.load(std::memory_order_relaxed) / (1000.0 * calls) : 0.0,
                images ? imageSceneUs.load(std::memory_order_relaxed) / (1000.0 * images) : 0.0,
                images ? (lastImageCompactReadback.load(std::memory_order_relaxed) ? L"GPU reduction" : L"CPU fallback") : L"n/a",
                imageSceneFallbacks.load(std::memory_order_relaxed));
        }
        if (variableTimingDetected.load(std::memory_order_relaxed)
                || owner->m_Sets.iRifeDuplicateRemoval != RIFE_DUPLICATES_Keep) {
            diagnostics += std::format(L"\nRIFE source   : {:.3f} fps, retained {:.3f} fps, {}",
                sourceFpsMilli.load(std::memory_order_relaxed) / 1000.0,
                retainedFpsMilli.load(std::memory_order_relaxed) / 1000.0,
                variableTimingDetected.load(std::memory_order_relaxed) ? L"VFR" : L"CFR");
        }
        if (ruleIndex >= 0) {
            diagnostics += std::format(L"\nRIFE rule     : #{}", ruleIndex + 1);
            const auto multiplier = ruleMaxMultiplierMilli.load(std::memory_order_relaxed);
            const auto fps = ruleMaxOutputFpsMilli.load(std::memory_order_relaxed);
            if (multiplier) diagnostics += std::format(L" - max {:.3g}x", multiplier / 1000.0);
            if (fps) diagnostics += std::format(L" - max {:.3f} fps", fps / 1000.0);
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

        const auto presentReadyFront = [&]() -> bool {
            if (pendingPairs.empty()) {
                return false;
            }
            auto job = pendingPairs.front();
            const size_t presentedBefore = job ? job->presentedResults : 0;
            if (!stop.load(std::memory_order_acquire) && job && job->second && IsCurrent(*job->second)) {
                PresentPairJob(*job);
            } else if (job) {
                job->presentedResults = job->done.load(std::memory_order_acquire)
                    ? job->results.size()
                    : std::min(job->readyResults.load(std::memory_order_acquire), job->results.size());
            }
            const bool complete = job && job->done.load(std::memory_order_acquire)
                && job->presentedResults >= job->results.size();
            if (complete) {
                pendingPairs.pop_front();
            }
            return complete || (job && job->presentedResults != presentedBefore);
        };

        const auto waitForFront = [&](const bool present) {
            if (pendingPairs.empty()) return;
            auto job = pendingPairs.front();
            while (!job->done.load(std::memory_order_acquire)) {
                if (present && !stop.load(std::memory_order_acquire)
                        && job->second && IsCurrent(*job->second)) {
                    PresentPairJob(*job);
                }
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] {
                    return job->done.load(std::memory_order_acquire)
                        || job->readyResults.load(std::memory_order_acquire) > job->presentedResults;
                });
            }
            if (present && !stop.load(std::memory_order_acquire)
                    && job->second && IsCurrent(*job->second)) {
                PresentPairJob(*job);
            }
            pendingPairs.pop_front();
        };

        const auto drainPending = [&](const bool present) {
            while (!pendingPairs.empty()) {
                waitForFront(present);
            }
        };

        while (!stop.load(std::memory_order_acquire)) {
            while (presentReadyFront()) {
            }

            const uint64_t currentSerial = resetSerial.load(std::memory_order_acquire);
            if (currentSerial != activeSerial) {
                drainPending(false);
                DrainAllRetainedInputs();
                previous.reset();
                activeSerial = currentSerial;
                nextWorker = 0;
                const bool hardReset = hardResetPending.exchange(false, std::memory_order_acq_rel);
                ResetSequenceState(!hardReset
                    && preserveAdaptiveForResume.load(std::memory_order_acquire));
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
                            && (pendingPairs.front()->done.load(std::memory_order_acquire)
                                || pendingPairs.front()->readyResults.load(std::memory_order_acquire)
                                    > pendingPairs.front()->presentedResults))
                        || (canConsumeSource && !queue.empty());
                });
                if (stop.load(std::memory_order_acquire)) {
                    break;
                }
                if (resetSerial.load(std::memory_order_acquire) != activeSerial
                        || (!pendingPairs.empty()
                            && (pendingPairs.front()->done.load(std::memory_order_acquire)
                                || pendingPairs.front()->readyResults.load(std::memory_order_acquire)
                                    > pendingPairs.front()->presentedResults))) {
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
                    alternateSkippedFrames.fetch_add(1, std::memory_order_relaxed);
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
            const FrameRate sourceRate = currentFrame->frameDuration > 0
                ? RetainedSourceRate(SourceRateFromDuration(currentFrame->frameDuration),
                    currentFrame->settings.iRifeDuplicateRemoval == RIFE_DUPLICATES_RemoveEveryOther ? 2u : 1u)
                : SourceRateFromDuration(currentFrame->time - previous->time);
            ID3D11Device* device = currentFrame->processor
                ? currentFrame->processor->GetRifeDevice() : nullptr;
            D3D11_TEXTURE2D_DESC desc = {};
            if (currentFrame->texture) currentFrame->texture->GetDesc(&desc);
            if (!device || !desc.Width || !desc.Height) {
                drainPending(true);
                UpdateAdaptiveLoadLimit(*currentFrame, sourceRate, pendingPairs.size(), false);
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
                    || runtimeKey->model != currentFrame->settings.iRifeModel
                    || runtimeKey->format != desc.Format
                    || runtimeKey->featureReuse != (currentFrame->settings.bRifeFeatureReuse
                        && (currentFrame->settings.iRifeModel == RIFE_MODEL_415_LITE
                            || currentFrame->settings.iRifeModel == RIFE_MODEL_425))
                    || runtimeKey->performanceBoost != currentFrame->settings.bRifePerformanceBoost);
            if (runtimeSettingsChanged) {
                // Retire all work using the previous runtime before switching a
                // setting that can create another registration cache for the
                // same D3D11 source pool.
                drainPending(true);
                DrainAllRetainedInputs();
                nextWorker = 0;
                ResetAdaptiveLoadState();
            }

            EnsureRuntimeBuild(*currentFrame, device, desc.Width, desc.Height);
            auto runtime = ReadyRuntime();
            // Evaluate source-only compilation separately from active RIFE,
            // and apply any new cap before scheduling this pair's targets.
            UpdateAdaptiveLoadLimit(*currentFrame, sourceRate, pendingPairs.size(), runtime != nullptr);
            const auto targets = scheduler.Schedule(previous->time, currentFrame->time, sourceRate);
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
            job->results.resize(targets.size());
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
    REFERENCE_TIME& frameDuration)
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

void CRifePlaybackPipeline::Suspend() noexcept
{
    if (m_impl) {
        m_impl->ResetNonBlocking(true);
    }
}

std::wstring CRifePlaybackPipeline::GetDiagnostics(const bool detailed) const
{
    return m_impl ? (detailed ? m_impl->Diagnostics() : m_impl->CompactDiagnostics()) : std::wstring();
}
