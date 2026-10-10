/* Optional CUDA BGRA output leases. Independent of the existing RIFE ABI-2.
 * The runtime owns pixels until Release; InterpolateCuda returns completed data.
 * Consumers must complete all reads before Release. Keep the runtime loaded and
 * its handle alive while a lease exists. No input/request struct is extended.
 */
#pragma once
#include "RifeRuntimeApi.h"

inline constexpr uint32_t MPCVR_RIFE_CUDA_OUTPUT_ABI = 1;
inline constexpr uint32_t MPCVR_RIFE_CUDA_OUTPUT_BGRA8 = 1;
inline constexpr uint32_t MPCVR_RIFE_CUDA_OUTPUT_SLOTS = 8;
inline constexpr int MPCVR_RIFE_CUDA_OUTPUT_BUSY = -100;

struct MpcvrRifeCudaOutput {
    uint32_t size = sizeof(MpcvrRifeCudaOutput);
    uint32_t abiVersion = MPCVR_RIFE_CUDA_OUTPUT_ABI;
    uint64_t leaseId = 0;
    void* pixels = nullptr;
    uint64_t pitch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t contentWidth = 0;
    uint32_t contentHeight = 0;
    uint32_t cudaDevice = UINT32_MAX;
    uint32_t format = MPCVR_RIFE_CUDA_OUTPUT_BGRA8;
};

using MpcvrRifeGetCudaOutputAbiFn = uint32_t(WINAPI*)();
using MpcvrRifeAcquireCudaOutputFn = int(WINAPI*)(void*, MpcvrRifeCudaOutput*);
using MpcvrRifeInterpolateCudaFn = int(WINAPI*)(void*, const MpcvrRifeRequest*, uint64_t, MpcvrRifeStats*);
using MpcvrRifeExportCudaOutputFn = int(WINAPI*)(void*, uint64_t, ID3D11Texture2D*);
using MpcvrRifeReleaseCudaOutputFn = int(WINAPI*)(void*, uint64_t);

static_assert(std::is_standard_layout_v<MpcvrRifeCudaOutput>);

#ifdef _WIN64
static_assert(sizeof(MpcvrRifeCudaOutput) == 56);
static_assert(offsetof(MpcvrRifeCudaOutput, pixels) == 16);
static_assert(offsetof(MpcvrRifeCudaOutput, pitch) == 24);
#endif
