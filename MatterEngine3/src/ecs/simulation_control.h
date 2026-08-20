// MatterEngine3/src/ecs/simulation_control.h
//
// Editor play-state control: the Edit/Play/Pause state machine and the scene
// snapshot that makes Stop non-destructive.
//
// `SimulationMode` itself is declared in matter/scene.h; this header owns the
// object that transitions between its values and the snapshot types those
// transitions use. The editor holds one `SimulationControl` per session and
// asks `should_advance_fixed()` / `consume_pending_step()` each frame to decide
// whether to run the fixed pipeline.
//
// Snapshot scope is a deliberate WHITELIST (see EntitySnapshot): identity,
// parent, name, transform, PartInstance, RigidBody, PhysicsVelocity and all
// four colliders (box/sphere/capsule/convex-hull). Components outside it —
// SectorStreaming, anything gameplay adds — are not restored by stop().
//
// Threading: app-thread affine. Every method that takes a `flecs::world&`
// mutates it directly and takes no locks.

#pragma once

#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/scene.h"
#include "animation/animation_evaluator.h"

#include "flecs.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace matter { class AnimationService; }

namespace matter::scene {

// Snapshot of one entity's editable state (whitelisted components only).
//
// Pure value type — no flecs handles — so it stays meaningful after the entity
// it came from is destroyed, which is exactly what stop() relies on. Each
// optional component is a value plus a `has_*` flag; the flag is what decides
// whether the component is re-added on restore, so a default-valued component
// and an absent one stay distinguishable.
struct EntitySnapshot {
    SceneEntityId id{};
    SceneEntityId parent_id{};  // value == 0 means "was a root entity"
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
    physics::ConvexHullCollider convex_hull_collider{};
    bool has_convex_hull_collider = false;
};

// Complete scene snapshot taken at Play transition.
//
// `valid` is set only after the whole capture succeeded, and is what
// `has_snapshot()` reports; stop() replaces the whole struct with a default one
// on the way back to Edit. The checkpoint vector is budgeted at 64 KiB
// serialized in total.
struct SceneSnapshot {
    std::vector<EntitySnapshot> entities;
    std::vector<animation::AnimatorCheckpoint> animator_checkpoints;
    bool valid = false;
};

// Manages simulation mode transitions and scene snapshot/restore.
//
// One instance per editor session, owned by the caller; default-constructed in
// Edit mode with no snapshot. Every transition is gated on the current mode and
// returns false with an explanatory `error` string rather than forcing the
// change: play() only from Edit, pause() only from Play, step() only from
// Pause, stop() from anything but Edit.
//
// App-thread affine, and not internally synchronized. The methods taking a
// `flecs::world&` destroy and recreate entities, so they must not be called
// from inside a running system or while a frame is iterating the world.
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
    // Optional runtime binding.  When present, Play/Stop captures/restores
    // every descriptor-bound animator through the service rather than relying
    // on callers to hand-maintain a parallel checkpoint vector.
    void attach_animation_service(AnimationService* service) noexcept { animation_service_ = service; }
    bool set_animator_checkpoints(std::vector<animation::AnimatorCheckpoint> checkpoints);
    const std::vector<animation::AnimatorCheckpoint>& animator_checkpoints() const { return animator_checkpoints_; }

private:
    bool capture_snapshot(flecs::world& world);
    bool restore_snapshot(flecs::world& world);

    SimulationMode mode_ = SimulationMode::Edit;
    SceneSnapshot snapshot_;
    bool step_pending_ = false;  // latched by step(), drained by consume_pending_step()
    // Caller-maintained checkpoint set, used as the starting point for a
    // capture and overwritten from the snapshot on restore.
    std::vector<animation::AnimatorCheckpoint> animator_checkpoints_;
    // Non-owning; null means "no animation runtime bound" and the checkpoint
    // capture/validate/restore steps are skipped entirely.
    AnimationService* animation_service_ = nullptr;
};

} // namespace matter::scene
