#pragma once

#include "../../Source/RifeRuntimeApi.h"
#include <exception>
#include <memory>

// Initialization runs on a detached renderer worker. File-system/allocation
// exceptions must become a normal initialization failure, not escape the DLL
// boundary and terminate the player's worker thread.
template <typename Runtime>
int CreateRifeRuntime(const MpcvrRifeCreateParams* params, void** handle) noexcept
{
    if (handle) *handle = nullptr;
    constexpr size_t baseSize = offsetof(MpcvrRifeCreateParams, flags);
    if (!params || !handle || params->size < baseSize || params->abiVersion != MPCVR_RIFE_RUNTIME_ABI) {
        return MPCVR_RIFE_INVALID_ARGUMENT;
    }
    try {
        auto runtime = std::make_unique<Runtime>();
        const int result = runtime->Initialize(*params);
        if (result != MPCVR_RIFE_OK) return result;
        *handle = runtime.release();
        return MPCVR_RIFE_OK;
    } catch (const std::exception& error) {
        OutputDebugStringA("MPCVR RIFE: runtime initialization exception: ");
        OutputDebugStringA(error.what());
        OutputDebugStringA("\n");
    } catch (...) {
        OutputDebugStringA("MPCVR RIFE: unknown runtime initialization exception\n");
    }
    return MPCVR_RIFE_TENSORRT_FAILURE;
}
