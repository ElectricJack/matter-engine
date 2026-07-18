#include "physics_context.h"
#include "physics_shapes.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <box3d/box3d.h>

namespace matter::physics::detail {
namespace {

struct BridgeRecord {
    const flecs::world_t* owning_world = nullptr;
    flecs::entity_t entity = 0;
    b3BodyId body = b3_nullBodyId;
    b3ShapeId shape = b3_nullShapeId;
    RigidBodyType type = RigidBodyType::Static;
    uint64_t configuration_hash = 0;
    DesiredBody desired{};
    bool live = false;
};

struct QueuedCommand {
    const flecs::world_t* originating_world = nullptr;
    flecs::entity_t entity = 0;
    PhysicsCommandKind kind = PhysicsCommandKind::Wake;
    Float3 primary{};
    Float3 secondary{};
    Quaternion rotation{};
};

struct QueuedCommandHash {
    size_t operator()(const QueuedCommand& command) const noexcept {
        const size_t world_hash =
            std::hash<const flecs::world_t*>{}(command.originating_world);
        const size_t entity_hash =
            std::hash<flecs::entity_t>{}(command.entity);
        const size_t kind_hash =
            std::hash<uint8_t>{}(static_cast<uint8_t>(command.kind));
        return world_hash ^ (entity_hash + 0x9e3779b9U +
                             (world_hash << 6U) + (world_hash >> 2U)) ^
               (kind_hash << 1U);
    }
};

struct QueuedCommandEqual {
    bool operator()(
        const QueuedCommand& first,
        const QueuedCommand& second) const noexcept {
        return first.originating_world == second.originating_world &&
               first.entity == second.entity && first.kind == second.kind;
    }
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
    std::mutex command_mutex;
    std::unordered_map<flecs::entity_t, QueuedCommand> teleports;
    std::unordered_map<flecs::entity_t, QueuedCommand> velocities;
    std::vector<QueuedCommand> forces;
    std::vector<QueuedCommand> impulses;
    std::unordered_set<
        QueuedCommand, QueuedCommandHash, QueuedCommandEqual> wakes;
    std::vector<PhysicsCommandTraceEntry> last_command_trace;
    uint32_t configured_substeps = 1;
    uint32_t last_step_substeps = 0;
    std::vector<PhysicsSystemStage> fixed_step_trace;
    flecs::entity_t tombstoned_event_participant_for_test = 0;
};

namespace {

bool is_live_dynamic_bridge(const BridgeRecord& bridge) {
    return bridge.live && bridge.type == RigidBodyType::Dynamic &&
           bridge.entity != 0 && b3Body_IsValid(bridge.body) &&
           b3Shape_IsValid(bridge.shape) &&
           b3Body_GetType(bridge.body) == b3_dynamicBody &&
           b3Body_GetUserData(bridge.body) == &bridge &&
           b3Shape_GetUserData(bridge.shape) == &bridge;
}

bool can_enqueue_command(
    const std::unordered_map<
        flecs::entity_t, std::unique_ptr<BridgeRecord>>& bridges,
    const PhysicsContext* expected_context,
    const flecs::world_t* originating_world,
    flecs::entity_t entity_id) {
    if (expected_context == nullptr || originating_world == nullptr ||
        entity_id == 0 || !ecs_is_alive(originating_world, entity_id)) {
        return false;
    }
    flecs::world normalized_world(
        const_cast<flecs::world_t*>(originating_world));
    const PhysicsContextRef* owner =
        normalized_world.try_get<PhysicsContextRef>();
    if (owner == nullptr || owner->value != expected_context) {
        return false;
    }
    const auto found = bridges.find(entity_id);
    if (found == bridges.end() || found->second == nullptr ||
        found->second->entity != entity_id ||
        !is_live_dynamic_bridge(*found->second)) {
        return false;
    }
    const flecs::entity entity(
        const_cast<flecs::world_t*>(originating_world), entity_id);
    const RigidBody* body = entity.try_get<RigidBody>();
    return body != nullptr && body->type == RigidBodyType::Dynamic &&
           !entity.has<PhysicsError>();
}

BridgeRecord* validate_queued_command(
    const QueuedCommand& command,
    const flecs::world_t* runtime_world,
    flecs::world& world,
    std::unordered_map<
        flecs::entity_t, std::unique_ptr<BridgeRecord>>& bridges) {
    if (command.originating_world == nullptr ||
        command.originating_world != runtime_world || command.entity == 0 ||
        !world.is_alive(command.entity)) {
        return nullptr;
    }
    const auto found = bridges.find(command.entity);
    if (found == bridges.end() || found->second == nullptr ||
        found->second->entity != command.entity ||
        !is_live_dynamic_bridge(*found->second)) {
        return nullptr;
    }

    const flecs::entity entity(world.c_ptr(), command.entity);
    const RigidBody* body = entity.try_get<RigidBody>();
    if (body == nullptr || body->type != RigidBodyType::Dynamic ||
        entity.has<PhysicsError>()) {
        return nullptr;
    }
    return found->second.get();
}

std::vector<QueuedCommand> sorted_map_commands(
    const std::unordered_map<flecs::entity_t, QueuedCommand>& commands) {
    std::vector<QueuedCommand> sorted;
    sorted.reserve(commands.size());
    for (const auto& entry : commands) {
        sorted.push_back(entry.second);
    }
    std::sort(
        sorted.begin(), sorted.end(),
        [](const QueuedCommand& first, const QueuedCommand& second) {
            return first.entity < second.entity;
        });
    return sorted;
}

std::vector<QueuedCommand> sorted_wake_commands(
    const std::unordered_set<
        QueuedCommand, QueuedCommandHash, QueuedCommandEqual>& commands) {
    std::vector<QueuedCommand> sorted(commands.begin(), commands.end());
    std::sort(
        sorted.begin(), sorted.end(),
        [](const QueuedCommand& first, const QueuedCommand& second) {
            return first.entity < second.entity;
        });
    return sorted;
}

PhysicsCommandTraceEntry trace_entry(const QueuedCommand& command) {
    return {command.kind, command.entity, command.primary,
            command.secondary, command.rotation};
}

} // namespace

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

bool PhysicsContext::enqueue_teleport(
    const flecs::world_t* originating_world,
    flecs::entity_t entity,
    Float3 position,
    Quaternion rotation) noexcept {
    if (impl_ == nullptr || !finite(position) || !normalize(rotation) ||
        !can_enqueue_command(
            impl_->bridges, this, originating_world, entity)) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->command_mutex);
        impl_->teleports[entity] = {
            originating_world, entity, PhysicsCommandKind::Teleport,
            position, {}, rotation};
        return true;
    } catch (...) {
        return false;
    }
}

bool PhysicsContext::enqueue_velocity(
    const flecs::world_t* originating_world,
    flecs::entity_t entity,
    Float3 linear,
    Float3 angular) noexcept {
    if (impl_ == nullptr || !finite(linear) || !finite(angular) ||
        !can_enqueue_command(
            impl_->bridges, this, originating_world, entity)) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->command_mutex);
        impl_->velocities[entity] = {
            originating_world, entity, PhysicsCommandKind::Velocity,
            linear, angular, {}};
        return true;
    } catch (...) {
        return false;
    }
}

bool PhysicsContext::enqueue_force(
    const flecs::world_t* originating_world,
    flecs::entity_t entity,
    Float3 force) noexcept {
    if (impl_ == nullptr || !finite(force) ||
        !can_enqueue_command(
            impl_->bridges, this, originating_world, entity)) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->command_mutex);
        impl_->forces.push_back({
            originating_world, entity, PhysicsCommandKind::Force,
            force, {}, {}});
        return true;
    } catch (...) {
        return false;
    }
}

bool PhysicsContext::enqueue_impulse(
    const flecs::world_t* originating_world,
    flecs::entity_t entity,
    Float3 impulse) noexcept {
    if (impl_ == nullptr || !finite(impulse) ||
        !can_enqueue_command(
            impl_->bridges, this, originating_world, entity)) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->command_mutex);
        impl_->impulses.push_back({
            originating_world, entity, PhysicsCommandKind::Impulse,
            impulse, {}, {}});
        return true;
    } catch (...) {
        return false;
    }
}

bool PhysicsContext::enqueue_wake(
    const flecs::world_t* originating_world,
    flecs::entity_t entity) noexcept {
    if (impl_ == nullptr ||
        !can_enqueue_command(
            impl_->bridges, this, originating_world, entity)) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->command_mutex);
        impl_->wakes.insert({
            originating_world, entity, PhysicsCommandKind::Wake,
            {}, {}, {}});
        return true;
    } catch (...) {
        return false;
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
        replacement->owning_world = ecs_get_world(world.c_ptr());
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

    std::unordered_map<flecs::entity_t, QueuedCommand> teleports;
    std::unordered_map<flecs::entity_t, QueuedCommand> velocities;
    std::vector<QueuedCommand> forces;
    std::vector<QueuedCommand> impulses;
    std::unordered_set<
        QueuedCommand, QueuedCommandHash, QueuedCommandEqual> wakes;
    {
        std::lock_guard<std::mutex> lock(impl_->command_mutex);
        teleports.swap(impl_->teleports);
        velocities.swap(impl_->velocities);
        forces.swap(impl_->forces);
        impulses.swap(impl_->impulses);
        wakes.swap(impl_->wakes);
    }
    impl_->last_command_trace.clear();
    impl_->last_command_trace.reserve(
        teleports.size() + velocities.size() + forces.size() +
        impulses.size() + wakes.size());

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

    const flecs::world_t* runtime_world = ecs_get_world(world.c_ptr());
    auto apply_command = [&](const QueuedCommand& command, auto apply) {
        BridgeRecord* bridge = validate_queued_command(
            command, runtime_world, world, impl_->bridges);
        if (bridge == nullptr) {
            ++stats_.failed_commands;
            return;
        }
        apply(*bridge);
        impl_->last_command_trace.push_back(trace_entry(command));
    };

    for (const QueuedCommand& command : sorted_map_commands(teleports)) {
        apply_command(command, [&](const BridgeRecord& bridge) {
            b3Body_SetTransform(
                bridge.body, box_position(command.primary),
                box_quaternion(command.rotation));
            b3Body_SetAwake(bridge.body, true);
        });
    }
    for (const QueuedCommand& command : sorted_map_commands(velocities)) {
        apply_command(command, [&](const BridgeRecord& bridge) {
            b3Body_SetLinearVelocity(
                bridge.body, box_vector(command.primary));
            b3Body_SetAngularVelocity(
                bridge.body, box_vector(command.secondary));
        });
    }
    for (const QueuedCommand& command : forces) {
        apply_command(command, [&](const BridgeRecord& bridge) {
            b3Body_ApplyForceToCenter(
                bridge.body, box_vector(command.primary), true);
        });
    }
    for (const QueuedCommand& command : impulses) {
        apply_command(command, [&](const BridgeRecord& bridge) {
            b3Body_ApplyLinearImpulseToCenter(
                bridge.body, box_vector(command.primary), true);
        });
    }
    for (const QueuedCommand& command : sorted_wake_commands(wakes)) {
        apply_command(command, [](const BridgeRecord& bridge) {
            b3Body_SetAwake(bridge.body, true);
        });
    }
}

void PhysicsContext::step(flecs::world& world, float fixed_delta) {
    if (!world_is_valid() || !std::isfinite(fixed_delta) ||
        fixed_delta <= 0.0f) {
        return;
    }
    impl_->fixed_step_trace.push_back(PhysicsSystemStage::Step);
    const uint32_t substeps = impl_->configured_substeps;
    b3World_Step(
        impl_->world_id, fixed_delta, static_cast<int>(substeps));
    capture_events(world);
    impl_->last_step_substeps = substeps;
    ++stats_.steps;
}

void PhysicsContext::capture_events(flecs::world& world) {
    PhysicsEvents next;
    const flecs::world_t* owning_world = ecs_get_world(world.c_ptr());
    const flecs::entity_t tombstoned_participant =
        impl_->tombstoned_event_participant_for_test;
    impl_->tombstoned_event_participant_for_test = 0;

    auto resolve_bridge = [&](b3ShapeId shape) -> BridgeRecord* {
        if (!b3Shape_IsValid(shape)) {
            ++stats_.stale_events;
            return nullptr;
        }
        BridgeRecord* bridge =
            static_cast<BridgeRecord*>(b3Shape_GetUserData(shape));
        if (bridge == nullptr || bridge->owning_world != owning_world ||
            bridge->entity == 0 || !bridge->live ||
            bridge->entity == tombstoned_participant ||
            !B3_ID_EQUALS(bridge->shape, shape)) {
            ++stats_.stale_events;
            return nullptr;
        }
        const auto found = impl_->bridges.find(bridge->entity);
        if (found == impl_->bridges.end() ||
            found->second.get() != bridge ||
            !ecs_is_alive(owning_world, bridge->entity)) {
            ++stats_.stale_events;
            return nullptr;
        }
        return bridge;
    };

    auto append_pair = [&](auto& destination, b3ShapeId shape_a,
                           b3ShapeId shape_b) {
        BridgeRecord* first_bridge = resolve_bridge(shape_a);
        BridgeRecord* second_bridge = resolve_bridge(shape_b);
        if (first_bridge == nullptr || second_bridge == nullptr) {
            return;
        }
        flecs::entity_t first = first_bridge->entity;
        flecs::entity_t second = second_bridge->entity;
        if (second < first) {
            std::swap(first, second);
        }
        destination.push_back({first, second});
    };

    const b3BodyEvents body_events =
        b3World_GetBodyEvents(impl_->world_id);
    next.body.reserve(static_cast<size_t>(body_events.moveCount));
    for (int index = 0; index < body_events.moveCount; ++index) {
        const b3BodyMoveEvent& event = body_events.moveEvents[index];
        BridgeRecord* bridge = static_cast<BridgeRecord*>(event.userData);
        if (bridge == nullptr || bridge->owning_world != owning_world ||
            bridge->entity == 0 || !bridge->live ||
            bridge->entity == tombstoned_participant ||
            !B3_ID_EQUALS(bridge->body, event.bodyId)) {
            ++stats_.stale_events;
            continue;
        }
        const auto found = impl_->bridges.find(bridge->entity);
        if (found == impl_->bridges.end() || found->second.get() != bridge ||
            !ecs_is_alive(owning_world, bridge->entity) ||
            !b3Body_IsValid(bridge->body)) {
            ++stats_.stale_events;
            continue;
        }
        next.body.push_back(
            {bridge->entity, b3Body_IsAwake(bridge->body)});
    }

    const b3ContactEvents contacts =
        b3World_GetContactEvents(impl_->world_id);
    next.contact_begin.reserve(static_cast<size_t>(contacts.beginCount));
    next.contact_end.reserve(static_cast<size_t>(contacts.endCount));
    next.contact_hit.reserve(static_cast<size_t>(contacts.hitCount));
    for (int index = 0; index < contacts.beginCount; ++index) {
        const b3ContactBeginTouchEvent& event = contacts.beginEvents[index];
        append_pair(next.contact_begin, event.shapeIdA, event.shapeIdB);
    }
    for (int index = 0; index < contacts.endCount; ++index) {
        const b3ContactEndTouchEvent& event = contacts.endEvents[index];
        append_pair(next.contact_end, event.shapeIdA, event.shapeIdB);
    }
    for (int index = 0; index < contacts.hitCount; ++index) {
        const b3ContactHitEvent& event = contacts.hitEvents[index];
        BridgeRecord* first_bridge = resolve_bridge(event.shapeIdA);
        BridgeRecord* second_bridge = resolve_bridge(event.shapeIdB);
        if (first_bridge == nullptr || second_bridge == nullptr) {
            continue;
        }
        flecs::entity_t first = first_bridge->entity;
        flecs::entity_t second = second_bridge->entity;
        Float3 normal = engine_vector(event.normal);
        if (second < first) {
            std::swap(first, second);
            normal = {-normal.x, -normal.y, -normal.z};
        }
        next.contact_hit.push_back(
            {first, second, engine_position(event.point), normal,
             event.approachSpeed});
    }

    const b3SensorEvents sensors =
        b3World_GetSensorEvents(impl_->world_id);
    next.sensor_begin.reserve(static_cast<size_t>(sensors.beginCount));
    next.sensor_end.reserve(static_cast<size_t>(sensors.endCount));
    for (int index = 0; index < sensors.beginCount; ++index) {
        const b3SensorBeginTouchEvent& event = sensors.beginEvents[index];
        append_pair(next.sensor_begin, event.sensorShapeId,
                    event.visitorShapeId);
    }
    for (int index = 0; index < sensors.endCount; ++index) {
        const b3SensorEndTouchEvent& event = sensors.endEvents[index];
        append_pair(next.sensor_end, event.sensorShapeId,
                    event.visitorShapeId);
    }

    std::sort(next.body.begin(), next.body.end(),
              [](const PhysicsBodyEvent& a, const PhysicsBodyEvent& b) {
                  return a.entity < b.entity;
              });
    auto pair_less = [](const PhysicsPairEvent& a,
                        const PhysicsPairEvent& b) {
        return a.first < b.first ||
               (a.first == b.first && a.second < b.second);
    };
    std::sort(next.contact_begin.begin(), next.contact_begin.end(), pair_less);
    std::sort(next.contact_end.begin(), next.contact_end.end(), pair_less);
    std::sort(next.sensor_begin.begin(), next.sensor_begin.end(), pair_less);
    std::sort(next.sensor_end.begin(), next.sensor_end.end(), pair_less);
    std::sort(
        next.contact_hit.begin(), next.contact_hit.end(),
        [](const PhysicsHitEvent& a, const PhysicsHitEvent& b) {
            return a.first < b.first ||
                   (a.first == b.first && a.second < b.second);
        });

    events_ = std::move(next);
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

const std::vector<PhysicsCommandTraceEntry>&
PhysicsContext::last_command_trace() const noexcept {
    static const std::vector<PhysicsCommandTraceEntry> empty;
    return impl_ != nullptr ? impl_->last_command_trace : empty;
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

bool PhysicsContext::tombstone_event_participant_for_test(
    flecs::entity_t entity) noexcept {
    if (impl_ == nullptr || entity == 0) {
        return false;
    }
    const auto found = impl_->bridges.find(entity);
    if (found == impl_->bridges.end() || found->second == nullptr ||
        found->second->entity != entity || !found->second->live) {
        return false;
    }
    impl_->tombstoned_event_participant_for_test = entity;
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
namespace {

struct CommandTarget {
    detail::PhysicsContext* context = nullptr;
    const flecs::world_t* originating_world = nullptr;
    flecs::entity_t entity = 0;
};

bool resolve_command_target(flecs::entity entity, CommandTarget& target) {
    const flecs::entity_t entity_id = entity.id();
    flecs::world caller_world = entity.world();
    flecs::world_t* caller_world_pointer = caller_world.c_ptr();
    if (caller_world_pointer == nullptr || entity_id == 0) {
        return false;
    }
    const flecs::world_t* real_world = ecs_get_world(caller_world_pointer);
    if (real_world == nullptr || !ecs_is_alive(real_world, entity_id)) {
        return false;
    }

    const RigidBody* body = entity.try_get<RigidBody>();
    if (body == nullptr || body->type != RigidBodyType::Dynamic ||
        entity.has<PhysicsError>()) {
        return false;
    }

    flecs::world normalized_world(
        const_cast<flecs::world_t*>(real_world));
    const detail::PhysicsContextRef* ref =
        normalized_world.try_get<detail::PhysicsContextRef>();
    if (ref == nullptr || ref->value == nullptr) {
        return false;
    }
    target = {ref->value, real_world, entity_id};
    return true;
}

} // namespace

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

bool physics_teleport(
    flecs::entity entity,
    Float3 position,
    Quaternion rotation) {
    CommandTarget target;
    return resolve_command_target(entity, target) &&
           target.context->enqueue_teleport(
               target.originating_world, target.entity, position, rotation);
}

bool physics_set_velocity(
    flecs::entity entity,
    Float3 linear,
    Float3 angular) {
    CommandTarget target;
    return resolve_command_target(entity, target) &&
           target.context->enqueue_velocity(
               target.originating_world, target.entity, linear, angular);
}

bool physics_apply_force(flecs::entity entity, Float3 force) {
    CommandTarget target;
    return resolve_command_target(entity, target) &&
           target.context->enqueue_force(
               target.originating_world, target.entity, force);
}

bool physics_apply_impulse(flecs::entity entity, Float3 impulse) {
    CommandTarget target;
    return resolve_command_target(entity, target) &&
           target.context->enqueue_impulse(
               target.originating_world, target.entity, impulse);
}

bool physics_wake(flecs::entity entity) {
    CommandTarget target;
    return resolve_command_target(entity, target) &&
           target.context->enqueue_wake(
               target.originating_world, target.entity);
}

} // namespace matter::physics
