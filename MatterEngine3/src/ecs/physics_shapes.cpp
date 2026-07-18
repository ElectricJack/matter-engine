#include "physics_shapes.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace matter::physics::detail {
namespace {

constexpr uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);

bool finite(float value) {
    return std::isfinite(value);
}

bool finite(Float3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool normalize(Quaternion& value) {
    if (!finite(value.x) || !finite(value.y) || !finite(value.z) ||
        !finite(value.w)) {
        return false;
    }
    const float length_squared = value.x * value.x + value.y * value.y +
                                 value.z * value.z + value.w * value.w;
    if (!finite(length_squared) || length_squared <= 0.0f) {
        return false;
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    value.w *= inverse_length;
    return finite(value.x) && finite(value.y) && finite(value.z) &&
           finite(value.w);
}

bool unit_scale(Float3 scale) {
    constexpr float tolerance = 1.0e-5f;
    return finite(scale) && std::fabs(scale.x - 1.0f) <= tolerance &&
           std::fabs(scale.y - 1.0f) <= tolerance &&
           std::fabs(scale.z - 1.0f) <= tolerance;
}

bool valid_body(const RigidBody& body) {
    const bool known_type = body.type == RigidBodyType::Static ||
                            body.type == RigidBodyType::Kinematic ||
                            body.type == RigidBodyType::Dynamic;
    return known_type && finite(body.linear_damping) &&
           body.linear_damping >= 0.0f && finite(body.angular_damping) &&
           body.angular_damping >= 0.0f && finite(body.gravity_scale) &&
           finite(body.sleep_threshold) && body.sleep_threshold >= 0.0f;
}

bool valid_properties(
    const ColliderProperties& properties,
    RigidBodyType body_type) {
    const bool density_valid = finite(properties.density) &&
        (body_type == RigidBodyType::Dynamic
             ? properties.density > 0.0f
             : properties.density >= 0.0f);
    return density_valid && finite(properties.friction) &&
           properties.friction >= 0.0f && finite(properties.restitution) &&
           properties.restitution >= 0.0f;
}

Float3 subtract(Float3 first, Float3 second) {
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Float3 cross(Float3 first, Float3 second) {
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

float dot(Float3 first, Float3 second) {
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

bool has_non_coplanar_tetrahedron(const ConvexHullCollider& hull) {
    for (uint32_t a = 0; a + 3 < hull.point_count; ++a) {
        for (uint32_t b = a + 1; b + 2 < hull.point_count; ++b) {
            for (uint32_t c = b + 1; c + 1 < hull.point_count; ++c) {
                const Float3 normal = cross(
                    subtract(hull.points[b], hull.points[a]),
                    subtract(hull.points[c], hull.points[a]));
                for (uint32_t d = c + 1; d < hull.point_count; ++d) {
                    const float volume =
                        dot(normal, subtract(hull.points[d], hull.points[a]));
                    if (finite(volume) && volume != 0.0f) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

struct HullDeleter {
    void operator()(b3HullData* hull) const {
        if (hull != nullptr) {
            b3DestroyHull(hull);
        }
    }
};

void hash_bytes(uint64_t& hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
}

template <typename Value>
void hash_value(uint64_t& hash, const Value& value) {
    hash_bytes(hash, &value, sizeof(value));
}

void hash_float3(uint64_t& hash, Float3 value) {
    hash_value(hash, value.x);
    hash_value(hash, value.y);
    hash_value(hash, value.z);
}

void hash_quaternion(uint64_t& hash, Quaternion value) {
    hash_value(hash, value.x);
    hash_value(hash, value.y);
    hash_value(hash, value.z);
    hash_value(hash, value.w);
}

void hash_properties(uint64_t& hash, const ColliderProperties& value) {
    hash_value(hash, value.density);
    hash_value(hash, value.friction);
    hash_value(hash, value.restitution);
    hash_value(hash, value.category_bits);
    hash_value(hash, value.mask_bits);
    hash_value(hash, value.sensor);
    hash_value(hash, value.contact_events);
    hash_value(hash, value.hit_events);
}

uint64_t configuration_hash(const DesiredBody& desired) {
    uint64_t hash = kFnvOffset;
    hash_value(hash, desired.body.type);
    hash_value(hash, desired.body.linear_damping);
    hash_value(hash, desired.body.angular_damping);
    hash_value(hash, desired.body.gravity_scale);
    hash_value(hash, desired.body.sleep_threshold);
    hash_value(hash, desired.body.enable_sleep);
    hash_value(hash, desired.body.continuous);
    hash_value(hash, desired.shape_kind);
    switch (desired.shape_kind) {
        case DesiredShapeKind::Sphere:
            hash_properties(hash, desired.sphere.properties);
            hash_float3(hash, desired.sphere.center);
            hash_value(hash, desired.sphere.radius);
            break;
        case DesiredShapeKind::Capsule:
            hash_properties(hash, desired.capsule.properties);
            hash_float3(hash, desired.capsule.point_a);
            hash_float3(hash, desired.capsule.point_b);
            hash_value(hash, desired.capsule.radius);
            break;
        case DesiredShapeKind::Box:
            hash_properties(hash, desired.box.properties);
            hash_float3(hash, desired.box.center);
            hash_quaternion(hash, desired.box.rotation);
            hash_float3(hash, desired.box.half_extents);
            break;
        case DesiredShapeKind::Hull:
            hash_properties(hash, desired.hull.properties);
            hash_value(hash, desired.hull.point_count);
            for (uint32_t index = 0; index < desired.hull.point_count; ++index) {
                hash_float3(hash, desired.hull.points[index]);
            }
            break;
    }
    return hash;
}

const ColliderProperties& properties(const DesiredBody& desired) {
    switch (desired.shape_kind) {
        case DesiredShapeKind::Sphere: return desired.sphere.properties;
        case DesiredShapeKind::Capsule: return desired.capsule.properties;
        case DesiredShapeKind::Box: return desired.box.properties;
        case DesiredShapeKind::Hull: return desired.hull.properties;
    }
    return desired.sphere.properties;
}

b3Vec3 box_vector(Float3 value) {
    return {value.x, value.y, value.z};
}

b3Quat box_quaternion(Quaternion value) {
    return {{value.x, value.y, value.z}, value.w};
}

} // namespace

ValidationResult validate_desired_body(flecs::entity entity) {
    ValidationResult result{};

    const ecs::LocalTransform* transform = entity.try_get<ecs::LocalTransform>();
    if (transform == nullptr) {
        result.error = PhysicsErrorCode::MissingTransform;
        return result;
    }
    result.desired.transform = *transform;
    if (entity.target(flecs::ChildOf).id() != 0) {
        result.error = PhysicsErrorCode::HasParent;
        return result;
    }
    if (!unit_scale(result.desired.transform.scale)) {
        result.error = PhysicsErrorCode::NonUnitScale;
        return result;
    }

    const int collider_count =
        static_cast<int>(entity.has<SphereCollider>()) +
        static_cast<int>(entity.has<CapsuleCollider>()) +
        static_cast<int>(entity.has<BoxCollider>()) +
        static_cast<int>(entity.has<ConvexHullCollider>());
    if (collider_count == 0) {
        result.error = PhysicsErrorCode::MissingCollider;
        return result;
    }
    if (collider_count != 1) {
        result.error = PhysicsErrorCode::MultipleColliders;
        return result;
    }

    const RigidBody* body = entity.try_get<RigidBody>();
    if (body == nullptr) {
        result.error = PhysicsErrorCode::InvalidBody;
        return result;
    }
    result.desired.body = *body;
    if (!valid_body(result.desired.body) ||
        !finite(result.desired.transform.translation) ||
        !normalize(result.desired.transform.rotation)) {
        result.error = PhysicsErrorCode::InvalidBody;
        return result;
    }
    if (const PhysicsVelocity* velocity = entity.try_get<PhysicsVelocity>()) {
        result.desired.velocity = *velocity;
        result.desired.has_velocity = true;
        if (!finite(velocity->linear) || !finite(velocity->angular)) {
            result.error = PhysicsErrorCode::InvalidBody;
            return result;
        }
    }

    const ColliderProperties* collider_properties = nullptr;
    if (const SphereCollider* sphere = entity.try_get<SphereCollider>()) {
        result.desired.shape_kind = DesiredShapeKind::Sphere;
        result.desired.sphere = *sphere;
        collider_properties = &result.desired.sphere.properties;
        if (!finite(sphere->center) || !finite(sphere->radius) ||
            sphere->radius <= 0.0f) {
            result.error = PhysicsErrorCode::InvalidCollider;
            return result;
        }
    } else if (const CapsuleCollider* capsule =
                   entity.try_get<CapsuleCollider>()) {
        result.desired.shape_kind = DesiredShapeKind::Capsule;
        result.desired.capsule = *capsule;
        collider_properties = &result.desired.capsule.properties;
        if (!finite(capsule->point_a) || !finite(capsule->point_b) ||
            !finite(capsule->radius) || capsule->radius <= 0.0f) {
            result.error = PhysicsErrorCode::InvalidCollider;
            return result;
        }
    } else if (const BoxCollider* box = entity.try_get<BoxCollider>()) {
        result.desired.shape_kind = DesiredShapeKind::Box;
        result.desired.box = *box;
        collider_properties = &result.desired.box.properties;
        if (!finite(box->center) || !finite(box->half_extents) ||
            box->half_extents.x <= 0.0f || box->half_extents.y <= 0.0f ||
            box->half_extents.z <= 0.0f ||
            !normalize(result.desired.box.rotation)) {
            result.error = PhysicsErrorCode::InvalidCollider;
            return result;
        }
    } else {
        const ConvexHullCollider* hull =
            entity.try_get<ConvexHullCollider>();
        result.desired.shape_kind = DesiredShapeKind::Hull;
        result.desired.hull = *hull;
        collider_properties = &result.desired.hull.properties;
        if (hull->point_count < 4 || hull->point_count > 32) {
            result.error = PhysicsErrorCode::InvalidCollider;
            return result;
        }
        for (uint32_t index = 0; index < hull->point_count; ++index) {
            if (!finite(hull->points[index])) {
                result.error = PhysicsErrorCode::InvalidCollider;
                return result;
            }
        }
        if (!has_non_coplanar_tetrahedron(*hull)) {
            result.error = PhysicsErrorCode::InvalidCollider;
            return result;
        }
        b3Vec3 points[32]{};
        for (uint32_t index = 0; index < hull->point_count; ++index) {
            points[index] = box_vector(hull->points[index]);
        }
        std::unique_ptr<b3HullData, HullDeleter> validated(
            b3CreateHull(points, static_cast<int>(hull->point_count), 32));
        if (validated == nullptr) {
            result.error = PhysicsErrorCode::HullBuildFailed;
            return result;
        }
    }

    if (!valid_properties(*collider_properties, result.desired.body.type)) {
        result.error = PhysicsErrorCode::InvalidCollider;
        return result;
    }
    result.desired.configuration_hash = configuration_hash(result.desired);
    return result;
}

b3ShapeId create_shape(
    b3BodyId body,
    const DesiredBody& desired,
    b3HullData*& temporary_hull) {
    temporary_hull = nullptr;
    const ColliderProperties& collider = properties(desired);
    b3ShapeDef definition = b3DefaultShapeDef();
    definition.baseMaterial.friction = collider.friction;
    definition.baseMaterial.restitution = collider.restitution;
    definition.density = collider.density;
    definition.filter.categoryBits = collider.category_bits;
    definition.filter.maskBits = collider.mask_bits;
    definition.isSensor = collider.sensor;
    definition.enableSensorEvents = true;
    definition.enableContactEvents = collider.contact_events;
    definition.enableHitEvents = collider.hit_events;

    switch (desired.shape_kind) {
        case DesiredShapeKind::Sphere: {
            const b3Sphere sphere{
                box_vector(desired.sphere.center), desired.sphere.radius};
            return b3CreateSphereShape(body, &definition, &sphere);
        }
        case DesiredShapeKind::Capsule: {
            const b3Capsule capsule{
                box_vector(desired.capsule.point_a),
                box_vector(desired.capsule.point_b),
                desired.capsule.radius};
            return b3CreateCapsuleShape(body, &definition, &capsule);
        }
        case DesiredShapeKind::Box: {
            const b3Transform transform{
                box_vector(desired.box.center),
                box_quaternion(desired.box.rotation)};
            const b3BoxHull box = b3MakeTransformedBoxHull(
                desired.box.half_extents.x,
                desired.box.half_extents.y,
                desired.box.half_extents.z,
                transform);
            return b3CreateHullShape(body, &definition, &box.base);
        }
        case DesiredShapeKind::Hull: {
            b3Vec3 points[32]{};
            for (uint32_t index = 0; index < desired.hull.point_count; ++index) {
                points[index] = box_vector(desired.hull.points[index]);
            }
            temporary_hull = b3CreateHull(
                points, static_cast<int>(desired.hull.point_count), 32);
            if (temporary_hull == nullptr) {
                return b3_nullShapeId;
            }
            return b3CreateHullShape(body, &definition, temporary_hull);
        }
    }
    return b3_nullShapeId;
}

} // namespace matter::physics::detail
