#pragma once
// MatterEngine3/src/tileset_settle.h — box3d rigid-body settle for one
// tileset torus.
//
// What it is: a deterministic wrapper around a single box3d world used at
// BAKE time to drop tileset litter (pebbles, twigs, leaves) onto the base
// terrain and let it come to rest. The resulting poses are baked into the
// tileset; nothing in this file runs at play time.
//
// Where it sits: tileset_bake.cpp builds a SettlePlan (colliders, spawns,
// sync groups) and drives this class; the final poses become
// SettledTorus::instances, which tileset_torus_bvh.cpp turns into BLAS/TLAS.
// The interactive Settle Lab drives the same class one tick at a time through
// the step API.
//
// Lifecycle
//   1. Construct with the torus size, the base HeightField and SettleParams.
//   2. add_sync_group() for each portal-synced strip, in a fixed order -- the
//      returned id is what BodySpawn::sync_group refers to.
//   3. One settle_layer() (or begin_layer / step / layer_converged /
//      end_layer) per script layer, in declaration order. Earlier layers stay
//      dynamic and can be disturbed by later ones.
//   4. finalize(), then poses() / pose_hash().
//
// Units and spaces
//   - The public API is world-space metres, seconds, kg/m^3, xyzw
//     quaternions. Internally everything is multiplied by
//     SettleParams::sim_scale, because box3d's linear slop is tuned for
//     human-scale objects while tileset litter is centimetre-scale; poses
//     handed back are divided out again.
//   - Positions are torus space: x/z are wrapped into [0, torus_size) every
//     tick, y is unbounded height.
//
// Threading and determinism
//   Not thread-safe: a SettleWorld belongs to the thread that built it. The
//   whole point of the class is bit-reproducibility -- pose_hash() is the
//   determinism gate the tests assert on, so any change to tick order, spawn
//   order or step counts changes baked output.
//
// Gotcha: BodySpawn::collider is a BORROWED pointer. Whatever owns the
// ColliderFit objects (SettlePlan::colliders in tileset_bake.h) must outlive
// the settle run.

#include <cstdint>
#include <memory>
#include <vector>
#include "tileset_collider.h"

namespace tileset {

// World-space rigid pose, meters, xyzw quaternion.
struct Pose { float px, py, pz; float qx, qy, qz, qw; };

// One dynamic body to spawn at the start of a layer. Everything is world-space
// metres / SI; SettleWorld applies sim_scale internally, including dividing
// `density` by sim_scale^3 so the resulting mass is scale-invariant.
struct BodySpawn {
    const ColliderFit* collider = nullptr;  // borrowed for the settle_layer call
    Pose start = { 0, 0, 0, 0, 0, 0, 1 };
    float density = 400.0f;   // kg/m^3 (dry wood-ish default)
    float friction = 0.6f;
    float vx = 0, vy = 0, vz = 0;
    int sync_group = -1;      // -1 = free body; >= 0 = portal-sync group id
    int instance = 0;         // occurrence index within the sync group
};

// Fixed-step simulation settings, shared by every layer of one bake. These are
// part of the determinism contract: change any of them and every baked pose
// moves.
struct SettleParams {
    // Seconds of sim time per tick, and box3d solver substeps within a tick.
    float dt = 1.0f / 120.0f;
    int substeps = 4;
    float max_sim_time = 10.0f;    // per layer
    float sleep_fraction = 0.99f;  // converged when this fraction is asleep
    float sim_scale = 4.0f;        // meters -> sim units (linear-slop tuning)
    int micro_relax_steps = 30;    // finalize(): relax after snap
};

// Base terrain over the full torus; row-major heights, `cell` meters apart.
// Heights are metres. It becomes a b3HeightField shape rather than a triangle
// mesh (see the constructor for why), and SettleWorld passes heights.data()
// straight to box3d without copying it, so the HeightField must stay alive and
// unresized for as long as the SettleWorld exists.
struct HeightField {
    int count_x = 0, count_z = 0;
    float cell = 0.0f;
    std::vector<float> heights;
};

// Outcome of one layer's settle. converged == false means the layer used up
// max_sim_time with more than (1 - sleep_fraction) of ITS bodies still awake:
// a quality warning reported through SettleReport::converged_all, not a hard
// error. Note the asymmetry -- convergence is judged on this layer's bodies
// only, while awake_count counts every body in the world.
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

// Owns the box3d world and every body in it; non-copyable, and the destructor
// tears down both the world and the heightfield data. Bodies accumulate across
// layers and are never removed, so a body's spawn-order index is stable for
// the whole run -- that index is what poses() and body_state() use.
//
// Call order matters: add_sync_group() before any spawn referencing it, layers
// in declaration order, finalize() exactly once at the end. poses() is only
// refreshed by end_layer() and finalize(), so it is stale mid-layer.
//
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

    // Live inspection for the Settle Lab and the tests. body_state() does not
    // bounds-check `index`; it must be in [0, body_count()).
    int       body_count() const;
    BodyState body_state(int index) const;   // index = spawn order

    // After the last layer: snap sync groups to exact shared poses, switch
    // them kinematic, micro-relax, and refresh poses().
    void finalize();

    // Final world-space poses, in spawn order across all layers.
    const std::vector<Pose>& poses() const;

    // FNV-1a hash over all final poses (determinism gate).
    // Hashes the raw float bytes, so it is exact-bit sensitive and shifts on
    // any floating-point difference. It reflects whatever the last end_layer()
    // / finalize() wrote into poses(), not the live body state.
    uint64_t pose_hash() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tileset
