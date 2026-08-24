#include "authored_fluid_colliders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace hydrology {
namespace {

constexpr std::uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);
constexpr float kPi = 3.14159265358979323846f;

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
}

void hash_string(std::uint64_t& hash, const std::string& value) {
    const std::uint64_t size = static_cast<std::uint64_t>(value.size());
    hash_bytes(hash, &size, sizeof(size));
    hash_bytes(hash, value.data(), value.size());
}

bool finite(float value) { return std::isfinite(value); }

bool finite(matter::Float3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(const matter::Mat4f& value) {
    return std::all_of(std::begin(value.m), std::end(value.m),
                       [](float component) { return finite(component); });
}

bool affine(const matter::Mat4f& value) {
    return value.m[12] == 0.0f && value.m[13] == 0.0f &&
           value.m[14] == 0.0f && value.m[15] == 1.0f;
}

float determinant3(const matter::Mat4f& value) {
    return value.m[0] * (value.m[5] * value.m[10] -
                         value.m[6] * value.m[9]) -
           value.m[1] * (value.m[4] * value.m[10] -
                         value.m[6] * value.m[8]) +
           value.m[2] * (value.m[4] * value.m[9] -
                         value.m[5] * value.m[8]);
}

matter::Float3 transform_point(const matter::Mat4f& transform,
                              matter::Float3 point) {
    return {
        transform.m[0] * point.x + transform.m[1] * point.y +
            transform.m[2] * point.z + transform.m[3],
        transform.m[4] * point.x + transform.m[5] * point.y +
            transform.m[6] * point.z + transform.m[7],
        transform.m[8] * point.x + transform.m[9] * point.y +
            transform.m[10] * point.z + transform.m[11],
    };
}

matter::Float3 add(matter::Float3 a, matter::Float3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

bool valid_bounds(const matter::Aabb& bounds) {
    return finite(bounds.minimum) && finite(bounds.maximum) &&
           bounds.minimum.x <= bounds.maximum.x &&
           bounds.minimum.y <= bounds.maximum.y &&
           bounds.minimum.z <= bounds.maximum.z;
}

bool intersects(const matter::Aabb& a, const matter::Aabb& b) {
    return a.minimum.x <= b.maximum.x && a.maximum.x >= b.minimum.x &&
           a.minimum.y <= b.maximum.y && a.maximum.y >= b.minimum.y &&
           a.minimum.z <= b.maximum.z && a.maximum.z >= b.minimum.z;
}

matter::Aabb mesh_bounds(const FluidCollisionMesh& mesh) {
    matter::Aabb bounds{};
    bounds.minimum = {std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity()};
    bounds.maximum = {-std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()};
    for (const auto vertex : mesh.vertices) {
        bounds.minimum.x = std::min(bounds.minimum.x, vertex.x);
        bounds.minimum.y = std::min(bounds.minimum.y, vertex.y);
        bounds.minimum.z = std::min(bounds.minimum.z, vertex.z);
        bounds.maximum.x = std::max(bounds.maximum.x, vertex.x);
        bounds.maximum.y = std::max(bounds.maximum.y, vertex.y);
        bounds.maximum.z = std::max(bounds.maximum.z, vertex.z);
    }
    return bounds;
}

FluidCollisionMesh sphere_mesh(float radius) {
    constexpr std::uint32_t kSectors = 12u;
    constexpr std::uint32_t kStacks = 6u;
    FluidCollisionMesh mesh{};
    mesh.vertices.push_back({0.0f, radius, 0.0f});
    for (std::uint32_t stack = 1u; stack < kStacks; ++stack) {
        const float latitude = kPi * static_cast<float>(stack) /
                               static_cast<float>(kStacks);
        const float y = radius * std::cos(latitude);
        const float ring = radius * std::sin(latitude);
        for (std::uint32_t sector = 0u; sector < kSectors; ++sector) {
            const float longitude = 2.0f * kPi * static_cast<float>(sector) /
                                    static_cast<float>(kSectors);
            mesh.vertices.push_back(
                {ring * std::cos(longitude), y, ring * std::sin(longitude)});
        }
    }
    const std::uint32_t bottom =
        static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({0.0f, -radius, 0.0f});
    for (std::uint32_t sector = 0u; sector < kSectors; ++sector) {
        const std::uint32_t next = (sector + 1u) % kSectors;
        mesh.indices.insert(mesh.indices.end(), {0u, 1u + next, 1u + sector});
    }
    for (std::uint32_t stack = 0u; stack + 2u < kStacks; ++stack) {
        const std::uint32_t first = 1u + stack * kSectors;
        const std::uint32_t next_ring = first + kSectors;
        for (std::uint32_t sector = 0u; sector < kSectors; ++sector) {
            const std::uint32_t next = (sector + 1u) % kSectors;
            mesh.indices.insert(mesh.indices.end(),
                                {first + sector, first + next,
                                 next_ring + sector,
                                 first + next, next_ring + next,
                                 next_ring + sector});
        }
    }
    const std::uint32_t last_ring = bottom - kSectors;
    for (std::uint32_t sector = 0u; sector < kSectors; ++sector) {
        const std::uint32_t next = (sector + 1u) % kSectors;
        mesh.indices.insert(mesh.indices.end(),
                            {last_ring + sector, last_ring + next, bottom});
    }
    return mesh;
}

FluidCollisionMesh box_mesh(matter::Float3 half) {
    FluidCollisionMesh mesh{};
    mesh.vertices = {
        {-half.x, -half.y, -half.z}, {half.x, -half.y, -half.z},
        {half.x, half.y, -half.z}, {-half.x, half.y, -half.z},
        {-half.x, -half.y, half.z}, {half.x, -half.y, half.z},
        {half.x, half.y, half.z}, {-half.x, half.y, half.z},
    };
    mesh.indices = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
        0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5,
    };
    return mesh;
}

bool validate(const AuthoredFluidCollider& collider, FluidBakeError& error) {
    if (collider.id.empty()) {
        error = {FluidBakeCode::InvalidInput,
                 "authored fluid collider id must not be empty"};
        return false;
    }
    if (!finite(collider.object_to_world) || !affine(collider.object_to_world) ||
        !finite(determinant3(collider.object_to_world)) ||
        std::fabs(determinant3(collider.object_to_world)) <= 1.0e-8f) {
        error = {FluidBakeCode::InvalidInput,
                 "authored fluid collider transform must be finite, affine, and nonsingular"};
        return false;
    }
    if (!finite(collider.shape.center_m)) {
        error = {FluidBakeCode::InvalidInput,
                 "authored fluid collider center must be finite"};
        return false;
    }
    switch (collider.shape.shape) {
        case matter::WorldFluidColliderShape::Sphere:
            if (!finite(collider.shape.radius_m) ||
                collider.shape.radius_m <= 0.0f) {
                error = {FluidBakeCode::InvalidInput,
                         "authored sphere radius must be finite and positive"};
                return false;
            }
            break;
        case matter::WorldFluidColliderShape::Box:
            if (!finite(collider.shape.half_extents_m) ||
                collider.shape.half_extents_m.x <= 0.0f ||
                collider.shape.half_extents_m.y <= 0.0f ||
                collider.shape.half_extents_m.z <= 0.0f) {
                error = {FluidBakeCode::InvalidInput,
                         "authored box half-extents must be finite and positive"};
                return false;
            }
            break;
        default:
            error = {FluidBakeCode::InvalidInput,
                     "authored fluid collider shape is unsupported"};
            return false;
    }
    return true;
}

FluidCollisionSurface build_surface(const AuthoredFluidCollider& collider) {
    FluidCollisionSurface surface{};
    surface.kind = FluidCollisionSurfaceKind::Boulder;
    surface.mesh = collider.shape.shape == matter::WorldFluidColliderShape::Sphere
                       ? sphere_mesh(collider.shape.radius_m)
                       : box_mesh(collider.shape.half_extents_m);
    for (auto& vertex : surface.mesh.vertices) {
        vertex = transform_point(collider.object_to_world,
                                 add(vertex, collider.shape.center_m));
    }
    surface.local_to_world = {{1.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f, 0.0f,
                               0.0f, 0.0f, 1.0f, 0.0f,
                               0.0f, 0.0f, 0.0f, 1.0f}};
    return surface;
}

}  // namespace

std::uint64_t authored_fluid_collider_revision(
    const AuthoredFluidCollider& collider) {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, collider.id);
    const auto shape = static_cast<std::uint8_t>(collider.shape.shape);
    hash_bytes(hash, &shape, sizeof(shape));
    hash_bytes(hash, collider.object_to_world.m,
               sizeof(collider.object_to_world.m));
    hash_bytes(hash, &collider.shape.radius_m,
               sizeof(collider.shape.radius_m));
    hash_bytes(hash, &collider.shape.half_extents_m,
               sizeof(collider.shape.half_extents_m));
    hash_bytes(hash, &collider.shape.center_m,
               sizeof(collider.shape.center_m));
    return hash;
}

bool build_authored_fluid_collision_surfaces(
    const std::vector<AuthoredFluidCollider>& colliders,
    const matter::Aabb& selection_bounds_m,
    std::vector<FluidCollisionSurface>& surfaces,
    std::uint64_t& revision,
    FluidBakeError& error) {
    surfaces.clear();
    revision = 0;
    error = {};
    if (!valid_bounds(selection_bounds_m)) {
        error = {FluidBakeCode::InvalidInput,
                 "authored collider selection bounds are invalid"};
        return false;
    }
    std::vector<const AuthoredFluidCollider*> ordered;
    ordered.reserve(colliders.size());
    for (const auto& collider : colliders) ordered.push_back(&collider);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto* a, const auto* b) { return a->id < b->id; });
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        if (index != 0u && ordered[index - 1u]->id == ordered[index]->id) {
            error = {FluidBakeCode::InvalidInput,
                     "authored fluid collider ids must be unique"};
            surfaces.clear();
            return false;
        }
        if (!validate(*ordered[index], error)) {
            surfaces.clear();
            return false;
        }
    }

    std::uint64_t hash = kFnvOffset;
    for (const auto* collider : ordered) {
        FluidCollisionSurface surface = build_surface(*collider);
        if (!intersects(mesh_bounds(surface.mesh), selection_bounds_m)) continue;
        const std::uint64_t collider_revision =
            authored_fluid_collider_revision(*collider);
        hash_bytes(hash, &collider_revision, sizeof(collider_revision));
        surfaces.push_back(std::move(surface));
    }
    const std::uint64_t count = static_cast<std::uint64_t>(surfaces.size());
    hash_bytes(hash, &count, sizeof(count));
    revision = hash;
    return true;
}

}  // namespace hydrology
