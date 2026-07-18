#pragma once

#include <mutex>
#include <vector>

#include "matter/ecs.h"
#include "matter/world_session.h"

namespace matter::ecs {

// Private Runtime tick seam. Exposed only from this internal header so tests
// can verify command retention without running a pipeline inside an outer defer.
void drain_hierarchy_commands(flecs::world& world);

} // namespace matter::ecs

namespace matter::ecs_runtime {

struct TickResult {
    uint32_t fixed_steps = 0;
    uint32_t dropped_steps = 0;
    bool invalid = false;
};

enum class WorldStateCommandKind { Loading, Ready, Failed };

struct WorldStateCommand {
    WorldStateCommandKind kind;
};

class Runtime {
public:
    Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    flecs::world& world() noexcept;
    const flecs::world& world() const noexcept;
    void enqueue_world_state(WorldStateCommand command);
    TickResult tick(const TickDesc& desc);

private:
    void drain_world_state_commands();

    flecs::world world_;
    flecs::entity fixed_pipeline_;
    flecs::entity frame_pipeline_;
    double accumulator_seconds_ = 0.0;
    std::mutex world_state_mutex_;
    std::vector<WorldStateCommand> world_state_commands_;
};

} // namespace matter::ecs_runtime
