#pragma once

#include "matter/math_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace matter {

namespace detail {
class RiverRuntimeBindingAccess;
}

enum class RiverFeature : std::uint8_t {
    Calm = 0,
    Current = 1,
    Rapid = 2,
    Waterfall = 3,
    Impact = 4,
    Spillway = 5,
    Pool = 6
};

struct RiverFieldSample {
    Float3 surface_position_m{};
    Float3 surface_normal{0.0f, 1.0f, 0.0f};
    Float3 velocity_mps{};
    float depth_m = 0.0f;
    float turbulence = 0.0f;
    float aeration = 0.0f;
    float foam_potential = 0.0f;
    RiverFeature feature = RiverFeature::Calm;
    bool wet_valid = false;
};

struct RiverFloatBody {
    float effective_density_kg_m3 = 650.0f;
    float displaced_volume_scale = 1.0f;
    std::uint8_t probes_x = 2;
    std::uint8_t probes_y = 2;
    std::uint8_t probes_z = 2;
    float probe_inset = 0.15f;
    float buoyancy_response = 1.0f;
    float longitudinal_drag = 0.8f;
    float lateral_drag = 1.4f;
    float vertical_drag = 1.8f;
    float angular_damping = 0.4f;
    float max_force_per_probe_n = 30000.0f;
    float max_total_force_n = 120000.0f;
    Float3 diagnostic_color{0.2f, 0.8f, 1.0f};
};

static_assert(sizeof(RiverFloatBody) == 56,
              "RiverFloatBody public ABI size changed");
static_assert(alignof(RiverFloatBody) == 4,
              "RiverFloatBody public ABI alignment changed");

struct RiverFloatForces {};

class WorldSession;

class RiverRuntimeBinding {
public:
    RiverRuntimeBinding() noexcept = default;
    ~RiverRuntimeBinding() noexcept = default;

    std::uint64_t generation() const noexcept;
    std::uint64_t runtime_digest() const noexcept;
    std::uint64_t presentation_digest() const noexcept;
    bool sample(Float3 world_position_m, RiverFieldSample& out) const noexcept;
    std::size_t sample_batch(const Float3* positions, RiverFieldSample* samples,
                             std::size_t count) const noexcept;

private:
    struct Storage;
    std::shared_ptr<const Storage> storage_;
    friend class detail::RiverRuntimeBindingAccess;
};

}  // namespace matter
