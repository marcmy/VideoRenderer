/*
 * Minimal NVIDIA Optical Flow D3D11 ABI declarations.
 *
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DAMAGES ARISING FROM USE.
 *
 * This file intentionally contains only the public ABI surface required by the
 * standalone driver-only probe. No proprietary SDK binary is included.
 */

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <cstdint>

namespace nvof {

constexpr uint32_t ApiVersion20 = 0x20;

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
};

enum StereoDisparityRange : int {
    StereoRangeUndefined = 0,
    StereoRange128 = 128,
    StereoRange256 = 256,
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
};

struct FlowVector {
    int16_t x;
    int16_t y;
};

static_assert(sizeof(InitParams) == 48);
static_assert(sizeof(ExecuteInputParams) == 56);
static_assert(sizeof(ExecuteOutputParams) == 24);
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
