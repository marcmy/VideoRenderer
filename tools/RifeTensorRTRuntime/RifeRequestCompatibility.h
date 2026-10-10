#pragma once

#include "../../Source/RifeRuntimeApi.h"
#include <algorithm>
#include <cstddef>
#include <cstring>

inline bool CopyRifeRuntimeRequest(const MpcvrRifeRequest* source, MpcvrRifeRequest& request)
{
    constexpr size_t baseSize = offsetof(MpcvrRifeRequest, inputPairId);
    if (!source || source->size < baseSize) return false;
    request = {};
    std::memcpy(&request, source, std::min<size_t>(source->size, sizeof(request)));
    // A truncated optional field is not a valid generation, even if its low
    // bytes happen to match a previous request.
    if (source->size < sizeof(request)) request.inputPairId = 0;
    return true;
}

inline void CopyRifeRuntimeStats(MpcvrRifeStats* destination, const MpcvrRifeStats& stats)
{
    const uint32_t callerSize = destination->size;
    std::memcpy(destination, &stats, std::min<size_t>(callerSize, sizeof(stats)));
    destination->size = callerSize;
}
