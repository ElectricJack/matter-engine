#include "physics_context.h"
#include "physics_shapes.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <box3d/box3d.h>

#include "matter/event/event_hub.h"
#include "matter/events/physics_events.h"
#include "terrain_collision/terrain_collision_artifact.h"

namespace matter::physics::detail {
namespace {

constexpr std::size_t kForceCommandCapacity = 4096;

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
    std::shared_ptr<const void> guard_owner;
    PhysicsCommandGuardBegin guard_begin = nullptr;
    PhysicsCommandGuardEnd guard_end = nullptr;
    std::uint64_t guarded_batch_id = 0;
    std::size_t guarded_batch_count = 0;
    std::size_t guarded_batch_index = 0;
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

bool valid_character_input(const CharacterMoveInput& input) noexcept {
    return finite(input.position) && finite(input.velocity) &&
           finite(input.desired_horizontal_velocity) && finite(input.gravity) &&
           std::isfinite(input.radius) && input.radius > 0.0f &&
           std::isfinite(input.half_segment) && input.half_segment >= 0.0f &&
           std::isfinite(input.dt) && input.dt > 0.0f &&
           std::isfinite(input.max_slope_cos) && input.max_slope_cos >= 0.0f &&
           input.max_slope_cos <= 1.0f && std::isfinite(input.step_height) &&
           input.step_height >= 0.0f;
}

bool character_float_range(double value) noexcept {
    return std::isfinite(value) && value >= -FLT_MAX && value <= FLT_MAX;
}

bool character_position_range(b3Pos value) noexcept {
    return character_float_range(value.x) && character_float_range(value.y) &&
           character_float_range(value.z);
}

bool character_query_vector(b3Vec3 value) noexcept {
    // Box3D normalizes ray/capsule vectors and uses squared distances. Merely
    // finite components do not make those float operations representable.
    const double squared = static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y +
        static_cast<double>(value.z) * value.z;
    return character_float_range(squared) &&
           std::isfinite(b3LengthSquared(value));
}

bool character_query_bounds(b3Pos position, const b3Capsule& capsule,
                            b3Vec3 translation) noexcept {
    const float reach = capsule.center2.y + capsule.radius;
    const b3Vec3 extent{capsule.radius, reach, capsule.radius};
    const b3Pos lower{position.x - extent.x, position.y - extent.y,
                      position.z - extent.z};
    const b3Pos upper{position.x + extent.x, position.y + extent.y,
                      position.z + extent.z};
    if (!character_position_range(lower) || !character_position_range(upper))
        return false;
    const b3AABB box = b3OffsetAABB(
        {{-extent.x, -extent.y, -extent.z}, extent}, position);
    // BoxCast computes (lower + upper) / 2, (upper - lower) / 2, and
    // translated bounds in float, even though the query origin is double.
    return finite(engine_vector(b3Add(box.lowerBound, box.upperBound))) &&
           finite(engine_vector(b3Sub(box.upperBound, box.lowerBound))) &&
           finite(engine_vector(b3Add(box.lowerBound, translation))) &&
           finite(engine_vector(b3Add(box.upperBound, translation)));
}

bool mover_accepts_static_shape(b3ShapeId shape) {
    const b3BodyId body = b3Shape_GetBody(shape);
    return b3Body_IsValid(body) && b3Body_GetType(body) == b3_staticBody &&
           !b3Shape_IsSensor(shape);
}

struct StaticMoverPlaneGather {
    b3CollisionPlane* planes = nullptr;
    int* count = nullptr;
    int capacity = 0;
    bool valid = true;
};

bool static_mover_plane_callback(
    b3ShapeId shape, const b3PlaneResult* results, int count, void* context) {
    if (!mover_accepts_static_shape(shape)) return true;
    auto* gather = static_cast<StaticMoverPlaneGather*>(context);
    for (int index = 0; index < count && *gather->count < gather->capacity;
         ++index) {
        if (!character_query_vector(results[index].plane.normal) ||
            !std::isfinite(results[index].plane.offset)) {
            gather->valid = false;
            return false;
        }
        gather->planes[*gather->count] =
            {results[index].plane, FLT_MAX, 0.0f, true};
        ++*gather->count;
    }
    return true;
}

bool static_mover_filter(b3ShapeId shape, void*) {
    return mover_accepts_static_shape(shape);
}

struct StaticRayResult {
    bool hit = false;
    b3Pos point{};
    b3Vec3 normal{};
    float fraction = 1.0f;
    bool valid = true;
};

float static_ray_callback(
    b3ShapeId shape,
    b3Pos point,
    b3Vec3 normal,
    float fraction,
    uint64_t,
    int,
    int,
    void* context) {
    if (!mover_accepts_static_shape(shape)) return -1.0f;
    auto* result = static_cast<StaticRayResult*>(context);
    if (!character_position_range(point) || !character_query_vector(normal) ||
        !std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f) {
        result->valid = false;
        return 0.0f;
    }
    if (!result->hit || fraction < result->fraction) {
        result->hit = true;
        result->point = point;
        result->normal = normal;
        result->fraction = fraction;
    }
    return fraction;
}

struct HullDeleter {
    void operator()(b3HullData* hull) const {
        if (hull != nullptr) {
            b3DestroyHull(hull);
        }
    }
};

struct TerrainCollisionTileRuntime {
    terrain_collision::SectorCoordinate coordinate{};
    Float3 origin_m{};
    std::uint64_t tile_key = 0;
    std::vector<b3Vec3> vertices;
    std::vector<std::int32_t> indices;
    b3MeshData* mesh_data = nullptr;
    b3BodyId body = b3_nullBodyId;
    b3ShapeId shape = b3_nullShapeId;

    TerrainCollisionTileRuntime() = default;
    TerrainCollisionTileRuntime(const TerrainCollisionTileRuntime&) = delete;
    TerrainCollisionTileRuntime& operator=(
        const TerrainCollisionTileRuntime&) = delete;

    TerrainCollisionTileRuntime(
        TerrainCollisionTileRuntime&& other) noexcept
        : coordinate(other.coordinate),
          origin_m(other.origin_m),
          tile_key(other.tile_key),
          vertices(std::move(other.vertices)),
          indices(std::move(other.indices)),
          mesh_data(std::exchange(other.mesh_data, nullptr)),
          body(std::exchange(other.body, b3_nullBodyId)),
          shape(std::exchange(other.shape, b3_nullShapeId)) {}

    TerrainCollisionTileRuntime& operator=(
        TerrainCollisionTileRuntime&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        coordinate = other.coordinate;
        origin_m = other.origin_m;
        tile_key = other.tile_key;
        vertices = std::move(other.vertices);
        indices = std::move(other.indices);
        mesh_data = std::exchange(other.mesh_data, nullptr);
        body = std::exchange(other.body, b3_nullBodyId);
        shape = std::exchange(other.shape, b3_nullShapeId);
        return *this;
    }

    ~TerrainCollisionTileRuntime() {
        reset();
    }

    void reset() noexcept {
        // A body owns its attached shape. Destroying it first both destroys
        // the shape and ends every reference to mesh_data.
        if (b3Body_IsValid(body)) {
            b3DestroyBody(body);
        }
        shape = b3_nullShapeId;
        body = b3_nullBodyId;
        if (mesh_data != nullptr) {
            b3DestroyMesh(mesh_data);
            mesh_data = nullptr;
        }
    }
};

struct TerrainCollisionRuntime {
    TerrainCollisionPhysicsStats stats{};
    std::vector<TerrainCollisionTileRuntime> tiles;
};

} // namespace

TerrainCollisionMeshLayoutError checked_terrain_collision_mesh_layout(
    std::uint64_t vertex_count,
    std::uint64_t index_count,
    TerrainCollisionMeshLayout& layout) noexcept {
    layout = {};
    constexpr std::uint64_t signed_limit =
        static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    if (vertex_count < 3 || vertex_count > signed_limit) {
        return TerrainCollisionMeshLayoutError::VertexCount;
    }
    if (index_count == 0 || index_count % 3 != 0) {
        return TerrainCollisionMeshLayoutError::IndexCount;
    }

    const std::uint64_t triangle_count = index_count / 3;
    const std::uint64_t max_node_safe_triangles = (signed_limit + 1) / 2;
    if (triangle_count == 0 ||
        triangle_count > max_node_safe_triangles) {
        return TerrainCollisionMeshLayoutError::TriangleNodeCount;
    }
    const std::uint64_t node_count = 2 * triangle_count - 1;
    if (index_count > signed_limit) {
        return TerrainCollisionMeshLayoutError::IndexCount;
    }

    auto checked_multiply = [](
        std::uint64_t first,
        std::uint64_t second,
        std::uint64_t& product) noexcept {
        if (first != 0 && second >
                std::numeric_limits<std::uint64_t>::max() / first) {
            return false;
        }
        product = first * second;
        return true;
    };
    auto checked_align_eight = [](
        std::uint64_t value,
        std::uint64_t& aligned) noexcept {
        if (value > std::numeric_limits<std::uint64_t>::max() - 7) {
            return false;
        }
        aligned = (value + 7) & ~std::uint64_t{7};
        return true;
    };

    std::uint64_t byte_count = 0;
    if (!checked_align_eight(sizeof(b3MeshData), byte_count) ||
        byte_count > signed_limit) {
        return TerrainCollisionMeshLayoutError::RetainedLayout;
    }
    auto append_aligned = [&](
        std::uint64_t count,
        std::uint64_t element_size) noexcept {
        std::uint64_t bytes = 0;
        std::uint64_t aligned = 0;
        if (!checked_multiply(count, element_size, bytes) ||
            !checked_align_eight(bytes, aligned) ||
            aligned > signed_limit || byte_count > signed_limit - aligned) {
            return false;
        }
        byte_count += aligned;
        return true;
    };
    if (!append_aligned(node_count, sizeof(b3MeshNode)) ||
        !append_aligned(vertex_count, sizeof(b3Vec3)) ||
        !append_aligned(triangle_count, sizeof(b3MeshTriangle)) ||
        !append_aligned(triangle_count, sizeof(std::uint8_t)) ||
        !append_aligned(triangle_count, sizeof(std::uint8_t))) {
        return TerrainCollisionMeshLayoutError::RetainedLayout;
    }

    layout.vertex_count = static_cast<std::int32_t>(vertex_count);
    layout.index_count = static_cast<std::int32_t>(index_count);
    layout.triangle_count = static_cast<std::int32_t>(triangle_count);
    layout.node_count = static_cast<std::int32_t>(node_count);
    layout.worst_case_retained_bytes =
        static_cast<std::int32_t>(byte_count);
    return TerrainCollisionMeshLayoutError::None;
}

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
    std::vector<QueuedCommand> force_drain_buffer;
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
    std::uint64_t next_guarded_batch_id = 1;
    void* guarded_batch_hook_context = nullptr;
    GuardedBatchPostFirstRowHook guarded_batch_post_first_row_hook = nullptr;
    bool full_reconcile_required = false;
    uint64_t physics_transform_marker_allocations_for_test = 0;
    uint64_t ray_query_candidate_attempts_for_test = 0;
    uint64_t overlap_query_candidate_attempts_for_test = 0;
    bool stepping = false;
    std::thread::id owner_thread{};
    std::size_t fail_terrain_mesh_create_tile_for_test = 0;
    std::unique_ptr<TerrainCollisionRuntime> terrain_collision;
};

void fail_terrain_collision_mesh_create_on_tile_for_test(
    PhysicsContext& context,
    std::size_t one_based_non_empty_tile) noexcept {
    if (context.impl_ != nullptr &&
        std::this_thread::get_id() == context.impl_->owner_thread &&
        !context.impl_->stepping) {
        context.impl_->fail_terrain_mesh_create_tile_for_test =
            one_based_non_empty_tile;
    }
}

namespace {

bool validate_and_build_terrain_tile(
    b3WorldId world_id,
    const terrain_collision::TileCandidate& candidate,
    float friction,
    float restitution,
    bool inject_mesh_create_failure,
    TerrainCollisionTileRuntime& tile,
    std::string& error) {
    const std::size_t vertex_count = candidate.vertices.size();
    const std::size_t index_count = candidate.indices.size();
    if (vertex_count == 0 || index_count == 0) {
        error = "terrain collision tile has mismatched empty geometry";
        return false;
    }
    TerrainCollisionMeshLayout layout{};
    const TerrainCollisionMeshLayoutError layout_error =
        checked_terrain_collision_mesh_layout(
            static_cast<std::uint64_t>(vertex_count),
            static_cast<std::uint64_t>(index_count), layout);
    if (layout_error != TerrainCollisionMeshLayoutError::None) {
        switch (layout_error) {
        case TerrainCollisionMeshLayoutError::VertexCount:
            error = "terrain collision tile vertex count exceeds Box3D limits";
            break;
        case TerrainCollisionMeshLayoutError::IndexCount:
            error = "terrain collision tile index count is invalid for Box3D";
            break;
        case TerrainCollisionMeshLayoutError::TriangleNodeCount:
            error = "terrain collision tile triangle tree exceeds Box3D limits";
            break;
        case TerrainCollisionMeshLayoutError::RetainedLayout:
            error = "terrain collision tile retained layout exceeds Box3D limits";
            break;
        case TerrainCollisionMeshLayoutError::None:
            break;
        }
        return false;
    }
    if (!finite(candidate.origin_m)) {
        error = "terrain collision tile origin is not finite";
        return false;
    }
    for (const Float3 vertex : candidate.vertices) {
        if (!finite(vertex)) {
            error = "terrain collision tile vertex is not finite";
            return false;
        }
    }
    for (const std::uint32_t index : candidate.indices) {
        if (static_cast<std::size_t>(index) >= vertex_count) {
            error = "terrain collision tile index is out of range";
            return false;
        }
    }

    bool has_non_degenerate_triangle = false;
    b3AABB accepted_triangle_bounds{};
    const float minimum_area =
        0.01f * B3_LINEAR_SLOP * B3_LINEAR_SLOP;
    for (std::int32_t triangle = 0;
         triangle < layout.triangle_count;
         ++triangle) {
        const std::size_t offset =
            static_cast<std::size_t>(triangle) * 3;
        const std::uint32_t index1 = candidate.indices[offset];
        const std::uint32_t index2 = candidate.indices[offset + 1];
        const std::uint32_t index3 = candidate.indices[offset + 2];
        if (index1 == index2 || index1 == index3 || index2 == index3) {
            error = "terrain collision triangle repeats a vertex index";
            return false;
        }
        const b3Vec3 vertex1 = box_vector(candidate.vertices[index1]);
        const b3Vec3 vertex2 = box_vector(candidate.vertices[index2]);
        const b3Vec3 vertex3 = box_vector(candidate.vertices[index3]);
        const b3Vec3 normal = b3Cross(
            b3Sub(vertex2, vertex1), b3Sub(vertex3, vertex1));
        const float area = 0.5f * b3Length(normal);
        if (!std::isfinite(area)) {
            error = "terrain collision triangle area is not finite";
            return false;
        }
        if (area >= minimum_area) {
            const b3AABB triangle_bounds = {
                b3Min(vertex1, b3Min(vertex2, vertex3)),
                b3Max(vertex1, b3Max(vertex2, vertex3)),
            };
            accepted_triangle_bounds = has_non_degenerate_triangle
                ? b3AABB_Union(
                      accepted_triangle_bounds, triangle_bounds)
                : triangle_bounds;
            has_non_degenerate_triangle = true;
        }
    }
    if (!has_non_degenerate_triangle) {
        error = "terrain collision mesh has no triangle above Box3D's minimum area";
        return false;
    }
    if (!b3IsSaneAABB(accepted_triangle_bounds)) {
        error = "terrain collision mesh bounds exceed Box3D sanity limits";
        return false;
    }

    tile.coordinate = candidate.coordinate;
    tile.origin_m = candidate.origin_m;
    tile.tile_key = candidate.tile_key;
    tile.vertices.reserve(vertex_count);
    for (const Float3 vertex : candidate.vertices) {
        tile.vertices.push_back(box_vector(vertex));
    }
    tile.indices.reserve(index_count);
    for (const std::uint32_t index : candidate.indices) {
        tile.indices.push_back(static_cast<std::int32_t>(index));
    }

    b3BodyDef body_definition = b3DefaultBodyDef();
    body_definition.type = b3_staticBody;
    body_definition.position = box_position(candidate.origin_m);
    tile.body = b3CreateBody(world_id, &body_definition);
    if (!b3Body_IsValid(tile.body)) {
        error = "Box3D failed to create a terrain collision body";
        return false;
    }

    b3MeshDef mesh_definition{};
    mesh_definition.vertices = tile.vertices.data();
    mesh_definition.indices = tile.indices.data();
    mesh_definition.vertexCount = layout.vertex_count;
    mesh_definition.triangleCount = layout.triangle_count;
    mesh_definition.weldVertices = false;
    mesh_definition.identifyEdges = true;
    mesh_definition.useMedianSplit = true;
    std::vector<int> degenerate_indices(
        static_cast<std::size_t>(layout.triangle_count) + 1, -1);
    tile.mesh_data = inject_mesh_create_failure
        ? nullptr
        : b3CreateMesh(
              &mesh_definition,
              degenerate_indices.data(),
              layout.triangle_count + 1);
    if (tile.mesh_data == nullptr) {
        error = "Box3D rejected a terrain collision mesh";
        return false;
    }
    if (std::any_of(
            degenerate_indices.begin(), degenerate_indices.end(),
            [](int index) { return index != -1; })) {
        error = "Box3D reported a degenerate terrain collision triangle";
        return false;
    }
    if (tile.mesh_data->byteCount <= 0 ||
        tile.mesh_data->byteCount > layout.worst_case_retained_bytes) {
        error = "Box3D returned invalid terrain collision retained bytes";
        return false;
    }

    b3ShapeDef shape_definition = b3DefaultShapeDef();
    shape_definition.baseMaterial.friction = friction;
    shape_definition.baseMaterial.restitution = restitution;
    const ColliderProperties engine_default_filter{};
    shape_definition.filter.categoryBits =
        engine_default_filter.category_bits;
    shape_definition.filter.maskBits = engine_default_filter.mask_bits;
    tile.shape = b3CreateMeshShape(
        tile.body, &shape_definition, tile.mesh_data, b3Vec3_one);
    if (!b3Shape_IsValid(tile.shape)) {
        error = "Box3D failed to create a terrain collision shape";
        return false;
    }
    return true;
}

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
    impl_->owner_thread = std::this_thread::get_id();
    impl_->forces.reserve(kForceCommandCapacity);
    impl_->force_drain_buffer.reserve(kForceCommandCapacity);
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
    clear_terrain_collision();
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
    // If clear was defensively rejected (foreign-thread destruction or an
    // in-progress-step marker), the world has now invalidated every attached
    // body and shape. Releasing mesh data here cannot leave a dangling shape.
    impl_->terrain_collision.reset();
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

bool PhysicsContext::replace_terrain_collision(
    const terrain_collision::TerrainCollisionCandidate& candidate,
    std::string& error) {
    error.clear();
    if (impl_ == nullptr || !b3World_IsValid(impl_->world_id)) {
        error = "terrain collision replacement requires a live physics world";
        return false;
    }
    if (std::this_thread::get_id() != impl_->owner_thread) {
        error = "terrain collision replacement requires the physics owner thread";
        return false;
    }
    if (impl_->stepping) {
        error = "terrain collision replacement is forbidden while stepping";
        return false;
    }
    if (impl_->terrain_collision != nullptr &&
        impl_->terrain_collision->stats.installation_key ==
            candidate.installation_key) {
        return true;
    }
    if (!std::isfinite(candidate.friction) || candidate.friction < 0.0f ||
        candidate.friction > 1.0f ||
        !std::isfinite(candidate.restitution) ||
        candidate.restitution < 0.0f || candidate.restitution > 1.0f) {
        error = "terrain collision material must be finite and in [0, 1]";
        return false;
    }

    try {
        std::size_t non_empty_count = 0;
        for (const terrain_collision::TileCandidate& tile : candidate.tiles) {
            if (!tile.vertices.empty() || !tile.indices.empty()) {
                ++non_empty_count;
            }
        }
        if (non_empty_count > static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
            error = "terrain collision shape count exceeds runtime limits";
            return false;
        }

        auto replacement = std::make_unique<TerrainCollisionRuntime>();
        replacement->tiles.reserve(non_empty_count);
        replacement->stats.installation_key = candidate.installation_key;
        replacement->stats.replacements = impl_->terrain_collision != nullptr
            ? (impl_->terrain_collision->stats.replacements ==
                       std::numeric_limits<std::uint64_t>::max()
                   ? std::numeric_limits<std::uint64_t>::max()
                   : impl_->terrain_collision->stats.replacements + 1)
            : 0;

        std::size_t one_based_non_empty_tile = 0;
        for (const terrain_collision::TileCandidate& source : candidate.tiles) {
            if (source.vertices.empty() && source.indices.empty()) {
                if (!finite(source.origin_m)) {
                    error = "empty terrain collision tile origin is not finite";
                    return false;
                }
                continue;
            }
            ++one_based_non_empty_tile;
            const bool inject_mesh_create_failure =
                impl_->fail_terrain_mesh_create_tile_for_test ==
                one_based_non_empty_tile;
            if (inject_mesh_create_failure) {
                impl_->fail_terrain_mesh_create_tile_for_test = 0;
            }
            TerrainCollisionTileRuntime tile;
            if (!validate_and_build_terrain_tile(
                    impl_->world_id, source, candidate.friction,
                    candidate.restitution, inject_mesh_create_failure,
                    tile, error)) {
                return false;
            }
            const std::uint64_t tile_bytes =
                static_cast<std::uint64_t>(tile.mesh_data->byteCount);
            if (replacement->stats.retained_bytes >
                std::numeric_limits<std::uint64_t>::max() - tile_bytes) {
                error = "terrain collision retained byte count overflow";
                return false;
            }
            replacement->stats.retained_bytes += tile_bytes;
            replacement->tiles.push_back(std::move(tile));
        }
        replacement->stats.shape_count =
            static_cast<std::uint32_t>(replacement->tiles.size());

        std::unique_ptr<TerrainCollisionRuntime> retired =
            std::move(impl_->terrain_collision);
        impl_->terrain_collision = std::move(replacement);
        // Publish first, then retire. TerrainCollisionTileRuntime destroys the
        // body (and its attached shape) before releasing retained mesh data.
        retired.reset();
        return true;
    } catch (const std::bad_alloc&) {
        error = "terrain collision replacement allocation failed";
        return false;
    } catch (...) {
        error = "terrain collision replacement failed";
        return false;
    }
}

void PhysicsContext::clear_terrain_collision() noexcept {
    if (impl_ == nullptr ||
        std::this_thread::get_id() != impl_->owner_thread ||
        impl_->stepping) {
        return;
    }
    impl_->terrain_collision.reset();
}

TerrainCollisionPhysicsStats
PhysicsContext::terrain_collision_stats() const noexcept {
    return impl_ != nullptr && impl_->terrain_collision != nullptr
        ? impl_->terrain_collision->stats
        : TerrainCollisionPhysicsStats{};
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
        if (impl_->forces.size() >= kForceCommandCapacity) {
            ++stats_.failed_commands;
            return false;
        }
        impl_->forces.push_back({
            originating_world, entity, PhysicsCommandKind::Force,
            force, {}, {}});
        return true;
    } catch (...) {
        return false;
    }
}

bool PhysicsContext::enqueue_force_at_world_point(
    const flecs::world_t* originating_world,
    flecs::entity_t entity,
    Float3 force,
    Float3 world_point) noexcept {
    if (impl_ == nullptr || !finite(force) || !finite(world_point) ||
        !can_enqueue_command(
            impl_->bridges, this, originating_world, entity)) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->command_mutex);
        if (impl_->forces.size() >= kForceCommandCapacity) {
            ++stats_.failed_commands;
            return false;
        }
        impl_->forces.push_back({
            originating_world, entity, PhysicsCommandKind::ForceAtPoint,
            force, world_point, {}});
        return true;
    } catch (...) {
        return false;
    }
}

bool PhysicsContext::enqueue_guarded_force_at_world_points(
    const flecs::world_t* originating_world,
    flecs::entity_t entity,
    const GuardedForceAtWorldPoint* rows,
    std::size_t count,
    const std::shared_ptr<const void>& guard_owner,
    PhysicsCommandGuardBegin guard_begin,
    PhysicsCommandGuardEnd guard_end) noexcept {
    if (impl_ == nullptr || rows == nullptr || count == 0 ||
        !guard_owner || guard_begin == nullptr || guard_end == nullptr ||
        !can_enqueue_command(
            impl_->bridges, this, originating_world, entity)) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!finite(rows[index].force) || !finite(rows[index].world_point))
            return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->command_mutex);
        if (count > kForceCommandCapacity - impl_->forces.size()) {
            ++stats_.failed_commands;
            return false;
        }
        const std::size_t initial_size = impl_->forces.size();
        std::uint64_t batch_id = impl_->next_guarded_batch_id++;
        if (batch_id == 0) batch_id = impl_->next_guarded_batch_id++;
        try {
            for (std::size_t index = 0; index < count; ++index) {
                QueuedCommand command{
                    originating_world, entity,
                    PhysicsCommandKind::ForceAtPoint,
                    rows[index].force, rows[index].world_point, {}};
                command.guarded_batch_id = batch_id;
                command.guarded_batch_count = count;
                command.guarded_batch_index = index;
                if (index == 0) {
                    command.guard_owner = guard_owner;
                    command.guard_begin = guard_begin;
                    command.guard_end = guard_end;
                }
                impl_->forces.push_back(std::move(command));
            }
        } catch (...) {
            impl_->forces.resize(initial_size);
            return false;
        }
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
    std::vector<QueuedCommand> impulses;
    std::unordered_set<
        QueuedCommand, QueuedCommandHash, QueuedCommandEqual> wakes;
    {
        std::lock_guard<std::mutex> lock(impl_->command_mutex);
        teleports.swap(impl_->teleports);
        velocities.swap(impl_->velocities);
        impl_->force_drain_buffer.clear();
        impl_->force_drain_buffer.swap(impl_->forces);
        impulses.swap(impl_->impulses);
        wakes.swap(impl_->wakes);
    }
    impl_->last_command_trace.clear();
    impl_->last_command_trace.reserve(
        teleports.size() + velocities.size() +
        impl_->force_drain_buffer.size() +
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
    auto apply_command = [&](const QueuedCommand& command) {
        BridgeRecord* bridge = validate_queued_command(
            command, runtime_world, world, impl_->bridges);
        if (bridge == nullptr) {
            ++stats_.failed_commands;
            return;
        }
        switch (command.kind) {
            case PhysicsCommandKind::Teleport:
                b3Body_SetTransform(
                    bridge->body, box_position(command.primary),
                    box_quaternion(command.rotation));
                b3Body_SetAwake(bridge->body, true);
                update_query_proxy(impl_->query_tree, *bridge);
                break;
            case PhysicsCommandKind::Velocity:
                b3Body_SetLinearVelocity(
                    bridge->body, box_vector(command.primary));
                b3Body_SetAngularVelocity(
                    bridge->body, box_vector(command.secondary));
                break;
            case PhysicsCommandKind::Force:
                b3Body_ApplyForceToCenter(
                    bridge->body, box_vector(command.primary), true);
                break;
            case PhysicsCommandKind::ForceAtPoint:
                b3Body_ApplyForce(
                    bridge->body, box_vector(command.primary),
                    box_position(command.secondary), true);
                break;
            case PhysicsCommandKind::Impulse:
                b3Body_ApplyLinearImpulseToCenter(
                    bridge->body, box_vector(command.primary), true);
                break;
            case PhysicsCommandKind::Wake:
                b3Body_SetAwake(bridge->body, true);
                break;
        }
        impl_->last_command_trace.push_back(trace_entry(command));
    };

    for (const QueuedCommand& command : sorted_map_commands(teleports)) {
        apply_command(command);
    }
    for (const QueuedCommand& command : sorted_map_commands(velocities)) {
        apply_command(command);
    }
    for (const QueuedCommand& command : impl_->force_drain_buffer) {
        if (command.kind == PhysicsCommandKind::Force) {
            apply_command(command);
        }
    }
    for (std::size_t index = 0;
         index < impl_->force_drain_buffer.size();) {
        const QueuedCommand& command = impl_->force_drain_buffer[index];
        if (command.kind != PhysicsCommandKind::ForceAtPoint) {
            ++index;
            continue;
        }
        if (command.guarded_batch_id == 0) {
            apply_command(command);
            ++index;
            continue;
        }

        const std::size_t count = command.guarded_batch_count;
        bool structurally_valid = command.guarded_batch_index == 0 &&
            count != 0 && count <= impl_->force_drain_buffer.size() - index &&
            command.guard_owner && command.guard_begin != nullptr &&
            command.guard_end != nullptr;
        for (std::size_t offset = 0; structurally_valid && offset < count;
             ++offset) {
            const QueuedCommand& row =
                impl_->force_drain_buffer[index + offset];
            structurally_valid =
                row.kind == PhysicsCommandKind::ForceAtPoint &&
                row.originating_world == command.originating_world &&
                row.entity == command.entity &&
                row.guarded_batch_id == command.guarded_batch_id &&
                row.guarded_batch_count == count &&
                row.guarded_batch_index == offset;
        }
        BridgeRecord* bridge = structurally_valid
            ? validate_queued_command(
                  command, runtime_world, world, impl_->bridges)
            : nullptr;
        if (bridge == nullptr ||
            !command.guard_begin(command.guard_owner)) {
            ++stats_.failed_commands;
            index += structurally_valid ? count : 1;
            continue;
        }
        struct GuardScope {
            const QueuedCommand& command;
            ~GuardScope() noexcept {
                command.guard_end(command.guard_owner);
            }
        } guard{command};
        for (std::size_t offset = 0; offset < count; ++offset) {
            const QueuedCommand& row =
                impl_->force_drain_buffer[index + offset];
            b3Body_ApplyForce(
                bridge->body, box_vector(row.primary),
                box_position(row.secondary), true);
            impl_->last_command_trace.push_back(trace_entry(row));
            if (offset == 0 &&
                impl_->guarded_batch_post_first_row_hook != nullptr) {
                impl_->guarded_batch_post_first_row_hook(
                    impl_->guarded_batch_hook_context);
            }
        }
        index += count;
    }
    for (const QueuedCommand& command : impulses) {
        apply_command(command);
    }
    for (const QueuedCommand& command : sorted_wake_commands(wakes)) {
        apply_command(command);
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

bool PhysicsContext::move_character(
    const CharacterMoveInput& input, CharacterMoveOutput& output) {
    if (impl_ == nullptr ||
        std::this_thread::get_id() != impl_->owner_thread ||
        impl_->stepping || !world_is_valid() || !valid_character_input(input)) {
        return false;
    }

    const b3WorldId world = impl_->world_id;
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.maskBits = input.category_mask;
    const b3Capsule capsule{{0.0f, -input.half_segment, 0.0f},
                            {0.0f, input.half_segment, 0.0f}, input.radius};
    b3Pos position = box_position(input.position);
    b3Vec3 velocity = box_vector(input.velocity);
    const b3Vec3 gravity = box_vector(input.gravity);

    const float snap_reach = input.radius + input.step_height + 0.05f;
    const float support_reach = input.radius + snap_reach;
    const b3Vec3 down{0.0f, -support_reach, 0.0f};
    if (!character_query_vector(down) ||
        !character_query_vector({0.0f, 2.0f * input.half_segment, 0.0f}) ||
        !character_query_vector({input.radius, input.half_segment + input.radius,
                                 input.radius})) {
        return false;
    }
    auto probe_ground = [&](b3Pos at, StaticRayResult& result) {
        at.y -= input.half_segment;
        if (!character_position_range(at) ||
            !finite(engine_vector(b3Add(box_vector(engine_position(at)), down))))
            return false;
        b3World_CastRay(
            world, at, down, filter, static_ray_callback, &result);
        return result.valid;
    };

    const bool rising = velocity.y > 0.001f;
    StaticRayResult initial_ground{};
    if (!character_query_bounds(position, capsule, {}) ||
        !probe_ground(position, initial_ground)) return false;
    const bool standable = initial_ground.hit && !rising &&
        initial_ground.normal.y >= input.max_slope_cos;
    CharacterMoveOutput next{};
    next.ground_normal = {0.0f, 1.0f, 0.0f};
    if (standable) {
        next.ground_normal = engine_vector(initial_ground.normal);
        velocity.x = input.desired_horizontal_velocity.x;
        velocity.z = input.desired_horizontal_velocity.z;
        if (velocity.y < 0.0f) velocity.y = 0.0f;
    }

    velocity.x += gravity.x * input.dt;
    velocity.y += gravity.y * input.dt;
    velocity.z += gravity.z * input.dt;
    if (!finite(engine_vector(velocity))) return false;
    b3Vec3 move_velocity = velocity;
    if (standable) move_velocity.y = 0.0f;
    const b3Vec3 displacement{move_velocity.x * input.dt,
                              move_velocity.y * input.dt,
                              move_velocity.z * input.dt};
    if (!character_query_vector(displacement)) return false;
    b3Pos target = position;
    target.x += displacement.x;
    target.y += displacement.y;
    target.z += displacement.z;
    if (!character_position_range(target)) return false;

    constexpr int kPlaneCapacity = 32;
    b3CollisionPlane planes[kPlaneCapacity]{};
    int plane_count = 0;
    for (int iteration = 0; iteration < 5; ++iteration) {
        if (!character_query_bounds(position, capsule, {})) return false;
        plane_count = 0;
        StaticMoverPlaneGather gather{planes, &plane_count, kPlaneCapacity};
        b3World_CollideMover(
            world, position, &capsule, filter, static_mover_plane_callback,
            &gather);
        if (!gather.valid) return false;
        const b3Pos target_difference{target.x - position.x,
                                      target.y - position.y,
                                      target.z - position.z};
        if (!character_position_range(target_difference)) return false;
        const b3Vec3 target_delta{
            static_cast<float>(target_difference.x),
            static_cast<float>(target_difference.y),
            static_cast<float>(target_difference.z)};
        if (!character_query_vector(target_delta)) return false;
        b3Vec3 delta = b3SolvePlanes(target_delta, planes, plane_count).delta;
        if (!character_query_vector(delta) ||
            !character_query_bounds(position, capsule, delta)) return false;
        for (int index = 0; index < plane_count; ++index)
            if (!std::isfinite(planes[index].push)) return false;
        const float fraction = b3World_CastMover(
            world, position, &capsule, delta, filter, static_mover_filter,
            nullptr);
        if (!std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f)
            return false;
        delta.x *= fraction;
        delta.y *= fraction;
        delta.z *= fraction;
        position.x += delta.x;
        position.y += delta.y;
        position.z += delta.z;
        if (!character_position_range(position)) return false;
        if (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z <
            1.0e-4f) {
            break;
        }
    }
    if (plane_count > 0) {
        if (!character_query_vector(velocity)) return false;
        velocity = b3ClipVector(velocity, planes, plane_count);
        if (!finite(engine_vector(velocity))) return false;
    }

    if (!rising) {
        StaticRayResult ground{};
        if (!probe_ground(position, ground)) return false;
        if (ground.hit && ground.normal.y >= input.max_slope_cos) {
            constexpr float kSkin = 0.02f;
            const float normal_y = std::max(ground.normal.y, 0.5f);
            const float rest_y = static_cast<float>(ground.point.y) +
                input.radius / normal_y + input.half_segment + kSkin;
            const float delta_y = rest_y - static_cast<float>(position.y);
            if (!std::isfinite(rest_y) || !std::isfinite(delta_y)) return false;
            if (delta_y <= input.step_height &&
                delta_y >= -support_reach) {
                position.y = rest_y;
                if (velocity.y < 0.0f) velocity.y = 0.0f;
                next.grounded = true;
                next.ground_normal = engine_vector(ground.normal);
            }
        }
    }
    if (!character_position_range(position) || !finite(engine_vector(velocity)) ||
        !finite(next.ground_normal)) return false;
    next.position = engine_position(position);
    next.velocity = engine_vector(velocity);
    output = next;
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

void PhysicsContext::set_guarded_batch_post_first_row_hook_for_test(
    void* context, GuardedBatchPostFirstRowHook hook) noexcept {
    if (impl_ == nullptr) return;
    impl_->guarded_batch_hook_context = context;
    impl_->guarded_batch_post_first_row_hook = hook;
}

bool PhysicsContext::terrain_collision_tile_state_for_test(
    std::size_t index,
    TerrainCollisionPhysicsTileState& state) const noexcept {
    state = {};
    if (impl_ == nullptr || impl_->terrain_collision == nullptr ||
        index >= impl_->terrain_collision->tiles.size()) {
        return false;
    }
    const TerrainCollisionTileRuntime& tile =
        impl_->terrain_collision->tiles[index];
    if (!b3Body_IsValid(tile.body) || !b3Shape_IsValid(tile.shape) ||
        tile.mesh_data == nullptr || tile.mesh_data->byteCount <= 0) {
        return false;
    }
    const b3Filter filter = b3Shape_GetFilter(tile.shape);
    state.coordinate_x = tile.coordinate.x;
    state.coordinate_y = tile.coordinate.y;
    state.coordinate_z = tile.coordinate.z;
    state.tile_key = tile.tile_key;
    state.body_handle = b3StoreBodyId(tile.body);
    state.shape_handle = b3StoreShapeId(tile.shape);
    state.retained_bytes =
        static_cast<std::uint64_t>(tile.mesh_data->byteCount);
    state.origin_m = tile.origin_m;
    state.friction = b3Shape_GetFriction(tile.shape);
    state.restitution = b3Shape_GetRestitution(tile.shape);
    state.category_bits = filter.categoryBits;
    state.mask_bits = filter.maskBits;
    state.group_index = filter.groupIndex;
    state.body_is_static = b3Body_GetType(tile.body) == b3_staticBody;
    state.body_user_data_is_null = b3Body_GetUserData(tile.body) == nullptr;
    state.shape_user_data_is_null = b3Shape_GetUserData(tile.shape) == nullptr;
    return true;
}

bool PhysicsContext::terrain_collision_handles_are_valid_for_test(
    std::uint64_t body_handle,
    std::uint64_t shape_handle) const noexcept {
    return body_handle != 0 && shape_handle != 0 &&
           b3Body_IsValid(b3LoadBodyId(body_handle)) &&
           b3Shape_IsValid(b3LoadShapeId(shape_handle));
}

TerrainCollisionPhysicsWorldState
PhysicsContext::terrain_collision_world_state_for_test() const noexcept {
    TerrainCollisionPhysicsWorldState state{};
    if (impl_ == nullptr || !b3World_IsValid(impl_->world_id)) {
        return state;
    }
    const b3Counters counters = b3World_GetCounters(impl_->world_id);
    state.body_count = counters.bodyCount > 0
        ? static_cast<std::uint32_t>(counters.bodyCount) : 0;
    state.shape_count = counters.shapeCount > 0
        ? static_cast<std::uint32_t>(counters.shapeCount) : 0;
    return state;
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

bool physics_apply_force_at_world_point(
    flecs::entity entity,
    Float3 force,
    Float3 world_point) {
    CommandTarget target;
    return resolve_command_target(entity, target) &&
           target.context->enqueue_force_at_world_point(
               target.originating_world, target.entity, force, world_point);
}

namespace detail {

bool physics_apply_guarded_force_at_world_points(
    flecs::entity entity,
    const GuardedForceAtWorldPoint* rows,
    std::size_t count,
    const std::shared_ptr<const void>& guard_owner,
    PhysicsCommandGuardBegin guard_begin,
    PhysicsCommandGuardEnd guard_end) noexcept {
    CommandTarget target;
    return resolve_command_target(entity, target) &&
           target.context->enqueue_guarded_force_at_world_points(
               target.originating_world, target.entity, rows, count,
               guard_owner, guard_begin, guard_end);
}

} // namespace detail

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

bool physics_move_character(
    flecs::world& world,
    const CharacterMoveInput& input,
    CharacterMoveOutput& output) {
    const flecs::world_t* real_world = ecs_get_world(world.c_ptr());
    if (real_world == nullptr) {
        return false;
    }
    flecs::world normalized_world(const_cast<flecs::world_t*>(real_world));
    const detail::PhysicsContextRef* ref =
        normalized_world.try_get<detail::PhysicsContextRef>();
    return ref != nullptr && ref->value != nullptr &&
           ref->value->move_character(input, output);
}

} // namespace matter::physics
