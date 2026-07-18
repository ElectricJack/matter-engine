#include "matter/ecs.h"

namespace matter::ecs {

CoreModule::CoreModule(flecs::world& world) {
    const flecs::entity module = world.module<CoreModule>();
    const flecs::entity previous_scope = world.set_scope(module.parent().id());

    world.component<LocalTransform>();
    world.component<WorldTransform>();
    world.component<TransformDirty>();
    world.component<WorldRuntimeState>();
    world.component<FixedPreUpdate>();
    world.component<FixedUpdate>();
    world.component<PrePhysics>();
    world.component<Physics>();
    world.component<PostPhysics>();
    world.component<FixedPostUpdate>();
    world.component<FrameUpdate>();
    world.component<FixedPipelineSystem>();
    world.component<FramePipelineSystem>();

    world.set<WorldRuntimeState>(WorldRuntimeState{});
    world.set_scope(previous_scope.id());
}

} // namespace matter::ecs
