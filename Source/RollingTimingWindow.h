#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

template <size_t Capacity>
class CRollingTimingWindow
{
    static_assert(Capacity > 0);

public:
    struct Summary {
        size_t count = 0;
        double averageMs = 0.0;
        double minMs = 0.0;
        double p95Ms = 0.0;
        double maxMs = 0.0;
    };

    void AddMicroseconds(const uint64_t value)
    {
        std::lock_guard lock(m_mutex);
        m_samples[m_next] = value;
        m_next = (m_next + 1) % Capacity;
        m_count = std::min(m_count + 1, Capacity);
    }

    [[nodiscard]] Summary GetSummary() const
    {
        std::array<uint64_t, Capacity> samples = {};
        size_t count = 0;
        {
            std::lock_guard lock(m_mutex);
            count = m_count;
            std::copy_n(m_samples.begin(), count, samples.begin());
        }

        Summary summary = {};
        summary.count = count;
        if (!count) {
            return summary;
        }

        long double totalUs = 0.0;
        for (size_t i = 0; i < count; ++i) {
            totalUs += samples[i];
        }
        const size_t p95Index = ((count * 95 + 99) / 100) - 1;
        const auto [minIt, maxIt] = std::minmax_element(samples.begin(), samples.begin() + count);
        const uint64_t minUs = *minIt;
        const uint64_t maxUs = *maxIt;
        std::nth_element(samples.begin(), samples.begin() + p95Index, samples.begin() + count);
        summary.averageMs = static_cast<double>(totalUs / count) / 1000.0;
        summary.minMs = minUs / 1000.0;
        summary.p95Ms = samples[p95Index] / 1000.0;
        summary.maxMs = maxUs / 1000.0;
        return summary;
    }

private:
    mutable std::mutex m_mutex;
    std::array<uint64_t, Capacity> m_samples = {};
    size_t m_count = 0;
    size_t m_next = 0;
};
