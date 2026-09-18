/*
 * (C) 2018-2026 see Authors.txt
 *
 * Optional MPC Video Renderer RIFE runtime ABI.
 *
 * This header is intentionally limited to Win32/D3D11 types so the renderer
 * never needs CUDA or TensorRT headers merely to support optional RIFE loading.
 */

#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <type_traits>

#define MPCVR_RIFE_RUNTIME_ABI 2u

enum MpcvrRifeResult : int32_t {
    MPCVR_RIFE_OK = 0,
    MPCVR_RIFE_INVALID_ARGUMENT = -1,
    MPCVR_RIFE_UNSUPPORTED = -2,
    MPCVR_RIFE_CUDA_FAILURE = -3,
    MPCVR_RIFE_TENSORRT_FAILURE = -4,
    MPCVR_RIFE_BUILDER_RESOURCE_MISSING = -5,
    MPCVR_RIFE_UNSUPPORTED_COMPUTE_CAPABILITY = -6,
    MPCVR_RIFE_UNSUPPORTED_TENSOR_FORMAT = -7,
};

struct MpcvrRifeCreateParams {
    uint32_t size = sizeof(MpcvrRifeCreateParams);
    uint32_t abiVersion = MPCVR_RIFE_RUNTIME_ABI;
    ID3D11Device* device = nullptr;
    // width/height describe the allocated D3D11/tensor surface. The logical
    // video content can be smaller because RIFE surfaces are aligned to 32 px.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t contentWidth = 0;
    uint32_t contentHeight = 0;
    uint32_t gpuIndex = UINT32_MAX;
    uint32_t contextCount = 2;
    uint32_t performanceBoost = 0;
    const wchar_t* modelPath = nullptr;
    const wchar_t* cachePath = nullptr;
};

struct MpcvrRifeRequest {
    uint32_t size = sizeof(MpcvrRifeRequest);
    uint32_t contextIndex = 0;
    ID3D11Texture2D* first = nullptr;
    ID3D11Texture2D* second = nullptr;
    ID3D11Texture2D* output = nullptr;
    float timestep = 0.5f;
};

struct MpcvrRifeStats {
    uint32_t size = sizeof(MpcvrRifeStats);
    double inferenceMs = 0.0;
    uint64_t engineBytes = 0;
    // ABI-2 optional tail. New runtimes accept the original ABI-2 stats size
    // and only write these fields when the caller supplied enough space.
    double inputMapMs = 0.0;
    double inputPackMs = 0.0;
    double inputUnmapMs = 0.0;
    double outputMapMs = 0.0;
    double tensorRtMs = 0.0;
    double outputWriteMs = 0.0;
    double outputUnmapMs = 0.0;
    double contextLockWaitMs = 0.0;
    double cudaSetDeviceMs = 0.0;
    double registrationMs = 0.0;
    double inputPackLockWaitMs = 0.0;
    double totalRuntimeMs = 0.0;
    double tensorRtSubmitMs = 0.0;
    uint32_t tensorRtGraphUsed = 0;
    double packHostMs = 0.0;
    double writeHostMs = 0.0;
    double packSyncMs = 0.0;
    double writeSyncMs = 0.0;
    double handoffSyncMs = 0.0;
};

using MpcvrRifeGetAbiVersionFn = uint32_t(WINAPI*)();
using MpcvrRifeCreateFn = int(WINAPI*)(const MpcvrRifeCreateParams*, void**);
using MpcvrRifeInterpolateFn = int(WINAPI*)(void*, const MpcvrRifeRequest*, MpcvrRifeStats*);
using MpcvrRifeDestroyFn = void(WINAPI*)(void*);

static_assert(std::is_standard_layout_v<MpcvrRifeCreateParams>);
static_assert(std::is_standard_layout_v<MpcvrRifeRequest>);
static_assert(std::is_standard_layout_v<MpcvrRifeStats>);
