#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

// Infer aggregate service capacity from completed calls' actual overlap.
// Configuring two TensorRT contexts does not imply twice the throughput: at
// low demand only one may run, and under contention each call may take longer.
// Idle gaps are excluded, so this does not mistake a low requested rate for
// the GPU's limit. Presentation and scene analysis are measured separately.
template <size_t Capacity>
class CRifeInferenceTimingWindow
{
    static_assert(Capacity > 0);

public:
    struct Summary {
        size_t count = 0;
        uint64_t averageWallUs = 0;
        uint64_t busyUs = 0;
        uint32_t parallelismPermille = 0;
        uint64_t completedCalls = 0;
    };

    void AddMicroseconds(const uint64_t startUs, const uint64_t endUs)
    {
        if (endUs <= startUs) {
            return;
        }
        std::lock_guard lock(m_mutex);
        m_samples[m_next] = {startUs, endUs};
        m_next = (m_next + 1) % Capacity;
        m_count = std::min(m_count + 1, Capacity);
        ++m_completedCalls;
    }

    [[nodiscard]] Summary GetSummary() const
    {
        std::array<Interval, Capacity> samples = {};
        size_t count = 0;
        uint64_t completedCalls = 0;
        {
            std::lock_guard lock(m_mutex);
            count = m_count;
            completedCalls = m_completedCalls;
            std::copy_n(m_samples.begin(), count, samples.begin());
        }

        Summary summary = {};
        summary.count = count;
        summary.completedCalls = completedCalls;
        if (!count) {
            return summary;
        }
        std::sort(samples.begin(), samples.begin() + count,
            [](const Interval& first, const Interval& second) {
                return first.startUs < second.startUs;
            });

        long double totalWallUs = 0.0;
        uint64_t busyStartUs = samples[0].startUs;
        uint64_t busyEndUs = samples[0].endUs;
        for (size_t i = 0; i < count; ++i) {
            const auto& sample = samples[i];
            totalWallUs += sample.endUs - sample.startUs;
            if (sample.startUs > busyEndUs) {
                summary.busyUs += busyEndUs - busyStartUs;
                busyStartUs = sample.startUs;
            }
            busyEndUs = std::max(busyEndUs, sample.endUs);
        }
        summary.busyUs += busyEndUs - busyStartUs;
        summary.averageWallUs = static_cast<uint64_t>(totalWallUs / count);
        summary.parallelismPermille = static_cast<uint32_t>(std::clamp(
            std::llround(totalWallUs * 1000.0L / summary.busyUs), 0LL,
            static_cast<long long>(std::numeric_limits<uint32_t>::max())));
        return summary;
    }

    void Clear()
    {
        std::lock_guard lock(m_mutex);
        m_samples.fill({});
        m_next = 0;
        m_count = 0;
        m_completedCalls = 0;
    }

private:
    struct Interval {
        uint64_t startUs = 0;
        uint64_t endUs = 0;
    };
    mutable std::mutex m_mutex;
    std::array<Interval, Capacity> m_samples = {};
    size_t m_count = 0;
    size_t m_next = 0;
    uint64_t m_completedCalls = 0;
};
