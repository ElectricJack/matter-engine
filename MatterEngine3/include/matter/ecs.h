#pragma once

// MatterEngine3/include/matter/ecs.h
//
// Public surface of MatterEngine's Flecs-based entity/component runtime: the
// core transform components, the pipeline phase tags every subsystem hangs
// its systems off, and the two supported ways to change an entity's parent.
//
// How it fits:
//   * `CoreModule` is imported once per world by
//     MatterEngine3/src/ecs/ecs_runtime.cpp. Importing it registers Flecs
//     reflection for the types below, declares the phase chain, installs the
//     transform-propagation systems
//     (MatterEngine3/src/ecs/transform_system.cpp) and seeds the
//     WorldRuntimeState / AnimationFixedState / AnimationFrameState
//     singletons.
//   * Physics (matter/physics.h) and streaming (matter/streaming.h) register
//     their own modules and attach their systems to the phases declared here.
//
// Phase order established by CoreModule (the whole chain sits after
// flecs::PreUpdate):
//
//   FixedPreUpdate -> FixedUpdate -> PrePhysics -> Physics -> PostPhysics
//     -> PostPhysicsHierarchy -> FixedPostUpdate -> FrameUpdate
//
// The Fixed* phases belong to the fixed simulation step and may run zero or
// several times per rendered frame; FrameUpdate runs once per rendered frame
// and is presentation-only — nothing in it may advance simulation time.
//
// Conventions:
//   * Transform components use the POD types in matter/math_types.h.
//     Translation is in metres; `WorldTransform::matrix` is row-major.
//   * `TransformDirty` is the tag the propagation systems match on;
//     reparent/clear_parent mark the moved subtree with it for you.

#include <cstdint>

#include "flecs.h"
#include "matter/math_types.h"
#include "matter/streaming.h"

namespace matter::ecs {

// The authoring transform, relative to the entity's ChildOf parent (or to the
// world when it has none). This is the component gameplay and animation
// write; translation is in metres and `rotation` is expected to be a unit
// quaternion.
struct LocalTransform {
    Float3 translation{};
    Quaternion rotation{};
    Float3 scale{1.0f, 1.0f, 1.0f};
};

// Derived output of the transform-propagation systems: the accumulated
// world matrix for this entity. Engine code treats it as read-only — a value
// written by hand is replaced the next time the entity is propagated.
struct WorldTransform {
    Mat4f matrix{};
};

// Tag marking an entity as needing world-transform propagation. The
// hierarchy APIs at the bottom of this header set it across the whole moved
// subtree.
struct TransformDirty {};

// Load state of the world as a whole, reported through the WorldRuntimeState
// singleton below.
enum class WorldStatus : uint8_t {
    Loading,
    Ready,
    Failed
};

// Singleton describing the world as a whole; CoreModule seeds it with the
// default (status = Loading) at import. `content_generation` is a stamp meant
// to be compared against a previously observed value — do not attach meaning
// to the number itself.
struct WorldRuntimeState {
    WorldStatus status = WorldStatus::Loading;
    uint64_t content_generation = 0;
};

// Pipeline phase tags. CoreModule turns each of these into a flecs::Phase and
// chains them in the order given in the file header; a system declares its
// slot with `.kind<matter::ecs::FixedUpdate>()` and friends.
//
// FixedPipelineSystem / FramePipelineSystem are not phases — they are marker
// tags added to a system entity so the runtime can tell which of the two
// pipelines a system belongs to.
struct FixedPreUpdate {};
struct FixedUpdate {};
struct PrePhysics {};
struct Physics {};
struct PostPhysics {};
// Ordered tail of PostPhysics: user systems observe physics pulls first, then
// hierarchy propagation makes the pulled transforms current for FixedPostUpdate.
struct PostPhysicsHierarchy {};
struct FixedPostUpdate {};
struct FrameUpdate {};
struct FixedPipelineSystem {};
struct FramePipelineSystem {};

// Fixed simulation state advances only inside FixedPreUpdate.  Animation
// systems use this singleton to rotate their previous/current sampled state;
// presentation must never advance it from FrameUpdate.
struct AnimationFixedState {
    uint64_t previous_tick = 0;
    uint64_t current_tick = 0;
};

// Presentation-only state written once per rendered frame after fixed stepping.
// interpolation_alpha is the clamped fixed-time remainder; it never changes
// simulation time or the number of fixed steps executed.
struct AnimationFrameState {
    uint64_t frame_serial = 0;
    double interpolation_alpha = 0.0;
};

// Flecs module for everything above: import it with
// `world.import<matter::ecs::CoreModule>()`. Registers the components with
// reflection, declares the phase chain, installs transform propagation and
// seeds the singletons. Import it once per flecs::world, before registering
// any system that names one of the phases.
struct CoreModule {
    explicit CoreModule(flecs::world& world);
};

// These are MatterEngine's supported hierarchy mutation APIs. Direct ChildOf
// edits bypass MatterEngine validation. Destroying a parent retains Flecs'
// built-in ownership behavior and deletes descendants. Only one hierarchy
// mutation may be outstanding per child: reparent returns false and
// clear_parent does nothing while that child's earlier mutation is pending.
// reparent() also returns false for a dead entity, for two entities in
// different worlds, for child == parent and for any parent that would close a
// cycle; it returns TRUE without doing work when `parent` is already the
// child's effective parent. Both calls defer the actual ChildOf edit through
// flecs::world::defer and mark the moved subtree TransformDirty, so the
// hierarchy does not change under an iterating system.
bool reparent(flecs::entity child, flecs::entity parent);
void clear_parent(flecs::entity child);

// Queue a final desired hierarchy state for the beginning of the next valid
// runtime tick. Requests are last-write-wins per child and are safe to issue
// while that child's immediate hierarchy mutation is still pending.
void enqueue_reparent(flecs::entity child, flecs::entity parent);
void enqueue_clear_parent(flecs::entity child);

} // namespace matter::ecs
