#pragma once

#include <cuda_runtime_api.h>

struct cudaArray;
using cudaArray_t = cudaArray*;

cudaError_t MpcvrRifePackInput(
    cudaArray_t first,
    cudaArray_t second,
    void* tensor,
    bool tensorIsFp16,
    int sourceWidth,
    int sourceHeight,
    int paddedWidth,
    int paddedHeight,
    float timestep,
    cudaStream_t stream,
    cudaTextureObject_t* firstTextureOut,
    cudaTextureObject_t* secondTextureOut);

cudaError_t MpcvrRifeWriteOutput(
    const void* tensor,
    bool tensorIsFp16,
    cudaArray_t output,
    int sourceWidth,
    int sourceHeight,
    int paddedWidth,
    int paddedHeight,
    cudaStream_t stream,
    cudaSurfaceObject_t* surfaceOut);
