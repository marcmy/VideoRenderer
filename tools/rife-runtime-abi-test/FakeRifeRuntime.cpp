#include "../../Source/RifeRuntimeApi.h"
#include "../../Source/RifeCudaOutputApi.h"

#ifdef FAKE_CUDA_OUTPUT
namespace {
MpcvrRifeCudaOutput g_output;
uint32_t g_liveLeases = 0;
uint32_t g_d3dCalls = 0;
int g_cudaResult = MPCVR_RIFE_OK;
int g_d3dResult = MPCVR_RIFE_OK;
}
#endif

#ifndef FAKE_ABI_VERSION
#define FAKE_ABI_VERSION MPCVR_RIFE_RUNTIME_ABI
#endif

#ifndef FAKE_CREATE_RESULT
#define FAKE_CREATE_RESULT MPCVR_RIFE_OK
#endif

extern "C" __declspec(dllexport) uint32_t WINAPI MpcvrRifeGetAbiVersion()
{
    return FAKE_ABI_VERSION;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeCreate(
    const MpcvrRifeCreateParams* params, void** handle)
{
    if (!params || !handle || params->size < sizeof(MpcvrRifeCreateParams)
            || params->abiVersion != MPCVR_RIFE_RUNTIME_ABI
            || !params->width || !params->height
            || !params->contentWidth || !params->contentHeight) {
        return MPCVR_RIFE_INVALID_ARGUMENT;
    }
    constexpr int fakeCreateResult = FAKE_CREATE_RESULT;
    if constexpr (fakeCreateResult != MPCVR_RIFE_OK) {
        *handle = nullptr;
        return fakeCreateResult;
    }
    else {
#ifdef FAKE_CUDA_OUTPUT
        g_output.width = params->width;
        g_output.height = params->height;
        g_output.contentWidth = params->contentWidth;
        g_output.contentHeight = params->contentHeight;
        g_output.pitch = static_cast<uint64_t>(params->width) * 4;
        g_output.cudaDevice = 0;
#endif
        *handle = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
        return MPCVR_RIFE_OK;
    }
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeInterpolate(
    void* handle, const MpcvrRifeRequest* request, MpcvrRifeStats* stats)
{
    if (!handle || !request || !stats || !request->output) {
        return MPCVR_RIFE_INVALID_ARGUMENT;
    }
#ifdef FAKE_CUDA_OUTPUT
    ++g_d3dCalls;
    if (g_liveLeases) return MPCVR_RIFE_INVALID_ARGUMENT;
    if (g_d3dResult != MPCVR_RIFE_OK) return g_d3dResult;
#endif
    stats->inferenceMs = 1.0;
    stats->engineBytes = 1;
    return MPCVR_RIFE_OK;
}

extern "C" __declspec(dllexport) void WINAPI MpcvrRifeDestroy(void*)
{
}

#ifdef FAKE_CUDA_OUTPUT
extern "C" __declspec(dllexport) uint32_t WINAPI MpcvrRifeGetCudaOutputAbi()
{
    return MPCVR_RIFE_CUDA_OUTPUT_ABI;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeAcquireCudaOutput(void*, MpcvrRifeCudaOutput* output)
{
    if (g_liveLeases) return MPCVR_RIFE_CUDA_OUTPUT_BUSY;
    *output = g_output;
    output->leaseId = 1;
    output->pixels = reinterpret_cast<void*>(static_cast<uintptr_t>(4));
    ++g_liveLeases;
    return MPCVR_RIFE_OK;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeInterpolateCuda(
    void*, const MpcvrRifeRequest*, uint64_t, MpcvrRifeStats* stats)
{
    stats->inferenceMs = 2.0;
    return g_cudaResult;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeExportCudaOutput(void*, uint64_t, ID3D11Texture2D*)
{
    return MPCVR_RIFE_OK;
}

extern "C" __declspec(dllexport) int WINAPI MpcvrRifeReleaseCudaOutput(void*, uint64_t)
{
    --g_liveLeases;
    return MPCVR_RIFE_OK;
}

extern "C" __declspec(dllexport) void WINAPI FakeRifeSetOutputResults(int cudaResult, int d3dResult)
{
    g_cudaResult = cudaResult;
    g_d3dResult = d3dResult;
}

extern "C" __declspec(dllexport) uint32_t WINAPI FakeRifeGetD3DCalls()
{
    return g_d3dCalls;
}
#endif
