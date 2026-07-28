#ifndef MSL_MESH_TRANSFORM_HPP
#define MSL_MESH_TRANSFORM_HPP

#include "mesh_indexed.hpp"

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

#endif // MSL_MESH_TRANSFORM_HPP
