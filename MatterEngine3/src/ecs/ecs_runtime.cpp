#include "ecs_runtime.h"
#include "physics_context.h"
#include "streaming_systems.h"
#include "scene_registry.h"
#include "animation/animation_systems.h"
#include "animation/animation_store.h"
#include "animation/animation_world_queries.h"
#include "render/animation_rigid_bridge.h"
#include "render/animation_skin_bridge.h"
#include "../streaming/sector_streaming_coordinator.h"
#include "matter/physics.h"
#include "matter/streaming.h"
#include "matter/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace matter::physics {

void register_physics_systems(flecs::world& world);

} // namespace matter::physics

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
    world.component<PostPhysicsHierarchy>();
    world.component<FixedPostUpdate>();
    world.component<FrameUpdate>();
    world.component<FixedPipelineSystem>();
    world.component<FramePipelineSystem>();
    world.component<AnimationFixedState>()
        .member("previous_tick", &AnimationFixedState::previous_tick)
        .member("current_tick", &AnimationFixedState::current_tick);
    world.component<AnimationFrameState>()
        .member("frame_serial", &AnimationFrameState::frame_serial)
        .member("interpolation_alpha", &AnimationFrameState::interpolation_alpha);

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
    world.component<PostPhysicsHierarchy>()
        .add(flecs::Phase)
        .depends_on<PostPhysics>();
    world.component<FixedPostUpdate>()
        .add(flecs::Phase)
        .depends_on<PostPhysicsHierarchy>();
    world.component<FrameUpdate>()
        .add(flecs::Phase)
        .depends_on<FixedPostUpdate>();

    register_transform_systems(world);

    world.set<WorldRuntimeState>(WorldRuntimeState{});
    world.set<AnimationFixedState>(AnimationFixedState{});
    world.set<AnimationFrameState>(AnimationFrameState{});
    world.set_scope(previous_scope.id());
}

} // namespace matter::ecs

namespace matter::physics {

PhysicsModule::PhysicsModule(flecs::world& world) {
    const flecs::entity module = world.module<PhysicsModule>();
    const flecs::entity previous_scope = world.set_scope(module.parent().id());

    world.component<RigidBodyType>()
        .constant("Static", RigidBodyType::Static)
        .constant("Kinematic", RigidBodyType::Kinematic)
        .constant("Dynamic", RigidBodyType::Dynamic);
    world.component<RigidBody>()
        .member("type", &RigidBody::type)
        .member("linear_damping", &RigidBody::linear_damping)
        .member("angular_damping", &RigidBody::angular_damping)
        .member("gravity_scale", &RigidBody::gravity_scale)
        .member("sleep_threshold", &RigidBody::sleep_threshold)
        .member("enable_sleep", &RigidBody::enable_sleep)
        .member("continuous", &RigidBody::continuous);
    world.component<PhysicsVelocity>()
        .member("linear", &PhysicsVelocity::linear)
        .member("angular", &PhysicsVelocity::angular);
    world.component<ColliderProperties>()
        .member("density", &ColliderProperties::density)
        .member("friction", &ColliderProperties::friction)
        .member("restitution", &ColliderProperties::restitution)
        .member("category_bits", &ColliderProperties::category_bits)
        .member("mask_bits", &ColliderProperties::mask_bits)
        .member("sensor", &ColliderProperties::sensor)
        .member("contact_events", &ColliderProperties::contact_events)
        .member("hit_events", &ColliderProperties::hit_events);
    world.component<SphereCollider>()
        .member("properties", &SphereCollider::properties)
        .member("center", &SphereCollider::center)
        .member("radius", &SphereCollider::radius);
    world.component<CapsuleCollider>()
        .member("properties", &CapsuleCollider::properties)
        .member("point_a", &CapsuleCollider::point_a)
        .member("point_b", &CapsuleCollider::point_b)
        .member("radius", &CapsuleCollider::radius);
    world.component<BoxCollider>()
        .member("properties", &BoxCollider::properties)
        .member("center", &BoxCollider::center)
        .member("rotation", &BoxCollider::rotation)
        .member("half_extents", &BoxCollider::half_extents);
    world.component<ConvexHullCollider>()
        .member("properties", &ConvexHullCollider::properties)
        .member("point_count", &ConvexHullCollider::point_count)
        .member("points", &ConvexHullCollider::points);
    world.component<PhysicsSettings>()
        .member("gravity", &PhysicsSettings::gravity)
        .member("substeps", &PhysicsSettings::substeps);
    world.component<PhysicsErrorCode>()
        .constant("None", PhysicsErrorCode::None)
        .constant("MissingTransform", PhysicsErrorCode::MissingTransform)
        .constant("HasParent", PhysicsErrorCode::HasParent)
        .constant("NonUnitScale", PhysicsErrorCode::NonUnitScale)
        .constant("MissingCollider", PhysicsErrorCode::MissingCollider)
        .constant("MultipleColliders", PhysicsErrorCode::MultipleColliders)
        .constant("InvalidBody", PhysicsErrorCode::InvalidBody)
        .constant("InvalidCollider", PhysicsErrorCode::InvalidCollider)
        .constant("HullBuildFailed", PhysicsErrorCode::HullBuildFailed);
    world.component<PhysicsError>()
        .member("code", &PhysicsError::code);
    world.component<PhysicsBodyEvent>()
        .member("entity", &PhysicsBodyEvent::entity)
        .member("awake", &PhysicsBodyEvent::awake);
    world.component<PhysicsPairEvent>()
        .member("first", &PhysicsPairEvent::first)
        .member("second", &PhysicsPairEvent::second);
    world.component<PhysicsHitEvent>()
        .member("first", &PhysicsHitEvent::first)
        .member("second", &PhysicsHitEvent::second)
        .member("position", &PhysicsHitEvent::position)
        .member("normal", &PhysicsHitEvent::normal)
        .member("approach_speed", &PhysicsHitEvent::approach_speed);
    world.component<PhysicsStats>()
        .member("steps", &PhysicsStats::steps)
        .member("bodies_created", &PhysicsStats::bodies_created)
        .member("bodies_destroyed", &PhysicsStats::bodies_destroyed)
        .member("rejected_configurations", &PhysicsStats::rejected_configurations)
        .member("failed_commands", &PhysicsStats::failed_commands)
        .member("stale_events", &PhysicsStats::stale_events)
        .member("live_bodies", &PhysicsStats::live_bodies);
    world.component<PhysicsRayHit>()
        .member("entity", &PhysicsRayHit::entity)
        .member("position", &PhysicsRayHit::position)
        .member("normal", &PhysicsRayHit::normal)
        .member("fraction", &PhysicsRayHit::fraction);

    world.component<PhysicsReconcile>()
        .add(flecs::Phase)
        .depends_on<ecs::PrePhysics>();
    world.component<PhysicsPush>()
        .add(flecs::Phase)
        .depends_on<PhysicsReconcile>();
    world.component<ecs::Physics>()
        .depends_on<PhysicsPush>();
    world.component<PhysicsPull>()
        .add(flecs::Phase)
        .depends_on<ecs::Physics>();
    world.component<ecs::PostPhysics>()
        .depends_on<PhysicsPull>();

    register_physics_systems(world);

    world.set<PhysicsSettings>(PhysicsSettings{});
    world.set_scope(previous_scope.id());
}

} // namespace matter::physics

namespace matter::streaming {

StreamingModule::StreamingModule(flecs::world& world) {
    const flecs::entity module = world.module<StreamingModule>();
    const flecs::entity previous_scope = world.set_scope(module.parent().id());

    world.component<SectorStreaming>();
    world.component<SectorStreamingErrorCode>()
        .constant("None", SectorStreamingErrorCode::None)
        .constant("UnsupportedWorld", SectorStreamingErrorCode::UnsupportedWorld)
        .constant("OwnerAlreadyClaimed", SectorStreamingErrorCode::OwnerAlreadyClaimed);
    world.component<SectorStreamingError>()
        .member("code", &SectorStreamingError::code)
        .member("active_owner", &SectorStreamingError::active_owner);
    world.component<SectorStreamingState>()
        .constant("Detached", SectorStreamingState::Detached)
        .constant("PendingProfile", SectorStreamingState::PendingProfile)
        .constant("PendingTransform", SectorStreamingState::PendingTransform)
        .constant("Active", SectorStreamingState::Active)
        .constant("Detaching", SectorStreamingState::Detaching);
    world.component<SectorStreamingStatus>()
        .member("state", &SectorStreamingStatus::state)
        .member("generation", &SectorStreamingStatus::generation)
        .member("resident_sectors", &SectorStreamingStatus::resident_sectors)
        .member("inflight_sectors", &SectorStreamingStatus::inflight_sectors);
    world.component<StreamingUpdate>()
        .add(flecs::Phase)
        .depends_on<ecs::FrameUpdate>();

    detail::register_streaming_systems(world);

    world.set_scope(previous_scope.id());
}

} // namespace matter::streaming

namespace matter::ecs_runtime {
namespace {

constexpr double kMaxFrameContributionSeconds = 0.25;

float explicit_flecs_frame_delta(double contributed_delta) {
    if (contributed_delta != 0.0) {
        return static_cast<float>(contributed_delta);
    }

    // Pinned Flecs v4.1.6 uses a bitwise zero test: +0 requests measured wall
    // time, while signed zero carries an explicit zero without advancing time.
    return std::copysign(0.0f, -1.0f);
}

class FrameScope {
public:
    FrameScope(flecs::world& world, float delta) : world_(world) {
        world_.frame_begin(delta);
    }

    ~FrameScope() {
        world_.frame_end();
    }

    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;

private:
    flecs::world& world_;
};

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

void snap_half_ulp_shortfall_to_fixed_boundary(
    double& accumulator,
    float fixed_delta_input,
    double fixed_delta) {
    if (accumulator >= fixed_delta) {
        return;
    }

    const double previous_fixed_float =
        std::nextafter(fixed_delta_input, 0.0f);
    const double half_downward_float_ulp =
        (fixed_delta - previous_fixed_float) * 0.5;
    if (fixed_delta - accumulator <= half_downward_float_ulp) {
        accumulator = fixed_delta;
    }
}

} // namespace

Runtime::Runtime() {
    world_.import<ecs::CoreModule>();
    world_.import<physics::PhysicsModule>();
    world_.import<streaming::StreamingModule>();
    physics_ = std::make_unique<physics::detail::PhysicsContext>(
        world_.get<physics::PhysicsSettings>());
    world_.set<physics::detail::PhysicsContextRef>(
        physics::detail::PhysicsContextRef{physics_.get()});
    streaming_ = std::make_unique<streaming::detail::Coordinator>();
    world_.set<streaming::detail::StreamingContextRef>(
        streaming::detail::StreamingContextRef{streaming_.get()});
    animation_systems_ = std::make_unique<animation::AnimationSystems>();
    // The producer owns component lifecycle; DynamicSceneBridge is only a
    // renderer adapter and must never manufacture this component itself.
    world_.component<render::AnimationRigidBinding>();
    world_.component<render::AnimationSkinnedBinding>();
    // Animation's fixed world-query seam is backed by the same live Box3D
    // world as the Physics pipeline.  There is deliberately no production
    // no-hit default once Runtime owns a world.
    animation_world_queries_ = std::make_unique<animation::Box3DAnimationWorldQueries>(world_);
    animation_systems_->set_world_queries(animation_world_queries_.get());
    animation::register_animation_systems(world_, *animation_systems_);
    fixed_pipeline_ = build_pipeline<ecs::FixedPipelineSystem>(world_);
    frame_pipeline_ = build_pipeline<ecs::FramePipelineSystem>(world_);
}

Runtime::~Runtime() {
    if (bound_animation_service_ != nullptr && animation_systems_->has_service(bound_animation_service_)) {
        bound_animation_service_->attach_runtime_systems(nullptr);
        bound_animation_service_ = nullptr;
    }
    // Streaming teardown observers remain registered until world finalization.
    // Null their private lookup before releasing the coordinator they target.
    world_.set<streaming::detail::StreamingContextRef>({nullptr});
    streaming_.reset();

    // Physics observers remain registered until Flecs teardown and OnRemove
    // callbacks are emitted during world finalization. Fail their context
    // lookup closed before the Box3D owner is released.
    world_.set<physics::detail::PhysicsContextRef>({nullptr});
    physics_.reset();
}

void Runtime::set_physics_event_hub(matter::evt::Hub* hub) noexcept {
    if (physics_ != nullptr) {
        physics_->set_event_hub(hub);
    }
}

flecs::world& Runtime::world() noexcept {
    return world_;
}

const flecs::world& Runtime::world() const noexcept {
    return world_;
}

streaming::detail::Coordinator& Runtime::streaming_coordinator() noexcept {
    return *streaming_;
}

const streaming::detail::Coordinator&
Runtime::streaming_coordinator() const noexcept {
    return *streaming_;
}

animation::AnimationSystems& Runtime::animation_systems() noexcept {
    return *animation_systems_;
}

const animation::AnimationSystems& Runtime::animation_systems() const noexcept {
    return *animation_systems_;
}

void Runtime::attach_animation_service(AnimationService* service) noexcept {
    if (bound_animation_service_ == service) return;
    // Animator handles are service-local.  Even if a replacement service
    // happens to reuse the same slot/generation and asset hash, it must not
    // inherit render components produced by the previous service.
    std::vector<flecs::entity> attached;
    world_.each([&attached](flecs::entity entity,
                            const render::AnimationRigidBinding&) {
        attached.push_back(entity);
    });
    for (flecs::entity entity : attached)
        entity.remove<render::AnimationRigidBinding>();
    attached.clear();
    world_.each([&attached](flecs::entity entity,
                            const render::AnimationSkinnedBinding&) {
        attached.push_back(entity);
    });
    for (flecs::entity entity : attached)
        entity.remove<render::AnimationSkinnedBinding>();
    if (bound_animation_service_ != nullptr) bound_animation_service_->attach_runtime_systems(nullptr);
    bound_animation_service_ = service;
    if (bound_animation_service_ != nullptr) bound_animation_service_->attach_runtime_systems(animation_systems_.get());
}

bool Runtime::attach_animation_rigid_binding(
    flecs::entity entity, AnimatorInstanceHandle animator,
    const render::AnimationRigidAsset& asset, bool casts_shadow) {
    if (!entity.is_valid() || bound_animation_service_ == nullptr || !animator.valid() ||
        asset.identity == 0 || asset.generation == 0 || asset.bindings == nullptr ||
        asset.rig == nullptr) {
        return false;
    }
    AnimationRuntimeBindingLease lease;
    // The service is the authority for handle liveness.  A pointer-only asset
    // declaration is not enough: it must describe this exact immutable ANIM
    // asset, or a replaced/destroyed animator could render stale geometry.
    if (!bound_animation_service_->runtime_binding(animator, lease) ||
        lease.asset_identity != asset.identity) {
        return false;
    }
    entity.set<render::AnimationRigidBinding>(
        {animator, &asset, asset.generation, casts_shadow});
    return true;
}

void Runtime::detach_animation_rigid_binding(flecs::entity entity) {
    if (entity.is_valid() && entity.has<render::AnimationRigidBinding>())
        entity.remove<render::AnimationRigidBinding>();
}

bool Runtime::attach_animation_skinned_binding(
    flecs::entity entity, AnimatorInstanceHandle animator,
    const render::AnimationSkinnedAsset& asset, uint32_t lod, bool visible) {
    if (!entity.is_valid() || bound_animation_service_ == nullptr ||
        !animator.valid() || !render::valid_animation_skinned_asset(asset) ||
        lod >= asset.lods.size()) return false;
    AnimationRuntimeBindingLease lease;
    if (!bound_animation_service_->runtime_binding(animator, lease) ||
        lease.asset_identity != asset.identity) return false;
    entity.set<render::AnimationSkinnedBinding>(
        {animator, &asset, asset.generation, lod, visible});
    return true;
}

void Runtime::detach_animation_skinned_binding(flecs::entity entity) {
    if (entity.is_valid() && entity.has<render::AnimationSkinnedBinding>())
        entity.remove<render::AnimationSkinnedBinding>();
}

void Runtime::reconcile_animation_rigid_binding_lifecycle() {
    std::vector<flecs::entity> stale;
    world_.each([this, &stale](flecs::entity entity,
                               const render::AnimationRigidBinding& binding) {
        AnimationRuntimeBindingLease lease;
        const render::AnimationRigidAsset* asset = binding.asset;
        if (bound_animation_service_ == nullptr || asset == nullptr ||
            asset->identity == 0 || asset->generation == 0 ||
            asset->bindings == nullptr || asset->rig == nullptr ||
            binding.asset_generation != asset->generation ||
            !bound_animation_service_->runtime_binding(binding.animator, lease) ||
            lease.asset_identity != asset->identity) {
            stale.push_back(entity);
        }
    });
    for (flecs::entity entity : stale)
        entity.remove<render::AnimationRigidBinding>();
}

void Runtime::reconcile_animation_skinned_binding_lifecycle() {
    std::vector<flecs::entity> stale;
    world_.each([this, &stale](flecs::entity entity,
                               const render::AnimationSkinnedBinding& binding) {
        AnimationRuntimeBindingLease lease;
        const render::AnimationSkinnedAsset* asset = binding.asset;
        if (bound_animation_service_ == nullptr || asset == nullptr ||
            binding.asset_generation != asset->generation ||
            !render::valid_animation_skinned_asset(*asset) ||
            binding.lod >= asset->lods.size() ||
            !bound_animation_service_->runtime_binding(binding.animator, lease) ||
            lease.asset_identity != asset->identity) stale.push_back(entity);
    });
    for (flecs::entity entity : stale)
        entity.remove<render::AnimationSkinnedBinding>();
}

void Runtime::enqueue_world_state(WorldStateCommand command) {
    std::lock_guard<std::mutex> lock(world_state_mutex_);
    world_state_commands_.push_back(command);
}

void Runtime::drain_world_state_commands() {
    std::vector<WorldStateCommand> commands;
    {
        std::lock_guard<std::mutex> lock(world_state_mutex_);
        commands.swap(world_state_commands_);
    }
    if (commands.empty()) {
        return;
    }

    ecs::WorldRuntimeState state = world_.get<ecs::WorldRuntimeState>();
    for (auto& command : commands) {
        switch (command.kind) {
            case WorldStateCommandKind::Loading:
                state.status = ecs::WorldStatus::Loading;
                break;
            case WorldStateCommandKind::Ready: {
                state.status = ecs::WorldStatus::Ready;
                ++state.content_generation;
                if (!command.entities.empty()) {
                    scene::PartResolver resolver = command.part_resolver
                        ? command.part_resolver
                        : scene::PartResolver([](const std::string&, uint64_t& out_hash) {
                              out_hash = 0;
                              return true;
                          });
                    scene::RecipeError err;
                    if (!scene::bootstrap_transactional(world_, command.entities,
                                                       scene_generation_, resolver, err)) {
                        MATTER_LOGE("ecs", "bootstrap_transactional failed: %s (entity: %s)\n",
                                    err.message.c_str(), err.authored_id.c_str());
                    }
                }
                break;
            }
            case WorldStateCommandKind::Failed:
                state.status = ecs::WorldStatus::Failed;
                break;
        }
    }
    world_.set<ecs::WorldRuntimeState>(state);
}

TickResult Runtime::tick(const TickDesc& desc) {
    const double frame_delta = desc.frame_delta_seconds;
    const double fixed_delta = desc.fixed_delta_seconds;
    if (!std::isfinite(frame_delta) || frame_delta < 0.0 ||
        !std::isfinite(fixed_delta) || fixed_delta <= 0.0 ||
        desc.max_fixed_steps == 0) {
        return {0, 0, true, 0.0};
    }

    const double contributed_delta =
        std::min(frame_delta, kMaxFrameContributionSeconds);
    const FrameScope frame(
        world_, explicit_flecs_frame_delta(contributed_delta));
    drain_world_state_commands();
    reconcile_animation_rigid_binding_lifecycle();
    reconcile_animation_skinned_binding_lifecycle();
    ecs::drain_hierarchy_commands(world_);

    TickResult result{};
    // advance_fixed == false leaves accumulator_seconds_ untouched: no delta is
    // banked and no banked delta is spent, so a paused editor resumes from the
    // exact sub-step position it froze at. Everything above this block (the
    // Flecs frame, command drains, binding lifecycle) has already run, which is
    // the entire point of freezing rather than invalidating the tick.
    if (desc.advance_fixed) {
        accumulator_seconds_ += contributed_delta;
        while (result.fixed_steps < desc.max_fixed_steps) {
            snap_half_ulp_shortfall_to_fixed_boundary(
                accumulator_seconds_, desc.fixed_delta_seconds, fixed_delta);
            if (accumulator_seconds_ < fixed_delta) {
                break;
            }
            world_.run_pipeline(fixed_pipeline_, desc.fixed_delta_seconds);
            accumulator_seconds_ -= fixed_delta;
            ++result.fixed_steps;
        }

        const double complete_excess_steps =
            std::floor(accumulator_seconds_ / fixed_delta);
        if (complete_excess_steps > 0.0) {
            const double max_reportable =
                static_cast<double>(std::numeric_limits<uint32_t>::max());
            result.dropped_steps = static_cast<uint32_t>(
                std::min(complete_excess_steps, max_reportable));
            accumulator_seconds_ -= complete_excess_steps * fixed_delta;
        }
    }

    result.interpolation_alpha = std::max(
        0.0, std::min(1.0, accumulator_seconds_ / fixed_delta));
    animation_systems_->set_interpolation_alpha(result.interpolation_alpha);
    // Presentation cadence runs on wall time when the caller distinguishes it
    // from the (possibly time-scaled) simulation delta; otherwise the
    // simulation delta is the wall delta and remains exact.
    const double presentation_delta = desc.presentation_delta_seconds;
    animation_systems_->set_presentation_delta_seconds(
        std::isfinite(presentation_delta) && presentation_delta > 0.0
            ? std::min(presentation_delta, kMaxFrameContributionSeconds)
            : contributed_delta);

    world_.run_pipeline(
        frame_pipeline_, static_cast<float>(contributed_delta));
    return result;
}

} // namespace matter::ecs_runtime
