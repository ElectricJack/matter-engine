// Tests for MSL's mesh_retopo wrapper around autoremesher_core::remesh.
//
// NOTE ON TEST INPUT: the upstream autoremesher pipeline requires a closed
// manifold mesh with non-trivial curvature. A raw unit cube (12 tris, flat
// faces) causes the cross-field parameterization to collapse and returns
// ok=false. Following Task 6's smoke_cube, we use a "spherified" subdivided
// cube as the primary valid-mesh input: 6 cube faces each subdivided into an
// N*N grid, then every vertex projected radially onto the unit sphere.
// N=8 produces ~386 verts / 768 tris, which the parameterizer handles well.
//
// Tests 1 and 4 explicitly exercise the failure path with degenerate input.
#include "mesh_retopo.hpp"
#include "mesh_indexed.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

// Spherified subdivided cube — same algorithm as smoke_cube (Task 6).
// Each cube face is divided into N*N quads, boundary verts are welded across
// face seams (in cube-space before projection), and every vertex is projected
// onto the unit sphere. N=8: ~386 verts, 768 tris.
MeshIndexed make_spherified_cube(int N = 8) {
    struct Face { float o[3], u[3], v[3]; };
    const Face faces[6] = {
        { {-1,-1,-1}, {2,0,0}, {0,2,0} },   // z=-1
        { {-1,-1, 1}, {0,2,0}, {2,0,0} },   // z=+1
        { {-1,-1,-1}, {0,0,2}, {2,0,0} },   // y=-1
        { {-1, 1,-1}, {2,0,0}, {0,0,2} },   // y=+1
        { {-1,-1,-1}, {0,2,0}, {0,0,2} },   // x=-1
        { { 1,-1,-1}, {0,0,2}, {0,2,0} },   // x=+1
    };

    struct Key { int x, y, z;
                 bool operator==(const Key& o) const { return x==o.x && y==o.y && z==o.z; } };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return (std::size_t)(unsigned)k.x * 73856093u
                 ^ (std::size_t)(unsigned)k.y * 19349663u
                 ^ (std::size_t)(unsigned)k.z * 83492791u;
        }
    };
    std::unordered_map<Key, uint32_t, KeyHash> weld;

    std::vector<float>    flat_positions;
    std::vector<uint32_t> flat_indices;

    auto sphere_project = [](float x, float y, float z) {
        float r = std::sqrt(x*x + y*y + z*z);
        return std::array<float,3>{ x/r, y/r, z/r };
    };

    auto get_or_add = [&](float cx, float cy, float cz) -> uint32_t {
        Key k{ (int)std::lround(cx * 1e6f),
               (int)std::lround(cy * 1e6f),
               (int)std::lround(cz * 1e6f) };
        auto it = weld.find(k);
        if (it != weld.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(flat_positions.size() / 3u);
        auto p = sphere_project(cx, cy, cz);
        flat_positions.push_back(p[0]);
        flat_positions.push_back(p[1]);
        flat_positions.push_back(p[2]);
        weld.emplace(k, idx);
        return idx;
    };

    for (const auto& f : faces) {
        std::vector<uint32_t> grid((N+1)*(N+1));
        for (int j = 0; j <= N; ++j) {
            for (int i = 0; i <= N; ++i) {
                float s = static_cast<float>(i) / static_cast<float>(N);
                float t = static_cast<float>(j) / static_cast<float>(N);
                grid[j*(N+1)+i] = get_or_add(
                    f.o[0] + s*f.u[0] + t*f.v[0],
                    f.o[1] + s*f.u[1] + t*f.v[1],
                    f.o[2] + s*f.u[2] + t*f.v[2]);
            }
        }
        for (int j = 0; j < N; ++j) {
            for (int i = 0; i < N; ++i) {
                uint32_t a = grid[j*(N+1)+i];
                uint32_t b = grid[j*(N+1)+i+1];
                uint32_t c = grid[(j+1)*(N+1)+i];
                uint32_t d = grid[(j+1)*(N+1)+i+1];
                flat_indices.push_back(a); flat_indices.push_back(b); flat_indices.push_back(d);
                flat_indices.push_back(a); flat_indices.push_back(d); flat_indices.push_back(c);
            }
        }
    }

    // Convert flat arrays to MeshIndexed.
    MeshIndexed m;
    size_t vcount = flat_positions.size() / 3;
    m.positions.reserve(vcount);
    for (size_t i = 0; i < vcount; ++i) {
        m.positions.push_back(make_float3(flat_positions[i*3+0],
                                          flat_positions[i*3+1],
                                          flat_positions[i*3+2]));
    }
    m.indices = flat_indices;
    return m;
}

// --- Test 1: empty input returns ok=false with a non-empty error string -------
void test_empty_input_empty_output() {
    MeshIndexed empty;
    RetopoResult r = retopo(empty, {});
    assert(!r.ok);
    assert(!r.err.empty());
    assert(r.mesh.positions.empty());
    assert(r.mesh.indices.empty());
    std::printf("  [1/5] test_empty_input_empty_output: OK\n");
}

// --- Test 2: determinism — byte-identical output for identical inputs ---------
// Exercises Task 6's determinism guarantee through the MSL wrapper layer,
// including the extra temp-vector allocations in to_ar_mesh / from_ar_mesh.
void test_deterministic_output() {
    MeshIndexed in = make_spherified_cube(8);
    RetopoOptions opts;
    opts.seed    = 42;
    opts.threads = 1;

    RetopoResult r1 = retopo(in, opts);
    RetopoResult r2 = retopo(in, opts);

    assert(r1.ok);
    assert(r2.ok);
    assert(r1.mesh.positions.size() == r2.mesh.positions.size());
    assert(r1.mesh.indices.size()   == r2.mesh.indices.size());

    // Byte-identity check (float bits via memcmp semantics).
    bool positions_identical = true;
    for (size_t i = 0; i < r1.mesh.positions.size() && positions_identical; ++i) {
        uint32_t ax, bx, ay, by, az, bz;
        std::memcpy(&ax, &r1.mesh.positions[i].x, sizeof(ax));
        std::memcpy(&bx, &r2.mesh.positions[i].x, sizeof(bx));
        std::memcpy(&ay, &r1.mesh.positions[i].y, sizeof(ay));
        std::memcpy(&by, &r2.mesh.positions[i].y, sizeof(by));
        std::memcpy(&az, &r1.mesh.positions[i].z, sizeof(az));
        std::memcpy(&bz, &r2.mesh.positions[i].z, sizeof(bz));
        if (ax != bx || ay != by || az != bz) positions_identical = false;
    }
    bool indices_identical =
        std::equal(r1.mesh.indices.begin(), r1.mesh.indices.end(),
                   r2.mesh.indices.begin());

    if (!positions_identical || !indices_identical) {
        // Determinism regression — do NOT paper over. Report as a concern.
        std::printf("CONCERN [determinism]: wrapper outputs differ across calls "
                    "(positions_identical=%d indices_identical=%d, "
                    "verts %zu vs %zu, tris %zu vs %zu)\n",
                    (int)positions_identical, (int)indices_identical,
                    r1.mesh.positions.size(), r2.mesh.positions.size(),
                    r1.mesh.indices.size() / 3, r2.mesh.indices.size() / 3);
    }
    assert(positions_identical && "Byte-identity failed for positions across two retopo calls");
    assert(indices_identical   && "Byte-identity failed for indices across two retopo calls");
    std::printf("  [2/5] test_deterministic_output: OK\n");
}

// --- Test 3: reproject_triex wired up — materialId carried into output --------
// Sets materialId 1/2/3 (cycling) on input triangles. After retopo, every
// output triangle must have a materialId in {1,2,3} — verifying nearest-centroid
// transfer is active, not silently skipped.
void test_triex_materialid_preserved_via_reproject() {
    MeshIndexed in = make_spherified_cube(8);
    in.triex.resize(in.indices.size() / 3);
    for (size_t i = 0; i < in.triex.size(); ++i) {
        in.triex[i].materialId = (int)(i % 3) + 1;   // cycles 1, 2, 3
    }

    RetopoOptions opts;
    opts.seed = 0;
    RetopoResult r = retopo(in, opts);

    assert(r.ok);
    assert(r.mesh.triex.size() == r.mesh.indices.size() / 3);
    for (const TriEx& t : r.mesh.triex) {
        assert(t.materialId >= 1 && t.materialId <= 3);
    }
    std::printf("  [3/5] test_triex_materialid_preserved_via_reproject: OK\n");
}

// --- Test 4: failure path — degenerate input, ok=false with non-empty err -----
// A single flat triangle (1 tri, 0 curvature) triggers a failure inside the
// autoremesher pipeline. The wrapper must return ok=false with a non-empty err
// string and an empty output mesh.
void test_failure_fallback_populates_err() {
    MeshIndexed in;
    in.positions = { make_float3(0,0,0), make_float3(1,0,0), make_float3(0,1,0) };
    in.indices   = { 0, 1, 2 };
    RetopoResult r = retopo(in, {});
    assert(!r.ok);
    assert(!r.err.empty());
    // Output mesh must be empty so caller can safely fall back to the input.
    assert(r.mesh.positions.empty());
    assert(r.mesh.indices.empty());
    std::printf("  [4/5] test_failure_fallback_populates_err: OK\n");
}

// --- Test 5: elapsed_seconds populated; cube finishes within timeout budget ---
// Task 6's smoke_cube measured elapsed=0.044s on the spherified cube. With
// timeout_seconds=1, this should complete well under budget. If it times out,
// something is seriously wrong with the vendored library — fail the test to
// surface it. We also confirm elapsed_seconds is a positive, finite value.
void test_timeout_short_budget() {
    MeshIndexed in = make_spherified_cube(8);
    RetopoOptions opts;
    opts.timeout_seconds = 1;   // tight but generous vs. ~44ms observed

    RetopoResult r = retopo(in, opts);

    if (!r.ok) {
        std::printf("  test_timeout_short_budget: retopo failed err=\"%s\" elapsed=%.3fs\n",
                    r.err.c_str(), r.elapsed_seconds);
    }
    assert(r.ok);
    // elapsed_seconds must be positive and finite.
    assert(r.elapsed_seconds > 0.0);
    assert(r.elapsed_seconds < 1.0);   // well under 1s for a spherified cube
    std::printf("  [5/5] test_timeout_short_budget: OK (elapsed=%.3fs)\n",
                r.elapsed_seconds);
}

} // namespace

int main() {
    std::printf("mesh_retopo_tests:\n");
    test_empty_input_empty_output();
    test_deterministic_output();
    test_triex_materialid_preserved_via_reproject();
    test_failure_fallback_populates_err();
    test_timeout_short_budget();
    std::printf("mesh_retopo_tests: OK (5/5)\n");
    return 0;
}
