#pragma once

#include <cstdint>

namespace matter {

enum class RayTracingOverride : std::uint8_t {
    Inherit,
    Disabled,
    Enabled,
};

inline bool resolve_ray_traced(RayTracingOverride override_value,
                               bool part_default = true) noexcept {
    switch (override_value) {
        case RayTracingOverride::Disabled: return false;
        case RayTracingOverride::Enabled: return true;
        case RayTracingOverride::Inherit: return part_default;
    }
    return part_default;
}

}  // namespace matter
