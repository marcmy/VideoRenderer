#pragma once

#include <cuda_fp16.h>
#include <cstddef>

#ifdef __CUDACC__
#define MPCVR_RIFE_HOST_DEVICE __host__ __device__
#else
#define MPCVR_RIFE_HOST_DEVICE
#endif

template <typename T>
MPCVR_RIFE_HOST_DEVICE inline void RifeStoreInputValue(T* tensor, size_t index, float value)
{
    tensor[index] = static_cast<T>(value);
}

template <>
MPCVR_RIFE_HOST_DEVICE inline void RifeStoreInputValue<__half>(__half* tensor, size_t index, float value)
{
    tensor[index] = __float2half_rn(value);
}

// Match the original full pack, including its zero timestep outside the source
// extent. Only channel 6 changes; RGB and geometry remain in the context tensor.
template <typename T>
MPCVR_RIFE_HOST_DEVICE inline void RifeUpdateInputTimestepAt(
    T* tensor, int x, int y, int sourceWidth, int sourceHeight,
    int paddedWidth, int paddedHeight, float timestep)
{
    if (x >= paddedWidth || y >= paddedHeight) return;
    const size_t plane = static_cast<size_t>(paddedWidth) * paddedHeight;
    const size_t pixel = static_cast<size_t>(y) * paddedWidth + x;
    RifeStoreInputValue(tensor, 6 * plane + pixel,
        x < sourceWidth && y < sourceHeight ? timestep : 0.0f);
}

#undef MPCVR_RIFE_HOST_DEVICE
