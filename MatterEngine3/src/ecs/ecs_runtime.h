#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "matter/ecs.h"
#include "matter/world_session.h"
#include "matter/world_definition.h"  // RawEntityRecipe
#include "scene_registry.h"           // SceneGeneration, PartResolver

namespace matter::ecs {

// Private Runtime tick seam. Exposed only from this internal header so tests
// can verify command retention without running a pipeline inside an outer defer.
void drain_hierarchy_commands(flecs::world& world);

} // namespace matter::ecs

namespace matter::physics::detail {

class PhysicsContext;

} // namespace matter::physics::detail

namespace matter::streaming::detail {

class Coordinator;

} // namespace matter::streaming::detail

namespace matter::ecs_runtime {

struct TickResult {
    uint32_t fixed_steps = 0;
    uint32_t dropped_steps = 0;
    bool invalid = false;
};

enum class WorldStateCommandKind { Loading, Ready, Failed };

struct WorldStateCommand {
    WorldStateCommandKind kind;
    std::vector<RawEntityRecipe> entities;  // populated for Ready commands
};

class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    flecs::world& world() noexcept;
    const flecs::world& world() const noexcept;
    // Internal test/session seam; this header is not part of MatterEngine's
    // public include surface.
    streaming::detail::Coordinator& streaming_coordinator() noexcept;
    const streaming::detail::Coordinator& streaming_coordinator() const noexcept;
    void enqueue_world_state(WorldStateCommand command);
    TickResult tick(const TickDesc& desc);

private:
    void drain_world_state_commands();

    flecs::world world_;
    std::unique_ptr<physics::detail::PhysicsContext> physics_;
    std::unique_ptr<streaming::detail::Coordinator> streaming_;
    flecs::entity fixed_pipeline_;
    flecs::entity frame_pipeline_;
    double accumulator_seconds_ = 0.0;
    std::mutex world_state_mutex_;
    std::vector<WorldStateCommand> world_state_commands_;
    scene::SceneGeneration scene_generation_{};
};

} // namespace matter::ecs_runtime
