#include "hydrology/physx_collision_input.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace hydrology {
namespace {

constexpr float kDegenerateAreaSquared = 1.0e-16f;
constexpr float kAffineTolerance = 1.0e-6f;

bool finite(matter::Float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finite(const matter::Mat4f& value) {
    for (float component : value.m) {
        if (!std::isfinite(component)) return false;
    }
    return true;
}

bool valid_bounds(const matter::Aabb& bounds) {
    return finite(bounds.minimum) && finite(bounds.maximum) &&
           bounds.minimum.x <= bounds.maximum.x &&
           bounds.minimum.y <= bounds.maximum.y &&
           bounds.minimum.z <= bounds.maximum.z;
}

bool affine(const matter::Mat4f& value) {
    return std::fabs(value.m[12]) <= kAffineTolerance &&
           std::fabs(value.m[13]) <= kAffineTolerance &&
           std::fabs(value.m[14]) <= kAffineTolerance &&
           std::fabs(value.m[15] - 1.0f) <= kAffineTolerance;
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

matter::Float3 subtract(matter::Float3 left, matter::Float3 right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

matter::Float3 cross(matter::Float3 left, matter::Float3 right) {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

float length_squared(matter::Float3 value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

bool valid_kind(FluidCollisionSurfaceKind kind) {
    switch (kind) {
        case FluidCollisionSurfaceKind::Terrain:
        case FluidCollisionSurfaceKind::Boulder:
        case FluidCollisionSurfaceKind::InletBacking:
        case FluidCollisionSurfaceKind::VirtualDam:
            return true;
    }
    return false;
}

std::uint32_t float_bits(float value) {
    if (value == 0.0f) value = 0.0f;  // Canonicalize negative zero.
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float key width mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct VertexKey {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;

    bool operator==(const VertexKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& key) const {
        std::size_t hash = static_cast<std::size_t>(key.x);
        hash ^= static_cast<std::size_t>(key.y) + 0x9e3779b9u +
                (hash << 6u) + (hash >> 2u);
        hash ^= static_cast<std::size_t>(key.z) + 0x9e3779b9u +
                (hash << 6u) + (hash >> 2u);
        return hash;
    }
};

VertexKey vertex_key(matter::Float3 vertex) {
    return {float_bits(vertex.x), float_bits(vertex.y), float_bits(vertex.z)};
}

bool fail(FluidCollisionBuildOutput& output, FluidBakeError& error,
          std::string message) {
    output = {};
    error = {FluidBakeCode::InvalidInput, std::move(message)};
    return false;
}

}  // namespace

bool build_physx_collision_input(const FluidCollisionBuildInput& input,
                                 FluidCollisionBuildOutput& output,
                                 FluidBakeError& error) {
    output = {};
    error = {};

    if (input.surfaces.empty()) {
        return fail(output, error,
                    "PhysX collision input has no authored surfaces");
    }
    if (!valid_bounds(input.section_bounds_m) ||
        !std::isfinite(input.dry_margin_m) || input.dry_margin_m < 0.0f) {
        return fail(output, error,
                    "PhysX collision section bounds or dry margin are invalid");
    }

    FluidCollisionBuildOutput candidate{};
    candidate.dry_collar_bounds_m = {
        {input.section_bounds_m.minimum.x - input.dry_margin_m,
         input.section_bounds_m.minimum.y - input.dry_margin_m,
         input.section_bounds_m.minimum.z - input.dry_margin_m},
        {input.section_bounds_m.maximum.x + input.dry_margin_m,
         input.section_bounds_m.maximum.y + input.dry_margin_m,
         input.section_bounds_m.maximum.z + input.dry_margin_m},
    };
    if (!valid_bounds(candidate.dry_collar_bounds_m)) {
        return fail(output, error,
                    "PhysX collision dry collar overflowed finite bounds");
    }

    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> vertex_lookup;
    for (std::size_t surface_index = 0;
         surface_index < input.surfaces.size(); ++surface_index) {
        const FluidCollisionSurface& surface = input.surfaces[surface_index];
        std::ostringstream context;
        context << "PhysX collision surface " << surface_index;
        if (!valid_kind(surface.kind)) {
            return fail(output, error, context.str() + " has an invalid tag");
        }
        if (!finite(surface.local_to_world) ||
            !affine(surface.local_to_world)) {
            return fail(output, error,
                        context.str() + " has a non-finite or non-affine transform");
        }
        if (surface.mesh.vertices.empty() || surface.mesh.indices.empty() ||
            surface.mesh.indices.size() % 3u != 0u) {
            return fail(output, error,
                        context.str() + " is not a complete triangle mesh");
        }

        std::vector<matter::Float3> world_vertices;
        world_vertices.reserve(surface.mesh.vertices.size());
        for (matter::Float3 vertex : surface.mesh.vertices) {
            if (!finite(vertex)) {
                return fail(output, error,
                            context.str() + " contains a non-finite vertex");
            }
            const matter::Float3 world =
                transform_point(surface.local_to_world, vertex);
            if (!finite(world)) {
                return fail(output, error,
                            context.str() + " transform produced a non-finite vertex");
            }
            world_vertices.push_back(world);
        }

        const std::size_t first_index = candidate.mesh.indices.size();
        if (first_index > std::numeric_limits<std::uint32_t>::max() ||
            surface.mesh.indices.size() >
                std::numeric_limits<std::uint32_t>::max() - first_index) {
            return fail(output, error,
                        "PhysX collision index count exceeds 32-bit capacity");
        }

        for (std::size_t triangle = 0;
             triangle < surface.mesh.indices.size(); triangle += 3u) {
            const std::uint32_t local_indices[3] = {
                surface.mesh.indices[triangle],
                surface.mesh.indices[triangle + 1u],
                surface.mesh.indices[triangle + 2u],
            };
            for (std::uint32_t index : local_indices) {
                if (index >= world_vertices.size()) {
                    return fail(output, error,
                                context.str() + " contains an out-of-range index");
                }
            }

            const matter::Float3 edge_a = subtract(
                world_vertices[local_indices[1]], world_vertices[local_indices[0]]);
            const matter::Float3 edge_b = subtract(
                world_vertices[local_indices[2]], world_vertices[local_indices[0]]);
            const float area_squared = length_squared(cross(edge_a, edge_b));
            if (!std::isfinite(area_squared) ||
                !(area_squared > kDegenerateAreaSquared)) {
                return fail(output, error,
                            context.str() + " contains a degenerate triangle");
            }

            for (std::uint32_t local_index : local_indices) {
                const matter::Float3 vertex = world_vertices[local_index];
                const VertexKey key = vertex_key(vertex);
                auto found = vertex_lookup.find(key);
                if (found == vertex_lookup.end()) {
                    if (candidate.mesh.vertices.size() >=
                        std::numeric_limits<std::uint32_t>::max()) {
                        return fail(output, error,
                                    "PhysX collision vertex count exceeds 32-bit capacity");
                    }
                    const auto next_index = static_cast<std::uint32_t>(
                        candidate.mesh.vertices.size());
                    candidate.mesh.vertices.push_back(vertex);
                    found = vertex_lookup.emplace(key, next_index).first;
                }
                candidate.mesh.indices.push_back(found->second);
            }
        }

        candidate.ranges.push_back({
            surface.kind,
            static_cast<std::uint32_t>(first_index),
            static_cast<std::uint32_t>(surface.mesh.indices.size()),
        });
    }

    const std::size_t triangle_count = candidate.mesh.indices.size() / 3u;
    if (triangle_count > std::numeric_limits<std::uint32_t>::max()) {
        return fail(output, error,
                    "PhysX collision triangle count exceeds 32-bit capacity");
    }
    candidate.authored_triangle_count =
        static_cast<std::uint32_t>(triangle_count);
    output = std::move(candidate);
    return true;
}

}  // namespace hydrology
