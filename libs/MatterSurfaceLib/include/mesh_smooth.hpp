#ifndef MSL_MESH_SMOOTH_HPP
#define MSL_MESH_SMOOTH_HPP

// libs/MatterSurfaceLib/include/mesh_smooth.hpp
//
// Taubin lambda/mu smoothing over `MeshIndexed`. One stage of
// MatterSurfaceLib's mesh-transformation pipeline, alongside
// `mesh_simplifier.hpp` and `mesh_retopo.hpp`, used to take the stair-stepping
// off a marching-cubes or voxel-cube surface.
//
// Why Taubin rather than plain Laplacian: alternating a positive `lambda`
// step with a slightly larger negative `mu` step cancels the volume loss that
// makes repeated Laplacian passes shrink a mesh toward nothing.
//
// Contract: connectivity is never touched — only `positions` move. Vertices
// on an edge whose incidence is not 2 (open rims, cut edges, non-manifold
// junctions) are held fixed, which is what keeps a smoothed cell watertight
// against its neighbours and stable across LOD levels.
//
// Determinism: the 1-ring is built through an ordered `std::map` keyed on
// sorted vertex pairs, so neighbour accumulation order — and therefore the
// floating-point result — is byte-stable for identical input. Do not swap
// that for an unordered container.
//
// Threading: pure CPU, no GL, no shared state. Safe on worker threads.

#include "mesh_indexed.hpp"

#include <string>

// Taubin lambda/mu smoothing (shrink-free Laplacian) over the vertex 1-ring
// with uniform (umbrella) weights. Operates on MeshIndexed; connectivity is
// never changed. Boundary vertices (edge incidence != 2) are held fixed.
struct SmoothOptions {
    int   iterations = 2;      // >= 1
    float lambda     = 0.5f;   // positive step
    float mu         = -0.53f; // negative step (|mu| slightly > lambda)
};

struct SmoothResult {
    MeshIndexed mesh;
    bool        ok = false;
    std::string err;           // set when !ok
};

// Validates everything up front and returns `ok == false` with `err` set —
// never a partially smoothed mesh — for: empty input, an index count that is
// not a multiple of 3, an index past the end of `positions`, a non-empty
// `triex` that is not parallel to the triangles, or out-of-domain options
// (`iterations < 1`, `lambda <= 0`, `mu >= 0`). `mesh` is only meaningful
// when `ok` is true.
SmoothResult smooth(const MeshIndexed& in, const SmoothOptions& opts);

#endif // MSL_MESH_SMOOTH_HPP
