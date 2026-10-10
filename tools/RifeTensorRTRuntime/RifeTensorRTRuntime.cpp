#include "../../Source/RifeRuntimeApi.h"
#include "../../Source/RifeCudaOutputApi.h"
#include "RifeKernels.h"
#include "PackedInputReuse.h"
#include "RifeRequestCompatibility.h"
#include "D3D11InteropLock.h"
#include "EngineCacheFile.h"
#include "EngineProfile.h"
#include "RuntimeCreation.h"

#include <NvInfer.h>
#include <NvInferRuntime.h>
#include <NvOnnxParser.h>
#include <cuda_d3d11_interop.h>
#include <cuda_runtime.h>
#include <bcrypt.h>

#include <Windows.h>
#include <d3d11.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr uint32_t kMaxContexts = 3;
constexpr uint32_t kPadMultiple = 32;

class InputResourceClaim final {
public:
    InputResourceClaim(
        std::mutex& mutex,
        std::condition_variable& cv,
        std::unordered_set<cudaGraphicsResource_t>& active,
        const std::array<cudaGraphicsResource_t, 2>& resources)
        : m_mutex(mutex)
        , m_cv(cv)
        , m_active(active)
        , m_resources(resources)
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [&] {
            return !m_active.contains(m_resources[0])
                && !m_active.contains(m_resources[1]);
        });
        m_active.insert(m_resources[0]);
        m_active.insert(m_resources[1]);
    }

    ~InputResourceClaim()
    {
        Release();
    }

    void Release()
    {
        if (m_released) return;
        {
            std::lock_guard lock(m_mutex);
            m_active.erase(m_resources[0]);
            m_active.erase(m_resources[1]);
        }
        m_released = true;
        m_cv.notify_all();
    }

    InputResourceClaim(const InputResourceClaim&) = delete;
    InputResourceClaim& operator=(const InputResourceClaim&) = delete;

private:
    std::mutex& m_mutex;
    std::condition_variable& m_cv;
    std::unordered_set<cudaGraphicsResource_t>& m_active;
    std::array<cudaGraphicsResource_t, 2> m_resources{};
    bool m_released = false;
};

std::filesystem::path ThisModuleDirectory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ThisModuleDirectory), &module) || !module) {
        return {};
    }

    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return {};
    return std::filesystem::path(buffer.data()).parent_path();
}

uint32_t RoundUp(uint32_t value, uint32_t multiple)
{
    return (value + multiple - 1) / multiple * multiple;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto end = file.tellg();
    if (end <= 0) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) return {};
    return bytes;
}

std::string Sha256Hex(const std::vector<uint8_t>& bytes)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD digestLength = 0;
    DWORD cb = 0;
    std::vector<uint8_t> object;
    std::vector<uint8_t> digest;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) goto fail;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &cb, 0) < 0) goto fail;
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength), &cb, 0) < 0) goto fail;
    object.resize(objectLength);
    digest.resize(digestLength);
    if (BCryptCreateHash(alg, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) goto fail;
    if (BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0) goto fail;
    if (BCryptFinishHash(hash, digest.data(), digestLength, 0) < 0) goto fail;

    {
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (const auto byte : digest) out << std::setw(2) << static_cast<unsigned>(byte);
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return out.str();
    }

fail:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return {};
}

class Logger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* message) noexcept override
    {
        if (severity > Severity::kWARNING || !message) return;
        std::string line = "MPCVR RIFE TensorRT: ";
        line += message;
        line += "\n";
        OutputDebugStringA(line.c_str());
    }
};

Logger g_logger;

template <class T>
using TrtPtr = std::unique_ptr<T>;

size_t DataTypeBytes(nvinfer1::DataType type)
{
    switch (type) {
    case nvinfer1::DataType::kFLOAT: return 4;
    case nvinfer1::DataType::kHALF: return 2;
    default: return 0;
    }
}

size_t Volume(const nvinfer1::Dims& dims)
{
    size_t volume = 1;
    for (int i = 0; i < dims.nbDims; ++i) {
        if (dims.d[i] <= 0) return 0;
        volume *= static_cast<size_t>(dims.d[i]);
    }
    return volume;
}

std::string Sanitize(std::string text)
{
    for (char& c : text) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            c = '_';
        }
    }
    return text;
}

#include "FeatureReuseEngine.h"

struct ContextState {
    TrtPtr<nvinfer1::IExecutionContext> context;
    std::unique_ptr<FeatureReuseEngine::Context> features;
    cudaStream_t stream = nullptr;
    cudaStream_t inputReleaseStream = nullptr;
    cudaGraph_t tensorRtGraph = nullptr;
    cudaGraphExec_t tensorRtGraphExec = nullptr;
    cudaEvent_t preMapEvent = nullptr;
    cudaEvent_t startEvent = nullptr;
    cudaEvent_t packEndEvent = nullptr;
    cudaEvent_t inputReleasedEvent = nullptr;
    cudaEvent_t trtStartEvent = nullptr;
    cudaEvent_t trtEndEvent = nullptr;
    cudaEvent_t endEvent = nullptr;
    void* input = nullptr;
    void* output = nullptr;
    bool inputReleasePending = false;
    PackedInputReuse packedInput;

    ~ContextState()
    {
        if (tensorRtGraphExec) cudaGraphExecDestroy(tensorRtGraphExec);
        if (tensorRtGraph) cudaGraphDestroy(tensorRtGraph);
        features.reset();
        context.reset();
        if (output) cudaFree(output);
        if (input) cudaFree(input);
        if (endEvent) cudaEventDestroy(endEvent);
        if (trtEndEvent) cudaEventDestroy(trtEndEvent);
        if (trtStartEvent) cudaEventDestroy(trtStartEvent);
        if (inputReleasedEvent) cudaEventDestroy(inputReleasedEvent);
        if (packEndEvent) cudaEventDestroy(packEndEvent);
        if (startEvent) cudaEventDestroy(startEvent);
        if (preMapEvent) cudaEventDestroy(preMapEvent);
        if (inputReleaseStream) cudaStreamDestroy(inputReleaseStream);
        if (stream) cudaStreamDestroy(stream);
    }
};

class GraphicsRegistrationCache {
public:
    ~GraphicsRegistrationCache() { Clear(); }

    cudaGraphicsResource_t Get(ID3D11Texture2D* texture, bool writable)
    {
        if (!texture) return nullptr;
        auto& map = writable ? m_outputs : m_inputs;
        const auto it = map.find(texture);
        if (it != map.end()) return it->second;

        cudaGraphicsResource_t resource = nullptr;
        // CUDA's D3D11 registration API does not accept
        // cudaGraphicsRegisterFlagsReadOnly. Inputs use the default registration
        // mode; only outputs need surface load/store access for the CUDA kernel.
        const unsigned flags = writable ? cudaGraphicsRegisterFlagsSurfaceLoadStore : cudaGraphicsRegisterFlagsNone;
        if (cudaGraphicsD3D11RegisterResource(&resource, texture, flags) != cudaSuccess) return nullptr;
        texture->AddRef();
        map.emplace(texture, resource);
        return resource;
    }

    void Clear()
    {
        ClearMap(m_inputs);
        ClearMap(m_outputs);
    }

private:
    static void ClearMap(std::unordered_map<ID3D11Texture2D*, cudaGraphicsResource_t>& map)
    {
        for (auto& [texture, resource] : map) {
            if (resource) cudaGraphicsUnregisterResource(resource);
            if (texture) texture->Release();
        }
        map.clear();
    }

    std::unordered_map<ID3D11Texture2D*, cudaGraphicsResource_t> m_inputs;
    std::unordered_map<ID3D11Texture2D*, cudaGraphicsResource_t> m_outputs;
};

class RifeRuntime {
public:
    ~RifeRuntime()
    {
        if (m_cudaDevice >= 0) cudaSetDevice(m_cudaDevice);
        for (uint32_t i = 0; i < m_contexts.size(); ++i) {
            DrainContext(i);
        }
        {
            D3D11InteropLock interopLock(m_d3dMultithread);
            m_registrations.Clear();
        }
        m_contexts.clear();
        for (auto& output : m_cudaOutputs) {
            if (output.stream) cudaStreamSynchronize(output.stream);
            if (output.pixels) cudaFree(output.pixels);
            if (output.stream) cudaStreamDestroy(output.stream);
        }
        m_features.reset();
        m_engine.reset();
        m_runtime.reset();
        if (m_d3dMultithread) m_d3dMultithread->Release();
        if (m_device) m_device->Release();
    }

    int Initialize(const MpcvrRifeCreateParams& params)
    {
        if (!params.device || !params.modelPath || !params.cachePath
                || params.width == 0 || params.height == 0
                || params.contentWidth == 0 || params.contentHeight == 0) {
            return MPCVR_RIFE_INVALID_ARGUMENT;
        }

        m_contentWidth = params.contentWidth;
        m_contentHeight = params.contentHeight;
        m_paddedWidth = params.width;
        m_paddedHeight = params.height;
        // The renderer owns the model-specific alignment policy. Most bundled
        // models use 32 px, while RIFE 4.25 Lite requires 128 px. The runtime
        // only requires a surface large enough for the logical content and
        // aligned to the CUDA kernel's 32-pixel base granularity.
        if (m_paddedWidth < m_contentWidth || m_paddedHeight < m_contentHeight
                || m_paddedWidth % kPadMultiple != 0
                || m_paddedHeight % kPadMultiple != 0) {
            return MPCVR_RIFE_INVALID_ARGUMENT;
        }
        m_contextCount = std::clamp(params.contextCount, 1u, kMaxContexts);
        m_performanceBoost = params.performanceBoost != 0;
        m_deferInputRelease = params.size >= sizeof(MpcvrRifeCreateParams)
            && (params.flags & MPCVR_RIFE_CREATE_DEFER_INPUT_RELEASE) != 0;
        m_highPrecision = params.size >= sizeof(MpcvrRifeCreateParams)
            && (params.flags & MPCVR_RIFE_CREATE_HIGH_PRECISION) != 0;
        m_modelPath = params.modelPath;
        m_cachePath = params.cachePath;
        m_device = params.device;
        m_device->AddRef();

        ID3D11DeviceContext* immediate = nullptr;
        m_device->GetImmediateContext(&immediate);
        if (!immediate) return MPCVR_RIFE_UNSUPPORTED;
        const HRESULT threadingResult = immediate->QueryInterface(IID_PPV_ARGS(&m_d3dMultithread));
        immediate->Release();
        if (FAILED(threadingResult) || !m_d3dMultithread) return MPCVR_RIFE_UNSUPPORTED;
        m_d3dMultithread->SetMultithreadProtected(TRUE);

        const auto runtimeDirectory = ThisModuleDirectory();
        if (runtimeDirectory.empty()) return MPCVR_RIFE_TENSORRT_FAILURE;
        const std::string internalLibraryPath = runtimeDirectory.string();
        if (!nvinfer1::setInternalLibraryPath(internalLibraryPath.c_str())) {
            return MPCVR_RIFE_TENSORRT_FAILURE;
        }

        unsigned cudaCount = 0;
        std::array<int, 8> cudaDevices{};
        const auto d3dResult = cudaD3D11GetDevices(&cudaCount, cudaDevices.data(), static_cast<unsigned>(cudaDevices.size()),
            params.device, cudaD3D11DeviceListAll);
        if (d3dResult != cudaSuccess || cudaCount == 0) return MPCVR_RIFE_UNSUPPORTED;

        m_cudaDevice = cudaDevices[0];
        if (params.gpuIndex != UINT32_MAX && params.gpuIndex != static_cast<uint32_t>(m_cudaDevice)) {
            return MPCVR_RIFE_UNSUPPORTED;
        }
        if (cudaSetDevice(m_cudaDevice) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;

        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, m_cudaDevice) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
        m_gpuName = prop.name;
        m_computeMajor = prop.major;
        m_computeMinor = prop.minor;

        const bool supportedComputeCapability =
            (prop.major == 7 && prop.minor == 5) ||
            (prop.major == 8 && prop.minor == 6) ||
            (prop.major == 8 && prop.minor == 9) ||
            (prop.major == 12 && prop.minor == 0);
        if (!supportedComputeCapability) {
            return MPCVR_RIFE_UNSUPPORTED_COMPUTE_CAPABILITY;
        }

        const auto builder = runtimeDirectory /
            std::filesystem::path(std::format(
                L"nvinfer_builder_resource_sm{}{}_11.dll", prop.major, prop.minor));
        std::error_code builderError;
        if (!std::filesystem::exists(builder, builderError)) {
            return MPCVR_RIFE_BUILDER_RESOURCE_MISSING;
        }

        m_initializationFailure = MPCVR_RIFE_TENSORRT_FAILURE;
        if (!LoadOrBuildEngine()) return m_initializationFailure;
        if (params.size >= sizeof(MpcvrRifeCreateParams)
                && (params.flags & MPCVR_RIFE_CREATE_FEATURE_REUSE) && m_inputIsFp16 && m_outputIsFp16) {
            auto features = std::make_unique<FeatureReuseEngine>();
            const auto hash = Sha256Hex(ReadFile(m_modelPath));
            if (hash.size() >= 16 && features->Initialize(m_modelPath, m_cachePath, hash,
                    BuildCacheKey(hash), m_paddedWidth, m_paddedHeight)) {
                m_engineBytes += features->EngineBytes();
                m_features = std::move(features);
            }
        }
        if (!PrepareContexts()) {
            if (!m_features) return MPCVR_RIFE_TENSORRT_FAILURE;
            m_contexts.clear();
            m_contextMutexes.clear();
            m_features.reset();
            if (!PrepareContexts()) return MPCVR_RIFE_TENSORRT_FAILURE;
        }
        return MPCVR_RIFE_OK;
    }

    int Interpolate(const MpcvrRifeRequest& request, MpcvrRifeStats& stats,
        void* linearOutput = nullptr, size_t linearPitch = 0)
    {
        using Clock = std::chrono::steady_clock;
        const auto runtimeStart = Clock::now();
        const auto elapsedMs = [](const Clock::time_point start, const Clock::time_point end) {
            return std::chrono::duration<double, std::milli>(end - start).count();
        };
        if (request.contextIndex >= m_contexts.size()) return MPCVR_RIFE_INVALID_ARGUMENT;

        std::unique_lock contextLock(*m_contextMutexes[request.contextIndex], std::defer_lock);
        const auto contextLockStart = Clock::now();
        contextLock.lock();
        stats.contextLockWaitMs = elapsedMs(contextLockStart, Clock::now());
        auto& state = *m_contexts[request.contextIndex];
        const uint64_t inputPairId = request.size >= sizeof(MpcvrRifeRequest) ? request.inputPairId : 0;
        const bool reusePackedInput = state.packedInput.Begin(inputPairId, request.first, request.second);
        const bool linear = linearOutput != nullptr;
        if (linear && m_highPrecision) return MPCVR_RIFE_UNSUPPORTED;
        if (!request.first || !request.second || (!linear && !request.output)
                || !(request.timestep > 0.0f && request.timestep < 1.0f)) {
            return MPCVR_RIFE_INVALID_ARGUMENT;
        }
        if (!CudaTextureCompatible(request.first) || !CudaTextureCompatible(request.second)
                || (!linear && !CudaTextureCompatible(request.output))) return MPCVR_RIFE_INVALID_ARGUMENT;
        const auto setDeviceStart = Clock::now();
        if (cudaSetDevice(m_cudaDevice) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
        stats.cudaSetDeviceMs = elapsedMs(setDeviceStart, Clock::now());

        // In deferred-release mode, the previous request on this context may
        // still be finishing the D3D11/CUDA ownership transition for its input
        // textures. Contexts are round-robin and own disjoint staged inputs, so
        // waiting here gives that transition the entire intervening frame to
        // complete instead of charging it to the request that launched it.
        if (m_deferInputRelease && state.inputReleasePending) {
            const auto inputReleaseStart = Clock::now();
            const cudaError_t inputReleaseResult = cudaEventSynchronize(state.inputReleasedEvent);
            stats.inputReleaseSyncMs = elapsedMs(inputReleaseStart, Clock::now());
            if (inputReleaseResult != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
            state.inputReleasePending = false;
        }

        std::array<cudaGraphicsResource_t, 2> inputResources{};
        cudaGraphicsResource_t outputResource = nullptr;
        const auto registrationStart = Clock::now();
        {
            D3D11InteropLock interopLock(m_d3dMultithread);
            if (!reusePackedInput) {
                inputResources = {m_registrations.Get(request.first, false),
                    m_registrations.Get(request.second, false)};
            }
            if (!linear) outputResource = m_registrations.Get(request.output, true);
        }
        stats.registrationMs = elapsedMs(registrationStart, Clock::now());
        if ((!reusePackedInput && (!inputResources[0] || !inputResources[1])) || (!linear && !outputResource)) {
            return MPCVR_RIFE_CUDA_FAILURE;
        }

        const auto inputPackLockStart = Clock::now();
        std::optional<InputResourceClaim> inputResourceClaim;
        if (!reusePackedInput) {
            inputResourceClaim.emplace(
                m_inputResourceMutex, m_inputResourceCv, m_activeInputResources, inputResources);
        }
        stats.inputPackLockWaitMs = elapsedMs(inputPackLockStart, Clock::now());

        // Deferred mode retains both staged inputs through inference, so all
        // three resources can share one D3D11/CUDA ownership transition.
        const bool batchDeferred = m_deferInputRelease && !reusePackedInput && !linear;
        std::array<cudaGraphicsResource_t, 3> batchResources{
            inputResources[0], inputResources[1], outputResource};
        bool inputsMapped = false;
        bool outputMapped = false;
        cudaTextureObject_t firstTexture = 0;
        cudaTextureObject_t secondTexture = 0;
        cudaSurfaceObject_t outputSurface = 0;

        auto releaseInputs = [&](cudaStream_t stream) -> cudaError_t {
            if (!inputsMapped) return cudaSuccess;
            const auto unmapStart = Clock::now();
            cudaError_t unmapResult;
            {
                D3D11InteropLock interopLock(m_d3dMultithread);
                unmapResult = cudaGraphicsUnmapResources(
                    static_cast<int>(inputResources.size()), inputResources.data(), stream);
            }
            inputsMapped = false;
            stats.inputUnmapMs = elapsedMs(unmapStart, Clock::now());
            return unmapResult;
        };

        auto releaseOutput = [&]() -> cudaError_t {
            if (!outputMapped) return cudaSuccess;
            const auto unmapStart = Clock::now();
            cudaError_t unmapResult;
            {
                D3D11InteropLock interopLock(m_d3dMultithread);
                unmapResult = cudaGraphicsUnmapResources(
                    1, &outputResource, state.stream);
            }
            outputMapped = false;
            stats.outputUnmapMs = elapsedMs(unmapStart, Clock::now());
            return unmapResult;
        };

        auto releaseBatch = [&]() -> cudaError_t {
            const auto unmapStart = Clock::now();
            cudaError_t result;
            {
                D3D11InteropLock interopLock(m_d3dMultithread);
                result = cudaGraphicsUnmapResources(
                    static_cast<int>(batchResources.size()), batchResources.data(), state.stream);
            }
            inputsMapped = false;
            outputMapped = false;
            // Combined call is counted once, under output ownership release.
            stats.outputUnmapMs = elapsedMs(unmapStart, Clock::now());
            return result;
        };

        auto destroyCudaViews = [&]() -> cudaError_t {
            cudaError_t destroyResult = cudaSuccess;
            if (outputSurface) {
                const cudaError_t result = cudaDestroySurfaceObject(outputSurface);
                if (destroyResult == cudaSuccess && result != cudaSuccess) destroyResult = result;
                outputSurface = 0;
            }
            if (secondTexture) {
                const cudaError_t result = cudaDestroyTextureObject(secondTexture);
                if (destroyResult == cudaSuccess && result != cudaSuccess) destroyResult = result;
                secondTexture = 0;
            }
            if (firstTexture) {
                const cudaError_t result = cudaDestroyTextureObject(firstTexture);
                if (destroyResult == cudaSuccess && result != cudaSuccess) destroyResult = result;
                firstTexture = 0;
            }
            return destroyResult;
        };

        auto finishStream = [&]() -> cudaError_t {
            // Any failed request must hand ownership back before its resource
            // claim and source-pool references can be released.
            const cudaError_t inputReleaseResult = releaseInputs(state.stream);
            const cudaError_t outputReleaseResult = releaseOutput();
            const auto syncStart = Clock::now();
            const cudaError_t syncResult = cudaStreamSynchronize(state.stream);
            const cudaError_t inputReleaseSyncResult = cudaStreamSynchronize(state.inputReleaseStream);
            stats.handoffSyncMs = elapsedMs(syncStart, Clock::now());
            const cudaError_t destroyResult = destroyCudaViews();
            if (syncResult != cudaSuccess) return syncResult;
            if (inputReleaseSyncResult != cudaSuccess) return inputReleaseSyncResult;
            if (inputReleaseResult != cudaSuccess) return inputReleaseResult;
            if (outputReleaseResult != cudaSuccess) return outputReleaseResult;
            return destroyResult;
        };

        cudaArray_t firstArray = nullptr;
        cudaArray_t secondArray = nullptr;
        if (!reusePackedInput) {
            // MPC-VR normally passes role-specific staged input copies, so
            // adjacent A/B and B/C inference contexts can map and pack
            // concurrently. Keep the CUDA graphics-resource rule intact for any
            // caller that actually reuses an input texture across concurrent
            // requests by claiming only the two resources used by this request.
            if (cudaEventRecord(state.preMapEvent, state.stream) != cudaSuccess) {
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            const auto inputMapStart = Clock::now();
            {
                D3D11InteropLock interopLock(m_d3dMultithread);
                if (cudaGraphicsMapResources(
                        batchDeferred ? static_cast<int>(batchResources.size()) : static_cast<int>(inputResources.size()),
                        batchDeferred ? batchResources.data() : inputResources.data(), state.stream) != cudaSuccess) {
                    return MPCVR_RIFE_CUDA_FAILURE;
                }
            }
            inputsMapped = true;
            outputMapped = batchDeferred;
            stats.inputMapMs = elapsedMs(inputMapStart, Clock::now());

            if (cudaGraphicsSubResourceGetMappedArray(&firstArray, inputResources[0], 0, 0) != cudaSuccess ||
                    cudaGraphicsSubResourceGetMappedArray(&secondArray, inputResources[1], 0, 0) != cudaSuccess) {
                releaseInputs(state.stream);
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            if (cudaEventRecord(state.startEvent, state.stream) != cudaSuccess) {
                releaseInputs(state.stream);
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            const auto packHostStart = Clock::now();
            if (MpcvrRifePackInput(firstArray, secondArray, state.input, m_inputIsFp16,
                    static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight),
                    static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight),
                    request.timestep, state.stream, &firstTexture, &secondTexture, m_highPrecision) != cudaSuccess) {
                releaseInputs(state.stream);
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            stats.packHostMs = elapsedMs(packHostStart, Clock::now());
            if (cudaEventRecord(state.packEndEvent, state.stream) != cudaSuccess) {
                releaseInputs(state.stream);
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            // Generic callers still need their input ownership returned before
            // Interpolate() exits. The renderer's deferred mode intentionally
            // keeps the staged input textures mapped through inference instead:
            // on D3D11, an asynchronous cudaGraphicsUnmapResources() running on
            // this second stream can serialize unrelated graphics interop and
            // starve the main TensorRT/output stream. Deferred mode queues that
            // ownership transition only after the current output is handed back.
            if (!m_deferInputRelease) {
                if (cudaStreamWaitEvent(state.inputReleaseStream, state.packEndEvent, 0) != cudaSuccess) {
                    releaseInputs(state.stream);
                    finishStream();
                    return MPCVR_RIFE_CUDA_FAILURE;
                }
                if (releaseInputs(state.inputReleaseStream) != cudaSuccess) {
                    finishStream();
                    return MPCVR_RIFE_CUDA_FAILURE;
                }
                if (cudaEventRecord(state.inputReleasedEvent, state.inputReleaseStream) != cudaSuccess) {
                    finishStream();
                    return MPCVR_RIFE_CUDA_FAILURE;
                }
            }
        }

        if (reusePackedInput) {
            // The context owns an ordinary CUDA tensor, not a mapped D3D view.
            // Its previous inference completed before this call. Refresh only
            // timestep channel 6 on the same stream before TensorRT consumes it.
            if (cudaEventRecord(state.preMapEvent, state.stream) != cudaSuccess
                    || cudaEventRecord(state.startEvent, state.stream) != cudaSuccess) {
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            const auto packHostStart = Clock::now();
            if (MpcvrRifeUpdateTimestep(state.input, m_inputIsFp16,
                    static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight),
                    static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight),
                    request.timestep, state.stream) != cudaSuccess) {
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            stats.packHostMs = elapsedMs(packHostStart, Clock::now());
            if (cudaEventRecord(state.packEndEvent, state.stream) != cudaSuccess) {
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
        }

        if (!linear && !outputMapped) {
            const auto outputMapStart = Clock::now();
            {
                D3D11InteropLock interopLock(m_d3dMultithread);
                if (cudaGraphicsMapResources(1, &outputResource, state.stream) != cudaSuccess) {
                    finishStream();
                    return MPCVR_RIFE_CUDA_FAILURE;
                }
            }
            stats.outputMapMs = elapsedMs(outputMapStart, Clock::now());
            outputMapped = true;
        }

        cudaArray_t outputArray = nullptr;
        if (!linear && cudaGraphicsSubResourceGetMappedArray(&outputArray, outputResource, 0, 0) != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_CUDA_FAILURE;
        }

        if (cudaEventRecord(state.trtStartEvent, state.stream) != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_TENSORRT_FAILURE;
        }
        const auto tensorRtSubmitStart = Clock::now();
        const bool graphUsed = state.tensorRtGraphExec != nullptr;
        const bool featureSubmitted = !state.features || reusePackedInput || state.features->Enqueue(state.stream);
        const bool tensorRtSubmitted = featureSubmitted && (graphUsed
            ? cudaGraphLaunch(state.tensorRtGraphExec, state.stream) == cudaSuccess
            : state.context->enqueueV3(state.stream));
        stats.tensorRtSubmitMs = elapsedMs(tensorRtSubmitStart, Clock::now());
        stats.tensorRtGraphUsed = graphUsed ? 1u : 0u;
        if (!tensorRtSubmitted
                || cudaEventRecord(state.trtEndEvent, state.stream) != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_TENSORRT_FAILURE;
        }

        const auto writeHostStart = Clock::now();
        const cudaError_t writeResult = linear
            ? MpcvrRifeWriteLinearOutput(state.output, m_outputIsFp16, linearOutput, linearPitch,
                static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight),
                static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight), state.stream)
            : MpcvrRifeWriteOutput(state.output, m_outputIsFp16, outputArray,
                static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight),
                static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight), state.stream, &outputSurface, m_highPrecision);
        if (writeResult != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_CUDA_FAILURE;
        }

        stats.writeHostMs = elapsedMs(writeHostStart, Clock::now());
        if (cudaEventRecord(state.endEvent, state.stream) != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_CUDA_FAILURE;
        }

        // The renderer's role-specific staged inputs are disjoint across
        // parallel contexts. In deferred mode it retains their source-pool slots
        // until this context is invoked again, so the ownership transition may
        // finish asynchronously. Generic callers keep the original synchronous
        // behavior.
        if (!m_deferInputRelease && !reusePackedInput) {
            const auto inputReleaseStart = Clock::now();
            const cudaError_t inputReleaseResult = cudaEventSynchronize(state.inputReleasedEvent);
            stats.inputReleaseSyncMs = elapsedMs(inputReleaseStart, Clock::now());
            if (inputReleaseResult != cudaSuccess) {
                releaseOutput();
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
        }
        if (!m_deferInputRelease) {
            if (inputResourceClaim) inputResourceClaim->Release();
        }

        // We only need to wait until the RIFE kernels have finished using the
        // texture/surface objects. Do not synchronize the whole stream after
        // cudaGraphicsUnmapResources(): CUDA graphics interop already guarantees
        // that work issued before the unmap completes before later D3D11 work
        // begins. Waiting through the ownership transition here serializes the
        // inference worker with the graphics queue and defeats parallel contexts.
        const auto completionStart = Clock::now();

        // Split the wait before RIFE begins into two device-visible points:
        // preMapEvent is queued before cudaGraphicsMapResources(), while
        // startEvent is queued after the input resources have been mapped.
        // This distinguishes general CUDA/WDDM stream scheduling delay from
        // D3D11 -> CUDA ownership acquisition delay.
        const auto startWaitStart = Clock::now();
        const cudaError_t initialStartQueryResult = cudaEventQuery(state.startEvent);
        const cudaError_t preMapQueryResult = cudaEventQuery(state.preMapEvent);
        stats.handoffStartReady = initialStartQueryResult == cudaSuccess ? 1u : 0u;
        stats.handoffPreMapReady = preMapQueryResult == cudaSuccess ? 1u : 0u;
        cudaError_t preMapWaitResult = preMapQueryResult;
        const auto preMapWaitStart = Clock::now();
        if (preMapQueryResult == cudaErrorNotReady) {
            preMapWaitResult = cudaEventSynchronize(state.preMapEvent);
        }
        stats.handoffPreMapWaitMs = elapsedMs(preMapWaitStart, Clock::now());
        if (preMapWaitResult != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_CUDA_FAILURE;
        }

        cudaError_t startWaitResult = initialStartQueryResult;
        const auto mapWaitStart = Clock::now();
        if (initialStartQueryResult == cudaErrorNotReady) {
            startWaitResult = cudaEventSynchronize(state.startEvent);
        }
        stats.handoffMapWaitMs = elapsedMs(mapWaitStart, Clock::now());
        stats.handoffStartWaitMs = elapsedMs(startWaitStart, Clock::now());
        if (startWaitResult != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_CUDA_FAILURE;
        }

        const auto endWaitStart = Clock::now();
        const cudaError_t completionResult = cudaEventSynchronize(state.endEvent);
        stats.handoffEndWaitMs = elapsedMs(endWaitStart, Clock::now());
        stats.handoffSyncMs = elapsedMs(completionStart, Clock::now());
        if (completionResult != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_CUDA_FAILURE;
        }
        const cudaError_t destroyResult = destroyCudaViews();
        const cudaError_t releaseResult = batchDeferred ? releaseBatch() : releaseOutput();
        if (destroyResult != cudaSuccess || releaseResult != cudaSuccess) {
            releaseInputs(state.stream);
            finishStream();
            return MPCVR_RIFE_CUDA_FAILURE;
        }

        if (m_deferInputRelease && !reusePackedInput) {
            // Batched mode has queued both ownership transitions together.
            // Record completion without waiting; the renderer retains these
            // source slots until this context advances or explicitly drains.
            if (releaseInputs(state.stream) != cudaSuccess
                    || cudaEventRecord(state.inputReleasedEvent, state.stream) != cudaSuccess) {
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            state.inputReleasePending = true;
            if (inputResourceClaim) inputResourceClaim->Release();
        }

        float elapsed = 0.0f;
        if (cudaEventElapsedTime(&elapsed, state.startEvent, state.endEvent) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
        stats.inferenceMs = elapsed;
        float stageElapsed = 0.0f;
        if (cudaEventElapsedTime(&stageElapsed, state.startEvent, state.packEndEvent) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
        stats.inputPackMs = stageElapsed;
        if (cudaEventElapsedTime(&stageElapsed, state.trtStartEvent, state.trtEndEvent) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
        stats.tensorRtMs = stageElapsed;
        if (cudaEventElapsedTime(&stageElapsed, state.trtEndEvent, state.endEvent) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
        stats.outputWriteMs = stageElapsed;
        stats.engineBytes = m_engineBytes;
        stats.totalRuntimeMs = elapsedMs(runtimeStart, Clock::now());
        state.packedInput.Commit(inputPairId, request.first, request.second);
        stats.inputPairReuse = reusePackedInput ? 2u : 1u;
        stats.featureReuse = state.features ? (reusePackedInput ? 2u : 1u) : 0u;
        return MPCVR_RIFE_OK;
    }

    int DrainContext(const uint32_t contextIndex)
    {
        if (contextIndex >= m_contexts.size()) return MPCVR_RIFE_INVALID_ARGUMENT;
        std::unique_lock contextLock(*m_contextMutexes[contextIndex]);
        auto& state = *m_contexts[contextIndex];
        state.packedInput.Invalidate();
        if (cudaSetDevice(m_cudaDevice) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
        if (!state.inputReleasePending) return MPCVR_RIFE_OK;
        if (cudaEventSynchronize(state.inputReleasedEvent) != cudaSuccess) {
            return MPCVR_RIFE_CUDA_FAILURE;
        }
        state.inputReleasePending = false;
        return MPCVR_RIFE_OK;
    }

    int AcquireCudaOutput(MpcvrRifeCudaOutput& descriptor)
    {
        if (cudaSetDevice(m_cudaDevice) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
        for (auto& output : m_cudaOutputs) {
            std::unique_lock lock(output.mutex, std::try_to_lock);
            if (!lock.owns_lock() || output.leaseId) continue;
            if (!output.pixels) {
                if (cudaMallocPitch(&output.pixels, &output.pitch,
                        static_cast<size_t>(m_paddedWidth) * 4, m_paddedHeight) != cudaSuccess) {
                    return MPCVR_RIFE_CUDA_FAILURE;
                }
                if (cudaStreamCreateWithFlags(&output.stream, cudaStreamNonBlocking) != cudaSuccess) {
                    cudaFree(output.pixels);
                    output.pixels = nullptr;
                    return MPCVR_RIFE_CUDA_FAILURE;
                }
            }
            const size_t index = static_cast<size_t>(&output - m_cudaOutputs.data());
            output.leaseId = m_nextCudaLease.fetch_add(1, std::memory_order_relaxed)
                * MPCVR_RIFE_CUDA_OUTPUT_SLOTS + index + 1;
            output.ready = false;
            descriptor = {};
            descriptor.leaseId = output.leaseId;
            descriptor.pixels = output.pixels;
            descriptor.pitch = output.pitch;
            descriptor.width = m_paddedWidth;
            descriptor.height = m_paddedHeight;
            descriptor.contentWidth = m_contentWidth;
            descriptor.contentHeight = m_contentHeight;
            descriptor.cudaDevice = static_cast<uint32_t>(m_cudaDevice);
            return MPCVR_RIFE_OK;
        }
        return MPCVR_RIFE_CUDA_OUTPUT_BUSY;
    }

    int InterpolateCuda(const MpcvrRifeRequest& request, uint64_t leaseId, MpcvrRifeStats& stats)
    {
        if (!leaseId || !CudaTextureCompatible(request.first) || !CudaTextureCompatible(request.second)) {
            return MPCVR_RIFE_INVALID_ARGUMENT;
        }
        auto& output = m_cudaOutputs[(leaseId - 1) % MPCVR_RIFE_CUDA_OUTPUT_SLOTS];
        {
            std::unique_lock lock(output.mutex);
            if (output.leaseId != leaseId) return MPCVR_RIFE_INVALID_ARGUMENT;
            if (output.ready) return MPCVR_RIFE_INVALID_ARGUMENT;
            // Own a BGRA copy until release, never lend TensorRT's output tensor.
            const int result = Interpolate(request, stats, output.pixels, output.pitch);
            output.ready = result == MPCVR_RIFE_OK;
            return result;
        }
        return MPCVR_RIFE_INVALID_ARGUMENT;
    }

    int ExportCudaOutput(uint64_t leaseId, ID3D11Texture2D* texture)
    {
        if (!leaseId || !CudaTextureCompatible(texture)) return MPCVR_RIFE_INVALID_ARGUMENT;
        auto& output = m_cudaOutputs[(leaseId - 1) % MPCVR_RIFE_CUDA_OUTPUT_SLOTS];
        {
            std::unique_lock lock(output.mutex);
            if (output.leaseId != leaseId) return MPCVR_RIFE_INVALID_ARGUMENT;
            if (!output.ready) return MPCVR_RIFE_INVALID_ARGUMENT;
            if (cudaSetDevice(m_cudaDevice) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
            cudaGraphicsResource_t resource = nullptr;
            {
                D3D11InteropLock interopLock(m_d3dMultithread);
                resource = m_registrations.Get(texture, true);
                if (!resource || cudaGraphicsMapResources(1, &resource, output.stream) != cudaSuccess) {
                    return MPCVR_RIFE_CUDA_FAILURE;
                }
            }
            cudaArray_t array = nullptr;
            cudaError_t result = cudaGraphicsSubResourceGetMappedArray(&array, resource, 0, 0);
            if (result == cudaSuccess) {
                result = cudaMemcpy2DToArrayAsync(array, 0, 0, output.pixels, output.pitch,
                    static_cast<size_t>(m_paddedWidth) * 4, m_paddedHeight, cudaMemcpyDeviceToDevice, output.stream);
            }
            cudaError_t unmapResult;
            {
                D3D11InteropLock interopLock(m_d3dMultithread);
                unmapResult = cudaGraphicsUnmapResources(1, &resource, output.stream);
            }
            // Fallback consumes this owned buffer, so complete the read before
            // its lease can return to the pool even if export failed.
            const cudaError_t syncResult = cudaStreamSynchronize(output.stream);
            return result == cudaSuccess && unmapResult == cudaSuccess && syncResult == cudaSuccess
                ? MPCVR_RIFE_OK : MPCVR_RIFE_CUDA_FAILURE;
        }
        return MPCVR_RIFE_INVALID_ARGUMENT;
    }

    int ReleaseCudaOutput(uint64_t leaseId)
    {
        if (!leaseId) return MPCVR_RIFE_INVALID_ARGUMENT;
        auto& output = m_cudaOutputs[(leaseId - 1) % MPCVR_RIFE_CUDA_OUTPUT_SLOTS];
        {
            std::unique_lock lock(output.mutex);
            if (output.leaseId != leaseId) return MPCVR_RIFE_INVALID_ARGUMENT;
            output.leaseId = 0;
            output.ready = false;
            return MPCVR_RIFE_OK;
        }
        return MPCVR_RIFE_INVALID_ARGUMENT;
    }

private:
    bool CudaTextureCompatible(ID3D11Texture2D* texture) const
    {
        if (!texture) return false;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        ID3D11Device* device = nullptr;
        texture->GetDevice(&device);
        const bool matches = device == m_device;
        if (device) device->Release();
        return matches && desc.Width == m_paddedWidth && desc.Height == m_paddedHeight
            && desc.Format == (m_highPrecision ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM)
            && desc.SampleDesc.Count == 1
            && desc.MipLevels == 1 && desc.ArraySize == 1 && desc.Usage == D3D11_USAGE_DEFAULT;
    }

    bool LoadOrBuildEngine()
    {
        const auto model = ReadFile(m_modelPath);
        if (model.empty()) return false;
        const auto modelHash = Sha256Hex(model);
        if (modelHash.empty()) return false;

        const std::string cacheKey = BuildCacheKey(modelHash);
        const auto planPath = m_cachePath / std::filesystem::path(std::wstring(cacheKey.begin(), cacheKey.end()) + L".plan");
        auto plan = ReadFile(planPath);
        if (!plan.empty() && DeserializeEngine(plan)) {
            m_engineBytes = plan.size();
            return ConfigureEngineContract();
        }

        TrtPtr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(g_logger));
        if (!builder) return false;

        uint32_t networkFlags = 0;
#if NV_TENSORRT_MAJOR >= 10
        networkFlags |= 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kSTRONGLY_TYPED);
#endif
        TrtPtr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(networkFlags));
        TrtPtr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, g_logger));
        TrtPtr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
        if (!network || !parser || !config) return false;

        if (!parser->parse(model.data(), model.size())) return false;
        if (network->getNbInputs() != 1 || network->getNbOutputs() != 1) return false;

        auto* input = network->getInput(0);
        auto* output = network->getOutput(0);
        if (!input || !output) return false;
        const auto inputDims = input->getDimensions();
        const auto outputDims = output->getDimensions();
        if (inputDims.nbDims != 4 || outputDims.nbDims != 4 || inputDims.d[1] != 11 || outputDims.d[1] != 3) {
            return false;
        }

        const uint32_t linearFormatMask = 1u << static_cast<uint32_t>(nvinfer1::TensorFormat::kLINEAR);
        input->setAllowedFormats(linearFormatMask);
        output->setAllowedFormats(linearFormatMask);

        nvinfer1::IOptimizationProfile* profile = builder->createOptimizationProfile();
        if (!profile) return false;

        // Performance Boost deliberately uses the original fixed-shape plan for
        // the current padded resolution. Cached A/B throughput testing against
        // the shared dynamic plan shows the fixed profile is consistently
        // faster while avoiding the slower level-5 builder experiment.
        const auto bounds = GetRifeEngineProfile(m_paddedWidth, m_paddedHeight, m_performanceBoost);
        const int minW = static_cast<int>(bounds.minWidth);
        const int minH = static_cast<int>(bounds.minHeight);
        const int maxW = static_cast<int>(bounds.maxWidth);
        const int maxH = static_cast<int>(bounds.maxHeight);
        const int optW = static_cast<int>(m_paddedWidth);
        const int optH = static_cast<int>(m_paddedHeight);
        const nvinfer1::Dims4 minDims{1, 11, minH, minW};
        const nvinfer1::Dims4 optDims{1, 11, optH, optW};
        const nvinfer1::Dims4 maxDims{1, 11, maxH, maxW};
        if (!profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMIN, minDims) ||
            !profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kOPT, optDims) ||
            !profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMAX, maxDims)) {
            return false;
        }
        if (config->addOptimizationProfile(profile) < 0) return false;

        config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, size_t{2} << 30);

        TrtPtr<nvinfer1::IHostMemory> serialized(builder->buildSerializedNetwork(*network, *config));
        if (!serialized) return false;
        if (!WriteFileAtomically(planPath, serialized->data(), serialized->size())) {
            ReportEngineCacheSaveFailure(planPath);
        }

        std::vector<uint8_t> bytes(serialized->size());
        std::memcpy(bytes.data(), serialized->data(), serialized->size());
        if (!DeserializeEngine(bytes)) return false;
        m_engineBytes = bytes.size();
        return ConfigureEngineContract();
    }

    bool DeserializeEngine(const std::vector<uint8_t>& plan)
    {
        m_runtime.reset(nvinfer1::createInferRuntime(g_logger));
        if (!m_runtime) return false;
        m_engine.reset(m_runtime->deserializeCudaEngine(plan.data(), plan.size()));
        return m_engine != nullptr;
    }

    bool ConfigureEngineContract()
    {
        if (!m_engine || m_engine->getNbIOTensors() != 2) return false;
        const char* inputName = nullptr;
        const char* outputName = nullptr;
        for (int i = 0; i < m_engine->getNbIOTensors(); ++i) {
            const char* name = m_engine->getIOTensorName(i);
            if (!name) return false;
            const auto mode = m_engine->getTensorIOMode(name);
            if (mode == nvinfer1::TensorIOMode::kINPUT) inputName = name;
            if (mode == nvinfer1::TensorIOMode::kOUTPUT) outputName = name;
        }
        if (!inputName || !outputName) return false;
        m_inputName = inputName;
        m_outputName = outputName;

        const auto inputType = m_engine->getTensorDataType(inputName);
        const auto outputType = m_engine->getTensorDataType(outputName);
        const auto supportedType = [](nvinfer1::DataType type) {
            return type == nvinfer1::DataType::kFLOAT || type == nvinfer1::DataType::kHALF;
        };
        if (!supportedType(inputType) || !supportedType(outputType)) return false;
        const auto linearTensor = [&](const char* name) {
            return m_engine->getTensorFormat(name) == nvinfer1::TensorFormat::kLINEAR
                && m_engine->getTensorVectorizedDim(name) == -1;
        };
        if (!linearTensor(inputName) || !linearTensor(outputName)) {
            m_initializationFailure = MPCVR_RIFE_UNSUPPORTED_TENSOR_FORMAT;
            return false;
        }
        m_inputIsFp16 = inputType == nvinfer1::DataType::kHALF;
        m_outputIsFp16 = outputType == nvinfer1::DataType::kHALF;
        return true;
    }

    bool PrepareContexts()
    {
        if (!m_engine) return false;
        m_contexts.reserve(m_contextCount);
        m_contextMutexes.reserve(m_contextCount);

        const nvinfer1::Dims4 inputShape{1, 11, static_cast<int>(m_paddedHeight), static_cast<int>(m_paddedWidth)};
        const nvinfer1::Dims4 outputShape{1, 3, static_cast<int>(m_paddedHeight), static_cast<int>(m_paddedWidth)};
        const size_t inputBytes = Volume(inputShape) * DataTypeBytes(m_engine->getTensorDataType(m_inputName.c_str()));
        const size_t outputBytes = Volume(outputShape) * DataTypeBytes(m_engine->getTensorDataType(m_outputName.c_str()));
        if (!inputBytes || !outputBytes) return false;

        for (uint32_t i = 0; i < m_contextCount; ++i) {
            auto state = std::make_unique<ContextState>();
            state->context.reset((m_features ? m_features->Core() : m_engine.get())->createExecutionContext());
            if (!state->context) return false;
            int leastPriority = 0;
            int greatestPriority = 0;
            const cudaError_t priorityRangeResult =
                cudaDeviceGetStreamPriorityRange(&leastPriority, &greatestPriority);
            const cudaError_t inferenceStreamResult = priorityRangeResult == cudaSuccess
                ? cudaStreamCreateWithPriority(
                    &state->stream, cudaStreamNonBlocking, greatestPriority)
                : cudaStreamCreateWithFlags(
                    &state->stream, cudaStreamNonBlocking);
            if (inferenceStreamResult != cudaSuccess) return false;
            const cudaError_t releaseStreamResult = priorityRangeResult == cudaSuccess
                ? cudaStreamCreateWithPriority(
                    &state->inputReleaseStream, cudaStreamNonBlocking, greatestPriority)
                : cudaStreamCreateWithFlags(
                    &state->inputReleaseStream, cudaStreamNonBlocking);
            if (releaseStreamResult != cudaSuccess) return false;
            if (cudaEventCreate(&state->preMapEvent) != cudaSuccess
                    || cudaEventCreate(&state->startEvent) != cudaSuccess
                    || cudaEventCreate(&state->packEndEvent) != cudaSuccess
                    || cudaEventCreate(&state->inputReleasedEvent) != cudaSuccess
                    || cudaEventCreate(&state->trtStartEvent) != cudaSuccess
                    || cudaEventCreate(&state->trtEndEvent) != cudaSuccess
                    || cudaEventCreate(&state->endEvent) != cudaSuccess) return false;
            if (cudaMalloc(&state->input, inputBytes) != cudaSuccess || cudaMalloc(&state->output, outputBytes) != cudaSuccess) return false;

            // Each execution context owns fixed input/output device buffers for
            // its entire lifetime, and this runtime is keyed to one exact input
            // geometry. Bind the shape and addresses once rather than rebuilding
            // the execution state on every frame.
            if (!state->context->setInputShape(m_inputName.c_str(), inputShape)
                    || !state->context->setTensorAddress(m_inputName.c_str(), state->input)
                    || !state->context->setTensorAddress(m_outputName.c_str(), state->output)) {
                return false;
            }

            if (m_features) {
                state->features = m_features->CreateContext(state->input, state->context.get());
                if (!state->features) return false;
            }

            // TensorRT can be enqueue-bound on Windows/WDDM: host kernel-launch
            // overhead can substantially exceed the GPU execution time. Prime
            // deferred shape state once, then capture only the TensorRT section
            // into a per-context CUDA graph. Pack/write kernels remain outside
            // the graph because they operate on per-frame mapped D3D11 arrays.
            //
            // Graph capture is an optimization only. If this engine contains an
            // operation that cannot be captured, leave graph handles null and
            // Interpolate() will continue using enqueueV3().
            if (cudaMemsetAsync(state->input, 0, inputBytes, state->stream) == cudaSuccess
                    && (!state->features || state->features->Prime(state->stream))
                    && state->context->enqueueV3(state->stream)
                    && cudaStreamSynchronize(state->stream) == cudaSuccess
                    && cudaStreamBeginCapture(state->stream, cudaStreamCaptureModeGlobal) == cudaSuccess) {
                const bool captureEnqueued = state->context->enqueueV3(state->stream);
                cudaGraph_t graph = nullptr;
                const cudaError_t captureResult = cudaStreamEndCapture(state->stream, &graph);
                if (captureEnqueued && captureResult == cudaSuccess && graph) {
                    cudaGraphExec_t graphExec = nullptr;
                    if (cudaGraphInstantiate(&graphExec, graph, 0) == cudaSuccess && graphExec) {
                        state->tensorRtGraph = graph;
                        state->tensorRtGraphExec = graphExec;
                    } else {
                        cudaGraphDestroy(graph);
                    }
                } else if (graph) {
                    cudaGraphDestroy(graph);
                }
            }
            m_contexts.push_back(std::move(state));
            m_contextMutexes.push_back(std::make_unique<std::mutex>());
        }
        return true;
    }

    std::string BuildCacheKey(const std::string& modelHash) const
    {
        std::ostringstream out;
        out << "rife46_" << modelHash.substr(0, 16)
            << "_abi" << MPCVR_RIFE_RUNTIME_ABI
            << "_trt" << NV_TENSORRT_MAJOR << '_' << NV_TENSORRT_MINOR
            << "_cc" << m_computeMajor << m_computeMinor
            << '_' << Sanitize(m_gpuName);
        out << RifeEngineProfileCacheSuffix(m_paddedWidth, m_paddedHeight, m_performanceBoost);
        return out.str();
    }

    ID3D11Device* m_device = nullptr;
    ID3D11Multithread* m_d3dMultithread = nullptr;
    int m_cudaDevice = -1;
    uint32_t m_contentWidth = 0;
    uint32_t m_contentHeight = 0;
    uint32_t m_paddedWidth = 0;
    uint32_t m_paddedHeight = 0;
    uint32_t m_contextCount = 0;
    bool m_performanceBoost = false;
    bool m_inputIsFp16 = false;
    bool m_outputIsFp16 = false;
    std::filesystem::path m_modelPath;
    std::filesystem::path m_cachePath;
    std::string m_gpuName;
    int m_computeMajor = 0;
    int m_computeMinor = 0;
    std::string m_inputName;
    std::string m_outputName;
    uint64_t m_engineBytes = 0;
    int m_initializationFailure = MPCVR_RIFE_TENSORRT_FAILURE;
    TrtPtr<nvinfer1::IRuntime> m_runtime;
    TrtPtr<nvinfer1::ICudaEngine> m_engine;
    std::unique_ptr<FeatureReuseEngine> m_features;
    bool m_highPrecision = false;
    std::vector<std::unique_ptr<ContextState>> m_contexts;
    std::vector<std::unique_ptr<std::mutex>> m_contextMutexes;
    bool m_deferInputRelease = false;
    std::mutex m_inputResourceMutex;
    std::condition_variable m_inputResourceCv;
    std::unordered_set<cudaGraphicsResource_t> m_activeInputResources;
    GraphicsRegistrationCache m_registrations;
    struct CudaOutputSlot {
        std::mutex mutex;
        void* pixels = nullptr;
        size_t pitch = 0;
        cudaStream_t stream = nullptr;
        uint64_t leaseId = 0;
        bool ready = false;
    };
    std::array<CudaOutputSlot, MPCVR_RIFE_CUDA_OUTPUT_SLOTS> m_cudaOutputs;
    std::atomic_uint64_t m_nextCudaLease = 1;
};

} // namespace

extern "C" __declspec(dllexport) uint32_t WINAPI MpcvrRifeGetCapabilities()
{
    return MPCVR_RIFE_CAP_HIGH_PRECISION | MPCVR_RIFE_CAP_FEATURE_REUSE;
}

extern "C" __declspec(dllexport) uint32_t WINAPI MpcvrRifeGetAbiVersion()
{
    return MPCVR_RIFE_RUNTIME_ABI;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeCreate(const MpcvrRifeCreateParams* params, void** handle)
{
    return CreateRifeRuntime<RifeRuntime>(params, handle);
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeInterpolate(
    void* handle, const MpcvrRifeRequest* request, MpcvrRifeStats* stats)
{
    constexpr size_t kAbi2BaseStatsSize = offsetof(MpcvrRifeStats, inputMapMs);
    MpcvrRifeRequest localRequest = {};
    if (!handle || !stats || stats->size < kAbi2BaseStatsSize || !CopyRifeRuntimeRequest(request, localRequest)) {
        return MPCVR_RIFE_INVALID_ARGUMENT;
    }
    // Copy only the caller's actual prefix. An old ABI-2 caller has no pair ID
    // and must retain full-pack behavior without reading beyond its allocation.
    MpcvrRifeStats localStats = {};
    const int result = static_cast<RifeRuntime*>(handle)->Interpolate(localRequest, localStats);
    CopyRifeRuntimeStats(stats, localStats);
    return result;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeDrainContext(
    void* handle, const uint32_t contextIndex)
{
    if (!handle) return MPCVR_RIFE_INVALID_ARGUMENT;
    return static_cast<RifeRuntime*>(handle)->DrainContext(contextIndex);
}

extern "C" __declspec(dllexport) void WINAPI MpcvrRifeDestroy(void* handle)
{
    delete static_cast<RifeRuntime*>(handle);
}

extern "C" __declspec(dllexport) uint32_t WINAPI MpcvrRifeGetCudaOutputAbi()
{
    return MPCVR_RIFE_CUDA_OUTPUT_ABI;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeAcquireCudaOutput(
    void* handle, MpcvrRifeCudaOutput* descriptor)
{
    if (!handle || !descriptor || descriptor->size != sizeof(*descriptor)
            || descriptor->abiVersion != MPCVR_RIFE_CUDA_OUTPUT_ABI) return MPCVR_RIFE_INVALID_ARGUMENT;
    try {
        MpcvrRifeCudaOutput result{};
        const int status = static_cast<RifeRuntime*>(handle)->AcquireCudaOutput(result);
        if (status == MPCVR_RIFE_OK) *descriptor = result;
        return status;
    } catch (...) { return MPCVR_RIFE_CUDA_FAILURE; }
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeInterpolateCuda(
    void* handle, const MpcvrRifeRequest* request, uint64_t leaseId, MpcvrRifeStats* stats)
{
    MpcvrRifeRequest localRequest{};
    if (!handle || !stats || stats->size < offsetof(MpcvrRifeStats, inputMapMs)
            || !CopyRifeRuntimeRequest(request, localRequest)) return MPCVR_RIFE_INVALID_ARGUMENT;
    try {
        MpcvrRifeStats localStats{};
        const int result = static_cast<RifeRuntime*>(handle)->InterpolateCuda(localRequest, leaseId, localStats);
        CopyRifeRuntimeStats(stats, localStats);
        return result;
    } catch (...) { return MPCVR_RIFE_CUDA_FAILURE; }
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeExportCudaOutput(
    void* handle, uint64_t leaseId, ID3D11Texture2D* texture)
{
    if (!handle) return MPCVR_RIFE_INVALID_ARGUMENT;
    try { return static_cast<RifeRuntime*>(handle)->ExportCudaOutput(leaseId, texture); }
    catch (...) { return MPCVR_RIFE_CUDA_FAILURE; }
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeReleaseCudaOutput(void* handle, uint64_t leaseId)
{
    if (!handle) return MPCVR_RIFE_INVALID_ARGUMENT;
    try { return static_cast<RifeRuntime*>(handle)->ReleaseCudaOutput(leaseId); }
    catch (...) { return MPCVR_RIFE_CUDA_FAILURE; }
}
