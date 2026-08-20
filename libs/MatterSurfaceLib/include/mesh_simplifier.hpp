#ifndef MESH_SIMPLIFIER_HPP
#define MESH_SIMPLIFIER_HPP

// libs/MatterSurfaceLib/include/mesh_simplifier.hpp
//
// QEM (quadric error metric) edge-collapse decimation. This is the workhorse
// behind LOD: a part's ladder is built by re-simplifying the full-resolution
// mesh at successively smaller `target_ratio` values, then reprojecting the
// per-triangle `TriEx` onto each rung with `mesh_transform.hpp`'s
// `reproject_triex`.
//
// Where it sits: MatterSurfaceLib, above SpatialQueryLib and below
// MatterEngine3's bake pipeline. Two entry points with identical semantics —
// `simplify_mesh` for a raylib `Mesh` (the mesher's native output, still the
// format at the BLAS/upload boundary) and `simplify` for `MeshIndexed` (the
// pipeline-internal format from `mesh_indexed.hpp`).
//
// Threading: pure CPU, no GL, no mutable global state — safe to run on
// `MeshWorkerPool` workers, which is how cell meshes are decimated.
//
// Gotchas:
//   - Seam correctness comes from `lock_boundary`, not from the decimator.
//     Both locking classes documented on `SimplifyOptions` below exist to
//     keep neighbouring cells and adjacent LOD levels watertight; read them
//     before changing the flag.
//   - `target_ratio` is a request, not a guarantee. Locked vertices cannot
//     collapse and `max_error` stops the queue early, so the result can carry
//     many more triangles than asked for. Callers budgeting triangles must
//     measure the output rather than assume the ratio.
//   - `simplify_mesh` returns a NEW mesh allocated with raylib's `MemAlloc`.
//     Free it with `UnloadMesh` (main thread, if it was uploaded) or with
//     `unload_cpu_mesh` from `mesh_build_utils.h` (any thread, if it was not).
//   - The `MeshIndexed` overload round-trips through a raylib `Mesh`
//     internally, so it costs a full conversion each way — see its comment.

// Phase 4 (Step 4) of docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md:
// CellBounds moved off raylib's Vector3 onto matter_math.h's mm::Vec3 (C++-
// only, no C consumer). raylib.h stays included -- simplify_mesh's Mesh
// parameter/return type is out of this phase's scope (Mesh migration is
// deferred; see the Phase 4 brief).
#include "raylib.h"
#include "matter_math.h"
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
    mm::Vec3 min_bound;
    mm::Vec3 max_bound;
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
