#pragma once

#include <cstddef>

namespace RifeBackpressure {

// Keep the interpolation worker close to real time. If two source frames are
// already waiting when another sample arrives, the worker is more than one
// source interval behind and continuing to accumulate work only increases
// presentation latency.
constexpr size_t MaxQueuedSourceFrames = 2;

[[nodiscard]] constexpr bool ShouldCollapse(const size_t queuedFrames) noexcept
{
    return queuedFrames >= MaxQueuedSourceFrames;
}

} // namespace RifeBackpressure
