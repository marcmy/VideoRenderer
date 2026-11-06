#pragma once

#include <NvInfer.h>

#include <cstdint>
#include <vector>

// Network input the fused graph adds: [2,H,W,8] half, r g b 0 f0 f1 f2 f3 per image.
inline constexpr char kRifeWarpSourceName[] = "mpcvr_rife_warp_source";

// Only called for the hash-validated 4.25 Lite synthesis graph on SM75.
// Registration must precede deserializing plans containing this plugin.
bool RegisterRifeWarpPlugin() noexcept;
// TensorRT holds layer weights by reference: weightStorage must outlive the build.
bool FuseRifeSynthesisWarps(nvinfer1::INetworkDefinition& network, std::vector<uint16_t>& weightStorage) noexcept;
