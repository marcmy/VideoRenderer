/*
 * Minimal NVIDIA Optical Flow 5.0 D3D11 ABI declarations.
 *
 * Copyright (c) 2018-2023 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * This file intentionally contains only the public ABI surface required by the
 * standalone driver-only probe. No proprietary SDK binary is included.
 */

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <cstdint>

namespace nvof {

constexpr uint32_t ApiVersion50 = 0x50;

enum Status : int {
    Success = 0,
    OpticalFlowNotAvailable,
    UnsupportedDevice,
    DeviceDoesNotExist,
    InvalidPointer,
    InvalidParameter,
    InvalidCall,
    InvalidVersion,
    OutOfMemory,
    NotInitialized,
    UnsupportedFeature,
    GenericError,
};

enum Bool : int {
    False = 0,
    True = 1,
};

enum Caps : int {
    SupportedOutputGridSizes = 0,
    SupportedHintGridSizes,
    SupportHintWithOpticalFlow,
    SupportHintWithStereo,
    WidthMin,
    HeightMin,
    WidthMax,
    HeightMax,
    SupportRoi,
    SupportRoiMaxCount,
    SupportStereo,
};

enum PerfLevel : int {
    PerfUndefined = 0,
    PerfSlow = 5,
    PerfMedium = 10,
    PerfFast = 20,
};

enum OutputGridSize : int {
    OutputGridUndefined = 0,
    OutputGrid1 = 1,
    OutputGrid2 = 2,
    OutputGrid4 = 4,
};

enum HintGridSize : int {
    HintGridUndefined = 0,
    HintGrid1 = 1,
    HintGrid2 = 2,
    HintGrid4 = 4,
    HintGrid8 = 8,
};

enum Mode : int {
    ModeUndefined = 0,
    ModeOpticalFlow,
    ModeStereoDisparity,
};

enum BufferUsage : int {
    BufferUsageUndefined = 0,
    BufferUsageInput,
    BufferUsageOutput,
    BufferUsageHint,
    BufferUsageCost,
    BufferUsageGlobalFlow,
};

enum BufferFormat : int {
    BufferFormatUndefined = 0,
    BufferFormatGrayscale8,
    BufferFormatNv12,
    BufferFormatAbgr8,
    BufferFormatShort,
    BufferFormatShort2,
    BufferFormatUint,
    BufferFormatUint8,
};

enum StereoDisparityRange : int {
    StereoRangeUndefined = 0,
    StereoRange128 = 128,
    StereoRange256 = 256,
};

enum PredictionDirection : int {
    PredictionForward = 0,
    PredictionBoth = 2,
};

struct HandleStorage;
struct GpuBufferStorage;
struct PrivateDataStorage;
using Handle = HandleStorage*;
using GpuBufferHandle = GpuBufferStorage*;
using PrivateDataHandle = PrivateDataStorage*;

struct InitParams {
    uint32_t width;
    uint32_t height;
    OutputGridSize outputGridSize;
    HintGridSize hintGridSize;
    Mode mode;
    PerfLevel performance;
    Bool enableExternalHints;
    Bool enableOutputCost;
    PrivateDataHandle privateData;
    StereoDisparityRange disparityRange;
    Bool enableRoi;
    PredictionDirection predictionDirection;
    Bool enableGlobalFlow;
    BufferFormat inputBufferFormat;
};

struct RoiRect {
    uint32_t startX;
    uint32_t startY;
    uint32_t width;
    uint32_t height;
};

struct ExecuteInputParams {
    GpuBufferHandle inputFrame;
    GpuBufferHandle referenceFrame;
    GpuBufferHandle externalHints;
    Bool disableTemporalHints;
    uint32_t padding;
    PrivateDataHandle privateData;
    uint32_t padding2;
    uint32_t roiCount;
    RoiRect* roiData;
};

struct ExecuteOutputParams {
    GpuBufferHandle outputBuffer;
    GpuBufferHandle outputCostBuffer;
    PrivateDataHandle privateData;
    GpuBufferHandle backwardOutputBuffer;
    GpuBufferHandle backwardOutputCostBuffer;
    GpuBufferHandle globalFlowBuffer;
};

struct FlowVector {
    int16_t x;
    int16_t y;
};

static_assert(sizeof(InitParams) == 64);
static_assert(sizeof(ExecuteInputParams) == 56);
static_assert(sizeof(ExecuteOutputParams) == 48);
static_assert(sizeof(FlowVector) == 4);

using GetMaxSupportedApiVersionFn = Status (WINAPI*)(uint32_t* version);
using CreateOpticalFlowD3D11Fn = Status (WINAPI*)(
    ID3D11Device* device, ID3D11DeviceContext* context, Handle* handle);
using InitFn = Status (WINAPI*)(Handle handle, const InitParams* params);
using GetSurfaceFormatCountD3D11Fn = Status (WINAPI*)(
    Handle handle, BufferUsage usage, Mode mode, uint32_t* count);
using GetSurfaceFormatD3D11Fn = Status (WINAPI*)(
    Handle handle, BufferUsage usage, Mode mode, DXGI_FORMAT* formats);
using RegisterResourceD3D11Fn = Status (WINAPI*)(
    Handle handle, ID3D11Resource* resource, GpuBufferHandle* buffer);
using UnregisterResourceD3D11Fn = Status (WINAPI*)(GpuBufferHandle buffer);
using ExecuteFn = Status (WINAPI*)(
    Handle handle, const ExecuteInputParams* input, ExecuteOutputParams* output);
using DestroyFn = Status (WINAPI*)(Handle handle);
using GetLastErrorFn = Status (WINAPI*)(Handle handle, char error[], uint32_t* size);
using GetCapsFn = Status (WINAPI*)(
    Handle handle, Caps capability, uint32_t* values, uint32_t* size);

struct D3D11FunctionList {
    CreateOpticalFlowD3D11Fn createOpticalFlowD3D11 = nullptr;
    InitFn initialize = nullptr;
    GetSurfaceFormatCountD3D11Fn getSurfaceFormatCountD3D11 = nullptr;
    GetSurfaceFormatD3D11Fn getSurfaceFormatD3D11 = nullptr;
    RegisterResourceD3D11Fn registerResourceD3D11 = nullptr;
    UnregisterResourceD3D11Fn unregisterResourceD3D11 = nullptr;
    ExecuteFn execute = nullptr;
    DestroyFn destroy = nullptr;
    GetLastErrorFn getLastError = nullptr;
    GetCapsFn getCaps = nullptr;
};

static_assert(sizeof(D3D11FunctionList) == 10 * sizeof(void*));

using CreateInstanceD3D11Fn = Status (WINAPI*)(
    uint32_t apiVersion, D3D11FunctionList* functionList);

} // namespace nvof
