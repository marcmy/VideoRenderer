#pragma once

#include <cuda_runtime_api.h>

// Exact BGRA8 quantization, including padded pixels needed by D3D fallback.
cudaError_t MpcvrRifeWriteLinearOutput(const void* tensor, bool fp16, void* output, size_t pitch,
    int width, int height, int paddedWidth, int paddedHeight, cudaStream_t stream);

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
    cudaTextureObject_t* secondTextureOut,
    bool rgba16Input = false);

cudaError_t MpcvrRifeUpdateTimestep(
    void* tensor,
    bool tensorIsFp16,
    int sourceWidth,
    int sourceHeight,
    int paddedWidth,
    int paddedHeight,
    float timestep,
    cudaStream_t stream);

cudaError_t MpcvrRifeWriteOutput(
    const void* tensor,
    bool tensorIsFp16,
    cudaArray_t output,
    int sourceWidth,
    int sourceHeight,
    int paddedWidth,
    int paddedHeight,
    cudaStream_t stream,
    cudaSurfaceObject_t* surfaceOut,
    bool rgba16Output = false);
