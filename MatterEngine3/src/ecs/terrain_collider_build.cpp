#include "terrain_collider_build.h"

#include <algorithm>
#include <unordered_map>

namespace matter::terrain_collider {
namespace {

uint64_t edge_key(uint32_t a, uint32_t b) {
    const uint32_t lo = std::min(a, b);
    const uint32_t hi = std::max(a, b);
    return (static_cast<uint64_t>(lo) << 32) | static_cast<uint64_t>(hi);
}

}  // namespace

MeshColliderData build_mesh_collider(
    const viewer::IndexedPartGeometry& geom,
    bool volumetric,
    int64_t tx, int64_t ty, int64_t tz,
    float sector_size,
    float skirt_depth,
    float friction) {
    MeshColliderData out;
    out.header.friction = friction;
    // Mode-correct world offset: flat-mode y is already world-absolute in the
    // vertices, so only x/z shift; volumetric-mode y is tile-local and shifts too.
    out.header.translation = {
        static_cast<float>(tx) * sector_size,
        volumetric ? static_cast<float>(ty) * sector_size : 0.0f,
        static_cast<float>(tz) * sector_size};

    const size_t index_count = geom.indices.size();
    if (geom.vertex_count < 3 || index_count < 3) {
        return out;  // degenerate; the attach call will reject a null-span desc
    }

    out.vertices = geom.vertices;  // tile-local, copied verbatim
    out.indices = geom.indices;

    if (skirt_depth <= 0.0f) {
        return out;
    }

    // A boundary edge belongs to exactly one triangle; extrude each one downward
    // into a two-triangle curtain so a capsule cannot fall through the ~1-voxel
    // gap left at a cross-level tile border (lod_mesh_data is unwelded).
    std::unordered_map<uint64_t, int> edge_uses;
    edge_uses.reserve(index_count);
    const size_t triangle_count = index_count / 3;
    for (size_t t = 0; t < triangle_count; ++t) {
        const uint32_t i0 = geom.indices[3 * t + 0];
        const uint32_t i1 = geom.indices[3 * t + 1];
        const uint32_t i2 = geom.indices[3 * t + 2];
        ++edge_uses[edge_key(i0, i1)];
        ++edge_uses[edge_key(i1, i2)];
        ++edge_uses[edge_key(i2, i0)];
    }

    auto emit_skirt = [&](uint32_t a, uint32_t b) {
        // Read endpoint positions before any append (append may reallocate).
        const float ax = out.vertices[3 * a + 0];
        const float ay = out.vertices[3 * a + 1];
        const float az = out.vertices[3 * a + 2];
        const float bx = out.vertices[3 * b + 0];
        const float by = out.vertices[3 * b + 1];
        const float bz = out.vertices[3 * b + 2];
        const uint32_t a_low = static_cast<uint32_t>(out.vertices.size() / 3);
        out.vertices.insert(
            out.vertices.end(), {ax, ay - skirt_depth, az});
        const uint32_t b_low = static_cast<uint32_t>(out.vertices.size() / 3);
        out.vertices.insert(
            out.vertices.end(), {bx, by - skirt_depth, bz});
        // Quad (a, b, b_low, a_low) as two triangles. Winding is irrelevant —
        // Box3D mesh collision is two-sided.
        out.indices.insert(out.indices.end(), {a, b, b_low});
        out.indices.insert(out.indices.end(), {a, b_low, a_low});
    };

    for (size_t t = 0; t < triangle_count; ++t) {
        const uint32_t tri[3] = {
            geom.indices[3 * t + 0],
            geom.indices[3 * t + 1],
            geom.indices[3 * t + 2]};
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = tri[e];
            const uint32_t b = tri[(e + 1) % 3];
            if (edge_uses[edge_key(a, b)] == 1) {
                emit_skirt(a, b);
            }
        }
    }

    return out;
}

HeightFieldColliderData build_heightfield_collider(
    const std::function<float(float, float)>& sampler,
    int64_t tx, int64_t tz,
    float sector_size,
    int samples_per_axis,
    float global_min, float global_max,
    float friction) {
    HeightFieldColliderData out;
    const int n = std::max(2, samples_per_axis);
    const float spacing = sector_size / static_cast<float>(n - 1);
    const float x0 = static_cast<float>(tx) * sector_size;
    const float z0 = static_cast<float>(tz) * sector_size;

    out.heights.resize(static_cast<size_t>(n) * static_cast<size_t>(n));
    // Box3D indexes heights as row*countX + column, with column = x, row = z.
    for (int iz = 0; iz < n; ++iz) {
        const float wz = z0 + static_cast<float>(iz) * spacing;
        for (int ix = 0; ix < n; ++ix) {
            const float wx = x0 + static_cast<float>(ix) * spacing;
            out.heights[static_cast<size_t>(iz) * n + ix] = sampler(wx, wz);
        }
    }

    out.header.count_x = n;
    out.header.count_z = n;
    out.header.scale = {spacing, 1.0f, spacing};
    out.header.translation = {x0, 0.0f, z0};
    out.header.global_min = global_min;
    out.header.global_max = global_max;
    out.header.friction = friction;
    return out;
}

}  // namespace matter::terrain_collider
