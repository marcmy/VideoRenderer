#pragma once

#include <NvInfer.h>
#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Network input the fused graph adds: [2,H,W,8] half, r g b 0 f0 f1 f2 f3 per image.
inline constexpr char kRifeWarpSourceName[] = "mpcvr_rife_warp_source";

// Input and output of the plans flow reuse adds: one level at 1/4 scale, [1,C/4,W/4,16].
inline constexpr char kRifeLevelName[] = "mpcvr_rife_level";
inline constexpr char kRifeFlowsName[] = "mpcvr_rife_flows";
inline constexpr char kRifeAnchorLevelName[] = "mpcvr_rife_anchor_level";

// Whole: every block, ending in what MpcvrRifeFinishFrame makes the frame from: block 4's last
// deconvolution under the original output's name, and the accumulated flow as kRifeFlowsName.
// Anchor: blocks 0-3, ending in their accumulated flow. Tail: block 4 from one level, ending as
// Whole does. Field: every block, ending in final flow and mask.
enum class RifeFusedPlan { Whole, Anchor, Tail, Field };

// Only called for the hash-validated 4.25 Lite synthesis graph on SM75.
// Registration must precede deserializing plans containing this plugin.
bool RegisterRifeWarpPlugin() noexcept;
// TensorRT holds layer weights by reference: weightStorage must outlive the build.
bool FuseRifeSynthesisWarps(nvinfer1::INetworkDefinition& network, std::vector<uint16_t>& weightStorage, RifeFusedPlan plan = RifeFusedPlan::Whole) noexcept;

// Moves the anchor plan's level (t = 0.5) to timestep, for the tail plan, and stores
// timestep as one FP16 value at timestepSlot, where that plan reads it.
cudaError_t MpcvrRifeRetimeLevel(const void* anchor, void* level, int levelHeight, int levelWidth, float timestep, void* timestepSlot, cudaStream_t stream) noexcept;

// Where a frame goes: a surface, or BGRA8 in linear memory. With both images set, their
// imported surfaces are read through the GPU's filter instead of the warp source.
struct cudaArray;
struct RifeFrameOutput {
    cudaArray* array = nullptr;
    bool rgba16 = false;
    void* linear = nullptr;
    size_t pitch = 0;
    cudaTextureObject_t firstImage = 0;
    cudaTextureObject_t secondImage = 0;
};

// Makes the frame from the whole or tail plan's outputs. A surface is the caller's to destroy
// after the stream's completion point, as with MpcvrRifeWriteOutput.
cudaError_t MpcvrRifeFinishFrame(const void* flows, const void* low, const void* source, const RifeFrameOutput& output,
    int height, int width, cudaStream_t stream, cudaSurfaceObject_t* surfaceOut) noexcept;
// Makes a whole target from the field plan's output, with no block run. Its surface likewise.
cudaError_t MpcvrRifeRetimeFrame(const void* field, const void* source, const RifeFrameOutput& output,
    int height, int width, float timestep, cudaStream_t stream, cudaSurfaceObject_t* surfaceOut) noexcept;
