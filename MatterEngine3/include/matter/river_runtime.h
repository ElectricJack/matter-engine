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
