#ifndef MSL_MESH_RETOPO_HPP
#define MSL_MESH_RETOPO_HPP

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
