#ifndef MSL_MESH_RETOPO_HPP
#define MSL_MESH_RETOPO_HPP

// libs/MatterSurfaceLib/include/mesh_retopo.hpp
//
// MatterSurfaceLib's wrapper over `third_party/autoremesher_core`'s remesher.
// Takes a `MeshIndexed`, returns a retopologized `MeshIndexed` whose
// per-triangle `TriEx` has been carried across from the input via
// `mesh_transform.hpp`'s `reproject_triex`.
//
// Where it sits: one stage of MSL's mesh-transformation pipeline, alongside
// `mesh_simplifier.hpp` (decimation) and `mesh_smooth.hpp` (Taubin). The
// autoremesher dependency is only linked when the consuming build enables
// retopo (`RETOPO=1`, the MatterEditor default — see the repo CLAUDE.md);
// with `RETOPO=0` the schemas that ask for retopo take the warn-and-continue
// path in `MatterEngine3/src/modifier_apply.cpp` instead.
//
// Usage: fill a `RetopoOptions`, call `retopo`, and check `ok` *before* using
// `mesh`. The documented failure response is to keep the input mesh unchanged
// — the wrapper never mutates the caller's input.
//
// Gotchas:
//   - This is a bake-time operation. It is bounded only by
//     `timeout_seconds`, and nothing here belongs on a per-frame path.
//   - Input topology matters more than input size: the cross-field
//     parameterization collapses on flat/degenerate geometry. See the input
//     contract on `retopo` below before feeding it primitives.
//   - The `threads` pin is process-sticky — the first `retopo()` call in the
//     process fixes it for every later call. See `RetopoOptions`.
//   - `iterations` and `seed` are accepted and ignored in v1; they exist for
//     cache-key invalidation. Again, see `RetopoOptions`.

#include "mesh_indexed.hpp"

#include <cstdint>
#include <string>

// Options for the retopo wrapper. Defaults match autoremesher_core v1 defaults.
//
// v1 note: `iterations` and `seed` are accepted but silently ignored by
// autoremesher_core v1 — upstream's MIQ solver and parameterizer have no
// setter for these. They are kept in the struct so MSL's cache-key logic
// can include them (ensuring future versions that implement them will
// invalidate old cache entries).
//
// Determinism: pinning `threads=1` ensures FP-summation order is fixed across
// calls. The TBB scheduler is constructed once per process on the first
// remesh() call, so the threads value from that first invocation is baked in
// for all subsequent calls regardless of what later calls request.
struct RetopoOptions {
    float    target_ratio    = 1.0f;   // relative to input tri count, (0, 4.0]
    int      iterations      = 3;      // v1: reserved, ignored
    uint32_t seed            = 0;      // v1: reserved, ignored
    int      timeout_seconds = 60;     // 0 = no limit
    int      threads         = 1;      // pinned for determinism
};

// Outcome of one `retopo` call. `ok` is the gate: check it before using
// `mesh`, since on failure `err` carries the reason and the caller is
// expected to fall back to its own input mesh.
struct RetopoResult {
    MeshIndexed mesh;              // retopo'd; TriEx repopulated via reproject_triex
    bool        ok = false;
    std::string err;
    double      elapsed_seconds = 0.0;
};

// Wraps autoremesher_core::remesh. Handles:
//   - MeshIndexed -> autoremesher::Mesh format adaptation
//   - materialId/tint reprojection via reproject_triex
//   - AO / shading normals left at unbaked defaults; vertex_ao runs downstream
// On failure, ok=false and err populated; caller falls back to input mesh.
//
// Input must be a closed, manifold mesh with non-trivial curvature (flat faces
// such as a raw unit cube will cause the cross-field parameterization to
// collapse; use subdivided+spherified geometry or similar). Empty input is
// rejected immediately.
RetopoResult retopo(const MeshIndexed& in, const RetopoOptions& opts);

#endif // MSL_MESH_RETOPO_HPP
