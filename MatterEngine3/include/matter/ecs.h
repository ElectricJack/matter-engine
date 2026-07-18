#pragma once

#include <cstdint>

#include "flecs.h"
#include "matter/math_types.h"

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
struct FixedPostUpdate {};
struct FrameUpdate {};
struct FixedPipelineSystem {};
struct FramePipelineSystem {};

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

} // namespace matter::ecs
