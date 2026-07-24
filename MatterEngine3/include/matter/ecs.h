#pragma once

#include <cstdint>

#include "flecs.h"
#include "matter/math_types.h"
#include "matter/streaming.h"

namespace matter::ecs {

struct LocalTransform {
    Float3 translation{};
    Quaternion rotation{};
    Float3 scale{1.0f, 1.0f, 1.0f};
};

struct WorldTransform {
    Mat4f matrix{};
};

struct TransformDirty {};

enum class WorldStatus : uint8_t {
    Loading,
    Ready,
    Failed
};

struct WorldRuntimeState {
    WorldStatus status = WorldStatus::Loading;
    uint64_t content_generation = 0;
};

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

struct CoreModule {
    explicit CoreModule(flecs::world& world);
};

// These are MatterEngine's supported hierarchy mutation APIs. Direct ChildOf
// edits bypass MatterEngine validation. Destroying a parent retains Flecs'
// built-in ownership behavior and deletes descendants. Only one hierarchy
// mutation may be outstanding per child: reparent returns false and
// clear_parent does nothing while that child's earlier mutation is pending.
bool reparent(flecs::entity child, flecs::entity parent);
void clear_parent(flecs::entity child);

// Queue a final desired hierarchy state for the beginning of the next valid
// runtime tick. Requests are last-write-wins per child and are safe to issue
// while that child's immediate hierarchy mutation is still pending.
void enqueue_reparent(flecs::entity child, flecs::entity parent);
void enqueue_clear_parent(flecs::entity child);

} // namespace matter::ecs
