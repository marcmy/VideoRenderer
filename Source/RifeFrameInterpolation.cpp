/*
 * (C) 2018-2026 see Authors.txt
 *
 * Optional RIFE/TensorRT frame interpolation runtime loader.
 */

#include "RifeFrameInterpolation.h"

#include <Windows.h>
#include <ShlObj.h>

#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace {

constexpr wchar_t RuntimeDllName[] = L"MPCVRRifeRuntime64.dll";

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
    PWSTR rawPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &rawPath)) || !rawPath) {
        return {};
    }
    const std::filesystem::path path = std::filesystem::path(rawPath)
        / L"MPCVideoRenderer" / L"RIFE" / L"runtime";
    CoTaskMemFree(rawPath);
    return path;
}

std::filesystem::path EnvironmentRuntimeDirectory()
{
    const DWORD required = GetEnvironmentVariableW(L"MPCVR_RIFE_RUNTIME_DIR", nullptr, 0);
    if (!required) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"MPCVR_RIFE_RUNTIME_DIR", value.data(), required);
    if (!written || written >= required) {
        return {};
    }
    value.resize(written);
    return std::filesystem::path(value);
}

std::vector<std::filesystem::path> CandidateDirectories(const std::wstring& overrideDirectory)
{
    if (!overrideDirectory.empty()) {
        return {std::filesystem::path(overrideDirectory)};
    }

    std::vector<std::filesystem::path> directories;
    const auto env = EnvironmentRuntimeDirectory();
    if (!env.empty()) {
        directories.push_back(env);
    }
    const auto module = ThisModuleDirectory();
    if (!module.empty()) {
        directories.push_back(module / L"RIFE" / L"runtime");
    }
    const auto local = LocalAppDataRuntimeDirectory();
    if (!local.empty()) {
        directories.push_back(local);
    }
    return directories;
}

RifeRuntimeProbeResult ProbeModule(const std::filesystem::path& modulePath)
{
    RifeRuntimeProbeResult result;
    result.modulePath = modulePath.wstring();

    HMODULE module = LoadLibraryExW(modulePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        result.status = std::format(L"RIFE runtime not loadable: {} (Win32 error {})",
            modulePath.wstring(), GetLastError());
        return result;
    }

    const auto getAbi = reinterpret_cast<MpcvrRifeGetAbiVersionFn>(
        GetProcAddress(module, "MpcvrRifeGetAbiVersion"));
    const auto create = reinterpret_cast<MpcvrRifeCreateFn>(
        GetProcAddress(module, "MpcvrRifeCreate"));
    const auto interpolate = reinterpret_cast<MpcvrRifeInterpolateFn>(
        GetProcAddress(module, "MpcvrRifeInterpolate"));
    const auto destroy = reinterpret_cast<MpcvrRifeDestroyFn>(
        GetProcAddress(module, "MpcvrRifeDestroy"));

    if (!getAbi || !create || !interpolate || !destroy) {
        result.status = std::format(L"RIFE runtime is missing required ABI exports: {}", modulePath.wstring());
        FreeLibrary(module);
        return result;
    }

    result.abiVersion = getAbi();
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
        const auto modulePath = directory / RuntimeDllName;
        auto result = ProbeModule(modulePath);
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
