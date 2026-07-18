#include "ecs_runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace matter::ecs {

void drain_hierarchy_commands(flecs::world& world);

} // namespace matter::ecs

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

namespace matter::ecs_runtime {
namespace {

constexpr double kMaxFrameContributionSeconds = 0.25;
constexpr double kStepComparisonTolerance = 1e-6;

int compare_entity_ids(
    flecs::entity_t first,
    const void*,
    flecs::entity_t second,
    const void*) {
    return (first > second) - (first < second);
}

template <typename PipelineTag>
flecs::entity build_pipeline(flecs::world& world) {
    return world.pipeline()
        .with(flecs::System)
        .with(flecs::Phase).src().cascade(flecs::DependsOn)
        .with<PipelineTag>()
        .with(flecs::Disabled).src().up(flecs::DependsOn).not_()
        .with(flecs::Disabled).src().up(flecs::ChildOf).not_()
        .order_by(
            static_cast<flecs::entity_t>(0),
            compare_entity_ids)
        .build();
}

} // namespace

Runtime::Runtime() {
    world_.import<ecs::CoreModule>();
    fixed_pipeline_ = build_pipeline<ecs::FixedPipelineSystem>(world_);
    frame_pipeline_ = build_pipeline<ecs::FramePipelineSystem>(world_);
}

flecs::world& Runtime::world() noexcept {
    return world_;
}

const flecs::world& Runtime::world() const noexcept {
    return world_;
}

TickResult Runtime::tick(const TickDesc& desc) {
    const double frame_delta = desc.frame_delta_seconds;
    const double fixed_delta = desc.fixed_delta_seconds;
    if (!std::isfinite(frame_delta) || frame_delta < 0.0 ||
        !std::isfinite(fixed_delta) || fixed_delta <= 0.0 ||
        desc.max_fixed_steps == 0) {
        return {0, 0, true};
    }

    const double contributed_delta =
        std::min(frame_delta, kMaxFrameContributionSeconds);
    ecs::drain_hierarchy_commands(world_);
    accumulator_seconds_ += contributed_delta;

    TickResult result{};
    const double comparison_epsilon =
        fixed_delta * kStepComparisonTolerance;
    while (result.fixed_steps < desc.max_fixed_steps &&
           accumulator_seconds_ + comparison_epsilon >= fixed_delta) {
        world_.run_pipeline(fixed_pipeline_, desc.fixed_delta_seconds);
        accumulator_seconds_ -= fixed_delta;
        if (accumulator_seconds_ < 0.0 &&
            accumulator_seconds_ >= -comparison_epsilon) {
            accumulator_seconds_ = 0.0;
        }
        ++result.fixed_steps;
    }

    const double complete_excess_steps =
        std::floor((accumulator_seconds_ + comparison_epsilon) / fixed_delta);
    if (complete_excess_steps > 0.0) {
        const double max_reportable =
            static_cast<double>(std::numeric_limits<uint32_t>::max());
        result.dropped_steps = static_cast<uint32_t>(
            std::min(complete_excess_steps, max_reportable));
        accumulator_seconds_ -= complete_excess_steps * fixed_delta;
        if (accumulator_seconds_ < 0.0 &&
            accumulator_seconds_ >= -comparison_epsilon) {
            accumulator_seconds_ = 0.0;
        }
    }

    world_.run_pipeline(
        frame_pipeline_, static_cast<float>(contributed_delta));
    return result;
}

} // namespace matter::ecs_runtime
