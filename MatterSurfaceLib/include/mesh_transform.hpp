#ifndef MSL_MESH_TRANSFORM_HPP
#define MSL_MESH_TRANSFORM_HPP

#include "mesh_indexed.hpp"

// Shared TriEx reprojection helper for mesh transformations that change the
// triangle set (simplify, retopo). For each triangle in `target`, finds the
// nearest source triangle by centroid distance (uniform spatial hash over
// source centroids) and copies its TriEx. Shading normals N0/N1/N2 in the
// output are OVERWRITTEN with the target triangle's geometric face normal —
// the decimated/retopo'd surface no longer matches the source shading normals,
// so we recompute from the new geometry.
//
// Semantics match today's lod_bake::reproject_triex — this is a move of that
// logic into MSL so both mesh_simplifier and mesh_retopo can use it.
//
// `source.triex` must be populated and parallel to source triangles (i.e.
// source.triex.size() == source.indices.size()/3). If not, `target.triex` is
// cleared and the function returns without work.
//
// On return, `target.triex` is parallel to `target` triangles (size ==
// target.indices.size()/3).
void reproject_triex(const MeshIndexed& source, MeshIndexed& target);

#endif // MSL_MESH_TRANSFORM_HPP
