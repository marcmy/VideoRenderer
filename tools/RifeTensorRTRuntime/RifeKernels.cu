#include "RifeKernels.h"

#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <chrono>

namespace {

template <typename T>
__device__ inline void Store(T* base, size_t index, float value)
{
    base[index] = static_cast<T>(value);
}

template <>
__device__ inline void Store<__half>(__half* base, size_t index, float value)
{
    base[index] = __float2half_rn(value);
}

template <typename T>
__device__ inline float Load(const T* base, size_t index)
{
    return static_cast<float>(base[index]);
}

template <>
__device__ inline float Load<__half>(const __half* base, size_t index)
{
    return __half2float(base[index]);
}

template <typename T>
__global__ void PackInputKernel(
    cudaTextureObject_t first,
    cudaTextureObject_t second,
    T* tensor,
    int sourceWidth,
    int sourceHeight,
    int paddedWidth,
    int paddedHeight,
    float timestep)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= paddedWidth || y >= paddedHeight) {
        return;
    }

    const size_t plane = static_cast<size_t>(paddedWidth) * paddedHeight;
    const size_t pixel = static_cast<size_t>(y) * paddedWidth + x;

    float r0 = 0.0f, g0 = 0.0f, b0 = 0.0f;
    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    float timeValue = 0.0f;
    float gridX = 0.0f;
    float gridY = 0.0f;
    float multiplierX = 0.0f;
    float multiplierY = 0.0f;

    // The auxiliary grid must use the same spatial extent as the tensor fed
    // to RIFE. MPC-VR prepares an aligned D3D texture for that tensor and
    // crops it back to the logical content rectangle only at presentation.
    // Keeping model-space normalization here avoids shifting inferred frames
    // when content height (for example 1080) differs from the aligned texture
    // height (1088).
    if (x < sourceWidth && y < sourceHeight) {
        const uchar4 a = tex2D<uchar4>(first, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
        const uchar4 b = tex2D<uchar4>(second, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
        constexpr float inv255 = 1.0f / 255.0f;
        b0 = a.x * inv255;
        g0 = a.y * inv255;
        r0 = a.z * inv255;
        b1 = b.x * inv255;
        g1 = b.y * inv255;
        r1 = b.z * inv255;

        timeValue = timestep;
        gridX = sourceWidth > 1 ? (2.0f * x / static_cast<float>(sourceWidth - 1) - 1.0f) : 0.0f;
        gridY = sourceHeight > 1 ? (2.0f * y / static_cast<float>(sourceHeight - 1) - 1.0f) : 0.0f;
        multiplierX = sourceWidth > 1 ? 2.0f / static_cast<float>(sourceWidth - 1) : 0.0f;
        multiplierY = sourceHeight > 1 ? 2.0f / static_cast<float>(sourceHeight - 1) : 0.0f;
    }

    Store(tensor, 0 * plane + pixel, r0);
    Store(tensor, 1 * plane + pixel, g0);
    Store(tensor, 2 * plane + pixel, b0);
    Store(tensor, 3 * plane + pixel, r1);
    Store(tensor, 4 * plane + pixel, g1);
    Store(tensor, 5 * plane + pixel, b1);
    Store(tensor, 6 * plane + pixel, timeValue);
    Store(tensor, 7 * plane + pixel, gridX);
    Store(tensor, 8 * plane + pixel, gridY);
    Store(tensor, 9 * plane + pixel, multiplierX);
    Store(tensor, 10 * plane + pixel, multiplierY);
}

template <typename T>
__global__ void WriteOutputKernel(
    const T* tensor,
    cudaSurfaceObject_t output,
    int sourceWidth,
    int sourceHeight,
    int paddedWidth,
    int paddedHeight)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= sourceWidth || y >= sourceHeight) {
        return;
    }

    const size_t plane = static_cast<size_t>(paddedWidth) * paddedHeight;
    const size_t pixel = static_cast<size_t>(y) * paddedWidth + x;
    const float r = fminf(1.0f, fmaxf(0.0f, Load(tensor, 0 * plane + pixel)));
    const float g = fminf(1.0f, fmaxf(0.0f, Load(tensor, 1 * plane + pixel)));
    const float b = fminf(1.0f, fmaxf(0.0f, Load(tensor, 2 * plane + pixel)));

    const uchar4 bgra = make_uchar4(
        static_cast<unsigned char>(b * 255.0f + 0.5f),
        static_cast<unsigned char>(g * 255.0f + 0.5f),
        static_cast<unsigned char>(r * 255.0f + 0.5f),
        255);
    surf2Dwrite(bgra, output, x * static_cast<int>(sizeof(uchar4)), y);
}

cudaError_t CreateTexture(cudaArray_t array, cudaTextureObject_t* texture)
{
    cudaResourceDesc resource{};
    resource.resType = cudaResourceTypeArray;
    resource.res.array.array = array;

    cudaTextureDesc desc{};
    desc.addressMode[0] = cudaAddressModeClamp;
    desc.addressMode[1] = cudaAddressModeClamp;
    desc.filterMode = cudaFilterModePoint;
    desc.readMode = cudaReadModeElementType;
    desc.normalizedCoords = 0;
    return cudaCreateTextureObject(texture, &resource, &desc, nullptr);
}

cudaError_t CreateSurface(cudaArray_t array, cudaSurfaceObject_t* surface)
{
    cudaResourceDesc resource{};
    resource.resType = cudaResourceTypeArray;
    resource.res.array.array = array;
    return cudaCreateSurfaceObject(surface, &resource);
}

} // namespace

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
    cudaTextureObject_t* secondTextureOut)
{
    if (!first || !second || !tensor || sourceWidth <= 0 || sourceHeight <= 0 ||
        paddedWidth < sourceWidth || paddedHeight < sourceHeight ||
        !firstTextureOut || !secondTextureOut) {
        return cudaErrorInvalidValue;
    }

    *firstTextureOut = 0;
    *secondTextureOut = 0;

    cudaTextureObject_t firstTexture = 0;
    cudaTextureObject_t secondTexture = 0;
    cudaError_t err = CreateTexture(first, &firstTexture);
    if (err != cudaSuccess) return err;
    err = CreateTexture(second, &secondTexture);
    if (err != cudaSuccess) {
        cudaDestroyTextureObject(firstTexture);
        return err;
    }

    const dim3 block(16, 16);
    const dim3 grid((paddedWidth + block.x - 1) / block.x, (paddedHeight + block.y - 1) / block.y);
    if (tensorIsFp16) {
        PackInputKernel<<<grid, block, 0, stream>>>(firstTexture, secondTexture,
            static_cast<__half*>(tensor), sourceWidth, sourceHeight, paddedWidth, paddedHeight, timestep);
    } else {
        PackInputKernel<<<grid, block, 0, stream>>>(firstTexture, secondTexture,
            static_cast<float*>(tensor), sourceWidth, sourceHeight, paddedWidth, paddedHeight, timestep);
    }
    err = cudaGetLastError();
    if (err != cudaSuccess) {
        cudaDestroyTextureObject(secondTexture);
        cudaDestroyTextureObject(firstTexture);
        return err;
    }

    // The launch is asynchronous. The caller owns these texture objects until
    // the request-level stream completion point so input unmap, TensorRT, and
    // the other inference context can make progress without a host-side stall.
    *firstTextureOut = firstTexture;
    *secondTextureOut = secondTexture;
    return cudaSuccess;
}

cudaError_t MpcvrRifeWriteOutput(
    const void* tensor,
    bool tensorIsFp16,
    cudaArray_t output,
    int sourceWidth,
    int sourceHeight,
    int paddedWidth,
    int paddedHeight,
    cudaStream_t stream,
    cudaSurfaceObject_t* surfaceOut)
{
    if (!tensor || !output || sourceWidth <= 0 || sourceHeight <= 0 ||
        paddedWidth < sourceWidth || paddedHeight < sourceHeight || !surfaceOut) {
        return cudaErrorInvalidValue;
    }

    *surfaceOut = 0;

    cudaSurfaceObject_t surface = 0;
    cudaError_t err = CreateSurface(output, &surface);
    if (err != cudaSuccess) return err;

    const dim3 block(16, 16);
    const dim3 grid((sourceWidth + block.x - 1) / block.x, (sourceHeight + block.y - 1) / block.y);
    if (tensorIsFp16) {
        WriteOutputKernel<<<grid, block, 0, stream>>>(static_cast<const __half*>(tensor), surface,
            sourceWidth, sourceHeight, paddedWidth, paddedHeight);
    } else {
        WriteOutputKernel<<<grid, block, 0, stream>>>(static_cast<const float*>(tensor), surface,
            sourceWidth, sourceHeight, paddedWidth, paddedHeight);
    }
    err = cudaGetLastError();
    if (err != cudaSuccess) {
        cudaDestroySurfaceObject(surface);
        return err;
    }

    // Keep the CUDA surface alive through the request-level stream completion
    // point. D3D11 ownership is transferred back only after that point.
    *surfaceOut = surface;
    return cudaSuccess;
}
