#ifndef MESH_SIMPLIFIER_HPP
#define MESH_SIMPLIFIER_HPP

#include "raylib.h"
#include <cfloat>

// Options controlling QEM edge-collapse decimation.
struct SimplifyOptions {
    float target_ratio  = 0.5f;     // fraction of triangles to keep, (0..1]
    float max_error     = FLT_MAX;  // stop once min collapse cost exceeds this
    // When true, two classes of vertices are frozen (never moved or removed):
    //   (a) Face-plane lock: vertices on any of the 6 CellBounds face planes
    //       (eps 1e-4). Requires CellBounds to be supplied. Preserves watertight
    //       same-level seams between adjacent cells.
    //   (b) Topological boundary lock: endpoints of edges with incidence != 2 in
    //       the welded topology. These are open-sheet rims, cut edges, or
    //       non-manifold junctions. Locking them keeps cut-vertex positions
    //       bit-identical across LOD levels and between neighboring clusters.
    //       Active regardless of whether CellBounds is supplied.
    //       (Approved MSL extension, 2026-07-02.)
    bool  lock_boundary = true;
};

// Axis-aligned cell extent in cluster-local space. When supplied to
// simplify_mesh and lock_boundary is true, vertices on any of the 6 face
// planes are never moved or removed (guarantees watertight same-level seams).
struct CellBounds {
    Vector3 min_bound;
    Vector3 max_bound;
};

// Returns a NEW indexed Mesh allocated with raylib's allocator (MemAlloc),
// safe to pass to UploadMesh/UnloadMesh. Does NOT mutate or free `input`.
// On empty/degenerate input returns a zeroed Mesh (vertexCount == 0).
Mesh simplify_mesh(const Mesh& input,
                   const SimplifyOptions& opts,
                   const CellBounds* bounds = nullptr);

#include "mesh_indexed.hpp"

// MeshIndexed overload — same semantics as simplify_mesh(raylib::Mesh) above.
// Internally converts to raylib::Mesh, calls the existing implementation, and
// converts back. When lod_bake and other callers migrate to MeshIndexed
// end-to-end (Task 11+), the intermediate raylib::Mesh round-trip goes away.
MeshIndexed simplify(const MeshIndexed& in,
                     const SimplifyOptions& opts,
                     const CellBounds* bounds = nullptr);

#endif // MESH_SIMPLIFIER_HPP
