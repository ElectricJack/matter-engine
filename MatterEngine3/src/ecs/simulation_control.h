#pragma once

#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/scene.h"

#include "flecs.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace matter::scene {

// Snapshot of one entity's editable state (whitelisted components only).
struct EntitySnapshot {
    SceneEntityId id{};
    SceneEntityId parent_id{};
    std::string name;
    ecs::LocalTransform transform{};
    PartInstance part_instance{};
    bool has_part_instance = false;
    physics::RigidBody rigid_body{};
    bool has_rigid_body = false;
    physics::PhysicsVelocity velocity{};
    bool has_velocity = false;
    physics::BoxCollider box_collider{};
    bool has_box_collider = false;
    physics::SphereCollider sphere_collider{};
    bool has_sphere_collider = false;
    physics::CapsuleCollider capsule_collider{};
    bool has_capsule_collider = false;
};

// Complete scene snapshot taken at Play transition.
struct SceneSnapshot {
    std::vector<EntitySnapshot> entities;
    uint64_t generation = 0;
    bool valid = false;
};

// Manages simulation mode transitions and scene snapshot/restore.
class SimulationControl {
public:
    SimulationMode mode() const { return mode_; }

    // Transition to Play: captures snapshot of all SceneEntityId entities.
    bool play(flecs::world& world, std::string& error);

    // Transition to Pause (from Play only).
    bool pause(std::string& error);

    // Execute exactly one fixed step (from Pause only). Returns true
    // if the step should be applied by the caller.
    bool step(std::string& error);

    // Stop and restore: returns to Edit mode, restores snapshot.
    // Destroys Play-created entities, recreates deleted ones.
    bool stop(flecs::world& world, std::string& error);

    // Query whether fixed-step accumulation should advance this frame.
    bool should_advance_fixed() const;

    // Query whether exactly one step was requested (and consume it).
    bool consume_pending_step();

    // Was a snapshot taken (valid for restore)?
    bool has_snapshot() const { return snapshot_.valid; }

    // Get the snapshot (for testing).
    const SceneSnapshot& snapshot() const { return snapshot_; }

private:
    void capture_snapshot(flecs::world& world);
    void restore_snapshot(flecs::world& world);

    SimulationMode mode_ = SimulationMode::Edit;
    SceneSnapshot snapshot_;
    bool step_pending_ = false;
};

} // namespace matter::scene
