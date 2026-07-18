#include "physics_context.h"
#include "physics_shapes.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <box3d/box3d.h>

namespace matter::physics::detail {
namespace {

struct BridgeRecord {
    flecs::entity_t entity = 0;
    b3BodyId body = b3_nullBodyId;
    b3ShapeId shape = b3_nullShapeId;
    RigidBodyType type = RigidBodyType::Static;
    uint64_t configuration_hash = 0;
    DesiredBody desired{};
    bool live = false;
};

const PhysicsContext* try_context(const flecs::world& world) noexcept {
    const PhysicsContextRef* ref = world.try_get<PhysicsContextRef>();
    return ref != nullptr ? ref->value : nullptr;
}

b3BodyType box_body_type(RigidBodyType type) {
    switch (type) {
        case RigidBodyType::Static: return b3_staticBody;
        case RigidBodyType::Kinematic: return b3_kinematicBody;
        case RigidBodyType::Dynamic: return b3_dynamicBody;
    }
    return b3_staticBody;
}

b3Pos box_position(Float3 value) {
    return {value.x, value.y, value.z};
}

b3Vec3 box_vector(Float3 value) {
    return {value.x, value.y, value.z};
}

b3Quat box_quaternion(Quaternion value) {
    return {{value.x, value.y, value.z}, value.w};
}

b3WorldTransform box_transform(const ecs::LocalTransform& value) {
    return {box_position(value.translation), box_quaternion(value.rotation)};
}

Float3 engine_position(b3Pos value) {
    return {static_cast<float>(value.x), static_cast<float>(value.y),
            static_cast<float>(value.z)};
}

Float3 engine_vector(b3Vec3 value) {
    return {value.x, value.y, value.z};
}

Quaternion engine_quaternion(b3Quat value) {
    return {value.v.x, value.v.y, value.v.z, value.s};
}

void clear_and_destroy_bridge(BridgeRecord& bridge, PhysicsStats& stats) {
    if (!bridge.live) {
        return;
    }
    if (b3Shape_IsValid(bridge.shape)) {
        b3Shape_SetUserData(bridge.shape, nullptr);
    }
    if (b3Body_IsValid(bridge.body)) {
        b3Body_SetUserData(bridge.body, nullptr);
        b3DestroyBody(bridge.body);
    }
    bridge.shape = b3_nullShapeId;
    bridge.body = b3_nullBodyId;
    bridge.live = false;
    ++stats.bodies_destroyed;
    if (stats.live_bodies > 0) {
        --stats.live_bodies;
    }
}

void clear_partial_body(b3BodyId body, b3ShapeId shape) {
    if (b3Shape_IsValid(shape)) {
        b3Shape_SetUserData(shape, nullptr);
    }
    if (b3Body_IsValid(body)) {
        b3Body_SetUserData(body, nullptr);
        b3DestroyBody(body);
    }
}

void set_error(flecs::entity entity, PhysicsErrorCode code) {
    const PhysicsError* current = entity.try_get<PhysicsError>();
    if (current == nullptr || current->code != code) {
        entity.set<PhysicsError>({code});
    }
}

void remove_error(flecs::entity entity) {
    if (entity.has<PhysicsError>()) {
        entity.remove<PhysicsError>();
    }
}

bool read_state(const BridgeRecord& bridge, PhysicsBodyState& state) {
    if (!bridge.live || !b3Body_IsValid(bridge.body)) {
        return false;
    }
    state.position = engine_position(b3Body_GetPosition(bridge.body));
    state.rotation = engine_quaternion(b3Body_GetRotation(bridge.body));
    state.linear_velocity =
        engine_vector(b3Body_GetLinearVelocity(bridge.body));
    state.angular_velocity =
        engine_vector(b3Body_GetAngularVelocity(bridge.body));
    state.awake = b3Body_IsAwake(bridge.body);
    return true;
}

void write_state(const BridgeRecord& bridge, const PhysicsBodyState& state) {
    b3Body_SetTransform(
        bridge.body, box_position(state.position),
        box_quaternion(state.rotation));
    b3Body_SetLinearVelocity(
        bridge.body, box_vector(state.linear_velocity));
    b3Body_SetAngularVelocity(
        bridge.body, box_vector(state.angular_velocity));
    b3Body_SetAwake(bridge.body, state.awake);
}

uint32_t clamped_substeps(uint32_t substeps) {
    constexpr uint32_t kMaxSubsteps = 16;
    return std::max(1U, std::min(substeps, kMaxSubsteps));
}

bool finite(Float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool normalize(Quaternion& value) {
    const double length_squared =
        static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y +
        static_cast<double>(value.z) * value.z +
        static_cast<double>(value.w) * value.w;
    if (!std::isfinite(length_squared) || length_squared <= 0.0) {
        return false;
    }
    const float inverse_length =
        static_cast<float>(1.0 / std::sqrt(length_squared));
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    value.w *= inverse_length;
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

struct HullDeleter {
    void operator()(b3HullData* hull) const {
        if (hull != nullptr) {
            b3DestroyHull(hull);
        }
    }
};

} // namespace

struct PhysicsContext::Impl {
    b3WorldId world_id = b3_nullWorldId;
    std::unordered_map<flecs::entity_t, std::unique_ptr<BridgeRecord>> bridges;
    std::vector<flecs::entity_t> dirty_entities;
    uint32_t configured_substeps = 1;
    uint32_t last_step_substeps = 0;
    std::vector<PhysicsSystemStage> fixed_step_trace;
};

PhysicsContext::PhysicsContext(const PhysicsSettings& settings)
    : impl_(std::make_unique<Impl>()) {
    b3WorldDef world_def = b3DefaultWorldDef();
    world_def.workerCount = 1;
    world_def.gravity = {settings.gravity.x, settings.gravity.y,
                         settings.gravity.z};
    impl_->configured_substeps = clamped_substeps(settings.substeps);
    impl_->world_id = b3CreateWorld(&world_def);
    if (!b3World_IsValid(impl_->world_id)) {
        throw std::runtime_error("Box3D failed to create a physics world");
    }
}

PhysicsContext::~PhysicsContext() {
    if (impl_ == nullptr) {
        return;
    }
    for (auto& entry : impl_->bridges) {
        BridgeRecord& bridge = *entry.second;
        if (b3Shape_IsValid(bridge.shape)) {
            b3Shape_SetUserData(bridge.shape, nullptr);
        }
        if (b3Body_IsValid(bridge.body)) {
            b3Body_SetUserData(bridge.body, nullptr);
        }
        bridge.live = false;
    }
    if (b3World_IsValid(impl_->world_id)) {
        b3DestroyWorld(impl_->world_id);
        impl_->world_id = b3_nullWorldId;
    }
    impl_->bridges.clear();
}

const PhysicsEvents& PhysicsContext::events() const noexcept {
    return events_;
}

PhysicsStats PhysicsContext::stats() const noexcept {
    return stats_;
}

bool PhysicsContext::world_is_valid() const noexcept {
    return impl_ != nullptr && b3World_IsValid(impl_->world_id);
}

void PhysicsContext::mark_for_reconcile(flecs::entity_t entity) noexcept {
    if (impl_ != nullptr && entity != 0) {
        impl_->dirty_entities.push_back(entity);
    }
}

void PhysicsContext::reconcile(flecs::world& world) {
    impl_->fixed_step_trace.clear();
    impl_->fixed_step_trace.push_back(PhysicsSystemStage::Reconcile);
    if (!world_is_valid()) {
        return;
    }

    std::vector<flecs::entity_t> candidates;
    candidates.swap(impl_->dirty_entities);
    world.each<const RigidBody>(
        [&candidates](flecs::entity entity, const RigidBody&) {
            candidates.push_back(entity.id());
        });
    for (const auto& entry : impl_->bridges) {
        candidates.push_back(entry.first);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end()), candidates.end());

    for (flecs::entity_t entity_id : candidates) {
        auto existing = impl_->bridges.find(entity_id);
        const bool alive = world.is_alive(entity_id);
        if (!alive) {
            if (existing != impl_->bridges.end()) {
                clear_and_destroy_bridge(*existing->second, stats_);
                impl_->bridges.erase(existing);
            }
            continue;
        }

        flecs::entity entity(world.c_ptr(), entity_id);
        if (!entity.has<RigidBody>()) {
            if (existing != impl_->bridges.end()) {
                clear_and_destroy_bridge(*existing->second, stats_);
                impl_->bridges.erase(existing);
            }
            remove_error(entity);
            continue;
        }

        const ValidationResult validation = validate_desired_body(entity);
        if (!validation.valid()) {
            if (existing != impl_->bridges.end()) {
                clear_and_destroy_bridge(*existing->second, stats_);
                impl_->bridges.erase(existing);
            }
            ++stats_.rejected_configurations;
            set_error(entity, validation.error);
            continue;
        }

        if (existing != impl_->bridges.end()) {
            const BridgeRecord& bridge = *existing->second;
            if (bridge.live && b3Body_IsValid(bridge.body) &&
                b3Shape_IsValid(bridge.shape) &&
                bridge.configuration_hash ==
                    validation.desired.configuration_hash &&
                same_configuration(bridge.desired, validation.desired)) {
                remove_error(entity);
                continue;
            }
        }

        PhysicsBodyState preserved{};
        bool preserve_dynamic_state = false;
        if (existing != impl_->bridges.end() &&
            existing->second->type == RigidBodyType::Dynamic &&
            validation.desired.body.type == RigidBodyType::Dynamic) {
            preserve_dynamic_state = read_state(*existing->second, preserved);
        }

        auto replacement = std::make_unique<BridgeRecord>();
        replacement->entity = entity_id;
        replacement->type = validation.desired.body.type;
        replacement->configuration_hash =
            validation.desired.configuration_hash;
        replacement->desired = validation.desired;

        b3BodyDef body_definition = b3DefaultBodyDef();
        body_definition.type = box_body_type(validation.desired.body.type);
        body_definition.position =
            box_position(validation.desired.transform.translation);
        body_definition.rotation =
            box_quaternion(validation.desired.transform.rotation);
        if (validation.desired.has_velocity) {
            body_definition.linearVelocity =
                box_vector(validation.desired.velocity.linear);
            body_definition.angularVelocity =
                box_vector(validation.desired.velocity.angular);
        }
        body_definition.linearDamping =
            validation.desired.body.linear_damping;
        body_definition.angularDamping =
            validation.desired.body.angular_damping;
        body_definition.gravityScale = validation.desired.body.gravity_scale;
        body_definition.sleepThreshold =
            validation.desired.body.sleep_threshold;
        body_definition.enableSleep = validation.desired.body.enable_sleep;
        body_definition.isBullet = validation.desired.body.continuous;
        body_definition.userData = replacement.get();

        replacement->body = b3CreateBody(impl_->world_id, &body_definition);
        b3HullData* temporary_hull_raw = nullptr;
        if (b3Body_IsValid(replacement->body)) {
            replacement->shape = create_shape(
                replacement->body, validation.desired, temporary_hull_raw);
        }
        std::unique_ptr<b3HullData, HullDeleter> temporary_hull(
            temporary_hull_raw);

        if (!b3Body_IsValid(replacement->body) ||
            !b3Shape_IsValid(replacement->shape)) {
            clear_partial_body(replacement->body, replacement->shape);
            if (existing != impl_->bridges.end()) {
                clear_and_destroy_bridge(*existing->second, stats_);
                impl_->bridges.erase(existing);
            }
            ++stats_.rejected_configurations;
            set_error(
                entity,
                validation.desired.shape_kind == DesiredShapeKind::Hull
                    ? PhysicsErrorCode::HullBuildFailed
                    : PhysicsErrorCode::InvalidCollider);
            continue;
        }

        replacement->live = true;
        b3Shape_SetUserData(replacement->shape, replacement.get());
        std::unique_ptr<BridgeRecord> retired;
        if (existing == impl_->bridges.end()) {
            impl_->bridges.emplace(entity_id, std::move(replacement));
        } else {
            retired = std::move(existing->second);
            existing->second = std::move(replacement);
        }
        BridgeRecord& published = *impl_->bridges.at(entity_id);
        ++stats_.bodies_created;
        ++stats_.live_bodies;

        if (retired != nullptr) {
            clear_and_destroy_bridge(*retired, stats_);
        }
        if (preserve_dynamic_state) {
            write_state(published, preserved);
        }
        remove_error(entity);
    }
}

void PhysicsContext::push(flecs::world& world, float fixed_delta) {
    if (!world_is_valid()) {
        return;
    }
    impl_->fixed_step_trace.push_back(PhysicsSystemStage::Push);

    const PhysicsSettings settings = world.get<PhysicsSettings>();
    if (finite(settings.gravity)) {
        b3World_SetGravity(impl_->world_id, box_vector(settings.gravity));
    }
    impl_->configured_substeps = clamped_substeps(settings.substeps);

    std::vector<flecs::entity_t> entity_ids;
    entity_ids.reserve(impl_->bridges.size());
    for (const auto& entry : impl_->bridges) {
        entity_ids.push_back(entry.first);
    }
    std::sort(entity_ids.begin(), entity_ids.end());

    for (const flecs::entity_t entity_id : entity_ids) {
        BridgeRecord& bridge = *impl_->bridges.at(entity_id);
        if (!bridge.live || !b3Body_IsValid(bridge.body) ||
            bridge.type == RigidBodyType::Dynamic ||
            !world.is_alive(entity_id)) {
            continue;
        }

        const flecs::entity entity(world.c_ptr(), entity_id);
        const ecs::LocalTransform* source =
            entity.try_get<ecs::LocalTransform>();
        if (source == nullptr) {
            continue;
        }
        ecs::LocalTransform transform = *source;
        if (!normalize(transform.rotation)) {
            continue;
        }
        if (bridge.type == RigidBodyType::Static) {
            b3Body_SetTransform(
                bridge.body, box_position(transform.translation),
                box_quaternion(transform.rotation));
        } else if (fixed_delta > 0.0f && std::isfinite(fixed_delta)) {
            b3Body_SetTargetTransform(
                bridge.body, box_transform(transform), fixed_delta, true);
        }
    }
}

void PhysicsContext::step(float fixed_delta) {
    if (!world_is_valid() || !std::isfinite(fixed_delta) ||
        fixed_delta <= 0.0f) {
        return;
    }
    impl_->fixed_step_trace.push_back(PhysicsSystemStage::Step);
    const uint32_t substeps = impl_->configured_substeps;
    b3World_Step(
        impl_->world_id, fixed_delta, static_cast<int>(substeps));
    impl_->last_step_substeps = substeps;
    ++stats_.steps;
}

void PhysicsContext::pull(flecs::world& world) {
    if (!world_is_valid()) {
        return;
    }
    impl_->fixed_step_trace.push_back(PhysicsSystemStage::Pull);

    const b3BodyEvents events = b3World_GetBodyEvents(impl_->world_id);
    for (int event_index = 0; event_index < events.moveCount; ++event_index) {
        const b3BodyMoveEvent event = events.moveEvents[event_index];
        // Reconciliation is the only bridge-retirement point and precedes
        // Step, so Box3D movement user data remains a live heap record through
        // this Pull. Validate map identity before consuming the record state.
        BridgeRecord* bridge = static_cast<BridgeRecord*>(event.userData);
        if (bridge == nullptr) {
            ++stats_.stale_events;
            continue;
        }
        const auto found = impl_->bridges.find(bridge->entity);
        if (found == impl_->bridges.end() ||
            found->second.get() != bridge || !bridge->live) {
            ++stats_.stale_events;
            continue;
        }
        if (bridge->type != RigidBodyType::Dynamic) {
            continue;
        }
        if (!B3_ID_EQUALS(bridge->body, event.bodyId) ||
            !b3Body_IsValid(bridge->body) ||
            !world.is_alive(bridge->entity)) {
            ++stats_.stale_events;
            continue;
        }

        ecs::LocalTransform transform{};
        transform.translation = engine_position(event.transform.p);
        transform.rotation = engine_quaternion(event.transform.q);
        transform.scale = {1.0f, 1.0f, 1.0f};
        PhysicsVelocity velocity{};
        velocity.linear = engine_vector(
            b3Body_GetLinearVelocity(bridge->body));
        velocity.angular = engine_vector(
            b3Body_GetAngularVelocity(bridge->body));
        const flecs::entity entity(world.c_ptr(), bridge->entity);
        entity.set<ecs::LocalTransform>(transform);
        entity.set<PhysicsVelocity>(velocity);
        entity.add<ecs::TransformDirty>();
    }
}

uint32_t PhysicsContext::last_step_substeps() const noexcept {
    return impl_ != nullptr ? impl_->last_step_substeps : 0;
}

const std::vector<PhysicsSystemStage>&
PhysicsContext::fixed_step_trace() const noexcept {
    static const std::vector<PhysicsSystemStage> empty;
    return impl_ != nullptr ? impl_->fixed_step_trace : empty;
}

bool PhysicsContext::body_is_valid(flecs::entity_t entity) const noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->bridges.find(entity);
    return found != impl_->bridges.end() && found->second->live &&
           b3Body_IsValid(found->second->body);
}

bool PhysicsContext::shape_is_valid(flecs::entity_t entity) const noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->bridges.find(entity);
    return found != impl_->bridges.end() && found->second->live &&
           b3Shape_IsValid(found->second->shape);
}

flecs::entity_t PhysicsContext::user_data_entity(
    flecs::entity_t entity) const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    const auto found = impl_->bridges.find(entity);
    if (found == impl_->bridges.end() || !found->second->live ||
        !b3Body_IsValid(found->second->body) ||
        !b3Shape_IsValid(found->second->shape)) {
        return 0;
    }
    const void* body_data = b3Body_GetUserData(found->second->body);
    const void* shape_data = b3Shape_GetUserData(found->second->shape);
    if (body_data != found->second.get() || shape_data != found->second.get()) {
        return 0;
    }
    return found->second->entity;
}

bool PhysicsContext::get_body_state(
    flecs::entity_t entity,
    PhysicsBodyState& state) const noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->bridges.find(entity);
    return found != impl_->bridges.end() && read_state(*found->second, state);
}

bool PhysicsContext::set_body_state(
    flecs::entity_t entity,
    const PhysicsBodyState& state) noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->bridges.find(entity);
    if (found == impl_->bridges.end() || !found->second->live ||
        !b3Body_IsValid(found->second->body)) {
        return false;
    }
    write_state(*found->second, state);
    return true;
}

bool PhysicsContext::force_configuration_hash_for_test(
    flecs::entity_t entity,
    uint64_t hash) noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->bridges.find(entity);
    if (found == impl_->bridges.end() || !found->second->live) {
        return false;
    }
    found->second->configuration_hash = hash;
    return true;
}

PhysicsContext& context(flecs::world& world) {
    const PhysicsContext* value = try_context(world);
    if (value == nullptr) {
        throw std::runtime_error("Flecs world has no physics context");
    }
    return *const_cast<PhysicsContext*>(value);
}

const PhysicsContext& context(const flecs::world& world) {
    const PhysicsContext* value = try_context(world);
    if (value == nullptr) {
        throw std::runtime_error("Flecs world has no physics context");
    }
    return *value;
}

bool context_world_is_valid(const flecs::world& world) {
    const PhysicsContext* value = try_context(world);
    return value != nullptr && value->world_is_valid();
}

} // namespace matter::physics::detail

namespace matter::physics {

const PhysicsEvents& physics_events(const flecs::world& world) {
    static const PhysicsEvents empty;
    const detail::PhysicsContextRef* ref =
        world.try_get<detail::PhysicsContextRef>();
    if (ref == nullptr || ref->value == nullptr) {
        return empty;
    }
    return ref->value->events();
}

PhysicsStats physics_stats(const flecs::world& world) {
    const detail::PhysicsContextRef* ref =
        world.try_get<detail::PhysicsContextRef>();
    if (ref == nullptr || ref->value == nullptr) {
        return {};
    }
    return ref->value->stats();
}

} // namespace matter::physics
