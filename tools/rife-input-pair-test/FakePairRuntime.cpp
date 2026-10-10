#include "../RifeTensorRTRuntime/RifeRequestCompatibility.h"

extern "C" __declspec(dllexport) uint32_t WINAPI MpcvrRifeGetAbiVersion()
{
    return MPCVR_RIFE_RUNTIME_ABI;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeCreate(const MpcvrRifeCreateParams* params, void** handle)
{
    if (!params || !handle || params->size < offsetof(MpcvrRifeCreateParams, flags)) {
        return MPCVR_RIFE_INVALID_ARGUMENT;
    }
    *handle = reinterpret_cast<void*>(uintptr_t{1});
    return MPCVR_RIFE_OK;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeInterpolate(
    void* handle, const MpcvrRifeRequest* source, MpcvrRifeStats* destination)
{
    if (!handle || !source || !destination) return MPCVR_RIFE_INVALID_ARGUMENT;
    MpcvrRifeStats stats;
    stats.inferenceMs = 1.0;
#ifdef FAKE_LEGACY_RUNTIME
    // Simulate the unchanged old ABI-2 implementation: accept larger requests,
    // read only its original prefix and write only its original stats size.
    if (source->size < offsetof(MpcvrRifeRequest, inputPairId)) return MPCVR_RIFE_INVALID_ARGUMENT;
    stats.engineBytes = 1;
    const uint32_t callerSize = destination->size;
    std::memcpy(destination, &stats,
        std::min<size_t>(callerSize, offsetof(MpcvrRifeStats, inputPairReuse)));
    destination->size = callerSize;
#else
    MpcvrRifeRequest request;
    if (!CopyRifeRuntimeRequest(source, request)) return MPCVR_RIFE_INVALID_ARGUMENT;
    // Echo generation through an existing field to verify the renderer loader
    // actually transmitted it across the DLL boundary.
    stats.engineBytes = request.inputPairId;
    stats.inputPairReuse = 1;
    CopyRifeRuntimeStats(destination, stats);
#endif
    return MPCVR_RIFE_OK;
}

#ifndef FAKE_LEGACY_RUNTIME
extern "C" __declspec(dllexport) int WINAPI MpcvrRifeDrainContext(void*, uint32_t)
{
    return MPCVR_RIFE_OK;
}
#endif

extern "C" __declspec(dllexport) void WINAPI MpcvrRifeDestroy(void*) {}
