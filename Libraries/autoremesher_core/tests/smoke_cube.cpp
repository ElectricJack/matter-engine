// Standalone smoke test: run remesh() on a curved test mesh (a subdivided-cube
// projected onto a sphere) and verify basic invariants. This is a "smoke"
// signal that the library links and produces valid output on a well-behaved
// closed manifold input.
//
// NOTE: the upstream autoremesher pipeline is designed for meshes with
// several hundred+ triangles AND non-trivial curvature. A raw unit cube
// (12 tris, flat faces) does not produce any output — the cross-field
// parameterization collapses on flat surfaces. So we start from a
// subdivided cube and project each vertex onto a unit sphere. The result
// is topologically simple but geometrically curved, which the parameterizer
// handles well.
//
// The file is still named `smoke_cube` because it was authored under that
// name in the plan; the input topology is a sphere, but the test's
// purpose (does remesh() link + produce a valid triangle mesh?) is
// unchanged.
#include "autoremesher/remesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

// Build a "spherified" subdivided cube: each cube-face is subdivided into
// an (N x N) grid, boundary verts are welded across face seams, and every
// vertex is projected radially onto the unit sphere. N=8 -> ~386 verts,
// 768 tris.
static void build_spherified_cube(int N,
                                  std::vector<float>& positions,
                                  std::vector<uint32_t>& indices)
{
    struct Face { float o[3], u[3], v[3]; };
    // Cube faces expressed in [-1, 1]^3, so the projected sphere is
    // roughly the unit sphere (varies slightly with corner spherification).
    const Face faces[6] = {
        // z=-1 face (normal -z)
        { {-1,-1,-1}, {2,0,0}, {0,2,0} },
        // z=+1 face (normal +z)
        { {-1,-1, 1}, {0,2,0}, {2,0,0} },
        // y=-1 face (normal -y)
        { {-1,-1,-1}, {0,0,2}, {2,0,0} },
        // y=+1 face (normal +y)
        { {-1, 1,-1}, {2,0,0}, {0,0,2} },
        // x=-1 face (normal -x)
        { {-1,-1,-1}, {0,2,0}, {0,0,2} },
        // x=+1 face (normal +x)
        { { 1,-1,-1}, {0,0,2}, {0,2,0} },
    };

    positions.clear();
    indices.clear();

    // Vertex welding on quantized cube-space coords BEFORE sphere projection,
    // so shared boundary verts weld correctly.
    struct Key { int x, y, z; bool operator==(const Key& o) const { return x==o.x && y==o.y && z==o.z; } };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return (std::size_t)k.x * 73856093u ^ (std::size_t)k.y * 19349663u ^ (std::size_t)k.z * 83492791u;
        }
    };
    std::unordered_map<Key, uint32_t, KeyHash> weld;

    auto sphere_project = [](float x, float y, float z) {
        const float r = std::sqrt(x*x + y*y + z*z);
        return std::array<float, 3>{ x / r, y / r, z / r };
    };

    auto get_or_add = [&](float cx, float cy, float cz) -> uint32_t {
        Key k { (int)std::lround(cx * 1e6), (int)std::lround(cy * 1e6), (int)std::lround(cz * 1e6) };
        auto it = weld.find(k);
        if (it != weld.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(positions.size() / 3u);
        auto p = sphere_project(cx, cy, cz);
        positions.push_back(p[0]); positions.push_back(p[1]); positions.push_back(p[2]);
        weld.emplace(k, idx);
        return idx;
    };

    for (const auto& f : faces) {
        std::vector<uint32_t> grid((N + 1) * (N + 1));
        for (int j = 0; j <= N; ++j) {
            for (int i = 0; i <= N; ++i) {
                const float s = static_cast<float>(i) / static_cast<float>(N);
                const float t = static_cast<float>(j) / static_cast<float>(N);
                grid[j * (N + 1) + i] = get_or_add(
                    f.o[0] + s * f.u[0] + t * f.v[0],
                    f.o[1] + s * f.u[1] + t * f.v[1],
                    f.o[2] + s * f.u[2] + t * f.v[2]);
            }
        }
        for (int j = 0; j < N; ++j) {
            for (int i = 0; i < N; ++i) {
                const uint32_t a = grid[j * (N + 1) + i];
                const uint32_t b = grid[j * (N + 1) + i + 1];
                const uint32_t c = grid[(j + 1) * (N + 1) + i];
                const uint32_t d = grid[(j + 1) * (N + 1) + i + 1];
                indices.push_back(a); indices.push_back(b); indices.push_back(d);
                indices.push_back(a); indices.push_back(d); indices.push_back(c);
            }
        }
    }
}

// Run remesh() once on `in` with `opts`, validate basic invariants, print a
// per-call OK line tagged with `label`. Returns true on success (r.ok + shape
// checks pass), false otherwise; on failure a FAIL line is printed to stderr.
static bool run_once(const char* label,
                     const autoremesher::Mesh& in,
                     const autoremesher::Options& opts,
                     autoremesher::Result& out)
{
    out = autoremesher::remesh(in, opts);

    if (!out.ok) {
        std::fprintf(stderr, "FAIL [%s]: remesh returned ok=false, err=\"%s\"\n",
                     label, out.err.c_str());
        return false;
    }
    if (out.mesh.positions.size() < 12) {   // at least 4 verts
        std::fprintf(stderr, "FAIL [%s]: too few positions (%zu)\n",
                     label, out.mesh.positions.size());
        return false;
    }
    if (out.mesh.indices.size() % 3 != 0) {
        std::fprintf(stderr, "FAIL [%s]: indices not a multiple of 3 (%zu)\n",
                     label, out.mesh.indices.size());
        return false;
    }
    if (out.mesh.indices.empty()) {
        std::fprintf(stderr, "FAIL [%s]: no output triangles\n", label);
        return false;
    }

    std::printf("OK [%s]: %zu verts, %zu tris, elapsed=%.3fs\n",
                label,
                out.mesh.positions.size() / 3,
                out.mesh.indices.size() / 3,
                out.elapsed_seconds);
    return true;
}

int main() {
    autoremesher::Mesh in;
    build_spherified_cube(8, in.positions, in.indices);

    autoremesher::Options opts;
    opts.target_ratio    = 1.0f;
    opts.iterations      = 3;
    opts.seed            = 42;
    opts.timeout_seconds = 60;
    opts.threads         = 1;

    // First call. Also exercises one-time geogram + TBB scheduler init.
    autoremesher::Result r1;
    if (!run_once("call 1", in, opts, r1)) return 1;

    // Second call with identical input + options. This exercises the fix for
    // Task 6 review finding C1: the previous v1 driver constructed a fresh
    // tbb::task_scheduler_init on every call, which segfaulted on the second
    // invocation. It also verifies determinism (byte-identical output at
    // threads=1) which the cache-key contract in MSL depends on.
    autoremesher::Result r2;
    if (!run_once("call 2", in, opts, r2)) return 1;

    // Byte-identity check between call 1 and call 2.
    bool positions_identical =
        (r1.mesh.positions.size() == r2.mesh.positions.size()) &&
        std::equal(r1.mesh.positions.begin(), r1.mesh.positions.end(),
                   r2.mesh.positions.begin(),
                   [](float a, float b) {
                       // Byte-identity for floats — memcmp semantics via bit
                       // reinterpret so NaN patterns compare literally.
                       std::uint32_t ai, bi;
                       std::memcpy(&ai, &a, sizeof(ai));
                       std::memcpy(&bi, &b, sizeof(bi));
                       return ai == bi;
                   });
    bool indices_identical =
        (r1.mesh.indices.size() == r2.mesh.indices.size()) &&
        std::equal(r1.mesh.indices.begin(), r1.mesh.indices.end(),
                   r2.mesh.indices.begin());

    if (positions_identical && indices_identical) {
        std::printf("OK [determinism]: byte-identical positions + indices between calls\n");
    } else {
        // Do NOT fail the test — the report contract says a determinism
        // regression is a "concern to surface", not a paper-over. Print a
        // clear CONCERN line so the caller sees it in the stdout capture.
        std::printf("CONCERN [determinism]: outputs differ across calls "
                    "(positions_identical=%d indices_identical=%d, "
                    "sizes: verts %zu vs %zu, tris %zu vs %zu)\n",
                    (int)positions_identical, (int)indices_identical,
                    r1.mesh.positions.size() / 3,
                    r2.mesh.positions.size() / 3,
                    r1.mesh.indices.size() / 3,
                    r2.mesh.indices.size() / 3);
    }

    return 0;
}
