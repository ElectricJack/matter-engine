#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "tileset_collider.h"

namespace tileset {

// World-space rigid pose, meters, xyzw quaternion.
struct Pose { float px, py, pz; float qx, qy, qz, qw; };

struct BodySpawn {
    const ColliderFit* collider = nullptr;  // borrowed for the settle_layer call
    Pose start = { 0, 0, 0, 0, 0, 0, 1 };
    float density = 400.0f;   // kg/m^3 (dry wood-ish default)
    float friction = 0.6f;
    float vx = 0, vy = 0, vz = 0;
    int sync_group = -1;      // -1 = free body; >= 0 = portal-sync group id
    int instance = 0;         // occurrence index within the sync group
};

struct SettleParams {
    float dt = 1.0f / 120.0f;
    int substeps = 4;
    float max_sim_time = 10.0f;    // per layer
    float sleep_fraction = 0.99f;  // converged when this fraction is asleep
    float sim_scale = 4.0f;        // meters -> sim units (linear-slop tuning)
    int micro_relax_steps = 30;    // finalize(): relax after snap
};

// Base terrain over the full torus; row-major heights, `cell` meters apart.
struct HeightField {
    int count_x = 0, count_z = 0;
    float cell = 0.0f;
    std::vector<float> heights;
};

struct LayerResult {
    bool converged = false;
    int awake_count = 0;   // awake dynamic bodies when the loop ended
    float sim_time = 0.0f;
};

// Per-tick telemetry captured by SettleWorld::step().
struct TickStats {
    int   tick = 0;                // tick index within the current layer (0-based)
    float sim_time = 0.0f;         // accumulated sim seconds this layer
    int   layer_awake = 0, total_awake = 0;
    float max_lin_vel = 0.0f, max_ang_vel = 0.0f;   // this layer, sim units
    bool  first_contact = false;   // latched true after fall phase ends
    int   wake_events = 0;         // asleep->awake transitions this tick
    float step_ms = 0.0f;
};

// Per-body inspection state (world units, unscaled).
struct BodyState {
    Pose  pose;
    float lin_vel[3], ang_vel[3];
    bool  awake = false;
    int   ticks_awake = 0;
    int   sync_group = -1, instance = 0;
};

// One box3d world spanning the 4x4 torus. Bodies wrap toroidally in x/z.
class SettleWorld {
public:
    SettleWorld(float torus_size, const HeightField& base, const SettleParams& params);
    ~SettleWorld();
    SettleWorld(const SettleWorld&) = delete;
    SettleWorld& operator=(const SettleWorld&) = delete;

    // Portal-sync group: K world-space occurrence frames of one canonical
    // strip frame. Members are bound via BodySpawn::{sync_group, instance}.
    int add_sync_group(const std::vector<Pose>& occurrence_frames);

    // Spawn one layer and step until converged or out of time.
    // Earlier layers stay dynamic (asleep unless disturbed).
    // Implemented on top of the step API below; bit-identical to stepping
    // manually: begin_layer + step while (sim_time < max_sim_time &&
    // !layer_converged()) + end_layer.
    LayerResult settle_layer(const std::vector<BodySpawn>& bodies);

    // Step mode. begin_layer spawns bodies exactly as settle_layer does.
    // step() advances one tick: b3World_Step -> wrap_bodies -> sync_groups_step
    // -> telemetry. layer_converged() applies the same sleep_fraction rule
    // (false until the first step of the layer). end_layer() refreshes poses()
    // and returns the aggregate LayerResult.
    void      begin_layer(const std::vector<BodySpawn>& spawns);
    TickStats step();
    bool      layer_converged() const;
    LayerResult end_layer();

    int       body_count() const;
    BodyState body_state(int index) const;   // index = spawn order

    // After the last layer: snap sync groups to exact shared poses, switch
    // them kinematic, micro-relax, and refresh poses().
    void finalize();

    // Final world-space poses, in spawn order across all layers.
    const std::vector<Pose>& poses() const;

    // FNV-1a hash over all final poses (determinism gate).
    uint64_t pose_hash() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tileset
