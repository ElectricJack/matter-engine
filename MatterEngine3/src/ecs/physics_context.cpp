#include "physics_context.h"
#include "physics_shapes.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <box3d/box3d.h>
#include <box3d/collision.h>  // b3CreateMesh/b3CreateHeightField + destroyers

#include "matter/event/event_hub.h"
#include "matter/events/physics_events.h"

namespace matter::physics::detail {
namespace {

// E6: deliver one flecs entity event per endpoint of every captured pair, so
// each participant independently hears "I touched `other`". Emitted for the
// RigidBody id (every physics body carries it), on the tick thread, from the
// pull stage. `ecs_emit` dispatches observers synchronously here; structural
// work an observer does is still deferred by the enclosing system, exactly as
// the existing PostPhysics event readers rely on.
template <typename Event>
void emit_pair_events(
    flecs::world& world,
    const std::vector<PhysicsPairEvent>& pairs) {
    for (const PhysicsPairEvent& pair : pairs) {
        if (world.is_alive(pair.first)) {
            Event payload{pair.second};
            world.event<Event>()
                .template id<RigidBody>()
                .entity(pair.first)
                .ctx(payload)
                .emit();
        }
        if (world.is_alive(pair.second)) {
            Event payload{pair.first};
            world.event<Event>()
                .template id<RigidBody>()
                .entity(pair.second)
                .ctx(payload)
                .emit();
        }
    }
}

struct BridgeRecord {
    const flecs::world_t* owning_world = nullptr;
    flecs::entity_t entity = 0;
    b3BodyId body = b3_nullBodyId;
    b3ShapeId shape = b3_nullShapeId;
    RigidBodyType type = RigidBodyType::Static;
    uint64_t configuration_hash = 0;
    DesiredBody desired{};
    int query_proxy = -1;
    bool physics_transform_pending = false;
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

void clear_and_destroy_bridge(
    BridgeRecord& bridge,
    b3DynamicTree& query_tree,
    PhysicsStats& stats) {
    if (!bridge.live) {
        return;
    }
    if (bridge.query_proxy >= 0) {
        b3DynamicTree_DestroyProxy(&query_tree, bridge.query_proxy);
        bridge.query_proxy = -1;
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

void update_query_proxy(
    b3DynamicTree& query_tree,
    const BridgeRecord& bridge) {
    if (bridge.live && bridge.query_proxy >= 0 &&
        b3Shape_IsValid(bridge.shape)) {
        b3DynamicTree_MoveProxy(
            &query_tree, bridge.query_proxy, b3Shape_GetAABB(bridge.shape));
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
    b3DynamicTree query_tree{};
    bool query_tree_valid = false;
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
    flecs::entity_t tombstoned_query_participant_for_test = 0;
    flecs::entity_t duplicate_overlap_participant_for_test = 0;
    bool fail_next_reconcile_mark_for_test = false;
    bool full_reconcile_required = false;
    uint64_t physics_transform_marker_allocations_for_test = 0;
    uint64_t ray_query_candidate_attempts_for_test = 0;
    uint64_t overlap_query_candidate_attempts_for_test = 0;
    bool stepping = false;

    // Static terrain colliders keyed by opaque handle. Each owns a Box3D static
    // body plus the mesh/heightfield data the shape references, which must
    // outlive the shape and be freed only after the body is destroyed.
    struct TerrainCollider {
        b3BodyId body = b3_nullBodyId;
        b3MeshData* mesh = nullptr;
        b3HeightFieldData* height_field = nullptr;
    };
    std::unordered_map<TerrainColliderHandle, TerrainCollider> terrain_colliders;
    TerrainColliderHandle next_terrain_handle = 1;
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

struct QueryBridgeContext {
    const flecs::world_t* owning_world = nullptr;
    const std::unordered_map<
        flecs::entity_t, std::unique_ptr<BridgeRecord>>* bridges = nullptr;
    flecs::entity_t tombstoned_participant = 0;
};

BridgeRecord* resolve_indexed_query_bridge(
    const QueryBridgeContext& query,
    int proxy_id,
    uint64_t user_data) {
    if (query.owning_world == nullptr || query.bridges == nullptr ||
        user_data == 0) {
        return nullptr;
    }
    const flecs::entity_t entity = static_cast<flecs::entity_t>(user_data);
    const auto found = query.bridges->find(entity);
    if (found == query.bridges->end() || found->second == nullptr) {
        return nullptr;
    }
    BridgeRecord* bridge = found->second.get();
    if (bridge->owning_world != query.owning_world || bridge->entity == 0 ||
        bridge->entity != entity ||
        !bridge->live || bridge->query_proxy != proxy_id ||
        bridge->entity == query.tombstoned_participant ||
        !b3Body_IsValid(bridge->body) || !b3Shape_IsValid(bridge->shape) ||
        b3Body_GetUserData(bridge->body) != bridge ||
        b3Shape_GetUserData(bridge->shape) != bridge) {
        return nullptr;
    }
    if (!ecs_is_alive(query.owning_world, bridge->entity)) {
        return nullptr;
    }
    return bridge;
}

struct IndexedRayQuery {
    QueryBridgeContext bridge;
    Float3 origin{};
    Float3 translation{};
    PhysicsRayHit hit{};
    uint64_t* attempts = nullptr;
    bool found = false;
};

float indexed_ray_callback(
    const b3RayCastInput* input,
    int proxy_id,
    uint64_t user_data,
    void* opaque) {
    IndexedRayQuery& query = *static_cast<IndexedRayQuery*>(opaque);
    BridgeRecord* bridge = resolve_indexed_query_bridge(
        query.bridge, proxy_id, user_data);
    if (bridge == nullptr) {
        return input->maxFraction;
    }
    ++*query.attempts;
    const b3WorldCastOutput output = b3Shape_RayCast(
        bridge->shape, box_position(query.origin),
        box_vector(query.translation));
    if (!output.hit) {
        return input->maxFraction;
    }
    if (!query.found || output.fraction < query.hit.fraction ||
        (output.fraction == query.hit.fraction &&
         bridge->entity < query.hit.entity)) {
        query.hit = {
            bridge->entity, engine_position(output.point),
            engine_vector(output.normal), output.fraction};
        query.found = true;
    }
    return query.hit.fraction;
}

struct IndexedOverlapQuery {
    QueryBridgeContext bridge;
    Float3 center{};
    float radius = 0.0f;
    flecs::entity_t duplicate_participant = 0;
    uint64_t* attempts = nullptr;
    std::vector<flecs::entity_t> entities;
    bool allocation_failed = false;
};

bool indexed_overlap_callback(
    int proxy_id,
    uint64_t user_data,
    void* opaque) {
    IndexedOverlapQuery& query =
        *static_cast<IndexedOverlapQuery*>(opaque);
    BridgeRecord* bridge = resolve_indexed_query_bridge(
        query.bridge, proxy_id, user_data);
    if (bridge == nullptr) {
        return true;
    }
    ++*query.attempts;
    const b3Vec3 closest = b3Shape_GetClosestPoint(
        bridge->shape, box_vector(query.center));
    const float dx = closest.x - query.center.x;
    const float dy = closest.y - query.center.y;
    const float dz = closest.z - query.center.z;
    if (dx * dx + dy * dy + dz * dz > query.radius * query.radius) {
        return true;
    }
    try {
        query.entities.push_back(bridge->entity);
        if (bridge->entity == query.duplicate_participant) {
            query.entities.push_back(bridge->entity);
        }
    } catch (...) {
        query.allocation_failed = true;
        return false;
    }
    return true;
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
    impl_->query_tree = b3DynamicTree_Create(16);
    impl_->query_tree_valid = true;
}

PhysicsContext::~PhysicsContext() {
    if (impl_ == nullptr) {
        return;
    }
    for (auto& entry : impl_->bridges) {
        BridgeRecord& bridge = *entry.second;
        if (impl_->query_tree_valid && bridge.query_proxy >= 0) {
            b3DynamicTree_DestroyProxy(
                &impl_->query_tree, bridge.query_proxy);
            bridge.query_proxy = -1;
        }
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
    // The world destroy took the terrain collider bodies/shapes with it; free
    // the geometry they referenced now that no shape can touch it.
    for (auto& entry : impl_->terrain_colliders) {
        if (entry.second.mesh != nullptr) {
            b3DestroyMesh(entry.second.mesh);
        }
        if (entry.second.height_field != nullptr) {
            b3DestroyHeightField(entry.second.height_field);
        }
    }
    impl_->terrain_colliders.clear();
    impl_->bridges.clear();
    if (impl_->query_tree_valid) {
        b3DynamicTree_Destroy(&impl_->query_tree);
        impl_->query_tree_valid = false;
    }
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
    if (impl_ == nullptr || entity == 0) {
        return;
    }
    try {
        if (impl_->fail_next_reconcile_mark_for_test) {
            impl_->fail_next_reconcile_mark_for_test = false;
            throw std::bad_alloc();
        }
        impl_->dirty_entities.push_back(entity);
    } catch (...) {
        // Losing a removal/invalidation mark could leave a live native body.
        // A scalar fallback cannot allocate and forces the next pass to audit
        // every declarative body and every published bridge.
        impl_->full_reconcile_required = true;
    }
}

void PhysicsContext::mark_transform_for_reconcile(
    flecs::entity entity) noexcept {
    if (impl_ == nullptr || !entity || entity.id() == 0) {
        return;
    }
    const auto found = impl_->bridges.find(entity.id());
    if (found == impl_->bridges.end() || found->second == nullptr ||
        !found->second->physics_transform_pending) {
        mark_for_reconcile(entity.id());
        return;
    }
    found->second->physics_transform_pending = false;

    const ecs::LocalTransform* transform =
        entity.try_get<ecs::LocalTransform>();
    if (transform == nullptr || !is_live_dynamic_bridge(*found->second)) {
        mark_for_reconcile(entity.id());
        return;
    }
    const b3Pos position = b3Body_GetPosition(found->second->body);
    const b3Quat rotation = b3Body_GetRotation(found->second->body);
    if (transform->translation.x != static_cast<float>(position.x) ||
        transform->translation.y != static_cast<float>(position.y) ||
        transform->translation.z != static_cast<float>(position.z) ||
        transform->rotation.x != rotation.v.x ||
        transform->rotation.y != rotation.v.y ||
        transform->rotation.z != rotation.v.z ||
        transform->rotation.w != rotation.s ||
        transform->scale.x != 1.0f || transform->scale.y != 1.0f ||
        transform->scale.z != 1.0f) {
        mark_for_reconcile(entity.id());
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
    if (impl_->full_reconcile_required) {
        // Clear only after the candidate audit is fully materialized. If this
        // function throws while allocating, the next call still fails closed.
        world.each<const RigidBody>(
            [&candidates](flecs::entity entity, const RigidBody&) {
                candidates.push_back(entity.id());
            });
        for (const auto& entry : impl_->bridges) {
            candidates.push_back(entry.first);
        }
        impl_->full_reconcile_required = false;
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end()), candidates.end());

    for (flecs::entity_t entity_id : candidates) {
        auto existing = impl_->bridges.find(entity_id);
        const bool alive = world.is_alive(entity_id);
        if (!alive) {
            if (existing != impl_->bridges.end()) {
                clear_and_destroy_bridge(
                    *existing->second, impl_->query_tree, stats_);
                impl_->bridges.erase(existing);
            }
            continue;
        }

        flecs::entity entity(world.c_ptr(), entity_id);
        if (!entity.has<RigidBody>()) {
            if (existing != impl_->bridges.end()) {
                clear_and_destroy_bridge(
                    *existing->second, impl_->query_tree, stats_);
                impl_->bridges.erase(existing);
            }
            remove_error(entity);
            continue;
        }

        const ValidationResult validation = validate_desired_body(entity);
        if (!validation.valid()) {
            if (existing != impl_->bridges.end()) {
                clear_and_destroy_bridge(
                    *existing->second, impl_->query_tree, stats_);
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
                clear_and_destroy_bridge(
                    *existing->second, impl_->query_tree, stats_);
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
        replacement->query_proxy = b3DynamicTree_CreateProxy(
            &impl_->query_tree, b3Shape_GetAABB(replacement->shape),
            b3Shape_GetFilter(replacement->shape).categoryBits,
            static_cast<uint64_t>(replacement->entity));
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
            clear_and_destroy_bridge(
                *retired, impl_->query_tree, stats_);
        }
        if (preserve_dynamic_state) {
            write_state(published, preserved);
            update_query_proxy(impl_->query_tree, published);
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
        update_query_proxy(impl_->query_tree, bridge);
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
            update_query_proxy(impl_->query_tree, bridge);
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
    impl_->stepping = true;
    b3World_Step(
        impl_->world_id, fixed_delta, static_cast<int>(substeps));
    impl_->stepping = false;
    capture_events(world);
    impl_->last_step_substeps = substeps;
    ++stats_.steps;
}

bool PhysicsContext::ray_cast(
    flecs::world& world,
    Float3 origin,
    Float3 translation,
    uint64_t category_mask,
    PhysicsRayHit& hit) {
    hit = {};
    if (impl_ == nullptr || impl_->stepping || !world_is_valid() ||
        !finite(origin) || !finite(translation) ||
        (translation.x == 0.0f && translation.y == 0.0f &&
         translation.z == 0.0f)) {
        return false;
    }
    const flecs::world_t* owning_world = ecs_get_world(world.c_ptr());
    flecs::world normalized_world(const_cast<flecs::world_t*>(owning_world));
    const PhysicsContextRef* owner =
        owning_world != nullptr
            ? normalized_world.try_get<PhysicsContextRef>()
            : nullptr;
    if (owner == nullptr || owner->value != this) {
        return false;
    }

    IndexedRayQuery query{};
    query.bridge = {
        owning_world, &impl_->bridges,
        impl_->tombstoned_query_participant_for_test};
    query.origin = origin;
    query.translation = translation;
    query.attempts = &impl_->ray_query_candidate_attempts_for_test;
    impl_->tombstoned_query_participant_for_test = 0;
    const b3RayCastInput input{
        box_vector(origin), box_vector(translation), 1.0f};
    b3DynamicTree_RayCast(
        &impl_->query_tree, &input, category_mask, false,
        indexed_ray_callback, &query);
    if (!query.found) {
        return false;
    }
    hit = query.hit;
    return true;
}

std::vector<flecs::entity_t> PhysicsContext::overlap_sphere(
    flecs::world& world,
    Float3 center,
    float radius,
    uint64_t category_mask) {
    if (impl_ == nullptr || impl_->stepping || !world_is_valid() ||
        !finite(center) || !std::isfinite(radius) || radius <= 0.0f) {
        return {};
    }
    const flecs::world_t* owning_world = ecs_get_world(world.c_ptr());
    flecs::world normalized_world(const_cast<flecs::world_t*>(owning_world));
    const PhysicsContextRef* owner =
        owning_world != nullptr
            ? normalized_world.try_get<PhysicsContextRef>()
            : nullptr;
    if (owner == nullptr || owner->value != this) {
        return {};
    }

    IndexedOverlapQuery query{};
    query.bridge = {
        owning_world, &impl_->bridges,
        impl_->tombstoned_query_participant_for_test};
    query.center = center;
    query.radius = radius;
    query.attempts = &impl_->overlap_query_candidate_attempts_for_test;
    impl_->tombstoned_query_participant_for_test = 0;
    query.duplicate_participant =
        impl_->duplicate_overlap_participant_for_test;
    impl_->duplicate_overlap_participant_for_test = 0;
    const b3Vec3 lower{
        center.x - radius, center.y - radius, center.z - radius};
    const b3Vec3 upper{
        center.x + radius, center.y + radius, center.z + radius};
    b3DynamicTree_Query(
        &impl_->query_tree, {lower, upper}, category_mask, false,
        indexed_overlap_callback, &query);
    if (query.allocation_failed) {
        return {};
    }
    std::sort(query.entities.begin(), query.entities.end());
    query.entities.erase(
        std::unique(query.entities.begin(), query.entities.end()),
        query.entities.end());
    return query.entities;
}

bool PhysicsContext::cast_ray_world(
    Float3 origin,
    Float3 translation,
    uint64_t category_mask,
    PhysicsWorldRayHit& hit) {
    hit = {};
    if (impl_ == nullptr || impl_->stepping || !world_is_valid() ||
        !finite(origin) || !finite(translation) ||
        (translation.x == 0.0f && translation.y == 0.0f &&
         translation.z == 0.0f)) {
        return false;
    }
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.maskBits = category_mask;
    const b3RayResult result = b3World_CastRayClosest(
        impl_->world_id, box_position(origin), box_vector(translation), filter);
    if (!result.hit) {
        return false;
    }
    hit.position = {result.point.x, result.point.y, result.point.z};
    hit.normal = {result.normal.x, result.normal.y, result.normal.z};
    hit.fraction = result.fraction;
    hit.material_id = result.userMaterialId;
    hit.triangle = result.triangleIndex;
    hit.hit = true;
    return true;
}

TerrainColliderHandle PhysicsContext::attach_static_mesh(
    const StaticMeshCollider& desc) {
    if (impl_ == nullptr || impl_->stepping || !world_is_valid() ||
        desc.vertices == nullptr || desc.indices == nullptr ||
        desc.vertex_count < 3 || desc.triangle_count < 1) {
        return 0;
    }

    // The flat float/uint32 spans are layout-compatible with b3Vec3/int32; the
    // const_cast is safe because b3CreateMesh only reads and clones them.
    b3MeshDef mesh_def{};
    mesh_def.vertices =
        reinterpret_cast<b3Vec3*>(const_cast<float*>(desc.vertices));
    mesh_def.vertexCount = desc.vertex_count;
    mesh_def.indices =
        reinterpret_cast<int32_t*>(const_cast<uint32_t*>(desc.indices));
    mesh_def.triangleCount = desc.triangle_count;
    mesh_def.weldVertices = false;      // already welded upstream
    mesh_def.useMedianSplit = true;     // grid-shaped terrain; keeps the BVH shallow
    mesh_def.identifyEdges = true;      // internal-edge smoothing (winding is consistent)

    // NULL means a BVH-stack overflow or an all-degenerate mesh — a recoverable
    // skip, never a NULL-deref into b3CreateMeshShape.
    b3MeshData* mesh = b3CreateMesh(&mesh_def, nullptr, 0);
    if (mesh == nullptr) {
        return 0;
    }

    b3BodyDef body_def = b3DefaultBodyDef();
    body_def.type = b3_staticBody;
    body_def.position = box_position(desc.translation);
    const b3BodyId body = b3CreateBody(impl_->world_id, &body_def);
    if (!b3Body_IsValid(body)) {
        b3DestroyMesh(mesh);
        return 0;
    }

    b3ShapeDef shape_def = b3DefaultShapeDef();
    shape_def.baseMaterial.friction = desc.friction;
    const b3Vec3 unit_scale{1.0f, 1.0f, 1.0f};
    const b3ShapeId shape =
        b3CreateMeshShape(body, &shape_def, mesh, unit_scale);
    if (!b3Shape_IsValid(shape)) {
        b3DestroyBody(body);
        b3DestroyMesh(mesh);
        return 0;
    }

    const TerrainColliderHandle handle = impl_->next_terrain_handle++;
    impl_->terrain_colliders.emplace(
        handle, Impl::TerrainCollider{body, mesh, nullptr});
    return handle;
}

TerrainColliderHandle PhysicsContext::attach_static_heightfield(
    const StaticHeightFieldCollider& desc) {
    if (impl_ == nullptr || impl_->stepping || !world_is_valid() ||
        desc.heights == nullptr || desc.count_x < 2 || desc.count_z < 2 ||
        !(desc.scale.x > 0.0f && desc.scale.y > 0.0f && desc.scale.z > 0.0f)) {
        return 0;
    }

    b3HeightFieldDef hf_def{};
    hf_def.heights = const_cast<float*>(desc.heights);
    hf_def.materialIndices = nullptr;
    hf_def.scale = box_vector(desc.scale);
    hf_def.countX = desc.count_x;
    hf_def.countZ = desc.count_z;
    hf_def.globalMinimumHeight = desc.global_min;
    hf_def.globalMaximumHeight = desc.global_max;
    hf_def.clockwiseWinding = false;

    b3HeightFieldData* height_field = b3CreateHeightField(&hf_def);
    if (height_field == nullptr) {
        return 0;
    }

    b3BodyDef body_def = b3DefaultBodyDef();
    body_def.type = b3_staticBody;
    body_def.position = box_position(desc.translation);
    const b3BodyId body = b3CreateBody(impl_->world_id, &body_def);
    if (!b3Body_IsValid(body)) {
        b3DestroyHeightField(height_field);
        return 0;
    }

    b3ShapeDef shape_def = b3DefaultShapeDef();
    shape_def.baseMaterial.friction = desc.friction;
    const b3ShapeId shape =
        b3CreateHeightFieldShape(body, &shape_def, height_field);
    if (!b3Shape_IsValid(shape)) {
        b3DestroyBody(body);
        b3DestroyHeightField(height_field);
        return 0;
    }

    const TerrainColliderHandle handle = impl_->next_terrain_handle++;
    impl_->terrain_colliders.emplace(
        handle, Impl::TerrainCollider{body, nullptr, height_field});
    return handle;
}

bool PhysicsContext::detach_static(TerrainColliderHandle handle) {
    if (impl_ == nullptr || handle == 0) {
        return false;
    }
    const auto found = impl_->terrain_colliders.find(handle);
    if (found == impl_->terrain_colliders.end()) {
        return false;
    }
    // Destroy the body/shape first, then the geometry it referenced.
    if (b3Body_IsValid(found->second.body)) {
        b3DestroyBody(found->second.body);
    }
    if (found->second.mesh != nullptr) {
        b3DestroyMesh(found->second.mesh);
    }
    if (found->second.height_field != nullptr) {
        b3DestroyHeightField(found->second.height_field);
    }
    impl_->terrain_colliders.erase(found);
    return true;
}

namespace {

// Gathers the mover's contact planes as rigid, velocity-clipping planes (no soft
// push, no dynamic-body push — the character is a ghost capsule for M2).
struct MoverPlaneGather {
    b3CollisionPlane* planes;
    int* count;
    int capacity;
};

bool mover_plane_cb(b3ShapeId, const b3PlaneResult* results, int n, void* ctx) {
    auto* gather = static_cast<MoverPlaneGather*>(ctx);
    for (int i = 0; i < n && *gather->count < gather->capacity; ++i) {
        gather->planes[*gather->count] =
            b3CollisionPlane{results[i].plane, FLT_MAX, 0.0f, true};
        ++*gather->count;
    }
    return true;
}

bool mover_accept_all(b3ShapeId, void*) { return true; }

}  // namespace

bool PhysicsContext::move_character(
    const CharacterMoveInput& in, CharacterMoveOutput& out) {
    out.position = in.position;
    out.velocity = in.velocity;
    out.ground_normal = {0.0f, 1.0f, 0.0f};
    out.grounded = false;
    if (impl_ == nullptr || impl_->stepping || !world_is_valid()) {
        return false;
    }

    const b3WorldId world = impl_->world_id;
    const float radius = in.radius;
    const float half_seg = std::max(0.0f, in.half_segment);
    const float dt = in.dt;

    b3Pos p = box_position(in.position);   // double precision
    b3Vec3 vel = box_vector(in.velocity);  // float
    const b3Vec3 g = box_vector(in.gravity);

    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.maskBits = in.category_mask;

    // Probe for ground below the lower sphere; reach covers a step plus a snap
    // tolerance so we detect ledges to climb and ground to stick to.
    const float snap_reach = radius + in.step_height + 0.05f;
    auto probe_ground = [&](const b3Pos& at) -> b3RayResult {
        b3Pos origin = at;
        origin.y = at.y - half_seg;
        const b3Vec3 down{0.0f, -(radius + snap_reach), 0.0f};
        return b3World_CastRayClosest(world, origin, down, filter);
    };

    // --- grounding + slope-limit gate ---------------------------------------
    const bool rising = vel.y > 0.001f;  // leaving the ground (e.g. a jump)
    const b3RayResult ray = probe_ground(p);
    const bool touching = ray.hit && !rising;
    const bool standable = touching && ray.normal.y >= in.max_slope_cos;
    if (standable) {
        out.ground_normal = {ray.normal.x, ray.normal.y, ray.normal.z};
    }

    // --- horizontal control -------------------------------------------------
    // Instant control on walkable ground; on a too-steep surface or in the air
    // the horizontal velocity is left to momentum, so gravity + collide-and-
    // slide carry the character downhill (slide) and it can never walk up.
    if (standable) {
        vel.x = in.desired_horizontal_velocity.x;
        vel.z = in.desired_horizontal_velocity.z;
        if (vel.y < 0.0f) {
            vel.y = 0.0f;  // rest; gravity is re-applied just below
        }
    }

    vel.x += g.x * dt;
    vel.y += g.y * dt;
    vel.z += g.z * dt;

    // --- collide-and-slide (design §2) --------------------------------------
    // On walkable ground, move horizontally only and let the ground snap set
    // the height. Feeding gravity's downward component into the solver on a
    // slope would be reprojected into downhill motion, making a standing
    // character creep downhill; suppressing it is what keeps walkable slopes
    // walkable. In the air or on a too-steep surface the full velocity (gravity
    // included) drives the move, so ballistic fall and downhill slide happen.
    b3Vec3 move_vel = vel;
    if (standable) {
        move_vel.y = 0.0f;
    }
    const b3Capsule capsule{
        {0.0f, -half_seg, 0.0f}, {0.0f, half_seg, 0.0f}, radius};
    b3Pos target = p;
    target.x = p.x + move_vel.x * dt;
    target.y = p.y + move_vel.y * dt;
    target.z = p.z + move_vel.z * dt;

    constexpr int kPlaneCap = 32;
    b3CollisionPlane planes[kPlaneCap];
    int plane_count = 0;
    for (int iteration = 0; iteration < 5; ++iteration) {
        plane_count = 0;
        MoverPlaneGather gather{planes, &plane_count, kPlaneCap};
        b3World_CollideMover(
            world, p, &capsule, filter, mover_plane_cb, &gather);

        const b3Vec3 target_delta{
            static_cast<float>(target.x - p.x),
            static_cast<float>(target.y - p.y),
            static_cast<float>(target.z - p.z)};
        const b3PlaneSolverResult solved =
            b3SolvePlanes(target_delta, planes, plane_count);
        b3Vec3 delta = solved.delta;
        const float fraction = b3World_CastMover(
            world, p, &capsule, delta, filter, mover_accept_all, nullptr);
        delta.x *= fraction;
        delta.y *= fraction;
        delta.z *= fraction;
        p.x += delta.x;
        p.y += delta.y;
        p.z += delta.z;
        if (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z <
            1e-4f) {
            break;
        }
    }

    // Clip velocity against the resolved planes so wall contact and
    // depenetration do not accumulate speed.
    if (plane_count > 0) {
        vel = b3ClipVector(vel, planes, plane_count);
    }

    // --- ground snap --------------------------------------------------------
    // If we started on walkable ground, snap back onto it (down, or up to
    // step_height). This gives a stable rest height, slope following, and
    // free step-up without a separate step trace.
    if (standable) {
        const b3RayResult after = probe_ground(p);
        if (after.hit && after.normal.y >= in.max_slope_cos) {
            // Rest the lower sphere TANGENT to the surface: the vertical drop
            // from the sphere center to the point straight below is radius/cosθ,
            // not radius, on a slope. Add a small hover so the capsule never
            // penetrates the ground — otherwise each step's depenetration would
            // push a standing character downhill (slope creep).
            constexpr float kSkin = 0.02f;
            const float ncos = std::max(after.normal.y, 0.5f);
            const float rest_y =
                after.point.y + radius / ncos + half_seg + kSkin;
            const float dy = rest_y - static_cast<float>(p.y);
            if (dy <= in.step_height && dy >= -(radius + snap_reach)) {
                p.y = rest_y;
                if (vel.y < 0.0f) {
                    vel.y = 0.0f;
                }
                out.grounded = true;
                out.ground_normal = {after.normal.x, after.normal.y,
                                     after.normal.z};
            }
        }
    }

    out.position = {static_cast<float>(p.x), static_cast<float>(p.y),
                    static_cast<float>(p.z)};
    out.velocity = {vel.x, vel.y, vel.z};
    return true;
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
        update_query_proxy(impl_->query_tree, *bridge);
        if (bridge->entity == tombstoned_participant) {
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
        // Flecs may defer this OnSet observer until the system merge, so a
        // context-global scoped boolean cannot distinguish this write. The
        // stable bridge carries the pending bit without allocating, and the
        // transform observer consumes it only after comparing the final pose.
        bridge->physics_transform_pending = true;
        entity.set<ecs::LocalTransform>(transform);
        entity.set<PhysicsVelocity>(velocity);
        entity.add<ecs::TransformDirty>();
    }

    // E6 (docs/event-system.md S I.11 PhysicsEvents row): deliver the captured
    // snapshot as per-entity flecs gameplay events during this pull stage, then
    // mirror one aggregate onto the session hub trace for the inspector. The
    // snapshot (events_) was filled by the preceding step() this fixed tick.
    emit_pair_events<PhysContactBegin>(world, events_.contact_begin);
    emit_pair_events<PhysContactEnd>(world, events_.contact_end);
    emit_pair_events<PhysSensorEnter>(world, events_.sensor_begin);
    emit_pair_events<PhysSensorExit>(world, events_.sensor_end);

    if (event_hub_ != nullptr) {
        const uint32_t contacts = static_cast<uint32_t>(
            events_.contact_begin.size() + events_.contact_end.size());
        const uint32_t sensors = static_cast<uint32_t>(
            events_.sensor_begin.size() + events_.sensor_end.size());
        if (contacts + sensors > 0) {
            event_hub_->emit(matter::events::PhysStep{contacts, sensors});
        }
    }
}

void PhysicsContext::set_event_hub(matter::evt::Hub* hub) noexcept {
    event_hub_ = hub;
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
    update_query_proxy(impl_->query_tree, *found->second);
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

bool PhysicsContext::tombstone_query_participant_for_test(
    flecs::entity_t entity) noexcept {
    if (impl_ == nullptr || entity == 0) {
        return false;
    }
    const auto found = impl_->bridges.find(entity);
    if (found == impl_->bridges.end() || found->second == nullptr ||
        found->second->entity != entity || !found->second->live) {
        return false;
    }
    impl_->tombstoned_query_participant_for_test = entity;
    return true;
}

bool PhysicsContext::duplicate_overlap_participant_for_test(
    flecs::entity_t entity) noexcept {
    if (impl_ == nullptr || entity == 0) {
        return false;
    }
    const auto found = impl_->bridges.find(entity);
    if (found == impl_->bridges.end() || found->second == nullptr ||
        found->second->entity != entity || !found->second->live) {
        return false;
    }
    impl_->duplicate_overlap_participant_for_test = entity;
    return true;
}

void PhysicsContext::fail_next_reconcile_mark_for_test() noexcept {
    if (impl_ != nullptr) {
        impl_->fail_next_reconcile_mark_for_test = true;
    }
}

uint64_t PhysicsContext::physics_transform_marker_allocations_for_test()
    const noexcept {
    return impl_ != nullptr
        ? impl_->physics_transform_marker_allocations_for_test : 0;
}

uint64_t PhysicsContext::ray_query_candidate_attempts_for_test()
    const noexcept {
    return impl_ != nullptr ? impl_->ray_query_candidate_attempts_for_test : 0;
}

uint64_t PhysicsContext::overlap_query_candidate_attempts_for_test()
    const noexcept {
    return impl_ != nullptr
        ? impl_->overlap_query_candidate_attempts_for_test : 0;
}

void PhysicsContext::set_stepping_for_test(bool stepping) noexcept {
    if (impl_ != nullptr) {
        impl_->stepping = stepping;
    }
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

bool physics_ray_cast(
    flecs::world& world,
    Float3 origin,
    Float3 translation,
    uint64_t category_mask,
    PhysicsRayHit& hit) {
    const flecs::world_t* real_world = ecs_get_world(world.c_ptr());
    if (real_world == nullptr) {
        hit = {};
        return false;
    }
    flecs::world normalized_world(const_cast<flecs::world_t*>(real_world));
    const detail::PhysicsContextRef* ref =
        normalized_world.try_get<detail::PhysicsContextRef>();
    if (ref == nullptr || ref->value == nullptr) {
        hit = {};
        return false;
    }
    return ref->value->ray_cast(
        normalized_world, origin, translation, category_mask, hit);
}

std::vector<flecs::entity_t> physics_overlap_sphere(
    flecs::world& world,
    Float3 center,
    float radius,
    uint64_t category_mask) {
    const flecs::world_t* real_world = ecs_get_world(world.c_ptr());
    if (real_world == nullptr) {
        return {};
    }
    flecs::world normalized_world(const_cast<flecs::world_t*>(real_world));
    const detail::PhysicsContextRef* ref =
        normalized_world.try_get<detail::PhysicsContextRef>();
    if (ref == nullptr || ref->value == nullptr) {
        return {};
    }
    return ref->value->overlap_sphere(
        normalized_world, center, radius, category_mask);
}

namespace {

// Resolve the PhysicsContext owning `world`, or nullptr. Mirrors the guard in
// physics_ray_cast/physics_overlap_sphere.
detail::PhysicsContext* resolve_context(flecs::world& world) {
    const flecs::world_t* real_world = ecs_get_world(world.c_ptr());
    if (real_world == nullptr) {
        return nullptr;
    }
    flecs::world normalized_world(const_cast<flecs::world_t*>(real_world));
    const detail::PhysicsContextRef* ref =
        normalized_world.try_get<detail::PhysicsContextRef>();
    return ref != nullptr ? ref->value : nullptr;
}

} // namespace

bool physics_cast_ray_world(
    flecs::world& world,
    Float3 origin,
    Float3 translation,
    uint64_t category_mask,
    PhysicsWorldRayHit& hit) {
    detail::PhysicsContext* context = resolve_context(world);
    if (context == nullptr) {
        hit = {};
        return false;
    }
    return context->cast_ray_world(origin, translation, category_mask, hit);
}

TerrainColliderHandle physics_attach_static_mesh(
    flecs::world& world, const StaticMeshCollider& desc) {
    detail::PhysicsContext* context = resolve_context(world);
    return context != nullptr ? context->attach_static_mesh(desc) : 0;
}

TerrainColliderHandle physics_attach_static_heightfield(
    flecs::world& world, const StaticHeightFieldCollider& desc) {
    detail::PhysicsContext* context = resolve_context(world);
    return context != nullptr ? context->attach_static_heightfield(desc) : 0;
}

bool physics_detach_static(
    flecs::world& world, TerrainColliderHandle handle) {
    detail::PhysicsContext* context = resolve_context(world);
    return context != nullptr ? context->detach_static(handle) : false;
}

bool physics_move_character(
    flecs::world& world,
    const CharacterMoveInput& in,
    CharacterMoveOutput& out) {
    detail::PhysicsContext* context = resolve_context(world);
    if (context == nullptr) {
        out.position = in.position;
        out.velocity = in.velocity;
        out.ground_normal = {0.0f, 1.0f, 0.0f};
        out.grounded = false;
        return false;
    }
    return context->move_character(in, out);
}

} // namespace matter::physics
