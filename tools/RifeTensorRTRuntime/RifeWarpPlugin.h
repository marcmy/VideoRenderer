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
inline constexpr char kRifeAnchorLevelName[] = "mpcvr_rife_anchor_level";

// Whole: every block, then the frame. The original graph's meaning.
// Anchor: blocks 0-3, ending in their accumulated flow. Tail: block 4 and the frame, from
// one level. Field: every block, ending in final flow and mask instead of the frame.
enum class RifeFusedPlan { Whole, Anchor, Tail, Field };

// Only called for the hash-validated 4.25 Lite synthesis graph on SM75.
// Registration must precede deserializing plans containing this plugin.
bool RegisterRifeWarpPlugin() noexcept;
// TensorRT holds layer weights by reference: weightStorage must outlive the build.
bool FuseRifeSynthesisWarps(nvinfer1::INetworkDefinition& network, std::vector<uint16_t>& weightStorage, RifeFusedPlan plan = RifeFusedPlan::Whole) noexcept;

// Moves the anchor plan's level (t = 0.5) to timestep, for the tail plan, and stores
// timestep as one FP16 value at timestepSlot, where that plan reads it.
cudaError_t MpcvrRifeRetimeLevel(const void* anchor, void* level, int levelHeight, int levelWidth, float timestep, void* timestepSlot, cudaStream_t stream) noexcept;

// Makes a whole target from the field plan's output, with no block run. The surface is
// the caller's to destroy after the stream's completion point, as with MpcvrRifeWriteOutput.
struct cudaArray;
cudaError_t MpcvrRifeRetimeFrame(const void* field, const void* source, cudaArray* output, bool rgba16, int height, int width,
    float timestep, cudaStream_t stream, cudaSurfaceObject_t* surfaceOut) noexcept;
cudaError_t MpcvrRifeRetimeFrameLinear(const void* field, const void* source, void* output, size_t pitch, int height, int width,
    float timestep, cudaStream_t stream) noexcept;
