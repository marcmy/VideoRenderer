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
    const uint32_t gpuIndex,
    const uint32_t contextCount,
    const bool performanceBoost,
    const std::wstring& modelPath,
    const std::wstring& cachePath)
{
    Reset();

#ifndef _WIN64
    m_status = L"RIFE TensorRT runtime requires a 64-bit renderer";
    return false;
#else
    if (!width || !height || !contextCount || modelPath.empty()) {
        m_status = L"Invalid RIFE runtime initialization parameters";
        return false;
    }

    const auto directories = CandidateDirectories(overrideDirectory);
    std::wstring actionableStatus;
    for (const auto& directory : directories) {
        bool abiCompatibleRuntime = false;
        if (LoadAndCreate(
                AbsoluteModulePath(directory).wstring(), device, width, height, gpuIndex,
                contextCount, performanceBoost, modelPath, cachePath, &abiCompatibleRuntime)) {
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
    const uint32_t gpuIndex,
    const uint32_t contextCount,
    const bool performanceBoost,
    const std::wstring& modelPath,
    const std::wstring& cachePath,
    bool* abiCompatibleRuntime)
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

    MpcvrRifeCreateParams params = {};
    params.device = device;
    params.width = width;
    params.height = height;
    params.gpuIndex = gpuIndex;
    params.contextCount = contextCount;
    params.performanceBoost = performanceBoost ? 1u : 0u;
    params.modelPath = modelPath.c_str();
    params.cachePath = cachePath.empty() ? nullptr : cachePath.c_str();

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
    m_destroy = exports.destroy;
    m_modulePath = modulePath;
    m_status = std::format(L"RIFE runtime ready (ABI {}, {} contexts)",
        abiVersion, contextCount);
    return true;
}

bool CRifeFrameInterpolation::Interpolate(
    const uint32_t contextIndex,
    ID3D11Texture2D* first,
    ID3D11Texture2D* second,
    ID3D11Texture2D* output,
    const float timestep,
    MpcvrRifeStats& stats)
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

    stats = {};
    const int result = m_interpolate(m_handle, &request, &stats);
    if (result != 0) {
        m_status = std::format(L"RIFE inference failed with code {}", result);
        return false;
    }
    return true;
}

void CRifeFrameInterpolation::Reset() noexcept
{
    if (m_handle && m_destroy) {
        m_destroy(m_handle);
    }
    m_handle = nullptr;
    m_interpolate = nullptr;
    m_destroy = nullptr;

    if (m_module) {
        FreeLibrary(m_module);
    }
    m_module = nullptr;
    m_modulePath.clear();
    m_status.clear();
}
