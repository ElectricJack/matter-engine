#pragma once
#include <matter/asset/collision.h>

namespace matter::asset::collision {
struct HeightField {
    uint32_t count_x = 0, count_z = 0;
    float cell = 0;
    std::vector<float> heights;
};
struct Settings {
    float dt = 1.0f / 120.0f;
    uint32_t substeps = 4;
    float max_sim_time = 10, sleep_fraction = 0.99f, sim_scale = 4;
    uint32_t micro_relax_steps = 30;
    bool finalize = true;
};
struct Spawn {
    data::SemanticId body;
    data::Vec3 velocity{};
    int32_t sync_group = -1;
    uint32_t occurrence = 0;
};
struct Request {
    Product collision;
    float torus_size = 8;
    HeightField ground;
    Settings settings;
    std::vector<std::vector<Pose>> sync_groups;
    std::vector<std::vector<Spawn>> layers;
    std::vector<data::Capability> required_capabilities;
};
struct LayerReport { bool converged = false; uint32_t awake = 0; float sim_time = 0; };
struct SettledProduct {
    Product collision;
    std::vector<LayerReport> layers;
};
using SettleResult = services::JobResult<SettledProduct>;
// Available in the solver-free target too: disabled builds return an explicit
// unsupported_capability error. Each call is serialized around legacy Box3d's
// process-global world table; it never touches an ECS or writes a cache.
bool generation_available() noexcept;
SettleResult settle(const services::JobEnvelope&, const Request&,
                    const services::ResourceBudget& = {}, const services::JobControl& = {});
} // namespace matter::asset::collision
