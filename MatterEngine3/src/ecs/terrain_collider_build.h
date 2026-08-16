#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "matter/physics.h"  // StaticMeshCollider / StaticHeightFieldCollider
#include "render/indexed_part_geometry.h"

// Terrain collider builders (Milestone M1). Pure functions that turn a resident
// sector's CPU geometry into the physics-layer collider descriptors, applying
// the mode-correct world transform and seam skirts. They own the buffers the
// descriptors point at; keep the returned object alive across the matching
// physics_attach_* call. See
// docs/superpowers/specs/2026-08-15-character-controller-design.md §1.

namespace matter::terrain_collider {

// Owns the tile-local vertex/index buffers a StaticMeshCollider references.
struct MeshColliderData {
    std::vector<float> vertices;    // 3 floats/vertex, tile-local (+ skirt verts)
    std::vector<uint32_t> indices;  // 3/triangle (+ skirt triangles)
    physics::StaticMeshCollider header{};  // counts/translation/friction only

    // A ready-to-attach descriptor with its span pointers bound to the buffers
    // above. Recompute after any mutation; do not cache across a move.
    physics::StaticMeshCollider collider() const {
        physics::StaticMeshCollider desc = header;
        desc.vertices = vertices.data();
        desc.vertex_count = static_cast<int32_t>(vertices.size() / 3);
        desc.indices = indices.data();
        desc.triangle_count = static_cast<int32_t>(indices.size() / 3);
        return desc;
    }
};

// Build a mesh collider from one rung of a resident sector.
//   volumetric  : world_volumetric_sectors — picks the y-origin branch. Flat
//                 mode leaves y world-absolute (translation.y = 0); volumetric
//                 mode is fully tile-local (translation.y = ty*sector_size).
//   sector_size : the tile's own LEVEL sector size (S0 << level), not the base.
//   skirt_depth : downward extrusion on open boundary edges to bridge cross-
//                 level seam gaps (0 disables). ~2 collision voxels is typical.
MeshColliderData build_mesh_collider(
    const viewer::IndexedPartGeometry& geom,
    bool volumetric,
    int64_t tx, int64_t ty, int64_t tz,
    float sector_size,
    float skirt_depth,
    float friction = 0.6f);

// Owns the height grid a StaticHeightFieldCollider references.
struct HeightFieldColliderData {
    std::vector<float> heights;  // count_x*count_z, row-major: heights[iz*count_x+ix]
    physics::StaticHeightFieldCollider header{};

    physics::StaticHeightFieldCollider collider() const {
        physics::StaticHeightFieldCollider desc = header;
        desc.heights = heights.data();
        return desc;
    }
};

// Sample a world-space surface-height function over a sector's grid.
//   sampler(x,z) : world surface height (e.g. FieldRuntime::height_at).
//   samples_per_axis >= 2 grid lines per side.
// global_min/global_max should be shared across neighbouring sectors so their
// quantized edges align with no seam.
HeightFieldColliderData build_heightfield_collider(
    const std::function<float(float, float)>& sampler,
    int64_t tx, int64_t tz,
    float sector_size,
    int samples_per_axis,
    float global_min, float global_max,
    float friction = 0.6f);

}  // namespace matter::terrain_collider
