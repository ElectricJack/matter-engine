#pragma once
// MatterEngine3/src/ecs/physics_context.h
//
// PhysicsContext: the owner of a Box3D world and of the bridge records that pair
// each physics entity with its native body, shape and broadphase proxy.
//
// INTERNAL header (namespace matter::physics::detail). Gameplay code uses the
// free functions in matter/physics.h — physics_teleport, physics_ray_cast and
// friends — which resolve the context off the world and forward here. Systems
// reach it through the PhysicsContextRef singleton at the bottom of this file.
//
// Lifecycle: constructed by ecs_runtime::Runtime from the world's
// PhysicsSettings and published as PhysicsContextRef; THROWS from its
// constructor if Box3D cannot create a world. Runtime nulls the singleton before
// destroying it, because flecs observers outlive it. Neither copyable nor
// movable — bridge records store raw back-pointers into it via Box3D userData.
//
// Per fixed step the pipeline calls, in this order:
//   reconcile(world) -> push(world, dt) -> step(world, dt) -> pull(world)
// Calling them out of order is not diagnosed; reconcile() also resets the stage
// trace, so it must come first.
//
// Threading: the enqueue_* methods are safe from any thread (validated, then
// mutex-guarded, and noexcept — they return false rather than throwing). Every
// other member, including the queries, is tick-thread only, and the queries
// additionally refuse to run while a step is in progress.
//
// Units follow the engine's: metres, seconds, radians-free quaternions.

#include <cstdint>
#include <memory>
#include <vector>

#include "matter/physics.h"

namespace matter::evt {
class Hub;
}  // namespace matter::evt

namespace matter::physics::detail {

// The four fixed-step stages, in execution order. Recorded into
// fixed_step_trace() so tests can assert a step really ran the whole sequence
// (a stage that early-outs on an invalid world contributes no entry).
enum class PhysicsSystemStage : uint8_t { Reconcile, Push, Step, Pull };

// Which queued mutation a command carries. The kind also decides its queueing
// semantics: Teleport and Velocity are last-write-wins per entity within a step,
// Force and Impulse accumulate in enqueue order, and Wake deduplicates.
enum class PhysicsCommandKind : uint8_t {
    Teleport,
    Velocity,
    Force,
    Impulse,
    Wake
};

// One APPLIED command, recorded for tests and the inspector by push(). Commands
// that were dropped at revalidation never appear here — they are counted in
// PhysicsStats::failed_commands instead. The payload fields are kind-dependent:
// `primary` is the position/linear velocity/force/impulse, `secondary` the
// angular velocity, `rotation` the teleport orientation.
struct PhysicsCommandTraceEntry {
    PhysicsCommandKind kind = PhysicsCommandKind::Wake;
    flecs::entity_t entity = 0;
    Float3 primary{};
    Float3 secondary{};
    Quaternion rotation{};
};

// A native body's full simulation state, in world space: position in metres,
// unit rotation quaternion, linear velocity in m/s, angular velocity in rad/s.
// Used to carry dynamic state across a body rebuild and by the get/set
// accessors below; it is not an ECS component.
struct PhysicsBodyState {
    Float3 position{};
    Quaternion rotation{};
    Float3 linear_velocity{};
    Float3 angular_velocity{};
    bool awake = false;
};

// See the file header for lifecycle, stage order and threading. One per Runtime;
// it owns the Box3D world, one BridgeRecord per physics entity, its own
// broadphase tree for ray/overlap queries, and the queued command state.
class PhysicsContext {
public:
    explicit PhysicsContext(const PhysicsSettings& settings);
    ~PhysicsContext();

    PhysicsContext(const PhysicsContext&) = delete;
    PhysicsContext& operator=(const PhysicsContext&) = delete;
    PhysicsContext(PhysicsContext&&) = delete;
    PhysicsContext& operator=(PhysicsContext&&) = delete;

    const PhysicsEvents& events() const noexcept;
    PhysicsStats stats() const noexcept;

    // E6 trace mirror (docs/event-system.md S I.8 / S I.11): optional session
    // hub onto which pull() emits one aggregate events::PhysStep per active
    // fixed step. Null by default (headless physics tests need no hub); wired
    // by the owning WorldSession via ecs_runtime::Runtime::set_physics_event_hub.
    void set_event_hub(matter::evt::Hub* hub) noexcept;
    bool world_is_valid() const noexcept;
    // Mark an entity for the next reconcile pass. noexcept and allocation-safe:
    // if the mark cannot be recorded, the context fails CLOSED by flagging a
    // full audit of every body and bridge on the next pass, because a lost mark
    // could otherwise leave a native body alive for a dead entity.
    void mark_for_reconcile(flecs::entity_t entity) noexcept;
    // Same, for a transform change — but it first filters out the write that
    // pull() itself just made, so physics writing a pose back into the ECS does
    // not schedule a reconcile of the body it came from.
    void mark_transform_for_reconcile(flecs::entity entity) noexcept;
    // The fixed-step stages, called in this order by the physics phases (see
    // the file header). Each is a no-op if the Box3D world is invalid.
    //   reconcile — create/destroy/rebuild native bodies from the declarative
    //               components; writes PhysicsError onto entities; the only
    //               point at which a bridge is retired.
    //   push      — apply queued commands and drive static/kinematic bodies
    //               from their ECS transform. Dynamic bodies are not pushed.
    //   step      — advance the solver by fixed_delta and snapshot its events.
    //   pull      — write dynamic poses/velocities back to the ECS and deliver
    //               the captured contact/sensor events.
    // fixed_delta is in seconds.
    void reconcile(flecs::world& world);
    void push(flecs::world& world, float fixed_delta);
    void step(flecs::world& world, float fixed_delta);
    void pull(flecs::world& world);

    // Queued mutations, safe to call from any thread. Each returns false — with
    // nothing queued — for a non-finite payload, an entity that is not a live,
    // error-free DYNAMIC body of this context's world, or a failed allocation;
    // false is a normal outcome, not an exception path. Commands take effect in
    // the next push(), where they are revalidated and may still be dropped.
    // See PhysicsCommandKind for the per-kind collapsing rules.
    bool enqueue_teleport(
        const flecs::world_t* originating_world,
        flecs::entity_t entity,
        Float3 position,
        Quaternion rotation) noexcept;
    bool enqueue_velocity(
        const flecs::world_t* originating_world,
        flecs::entity_t entity,
        Float3 linear,
        Float3 angular) noexcept;
    bool enqueue_force(
        const flecs::world_t* originating_world,
        flecs::entity_t entity,
        Float3 force) noexcept;
    bool enqueue_impulse(
        const flecs::world_t* originating_world,
        flecs::entity_t entity,
        Float3 impulse) noexcept;
    bool enqueue_wake(
        const flecs::world_t* originating_world,
        flecs::entity_t entity) noexcept;

    // Spatial queries against the context's own broadphase tree, reflecting
    // proxy bounds as of the last body movement/teleport. `translation` is the
    // full ray vector, not a direction; `category_mask` is tested against the
    // category bits captured when each proxy was created. Both refuse to run
    // (false / empty) while a step is in progress or against a world this
    // context does not own. overlap_sphere allocates and returns entities sorted
    // and deduplicated.
    bool ray_cast(
        flecs::world& world,
        Float3 origin,
        Float3 translation,
        uint64_t category_mask,
        PhysicsRayHit& hit);
    std::vector<flecs::entity_t> overlap_sphere(
        flecs::world& world,
        Float3 center,
        float radius,
        uint64_t category_mask);

    // Ray cast against the live Box3D world, so static terrain colliders (which
    // are not ECS entities) are visible. See design C1.
    bool cast_ray_world(
        Float3 origin,
        Float3 translation,
        uint64_t category_mask,
        PhysicsWorldRayHit& hit);

    // Static terrain collider lifecycle. attach_* returns 0 on failure; the
    // detach destroys body/shape then the referenced geometry.
    TerrainColliderHandle attach_static_mesh(const StaticMeshCollider& desc);
    TerrainColliderHandle attach_static_heightfield(
        const StaticHeightFieldCollider& desc);
    bool detach_static(TerrainColliderHandle handle);

    // Kinematic capsule collide-and-slide against the live world (design §2).
    bool move_character(
        const CharacterMoveInput& in, CharacterMoveOutput& out);

    uint32_t last_step_substeps() const noexcept;
    const std::vector<PhysicsSystemStage>& fixed_step_trace() const noexcept;
    const std::vector<PhysicsCommandTraceEntry>&
    last_command_trace() const noexcept;

    bool body_is_valid(flecs::entity_t entity) const noexcept;
    bool shape_is_valid(flecs::entity_t entity) const noexcept;
    flecs::entity_t user_data_entity(flecs::entity_t entity) const noexcept;
    bool get_body_state(
        flecs::entity_t entity,
        PhysicsBodyState& state) const noexcept;
    bool set_body_state(
        flecs::entity_t entity,
        const PhysicsBodyState& state) noexcept;
    // ---- Fault-injection and inspection hooks for tests ----
    // Not for production use. Each tombstone/duplicate/fail hook arms a
    // one-shot condition that the next affected pass consumes and clears, which
    // is how the suites exercise stale-event and allocation-failure paths that
    // are otherwise unreachable.
    bool force_configuration_hash_for_test(
        flecs::entity_t entity,
        uint64_t hash) noexcept;
    bool tombstone_event_participant_for_test(
        flecs::entity_t entity) noexcept;
    bool tombstone_query_participant_for_test(
        flecs::entity_t entity) noexcept;
    bool duplicate_overlap_participant_for_test(
        flecs::entity_t entity) noexcept;
    void fail_next_reconcile_mark_for_test() noexcept;
    uint64_t ray_query_candidate_attempts_for_test() const noexcept;
    uint64_t overlap_query_candidate_attempts_for_test() const noexcept;
    void set_stepping_for_test(bool stepping) noexcept;

private:
    void capture_events(flecs::world& world);

    struct Impl;

    std::unique_ptr<Impl> impl_;
    PhysicsEvents events_;
    PhysicsStats stats_;
    matter::evt::Hub* event_hub_ = nullptr;  // E6 trace mirror; optional
};

// Singleton component published on the flecs world so systems and the free
// physics API can find the context without a global. Runtime sets it at
// construction and NULLS it during teardown while observers are still live, so
// `value` being null is a normal state and every reader must handle it.
struct PhysicsContextRef {
    PhysicsContext* value = nullptr;
};

// Fetch the context published on `world`. Both overloads THROW
// std::runtime_error when the singleton is missing or nulled;
// context_world_is_valid() is the non-throwing probe.
PhysicsContext& context(flecs::world& world);
const PhysicsContext& context(const flecs::world& world);
bool context_world_is_valid(const flecs::world& world);

} // namespace matter::physics::detail
