#include "../../Source/RifeRuntimeApi.h"

#ifndef FAKE_ABI_VERSION
#define FAKE_ABI_VERSION MPCVR_RIFE_RUNTIME_ABI
#endif

extern "C" __declspec(dllexport) uint32_t WINAPI MpcvrRifeGetAbiVersion()
{
    return FAKE_ABI_VERSION;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeCreate(
    const MpcvrRifeCreateParams* params, void** handle)
{
    if (!params || !handle || params->abiVersion != MPCVR_RIFE_RUNTIME_ABI) {
        return -1;
    }
    *handle = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    return 0;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeInterpolate(
    void* handle, const MpcvrRifeRequest* request, MpcvrRifeStats* stats)
{
    if (!handle || !request || !stats) {
        return -1;
    }
    stats->inferenceMs = 1.0;
    stats->engineBytes = 1;
    return 0;
}

extern "C" __declspec(dllexport) void WINAPI MpcvrRifeDestroy(void*)
{
}
