#pragma once

// MatterEngine3/include/matter/physics.h
//
// Public ECS surface of the rigid-body physics module: the components you add
// to a flecs entity, the per-step event payloads, and the free functions that
// command or query the simulation. This header is data and declarations only.
// The solver sits behind it:
//
//   src/ecs/physics_context.{h,cpp}  the Box3d world, command queues, event
//                                    capture, ray/overlap queries
//   src/ecs/physics_shapes.cpp       validation + entity -> b3 body/shape
//   src/ecs/physics_systems.cpp      flecs system + observer registration
//
// AUTHORING A BODY. Give one entity an `ecs::LocalTransform`, exactly ONE
// collider component (Sphere/Capsule/Box/ConvexHull) and a `RigidBody`.
// `validate_desired_body` (physics_shapes.cpp) rejects anything else and the
// context writes a `PhysicsError` component naming the reason; a rejected
// entity simply has no solver body until it is fixed. Two hard requirements
// that are easy to trip over: the entity must be a ROOT (a `flecs::ChildOf`
// target is `HasParent`) and its transform scale must be unit (`NonUnitScale`)
// — neither is approximated away.
//
// PIPELINE PHASES. `PhysicsModule` registers four systems, all tagged
// `ecs::FixedPipelineSystem`, so they run on the fixed-step tick:
//
//   PhysicsReconcile  create / destroy / re-spec bodies whose components moved
//   PhysicsPush       ECS transforms, velocities and queued commands -> solver
//   ecs::Physics      the solver step itself
//   PhysicsPull       solver -> `ecs::LocalTransform` + `PhysicsVelocity`,
//                     plus the per-entity event dispatch described further down
//
// `PhysicsReconcile`, `PhysicsPush` and `PhysicsPull` are empty PHASE TAGS used
// as `.kind<>()` markers — they are not components you set on an entity.
//
// THREADING. Everything here is ECS-tick affine. The systems run inside the
// flecs fixed-step tick, and the free command functions below mutate queues
// owned by the same context; call them from the thread that pumps the world,
// not from a render or bake thread.
//
// UNITS AND CONVENTIONS. Lengths are metres and time is seconds —
// `PhysicsSettings::gravity` defaults to -9.81 on Y. Rotations are quaternions
// in (x, y, z, w) order; validation normalizes the transform rotation and
// rejects non-finite values anywhere in the desired body.

#include <cstdint>
#include <vector>

#include "matter/ecs.h"

namespace matter::physics {

// How the solver integrates the body. Maps 1:1 onto `b3BodyType`.
//   Static     never moves; infinite mass
//   Kinematic  moved by the ECS transform, unaffected by forces
//   Dynamic    fully simulated
// Only Dynamic bodies accept the command functions at the bottom of this file
// (`resolve_command_target` rejects everything else).
enum class RigidBodyType : uint8_t { Static, Kinematic, Dynamic };

// The "this entity is a rigid body" component. Its presence (together with a
// transform and exactly one collider) is what makes the reconcile stage build a
// solver body; removing it destroys the body. Every float here is validated
// finite and non-negative — a NaN sneaked in from a script leaves the entity
// with `PhysicsErrorCode::InvalidBody` rather than poisoning the solver.
struct RigidBody {
    RigidBodyType type = RigidBodyType::Static;
    float linear_damping = 0.0f;    // velocity damping; must be finite and >= 0
    float angular_damping = 0.0f;   // spin damping; must be finite and >= 0
    float gravity_scale = 1.0f;     // multiplies PhysicsSettings::gravity (0 = float)
    float sleep_threshold = 0.05f;  // settle threshold; must be finite and >= 0
    bool enable_sleep = true;       // false keeps the body simulating forever
    bool continuous = false;        // -> b3BodyDef::isBullet (swept collision)
};

// World-space velocity, optional on an entity. When present it seeds the body
// at creation and is validated alongside the RigidBody; the pull stage writes
// the solver's current velocity back into it every fixed step, so it is both an
// input and a readback. Write it through `physics_set_velocity` rather than by
// hand while the simulation is running — a direct component write is only read
// again when reconcile re-specs the body.
struct PhysicsVelocity {
    Float3 linear{};   // metres per second
    Float3 angular{};  // radians per second, axis-angle style
};

// Material / filtering settings shared by all four collider shapes, embedded in
// each of them rather than being its own component (one entity carries one
// collider, so there is nothing to disambiguate). These map onto the Box3d shape
// definition built in `physics_shapes.cpp`. `category_bits` is also what the
// `category_mask` argument of physics_ray_cast / physics_overlap_sphere filters
// against.
struct ColliderProperties {
    float density = 1.0f;        // mass comes from density x shape volume
    float friction = 0.6f;       // friction coefficient; must be finite and >= 0
    float restitution = 0.0f;    // bounciness; 0 = fully inelastic
    uint64_t category_bits = 1;  // which layers this shape belongs to
    uint64_t mask_bits = UINT64_MAX;  // which layers it collides with
    bool sensor = false;         // overlap reporting only, no contact response
    bool contact_events = true;  // false: this shape never fills contact_begin/end
    bool hit_events = false;     // opt-in: off means contact_hit stays empty
};

// ---------------------------------------------------------------------------
// Collider shapes. Exactly one per entity — two of them is
// `PhysicsErrorCode::MultipleColliders`, none is `MissingCollider`. All offsets
// (`center`, `point_a`/`point_b`, hull `points`) are in the entity's own frame,
// metres, and are validated finite with a positive radius before a shape is
// built.
// ---------------------------------------------------------------------------
struct SphereCollider {
    ColliderProperties properties{};
    Float3 center{};
    float radius = 0.5f;
};

struct CapsuleCollider {
    ColliderProperties properties{};
    Float3 point_a{0.0f, -0.5f, 0.0f};
    Float3 point_b{0.0f, 0.5f, 0.0f};
    float radius = 0.5f;
};

struct BoxCollider {
    ColliderProperties properties{};
    Float3 center{};
    Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    Float3 half_extents{0.5f, 0.5f, 0.5f};
};

// Convex hull from an inline point cloud. The array is a FIXED 32-point budget
// so the component stays POD and copyable; only the first `point_count` entries
// are read, and a hull the solver cannot build from them yields
// `PhysicsErrorCode::HullBuildFailed`.
struct ConvexHullCollider {
    ColliderProperties properties{};
    uint32_t point_count = 0;
    Float3 points[32]{};
};

// World-level solver settings, held as a singleton on the flecs world. Read at
// module construction and again whenever the singleton is re-set; `substeps` is
// clamped by the context before use, so a 0 here does not stall the solver.
struct PhysicsSettings {
    Float3 gravity{0.0f, -9.81f, 0.0f};  // metres per second squared
    uint32_t substeps = 4;               // solver iterations per fixed step
};

// Why an entity that looks like it should be simulated is not. Produced by
// `validate_desired_body` / the shape builders in physics_shapes.cpp, in the
// order the checks run.
enum class PhysicsErrorCode : uint8_t {
    None,
    MissingTransform,   // no ecs::LocalTransform on the entity
    HasParent,          // physics bodies must be root entities
    NonUnitScale,       // the transform's scale is not 1,1,1
    MissingCollider,    // no collider component
    MultipleColliders,  // more than one collider component
    InvalidBody,        // no RigidBody, or a non-finite body/transform/velocity
    InvalidCollider,    // collider dimensions non-finite or non-positive
    HullBuildFailed     // the solver could not build a hull from `points`
};

// Added to the offending entity while its configuration is rejected, and
// removed once it validates. Its presence also disables the command functions
// below for that entity, so a mis-authored body cannot be teleported or pushed
// into looking alive.
struct PhysicsError {
    PhysicsErrorCode code = PhysicsErrorCode::None;
};

// Pipeline PHASE tags, used as `.kind<>()` on the module's systems (see the
// file header for the ordering). They are never added to a game entity; a
// system that must run between two physics stages declares one of these as its
// phase.
struct PhysicsReconcile {};
struct PhysicsPush {};
struct PhysicsPull {};

// ---------------------------------------------------------------------------
// Per-step capture buffer payloads. These fill `PhysicsEvents`, which is
// rebuilt every fixed step from the solver's event arrays; nothing here
// survives the next step, so copy anything you need to keep. Pair rows are
// normalized to (first < second) by entity id, which is why the sensor channel
// cannot tell you which endpoint was the sensor.
// ---------------------------------------------------------------------------
struct PhysicsBodyEvent {
    flecs::entity_t entity = 0;
    bool awake = false;      // true = woke this step, false = went to sleep
};

struct PhysicsPairEvent {
    flecs::entity_t first = 0;
    flecs::entity_t second = 0;
};

// The only channel that carries contact GEOMETRY. Requires
// `ColliderProperties::hit_events` on a participating shape — it is off by
// default, so an empty `contact_hit` usually means nobody opted in.
struct PhysicsHitEvent {
    flecs::entity_t first = 0;
    flecs::entity_t second = 0;
    Float3 position{};           // world-space contact point, metres
    Float3 normal{};             // world-space contact normal
    float approach_speed = 0.0f; // closing speed at impact, m/s
};

// The whole per-step capture, as returned by `physics_events()`. Overwritten
// each fixed step; the reference is only good until the next pull. `body`
// carries sleep/wake transitions, the pair channels carry begin/end for
// contacts and sensors, and `contact_hit` is the opt-in geometry channel.
struct PhysicsEvents {
    std::vector<PhysicsBodyEvent> body;
    std::vector<PhysicsPairEvent> contact_begin;
    std::vector<PhysicsPairEvent> contact_end;
    std::vector<PhysicsHitEvent> contact_hit;
    std::vector<PhysicsPairEvent> sensor_begin;
    std::vector<PhysicsPairEvent> sensor_end;
};

// --- E6: per-entity gameplay events (docs/event-system.md S I.4 / S I.11) ---
//
// The orphaned PhysicsEvents snapshot (above) is the per-step *capture buffer*.
// E6 adds *delivery*: entity-shaped gameplay events are dispatched as flecs
// entity events on the ECS tick thread during the physics pull stage — NOT
// through the in-house evt::Hub (that carries non-entity / cross-thread
// notifications). These are the payload structs; each is emitted for the
// RigidBody component of a participating entity, so a gameplay observer keys on
//
//   world.observer<const physics::RigidBody>("...")
//        .event<physics::PhysContactBegin>()
//        .each([](flecs::iter& it, size_t i, const physics::RigidBody&) {
//            auto* e = static_cast<const physics::PhysContactBegin*>(it.param());
//            // it.entity(i) touched e->other
//        });
//
// Per-entity emit decision: BOTH endpoints of a contact/sensor pair receive an
// event carrying the counterpart, i.e. each entity independently hears "I
// touched `other`". The capture buffer normalizes pairs to (first < second) by
// entity id (physics_context.cpp), which discards the sensor-vs-visitor role;
// sensor enter/exit therefore reach both endpoints symmetrically. Begin/end
// carry only the counterpart entity because the snapshot pair rows hold no
// contact geometry (that lives on the separate contact_hit channel).
struct PhysContactBegin {
    flecs::entity_t other = 0;
};

struct PhysContactEnd {
    flecs::entity_t other = 0;
};

struct PhysSensorEnter {
    flecs::entity_t other = 0;
};

struct PhysSensorExit {
    flecs::entity_t other = 0;
};

// Monotonic counters for diagnostics and tests — a cheap by-value snapshot, not
// a live view. `rejected_configurations` and `stale_events` are the two worth
// watching: a climbing rejection count means entities are being authored the
// solver refuses, and stale events mean solver events referenced bodies whose
// entities had already gone away.
struct PhysicsStats {
    uint64_t steps = 0;                    // fixed steps executed
    uint64_t bodies_created = 0;           // cumulative, not current
    uint64_t bodies_destroyed = 0;         // cumulative
    uint64_t rejected_configurations = 0;  // validation failures (PhysicsError)
    uint64_t failed_commands = 0;          // enqueued commands the solver dropped
    uint64_t stale_events = 0;             // events whose entity no longer resolves
    uint32_t live_bodies = 0;              // current body count (not cumulative)
};

// Result of `physics_ray_cast`. Only meaningful when that call returns true;
// `entity` is 0 when nothing was hit.
struct PhysicsRayHit {
    flecs::entity_t entity = 0;
    Float3 position{};       // world-space hit point, metres
    Float3 normal{};         // world-space surface normal at the hit
    float fraction = 0.0f;   // 0..1 along the cast `translation` vector
};

struct CharacterMoveInput {
    Float3 position{}, velocity{}, desired_horizontal_velocity{};
    Float3 gravity{0.0f, -9.81f, 0.0f};
    float radius = 0.4f;
    float half_segment = 0.5f;
    float dt = 1.0f / 60.0f;
    float max_slope_cos = 0.70710678f;
    float step_height = 0.45f;
    uint64_t category_mask = UINT64_MAX;
};

struct CharacterMoveOutput {
    Float3 position{}, velocity{}, ground_normal{0.0f, 1.0f, 0.0f};
    bool grounded = false;
};

bool physics_move_character(
    flecs::world&, const CharacterMoveInput&, CharacterMoveOutput&);
struct PhysicsModule {
    explicit PhysicsModule(flecs::world&);
};

const PhysicsEvents& physics_events(const flecs::world&);
PhysicsStats physics_stats(const flecs::world&);
bool physics_teleport(flecs::entity, Float3, Quaternion);
bool physics_set_velocity(flecs::entity, Float3, Float3);
bool physics_apply_force(flecs::entity, Float3);
bool physics_apply_force_at_world_point(
    flecs::entity entity, Float3 force, Float3 world_point);
bool physics_apply_impulse(flecs::entity, Float3);
bool physics_wake(flecs::entity);
bool physics_ray_cast(flecs::world&, Float3, Float3, uint64_t, PhysicsRayHit&);
std::vector<flecs::entity_t> physics_overlap_sphere(
    flecs::world& world, Float3 center, float radius, uint64_t category_mask);

} // namespace matter::physics
