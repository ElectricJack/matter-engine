// libs/MatterSurfaceLib/src/mesh_retopo.cpp
//
// MatterSurfaceLib's wrapper over `third_party/autoremesher_core`. It is a
// format adapter plus an attribute-restore step; none of the retopology math
// lives here.
//
// Pipeline for one call to `retopo`:
//   1. `to_ar_mesh`   — MeshIndexed (float3 positions) -> autoremesher::Mesh
//                       (flat xyz float array); indices pass through as-is.
//   2. `autoremesher::remesh` — the actual cross-field / MIQ remesher.
//   3. `from_ar_mesh` — back to MeshIndexed. TriEx is NOT produced here: the
//                       remesher invents a whole new triangle set, so there is
//                       nothing to carry over index-wise.
//   4. `reproject_triex` (mesh_transform.hpp) — restores materialId and tint by
//                       nearest-source lookup. Shading normals are recomputed
//                       smooth over the target (the default `SmoothTarget`
//                       mode), which is correct for retopo output: it is an
//                       organic quad-flow surface with no authored creases.
//                       AO stays at its unbaked default; `vertex_ao` runs
//                       downstream.
//
// Failure is a normal outcome, not an exception: on empty input or a remesher
// error the returned `RetopoResult` has `ok == false`, `err` set, and an empty
// `mesh` — callers are expected to fall back to the input mesh.
// `elapsed_seconds` is filled in from the library even on the failure path, so
// a timeout still reports how long it burned.
//
// Determinism / threading: `RetopoOptions::threads` defaults to 1 to pin
// floating-point summation order, but note the caveat in `mesh_retopo.hpp` —
// autoremesher_core builds its TBB scheduler once per PROCESS, so only the
// first call's `threads` value takes effect. Do not assume two concurrent
// `retopo` calls with different thread counts behave independently.
#include "mesh_retopo.hpp"
#include "mesh_transform.hpp"

#include "autoremesher/remesh.h"

#include <cstdint>

namespace {

// Convert MeshIndexed (float3 positions) to autoremesher::Mesh (flat float xyz array).
autoremesher::Mesh to_ar_mesh(const MeshIndexed& in) {
    autoremesher::Mesh out;
    out.positions.reserve(in.positions.size() * 3);
    for (float3 p : in.positions) {
        out.positions.push_back(p.x);
        out.positions.push_back(p.y);
        out.positions.push_back(p.z);
    }
    out.indices = in.indices;
    return out;
}

// Convert autoremesher::Mesh back to MeshIndexed. triex is left empty;
// caller runs reproject_triex to populate it.
MeshIndexed from_ar_mesh(const autoremesher::Mesh& in) {
    MeshIndexed out;
    size_t vcount = in.positions.size() / 3;
    out.positions.reserve(vcount);
    for (size_t i = 0; i < vcount; ++i) {
        out.positions.push_back(make_float3(in.positions[i * 3 + 0],
                                            in.positions[i * 3 + 1],
                                            in.positions[i * 3 + 2]));
    }
    out.indices = in.indices;
    return out;
}

} // namespace

RetopoResult retopo(const MeshIndexed& in, const RetopoOptions& opts) {
    RetopoResult r;

    if (in.positions.empty() || in.indices.empty()) {
        r.err = "empty input";
        return r;
    }

    autoremesher::Mesh    ar_in  = to_ar_mesh(in);
    autoremesher::Options ar_opts;
    ar_opts.target_ratio    = opts.target_ratio;
    ar_opts.iterations      = opts.iterations;   // v1: accepted, ignored by library
    ar_opts.seed            = opts.seed;          // v1: accepted, ignored by library
    ar_opts.timeout_seconds = opts.timeout_seconds;
    ar_opts.threads         = opts.threads;

    autoremesher::Result ar_result = autoremesher::remesh(ar_in, ar_opts);

    r.elapsed_seconds = ar_result.elapsed_seconds;

    if (!ar_result.ok) {
        r.err = ar_result.err;
        return r;  // r.ok stays false; r.mesh stays empty
    }

    r.mesh = from_ar_mesh(ar_result.mesh);

    // Reproject materialId + tint from source to output via nearest-centroid.
    // AO and shading normals stay at unbaked defaults — vertex_ao runs downstream.
    reproject_triex(in, r.mesh);

    r.ok = true;
    return r;
}
