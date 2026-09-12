/*
 * (C) 2018-2026 see Authors.txt
 *
 * Optional RIFE/TensorRT frame interpolation runtime loader.
 */

#pragma once

#include "RifeRuntimeApi.h"

#include <string>

struct RifeRuntimeProbeResult {
    bool available = false;
    uint32_t abiVersion = 0;
    std::wstring modulePath;
    std::wstring status;
};

class CRifeFrameInterpolation {
public:
    static RifeRuntimeProbeResult Probe(const std::wstring& overrideDirectory = {});
};
