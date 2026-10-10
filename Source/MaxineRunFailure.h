#pragma once

#include "MaxineSpatialPolicy.h"

struct MaxineRunBinding
{
    MaxineSpatialSize input, output;
    unsigned mode = 0;
    int gpu = -1;
    uint64_t adapter = 0;
    constexpr bool operator==(const MaxineRunBinding&) const = default;
};

// A rejected configuration must not retry every presentation. A different
// image geometry/model/device should nevertheless get its own initialization.
class MaxineRunFailure
{
public:
    void Clear() noexcept { m_recorded = false; }
    void Record(const MaxineRunBinding binding) noexcept { m_binding = binding; m_recorded = true; }
    bool HasDifferentBinding(const MaxineRunBinding binding) const noexcept {
        return m_recorded && !(binding == m_binding);
    }
private:
    bool m_recorded = false;
    MaxineRunBinding m_binding;
};
