#pragma once

#include <NvInfer.h>

#include <cstdint>
#include <vector>

// Only called for the hash-validated 4.25 Lite synthesis graph on SM75.
// Registration must precede deserializing plans containing this plugin.
bool RegisterRifeWarpPlugin() noexcept;
// TensorRT holds layer weights by reference: weightStorage must outlive the build.
bool FuseRifeSynthesisWarps(nvinfer1::INetworkDefinition& network, std::vector<uint16_t>& weightStorage) noexcept;
