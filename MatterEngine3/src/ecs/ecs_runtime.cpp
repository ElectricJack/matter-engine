#include "matter/ecs.h"

namespace matter::ecs {

void register_transform_systems(flecs::world& world);

CoreModule::CoreModule(flecs::world& world) {
    const flecs::entity module = world.module<CoreModule>();
    const flecs::entity previous_scope = world.set_scope(module.parent().id());

    world.component<Float3>()
        .member("x", &Float3::x)
        .member("y", &Float3::y)
        .member("z", &Float3::z);
    world.component<Quaternion>()
        .member("x", &Quaternion::x)
        .member("y", &Quaternion::y)
        .member("z", &Quaternion::z)
        .member("w", &Quaternion::w);
    world.component<Mat4f>()
        .member("m", &Mat4f::m);
    world.component<LocalTransform>()
        .member("translation", &LocalTransform::translation)
        .member("rotation", &LocalTransform::rotation)
        .member("scale", &LocalTransform::scale);

    // Reflected for inspection and serialization; engine code treats this
    // component as derived/read-only.
    world.component<WorldTransform>()
        .member("matrix", &WorldTransform::matrix);
    world.component<TransformDirty>();
    world.component<WorldStatus>()
        .constant("Loading", WorldStatus::Loading)
        .constant("Ready", WorldStatus::Ready)
        .constant("Failed", WorldStatus::Failed);
    world.component<WorldRuntimeState>()
        .member("status", &WorldRuntimeState::status)
        .member("content_generation", &WorldRuntimeState::content_generation);
    world.component<FixedPreUpdate>();
    world.component<FixedUpdate>();
    world.component<PrePhysics>();
    world.component<Physics>();
    world.component<PostPhysics>();
    world.component<FixedPostUpdate>();
    world.component<FrameUpdate>();
    world.component<FixedPipelineSystem>();
    world.component<FramePipelineSystem>();

    world.component<FixedPreUpdate>()
        .add(flecs::Phase)
        .depends_on(flecs::PreUpdate);
    world.component<FixedUpdate>()
        .add(flecs::Phase)
        .depends_on<FixedPreUpdate>();
    world.component<PrePhysics>()
        .add(flecs::Phase)
        .depends_on<FixedUpdate>();
    world.component<Physics>()
        .add(flecs::Phase)
        .depends_on<PrePhysics>();
    world.component<PostPhysics>()
        .add(flecs::Phase)
        .depends_on<Physics>();
    world.component<FixedPostUpdate>()
        .add(flecs::Phase)
        .depends_on<PostPhysics>();
    world.component<FrameUpdate>()
        .add(flecs::Phase)
        .depends_on<FixedPostUpdate>();

    register_transform_systems(world);

    world.set<WorldRuntimeState>(WorldRuntimeState{});
    world.set_scope(previous_scope.id());
}

} // namespace matter::ecs
