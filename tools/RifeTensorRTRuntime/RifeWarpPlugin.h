#pragma once

#include <NvInfer.h>

// Only called for the hash-validated 4.25 Lite synthesis graph on SM75.
// Registration must precede deserializing plans containing this plugin.
bool RegisterRifeWarpPlugin() noexcept;
bool FuseRifeSynthesisWarps(nvinfer1::INetworkDefinition& network) noexcept;
