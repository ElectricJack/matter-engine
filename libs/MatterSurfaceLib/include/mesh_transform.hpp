#ifndef MSL_MESH_TRANSFORM_HPP
#define MSL_MESH_TRANSFORM_HPP

#include "mesh_indexed.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

// How reproject_triex fills the output shading normals N0/N1/N2. The
// materialId/tint/uv/AO handling is identical in both modes; only the normals
// differ.
enum class ReprojectNormals {
    // Recompute smooth area-weighted vertex normals over the welded TARGET
    // mesh. Averages across every shared vertex, including creases — a cube's
    // 90-degree edges melt into a smooth gradient. Right for retopo, whose
    // output is an organic quad-flow surface with no authored creases.
    SmoothTarget,
    // Sample the SOURCE's authored shading normals at each target corner
    // (nearest crease-compatible source triangle, clamped-barycentric
    // interpolation). Inherits the source's shading character: a box's
    // per-face normals stay hard, an isosurface's smooth field stays smooth.
    // Right for LOD ladders, where the rung must shade like the authored mesh.
    SampleSource,
};

// Everything reproject_triex derives from `source`, built once and reusable
// across any number of targets.
//
// Why this exists: an LOD ladder reprojects the SAME full-res source onto every
// decimated rung, and the free function below rebuilds all of this per call —
// source centroids, the centroid hash, source face normals, and (in
// SampleSource mode) the triangle-AABB overlap grid, which inserts every
// triangle into every cell its AABB touches. None of it depends on the target.
// PartStore re-bakes a part's whole ladder on every load, so for a streamed
// world sector that rebuild ran once per rung, per sector, per publish, on the
// app/GL thread. Build one of these outside the rung loop and each rung pays
// only the target-side work.
//
// Holds a REFERENCE to `source`: it must outlive the index and must not be
// mutated while the index is in use. `valid()` is false when the source has no
// usable parallel TriEx, matching the free function's early-out.
class ReprojectSource {
public:
    ReprojectSource(const MeshIndexed& source_in, ReprojectNormals normals_in);

    bool valid() const { return valid_; }

private:
    friend void reproject_triex(const ReprojectSource& index, MeshIndexed& target);

    // SampleSource-only: triangle-AABB overlap grid + source face normals.
    // Split out of the constructor only to keep the two grid builds readable.
    void build_donor_machinery();

    // Nearest source triangle to `c` by centroid distance (attribute match).
    uint32_t nearest_src(const float3& c) const;
    // Nearest crease-compatible donor triangle to corner `p`; -1 when none.
    int nearest_donor(const float3& p, const float3& align) const;

    // Member names deliberately match the local variables these replaced, so
    // the query bodies moved over verbatim.
    const MeshIndexed& source;
    ReprojectNormals normals;
    bool valid_ = false;

    // Centroid grid — attribute match.
    std::vector<float3> src_centroids;
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    float mn[3] = {0, 0, 0};
    float cell = 1.0f;

    // SampleSource donor machinery; empty in SmoothTarget mode.
    std::vector<float3> src_face_n;
    std::unordered_map<uint64_t, std::vector<uint32_t>> overlap_grid;
    float vmn[3] = {0, 0, 0};
    float cell_ov = 1.0f;
    int   ov_span = 1;
};

// Shared TriEx reprojection helper for mesh transformations that change the
// triangle set (simplify, retopo). For each triangle in `target`, finds the
// nearest source triangle by centroid distance (uniform spatial hash over
// source centroids) and copies its TriEx; shading normals are then rewritten
// per `normals` above.
//
// `source.triex` must be populated and parallel to source triangles (i.e.
// source.triex.size() == source.indices.size()/3). If not, `target.triex` is
// cleared and the function returns without work.
//
// On return, `target.triex` is parallel to `target` triangles (size ==
// target.indices.size()/3).
void reproject_triex(const MeshIndexed& source, MeshIndexed& target,
                     ReprojectNormals normals = ReprojectNormals::SmoothTarget);

// Same reprojection against a prebuilt source index. Output is identical to
// reproject_triex(source, target, normals) for the source/normals the index was
// built from; only the redundant per-call source work is skipped.
void reproject_triex(const ReprojectSource& index, MeshIndexed& target);

#endif // MSL_MESH_TRANSFORM_HPP
