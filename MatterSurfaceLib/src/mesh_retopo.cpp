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
