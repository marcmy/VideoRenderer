#include "../../Source/RifeRuntimeApi.h"
#include "RifeKernels.h"
#include "D3D11InteropLock.h"

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

bool WriteFileAtomically(const std::filesystem::path& path, const void* data, size_t size)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    const auto temp = path.wstring() + L".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!file) return false;
    }
    std::filesystem::rename(temp, path, ec);
    if (!ec) return true;
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(temp, path, ec);
    return !ec;
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

struct ContextState {
    TrtPtr<nvinfer1::IExecutionContext> context;
    cudaStream_t stream = nullptr;
    cudaGraph_t tensorRtGraph = nullptr;
    cudaGraphExec_t tensorRtGraphExec = nullptr;
    cudaEvent_t startEvent = nullptr;
    cudaEvent_t packEndEvent = nullptr;
    cudaEvent_t inputReleasedEvent = nullptr;
    cudaEvent_t trtStartEvent = nullptr;
    cudaEvent_t trtEndEvent = nullptr;
    cudaEvent_t endEvent = nullptr;
    void* input = nullptr;
    void* output = nullptr;

    ~ContextState()
    {
        if (tensorRtGraphExec) cudaGraphExecDestroy(tensorRtGraphExec);
        if (tensorRtGraph) cudaGraphDestroy(tensorRtGraph);
        if (output) cudaFree(output);
        if (input) cudaFree(input);
        if (endEvent) cudaEventDestroy(endEvent);
        if (trtEndEvent) cudaEventDestroy(trtEndEvent);
        if (trtStartEvent) cudaEventDestroy(trtStartEvent);
        if (inputReleasedEvent) cudaEventDestroy(inputReleasedEvent);
        if (packEndEvent) cudaEventDestroy(packEndEvent);
        if (startEvent) cudaEventDestroy(startEvent);
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
        {
            D3D11InteropLock interopLock(m_d3dMultithread);
            m_registrations.Clear();
        }
        m_contexts.clear();
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
        if (m_paddedWidth != RoundUp(m_contentWidth, kPadMultiple)
                || m_paddedHeight != RoundUp(m_contentHeight, kPadMultiple)) {
            return MPCVR_RIFE_INVALID_ARGUMENT;
        }
        m_contextCount = std::clamp(params.contextCount, 1u, kMaxContexts);
        m_performanceBoost = params.performanceBoost != 0;
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
        if (!PrepareContexts()) return MPCVR_RIFE_TENSORRT_FAILURE;
        return MPCVR_RIFE_OK;
    }

    int Interpolate(const MpcvrRifeRequest& request, MpcvrRifeStats& stats)
    {
        using Clock = std::chrono::steady_clock;
        const auto runtimeStart = Clock::now();
        const auto elapsedMs = [](const Clock::time_point start, const Clock::time_point end) {
            return std::chrono::duration<double, std::milli>(end - start).count();
        };
        if (!request.first || !request.second || !request.output || request.timestep <= 0.0f || request.timestep >= 1.0f) {
            return MPCVR_RIFE_INVALID_ARGUMENT;
        }
        if (request.contextIndex >= m_contexts.size()) return MPCVR_RIFE_INVALID_ARGUMENT;

        std::unique_lock contextLock(*m_contextMutexes[request.contextIndex], std::defer_lock);
        const auto contextLockStart = Clock::now();
        contextLock.lock();
        stats.contextLockWaitMs = elapsedMs(contextLockStart, Clock::now());
        auto& state = *m_contexts[request.contextIndex];
        const auto setDeviceStart = Clock::now();
        if (cudaSetDevice(m_cudaDevice) != cudaSuccess) return MPCVR_RIFE_CUDA_FAILURE;
        stats.cudaSetDeviceMs = elapsedMs(setDeviceStart, Clock::now());

        std::array<cudaGraphicsResource_t, 2> inputResources{};
        cudaGraphicsResource_t outputResource = nullptr;
        const auto registrationStart = Clock::now();
        {
            D3D11InteropLock interopLock(m_d3dMultithread);
            inputResources = {m_registrations.Get(request.first, false),
                m_registrations.Get(request.second, false)};
            outputResource = m_registrations.Get(request.output, true);
        }
        stats.registrationMs = elapsedMs(registrationStart, Clock::now());
        if (!inputResources[0] || !inputResources[1] || !outputResource) return MPCVR_RIFE_CUDA_FAILURE;

        const auto inputPackLockStart = Clock::now();
        InputResourceClaim inputResourceClaim(
            m_inputResourceMutex, m_inputResourceCv, m_activeInputResources, inputResources);
        stats.inputPackLockWaitMs = elapsedMs(inputPackLockStart, Clock::now());

        bool inputsMapped = false;
        bool outputMapped = false;
        cudaTextureObject_t firstTexture = 0;
        cudaTextureObject_t secondTexture = 0;
        cudaSurfaceObject_t outputSurface = 0;

        auto releaseInputs = [&]() -> cudaError_t {
            if (!inputsMapped) return cudaSuccess;
            const auto unmapStart = Clock::now();
            cudaError_t unmapResult;
            {
                D3D11InteropLock interopLock(m_d3dMultithread);
                unmapResult = cudaGraphicsUnmapResources(
                    static_cast<int>(inputResources.size()), inputResources.data(), state.stream);
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
            const auto syncStart = Clock::now();
            const cudaError_t syncResult = cudaStreamSynchronize(state.stream);
            stats.handoffSyncMs = elapsedMs(syncStart, Clock::now());
            const cudaError_t destroyResult = destroyCudaViews();
            return syncResult != cudaSuccess ? syncResult : destroyResult;
        };

        cudaArray_t firstArray = nullptr;
        cudaArray_t secondArray = nullptr;
        {
            // MPC-VR normally passes role-specific staged input copies, so
            // adjacent A/B and B/C inference contexts can map and pack
            // concurrently. Keep the CUDA graphics-resource rule intact for any
            // caller that actually reuses an input texture across concurrent
            // requests by claiming only the two resources used by this request.
            const auto inputMapStart = Clock::now();
            {
                D3D11InteropLock interopLock(m_d3dMultithread);
                if (cudaGraphicsMapResources(
                        static_cast<int>(inputResources.size()), inputResources.data(), state.stream) != cudaSuccess) {
                    return MPCVR_RIFE_CUDA_FAILURE;
                }
            }
            inputsMapped = true;
            stats.inputMapMs = elapsedMs(inputMapStart, Clock::now());

            if (cudaGraphicsSubResourceGetMappedArray(&firstArray, inputResources[0], 0, 0) != cudaSuccess ||
                    cudaGraphicsSubResourceGetMappedArray(&secondArray, inputResources[1], 0, 0) != cudaSuccess) {
                releaseInputs();
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            if (cudaEventRecord(state.startEvent, state.stream) != cudaSuccess) {
                releaseInputs();
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            const auto packHostStart = Clock::now();
            if (MpcvrRifePackInput(firstArray, secondArray, state.input, m_inputIsFp16,
                    static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight),
                    static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight),
                    request.timestep, state.stream, &firstTexture, &secondTexture) != cudaSuccess) {
                releaseInputs();
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            stats.packHostMs = elapsedMs(packHostStart, Clock::now());
            if (cudaEventRecord(state.packEndEvent, state.stream) != cudaSuccess) {
                releaseInputs();
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            if (releaseInputs() != cudaSuccess) {
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
            if (cudaEventRecord(state.inputReleasedEvent, state.stream) != cudaSuccess) {
                finishStream();
                return MPCVR_RIFE_CUDA_FAILURE;
            }
        }

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

        cudaArray_t outputArray = nullptr;
        if (cudaGraphicsSubResourceGetMappedArray(&outputArray, outputResource, 0, 0) != cudaSuccess) {
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
        const bool tensorRtSubmitted = graphUsed
            ? cudaGraphLaunch(state.tensorRtGraphExec, state.stream) == cudaSuccess
            : state.context->enqueueV3(state.stream);
        stats.tensorRtSubmitMs = elapsedMs(tensorRtSubmitStart, Clock::now());
        stats.tensorRtGraphUsed = graphUsed ? 1u : 0u;
        if (!tensorRtSubmitted
                || cudaEventRecord(state.trtEndEvent, state.stream) != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_TENSORRT_FAILURE;
        }

        const auto writeHostStart = Clock::now();
        if (MpcvrRifeWriteOutput(state.output, m_outputIsFp16, outputArray,
                static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight),
                static_cast<int>(m_paddedWidth), static_cast<int>(m_paddedHeight), state.stream, &outputSurface) != cudaSuccess) {
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

        // Adjacent A/B and B/C jobs intentionally share the pre-staged B input.
        // Release that resource claim as soon as this stream has finished its
        // input pack/unmap, while TensorRT and output write continue on the same
        // stream. This preserves interop ownership without serializing the full
        // inference request across contexts.
        const auto inputReleaseStart = Clock::now();
        const cudaError_t inputReleaseResult = cudaEventSynchronize(state.inputReleasedEvent);
        stats.inputReleaseSyncMs = elapsedMs(inputReleaseStart, Clock::now());
        if (inputReleaseResult != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_CUDA_FAILURE;
        }
        inputResourceClaim.Release();

        // We only need to wait until the RIFE kernels have finished using the
        // texture/surface objects. Do not synchronize the whole stream after
        // cudaGraphicsUnmapResources(): CUDA graphics interop already guarantees
        // that work issued before the unmap completes before later D3D11 work
        // begins. Waiting through the ownership transition here serializes the
        // inference worker with the graphics queue and defeats parallel contexts.
        const auto completionStart = Clock::now();
        const cudaError_t completionResult = cudaEventSynchronize(state.endEvent);
        stats.handoffSyncMs = elapsedMs(completionStart, Clock::now());
        if (completionResult != cudaSuccess) {
            releaseOutput();
            finishStream();
            return MPCVR_RIFE_CUDA_FAILURE;
        }
        const cudaError_t destroyResult = destroyCudaViews();
        const cudaError_t releaseResult = releaseOutput();
        if (destroyResult != cudaSuccess || releaseResult != cudaSuccess) {
            return MPCVR_RIFE_CUDA_FAILURE;
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
        return MPCVR_RIFE_OK;
    }

private:
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

        const int minW = m_performanceBoost ? static_cast<int>(m_paddedWidth) : 128;
        const int minH = m_performanceBoost ? static_cast<int>(m_paddedHeight) : 128;
        const int maxW = m_performanceBoost ? static_cast<int>(m_paddedWidth) : std::max(3840, static_cast<int>(m_paddedWidth));
        const int maxH = m_performanceBoost ? static_cast<int>(m_paddedHeight) : std::max(2176, static_cast<int>(m_paddedHeight));
        const nvinfer1::Dims4 minDims{1, 11, minH, minW};
        const nvinfer1::Dims4 optDims{1, 11, static_cast<int>(m_paddedHeight), static_cast<int>(m_paddedWidth)};
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
            OutputDebugStringW(L"MPCVR RIFE: unable to write TensorRT engine cache\n");
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
            state->context.reset(m_engine->createExecutionContext());
            if (!state->context) return false;
            if (cudaStreamCreateWithFlags(&state->stream, cudaStreamNonBlocking) != cudaSuccess) return false;
            if (cudaEventCreate(&state->startEvent) != cudaSuccess
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
        if (m_performanceBoost) {
            out << '_' << m_paddedWidth << 'x' << m_paddedHeight << "_static";
        } else {
            const uint32_t maxWidth = std::max<uint32_t>(3840, m_paddedWidth);
            const uint32_t maxHeight = std::max<uint32_t>(2176, m_paddedHeight);
            out << "_dynamic_max" << maxWidth << 'x' << maxHeight;
        }
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
    std::vector<std::unique_ptr<ContextState>> m_contexts;
    std::vector<std::unique_ptr<std::mutex>> m_contextMutexes;
    std::mutex m_inputResourceMutex;
    std::condition_variable m_inputResourceCv;
    std::unordered_set<cudaGraphicsResource_t> m_activeInputResources;
    GraphicsRegistrationCache m_registrations;
};

} // namespace

extern "C" __declspec(dllexport) uint32_t WINAPI MpcvrRifeGetAbiVersion()
{
    return MPCVR_RIFE_RUNTIME_ABI;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeCreate(const MpcvrRifeCreateParams* params, void** handle)
{
    if (!params || !handle || params->size < sizeof(MpcvrRifeCreateParams) || params->abiVersion != MPCVR_RIFE_RUNTIME_ABI) {
        return MPCVR_RIFE_INVALID_ARGUMENT;
    }
    *handle = nullptr;
    auto runtime = std::make_unique<RifeRuntime>();
    const int result = runtime->Initialize(*params);
    if (result != MPCVR_RIFE_OK) return result;
    *handle = runtime.release();
    return MPCVR_RIFE_OK;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeInterpolate(
    void* handle, const MpcvrRifeRequest* request, MpcvrRifeStats* stats)
{
    constexpr size_t kAbi2BaseStatsSize = offsetof(MpcvrRifeStats, inputMapMs);
    if (!handle || !request || !stats || request->size < sizeof(MpcvrRifeRequest) || stats->size < kAbi2BaseStatsSize) {
        return MPCVR_RIFE_INVALID_ARGUMENT;
    }
    const uint32_t callerStatsSize = stats->size;
    MpcvrRifeStats localStats = {};
    const int result = static_cast<RifeRuntime*>(handle)->Interpolate(*request, localStats);
    const size_t copySize = std::min<size_t>(callerStatsSize, sizeof(localStats));
    std::memcpy(stats, &localStats, copySize);
    stats->size = callerStatsSize;
    return result;
}

extern "C" __declspec(dllexport) void WINAPI MpcvrRifeDestroy(void* handle)
{
    delete static_cast<RifeRuntime*>(handle);
}
