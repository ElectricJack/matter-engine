#pragma once

#include "matter/hydrology.h"

#include <array>
#include <cstdint>
#include <string>

namespace hydrology {

struct HydrologyKey {
    std::array<std::uint8_t, 32> bytes{};
};
inline bool operator==(const HydrologyKey& a, const HydrologyKey& b) {
    return a.bytes == b.bytes;
}
inline bool operator!=(const HydrologyKey& a, const HydrologyKey& b) {
    return !(a == b);
}

struct HydrologyDomain {
    matter::Float3 origin_m{};
    std::uint32_t nx = 0, ny = 0, nz = 0;
    float cell_size_m = 0.0f;
};

struct HydrologyBakeDescription {
    std::uint32_t schema_version = 1;
    std::uint32_t solver_contract_version = 1;
    std::uint32_t shader_contract_version = 1;
    HydrologyDomain domain{};
    float dt_s = 0.0f;
    float gravity_mps2 = 0.0f;
    matter::Float2 downstream_xz{1.0f, 0.0f};
    matter::Float2 residual_head_gradient_xz{};
    float inlet_flow_m3s = 0.0f;
    float inlet_head_m = 0.0f;
    float outlet_head_m = 0.0f;
    std::uint32_t batch_steps = 0, max_steps = 0;
    std::uint64_t terrain_revision = 0;
    HydrologyKey semantic_key{};
};

bool validate_and_key(const matter::HydrologyWorldSettings& authored,
                      std::uint64_t terrain_revision,
                      HydrologyBakeDescription& out,
                      std::string& error);

} // namespace hydrology
