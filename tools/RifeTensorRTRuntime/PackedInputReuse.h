#pragma once

#include <cstdint>

// One tensor belongs to one execution context. Texture addresses alone cannot
// identify its contents: the renderer's source pool recycles the same textures.
// A nonzero ID promises that this immutable source pair is still the same job.
class PackedInputReuse final {
public:
    [[nodiscard]] bool Begin(uint64_t pairId, const void* first, const void* second) noexcept
    {
        const bool reuse = pairId && first && second
            && pairId == m_pairId && first == m_first && second == m_second;
        // Every failure after Begin leaves the tensor invalid. Only a fully
        // completed interpolation may commit it for a later timestep.
        Invalidate();
        return reuse;
    }

    void Commit(uint64_t pairId, const void* first, const void* second) noexcept
    {
        if (pairId && first && second) {
            m_pairId = pairId;
            m_first = first;
            m_second = second;
        }
    }

    void Invalidate() noexcept
    {
        m_pairId = 0;
        m_first = m_second = nullptr;
    }

private:
    uint64_t m_pairId = 0;
    const void* m_first = nullptr;
    const void* m_second = nullptr;
};
