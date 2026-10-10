/*
 * (C) 2018-2026 see Authors.txt
 *
 * Optional RIFE/TensorRT frame interpolation runtime loader.
 */

#include "RifeFrameInterpolation.h"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace {

constexpr wchar_t RuntimeDllName[] = L"MPCVRRifeRuntime64.dll";

std::filesystem::path ReadEnvironmentPath(const wchar_t* name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (!required) {
        return {};
    }

    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (!written || written >= required) {
        return {};
    }
    value.resize(written);
    return std::filesystem::path(value);
}

std::filesystem::path ThisModuleDirectory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ThisModuleDirectory), &module) || !module) {
        return {};
    }

    std::array<wchar_t, 32768> path = {};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        return {};
    }
    return std::filesystem::path(path.data()).parent_path();
}

std::filesystem::path LocalAppDataRuntimeDirectory()
{
    const auto localAppData = ReadEnvironmentPath(L"LOCALAPPDATA");
    if (localAppData.empty()) {
        return {};
    }
    return localAppData / L"MPCVideoRenderer" / L"RIFE" / L"runtime";
}

std::vector<std::filesystem::path> CandidateDirectories(const std::wstring& overrideDirectory)
{
    if (!overrideDirectory.empty()) {
        return {std::filesystem::path(overrideDirectory)};
    }

    std::vector<std::filesystem::path> directories;
    const auto module = ThisModuleDirectory();
    if (!module.empty()) {
        directories.push_back(module / L"RIFE" / L"runtime");
    }
    const auto local = LocalAppDataRuntimeDirectory();
    if (!local.empty()) {
        directories.push_back(local);
    }
    const auto env = ReadEnvironmentPath(L"MPCVR_RIFE_RUNTIME_DIR");
    if (!env.empty()) {
        directories.push_back(env);
    }
    return directories;
}

std::filesystem::path AbsoluteModulePath(const std::filesystem::path& directory)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(directory / RuntimeDllName, error);
    return error ? directory / RuntimeDllName : absolute;
}

struct RuntimeExports {
    MpcvrRifeGetAbiVersionFn getAbi = nullptr;
    MpcvrRifeCreateFn create = nullptr;
    MpcvrRifeInterpolateFn interpolate = nullptr;
    MpcvrRifeDrainContextFn drainContext = nullptr;
    MpcvrRifeDestroyFn destroy = nullptr;

    [[nodiscard]] bool Complete() const noexcept
    {
        return getAbi && create && interpolate && destroy;
    }
};

RuntimeExports ResolveExports(HMODULE module)
{
    RuntimeExports exports;
    exports.getAbi = reinterpret_cast<MpcvrRifeGetAbiVersionFn>(
        GetProcAddress(module, "MpcvrRifeGetAbiVersion"));
    exports.create = reinterpret_cast<MpcvrRifeCreateFn>(
        GetProcAddress(module, "MpcvrRifeCreate"));
    exports.interpolate = reinterpret_cast<MpcvrRifeInterpolateFn>(
        GetProcAddress(module, "MpcvrRifeInterpolate"));
    exports.drainContext = reinterpret_cast<MpcvrRifeDrainContextFn>(
        GetProcAddress(module, "MpcvrRifeDrainContext"));
    exports.destroy = reinterpret_cast<MpcvrRifeDestroyFn>(
        GetProcAddress(module, "MpcvrRifeDestroy"));
    return exports;
}

RifeRuntimeProbeResult ProbeModule(const std::filesystem::path& directory)
{
    RifeRuntimeProbeResult result;
    const auto modulePath = AbsoluteModulePath(directory);
    result.modulePath = modulePath.wstring();

    HMODULE module = LoadLibraryExW(modulePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        result.status = std::format(L"RIFE runtime not loadable: {} (Win32 error {})",
            modulePath.wstring(), GetLastError());
        return result;
    }

    const auto exports = ResolveExports(module);
    if (!exports.Complete()) {
        result.status = std::format(L"RIFE runtime is missing required ABI exports: {}", modulePath.wstring());
        FreeLibrary(module);
        return result;
    }

    result.abiVersion = exports.getAbi();
    if (result.abiVersion != MPCVR_RIFE_RUNTIME_ABI) {
        result.status = std::format(L"RIFE runtime ABI {} is incompatible with renderer ABI {}",
            result.abiVersion, MPCVR_RIFE_RUNTIME_ABI);
        FreeLibrary(module);
        return result;
    }

    result.available = true;
    result.status = std::format(L"RIFE runtime ABI {} available: {}",
        result.abiVersion, modulePath.wstring());
    FreeLibrary(module);
    return result;
}

} // namespace

CRifeFrameInterpolation::~CRifeFrameInterpolation()
{
    Reset();
}

RifeRuntimeProbeResult CRifeFrameInterpolation::Probe(const std::wstring& overrideDirectory)
{
#ifndef _WIN64
    RifeRuntimeProbeResult result;
    result.status = L"RIFE TensorRT runtime requires a 64-bit renderer";
    return result;
#else
    const auto directories = CandidateDirectories(overrideDirectory);
    RifeRuntimeProbeResult lastResult;

    for (const auto& directory : directories) {
        auto result = ProbeModule(directory);
        if (result.available || result.abiVersion != 0) {
            return result;
        }
        lastResult = std::move(result);
    }

    if (lastResult.status.empty()) {
        lastResult.status = L"RIFE runtime was not found";
    }
    return lastResult;
#endif
}

bool CRifeFrameInterpolation::Initialize(
    const std::wstring& overrideDirectory,
    ID3D11Device* device,
    const uint32_t width,
    const uint32_t height,
    const uint32_t contentWidth,
    const uint32_t contentHeight,
    const uint32_t gpuIndex,
    const uint32_t contextCount,
    const bool performanceBoost,
    const std::wstring& modelPath,
    const std::wstring& cachePath, const bool highPrecision, const bool featureReuse)
{
    if (m_liveCudaLeases.load(std::memory_order_acquire)) return false;
    Reset();

#ifndef _WIN64
    m_status = L"RIFE TensorRT runtime requires a 64-bit renderer";
    return false;
#else
    if (!width || !height || !contentWidth || !contentHeight || !contextCount || modelPath.empty()) {
        m_status = L"Invalid RIFE runtime initialization parameters";
        return false;
    }

    const auto directories = CandidateDirectories(overrideDirectory);
    std::wstring actionableStatus;
    for (const auto& directory : directories) {
        bool abiCompatibleRuntime = false;
        if (LoadAndCreate(
                AbsoluteModulePath(directory).wstring(), device, width, height, contentWidth, contentHeight, gpuIndex,
                contextCount, performanceBoost, modelPath, cachePath, &abiCompatibleRuntime, highPrecision, featureReuse)) {
            return true;
        }
        if (abiCompatibleRuntime) {
            actionableStatus = m_status;
        }
    }

    if (!actionableStatus.empty()) {
        m_status = std::move(actionableStatus);
    }
    else if (m_status.empty()) {
        m_status = L"RIFE runtime was not found";
    }
    return false;
#endif
}

bool CRifeFrameInterpolation::LoadAndCreate(
    const std::wstring& modulePath,
    ID3D11Device* device,
    const uint32_t width,
    const uint32_t height,
    const uint32_t contentWidth,
    const uint32_t contentHeight,
    const uint32_t gpuIndex,
    const uint32_t contextCount,
    const bool performanceBoost,
    const std::wstring& modelPath,
    const std::wstring& cachePath,
    bool* abiCompatibleRuntime, const bool highPrecision, const bool featureReuse)
{
    if (abiCompatibleRuntime) {
        *abiCompatibleRuntime = false;
    }
    HMODULE module = LoadLibraryExW(modulePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        m_status = std::format(L"RIFE runtime not loadable: {} (Win32 error {})",
            modulePath, GetLastError());
        return false;
    }

    const auto exports = ResolveExports(module);
    if (!exports.Complete()) {
        m_status = std::format(L"RIFE runtime is missing required ABI exports: {}", modulePath);
        FreeLibrary(module);
        return false;
    }

    const uint32_t abiVersion = exports.getAbi();
    if (abiVersion != MPCVR_RIFE_RUNTIME_ABI) {
        m_status = std::format(L"RIFE runtime ABI {} is incompatible with renderer ABI {}",
            abiVersion, MPCVR_RIFE_RUNTIME_ABI);
        FreeLibrary(module);
        return false;
    }

    if (abiCompatibleRuntime) {
        *abiCompatibleRuntime = true;
    }

    const auto getCapabilities = reinterpret_cast<MpcvrRifeGetCapabilitiesFn>(GetProcAddress(module, "MpcvrRifeGetCapabilities"));
    const uint32_t capabilities = getCapabilities ? getCapabilities() : 0u;
    const uint32_t required = (highPrecision ? MPCVR_RIFE_CAP_HIGH_PRECISION : 0u)
        | (featureReuse ? MPCVR_RIFE_CAP_FEATURE_REUSE : 0u);
    if ((capabilities & required) != required) {
        m_status = L"Update the RIFE runtime to use precision or encoder feature reuse";
        FreeLibrary(module);
        return false;
    }

    MpcvrRifeCreateParams params = {};
    params.device = device;
    params.width = width;
    params.height = height;
    params.contentWidth = contentWidth;
    params.contentHeight = contentHeight;
    params.gpuIndex = gpuIndex;
    params.contextCount = contextCount;
    params.performanceBoost = performanceBoost ? 1u : 0u;
    params.modelPath = modelPath.c_str();
    params.cachePath = cachePath.empty() ? nullptr : cachePath.c_str();
    // Deferred input ownership requires the optional drain export so the
    // renderer can safely retire a context before switching runtime instances.
    params.flags = (exports.drainContext ? MPCVR_RIFE_CREATE_DEFER_INPUT_RELEASE : 0u)
        | (highPrecision ? MPCVR_RIFE_CREATE_HIGH_PRECISION : 0u)
        | (featureReuse ? MPCVR_RIFE_CREATE_FEATURE_REUSE : 0u);

    void* handle = nullptr;
    const int result = exports.create(&params, &handle);
    if (result != MPCVR_RIFE_OK || !handle) {
        switch (result) {
            case MPCVR_RIFE_INVALID_ARGUMENT:
                m_status = L"RIFE runtime initialization failed: invalid argument";
                break;
            case MPCVR_RIFE_UNSUPPORTED:
                m_status = L"RIFE runtime initialization failed: unsupported configuration";
                break;
            case MPCVR_RIFE_CUDA_FAILURE:
                m_status = L"RIFE runtime initialization failed: CUDA failure";
                break;
            case MPCVR_RIFE_TENSORRT_FAILURE:
                m_status = L"RIFE runtime initialization failed: TensorRT failure";
                break;
            case MPCVR_RIFE_BUILDER_RESOURCE_MISSING:
                m_status = L"RIFE TensorRT builder resource for this GPU architecture is missing";
                break;
            case MPCVR_RIFE_UNSUPPORTED_COMPUTE_CAPABILITY:
                m_status = L"RIFE runtime does not support this CUDA compute capability";
                break;
            case MPCVR_RIFE_UNSUPPORTED_TENSOR_FORMAT:
                m_status = L"RIFE TensorRT engine I/O is not linear NCHW";
                break;
            default:
                m_status = std::format(L"RIFE runtime initialization failed with code {}", result);
                break;
        }
        FreeLibrary(module);
        return false;
    }

    m_module = module;
    m_handle = handle;
    m_interpolate = exports.interpolate;
    m_drainContext = exports.drainContext;
    m_destroy = exports.destroy;
    const auto getCudaAbi = reinterpret_cast<MpcvrRifeGetCudaOutputAbiFn>(GetProcAddress(module, "MpcvrRifeGetCudaOutputAbi"));
    const auto acquire = reinterpret_cast<MpcvrRifeAcquireCudaOutputFn>(GetProcAddress(module, "MpcvrRifeAcquireCudaOutput"));
    const auto interpolateCuda = reinterpret_cast<MpcvrRifeInterpolateCudaFn>(GetProcAddress(module, "MpcvrRifeInterpolateCuda"));
    const auto exportCuda = reinterpret_cast<MpcvrRifeExportCudaOutputFn>(GetProcAddress(module, "MpcvrRifeExportCudaOutput"));
    const auto release = reinterpret_cast<MpcvrRifeReleaseCudaOutputFn>(GetProcAddress(module, "MpcvrRifeReleaseCudaOutput"));
    if (!highPrecision && getCudaAbi && acquire && interpolateCuda && exportCuda && release
            && getCudaAbi() == MPCVR_RIFE_CUDA_OUTPUT_ABI) {
        m_acquireCudaOutput = acquire;
        m_interpolateCuda = interpolateCuda;
        m_exportCudaOutput = exportCuda;
        m_releaseCudaOutput = release;
    }
    m_width = width;
    m_height = height;
    m_contentWidth = contentWidth;
    m_contentHeight = contentHeight;
    m_modulePath = modulePath;
    m_status = std::format(L"RIFE runtime ready (ABI {}, {} contexts, LINEAR I/O)",
        abiVersion, contextCount);
    return true;
}

bool CRifeFrameInterpolation::Interpolate(
    const uint32_t contextIndex,
    ID3D11Texture2D* first,
    ID3D11Texture2D* second,
    ID3D11Texture2D* output,
    const float timestep,
    MpcvrRifeStats& stats,
    const uint64_t inputPairId)
{
    if (!IsReady() || !first || !second || !output || !(timestep > 0.0f && timestep < 1.0f)) {
        return false;
    }

    MpcvrRifeRequest request = {};
    request.contextIndex = contextIndex;
    request.first = first;
    request.second = second;
    request.output = output;
    request.timestep = timestep;
    request.inputPairId = inputPairId;

    stats = {};
    const int result = m_interpolate(m_handle, &request, &stats);
    // Interpolate() is intentionally callable from multiple renderer workers.
    // Initialization status is immutable while those workers are active; do
    // not race on m_status when independent contexts report a transient error.
    return result == 0;
}

bool CRifeFrameInterpolation::DrainContext(const uint32_t contextIndex) noexcept
{
    if (!m_handle) {
        return true;
    }
    // Runtimes without this optional ABI-2 export never receive the deferred
    // release flag, so their Interpolate() calls are already synchronous.
    return !m_drainContext || m_drainContext(m_handle, contextIndex) == MPCVR_RIFE_OK;
}

void CRifeFrameInterpolation::Reset() noexcept
{
    // A normal model switch creates another runtime. Explicit Reset on the
    // same owner must also leave any queued GPU pixels and exports alive.
    if (m_liveCudaLeases.load(std::memory_order_acquire)) return;
    if (m_handle && m_destroy) {
        m_destroy(m_handle);
    }
    m_handle = nullptr;
    m_interpolate = nullptr;
    m_drainContext = nullptr;
    m_destroy = nullptr;
    m_acquireCudaOutput = nullptr;
    m_interpolateCuda = nullptr;
    m_exportCudaOutput = nullptr;
    m_releaseCudaOutput = nullptr;

    if (m_module) {
        FreeLibrary(m_module);
    }
    m_module = nullptr;
    m_modulePath.clear();
    m_status.clear();
}

std::shared_ptr<RifeCudaOutputLease> CRifeFrameInterpolation::AcquireCudaOutput()
{
    const auto owner = weak_from_this().lock();
    if (!owner || !IsReady() || !m_acquireCudaOutput) return {};
    auto lease = std::make_shared<RifeCudaOutputLease>(owner);
    MpcvrRifeCudaOutput image{};
    if (m_acquireCudaOutput(m_handle, &image) != MPCVR_RIFE_OK) return {};
    if (image.size != sizeof(image) || image.abiVersion != MPCVR_RIFE_CUDA_OUTPUT_ABI
            || !image.leaseId || !image.pixels || image.width != m_width || image.height != m_height
            || image.contentWidth != m_contentWidth || image.contentHeight != m_contentHeight
            || image.pitch < static_cast<uint64_t>(m_width) * 4 || image.pitch > INT_MAX
            || image.pitch % 4 || image.cudaDevice == UINT32_MAX || image.format != MPCVR_RIFE_CUDA_OUTPUT_BGRA8) {
        if (image.leaseId) m_releaseCudaOutput(m_handle, image.leaseId);
        return {};
    }
    lease->m_image = image;
    m_liveCudaLeases.fetch_add(1, std::memory_order_release);
    return lease;
}

RifeCudaOutputLease::~RifeCudaOutputLease()
{
    if (m_image.leaseId) {
        m_owner->m_releaseCudaOutput(m_owner->m_handle, m_image.leaseId);
        m_owner->m_liveCudaLeases.fetch_sub(1, std::memory_order_release);
    }
}

bool RifeCudaOutputLease::Interpolate(uint32_t contextIndex, ID3D11Texture2D* first,
    ID3D11Texture2D* second, float timestep, MpcvrRifeStats& stats, uint64_t inputPairId)
{
    if (!m_image.leaseId || !first || !second || !(timestep > 0.0f && timestep < 1.0f)) return false;
    MpcvrRifeRequest request{};
    request.contextIndex = contextIndex;
    request.first = first;
    request.second = second;
    request.timestep = timestep;
    request.inputPairId = inputPairId;
    stats = {};
    return m_owner->m_interpolateCuda(m_owner->m_handle, &request, m_image.leaseId, &stats) == MPCVR_RIFE_OK;
}

bool RifeCudaOutputLease::Export(ID3D11Texture2D* output) const
{
    return output && m_image.leaseId
        && m_owner->m_exportCudaOutput(m_owner->m_handle, m_image.leaseId, output) == MPCVR_RIFE_OK;
}
