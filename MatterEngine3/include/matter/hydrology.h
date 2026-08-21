#pragma once

#include "math_types.h"

#include <cstdint>
#include <string>

namespace matter {

struct HydrologyDomainSettings {
    Float3 origin_m{};
    std::uint32_t nx = 0;
    std::uint32_t ny = 0;
    std::uint32_t nz = 0;
    float cell_size_m = 0.0f;
};

// Static world input only. The runtime deliberately does not consume this
// declaration until the hydrology bake lifecycle is installed.
struct HydrologyWorldSettings {
    bool enabled = false;
    HydrologyDomainSettings domain{};
    float dt_s = 0.0f;
    float gravity_mps2 = 9.81f;
    Float2 downstream_xz{1.0f, 0.0f};
    Float2 residual_head_gradient_xz{};
    float inlet_flow_m3s = 0.0f;
    float inlet_head_m = 0.0f;
    float outlet_head_m = 0.0f;
    std::uint32_t batch_steps = 256;
    std::uint32_t max_steps = 0;
};

enum class HydrologyState : std::uint8_t { Pending, Baking, Ready, Stale, Invalid };

struct HydrologyStatus {
    HydrologyState state = HydrologyState::Pending;
    bool cache_hit = false;
    float progress = -1.0f;
    std::uint32_t completed_steps = 0;
    std::uint32_t wet_cells = 0;
    std::uint32_t invalid_cells = 0;
    std::uint32_t mesh_triangles = 0;
    double simulated_time_s = 0.0;
    std::string input_key;
    std::string payload_digest;
    std::string failure_reason;
};

} // namespace matter
