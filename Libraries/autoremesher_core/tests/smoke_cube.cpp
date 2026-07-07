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

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

int main() {
    autoremesher::Mesh in;
    build_spherified_cube(8, in.positions, in.indices);

    autoremesher::Options opts;
    opts.target_ratio    = 1.0f;
    opts.iterations      = 3;
    opts.seed            = 42;
    opts.timeout_seconds = 60;
    opts.threads         = 1;

    autoremesher::Result r = autoremesher::remesh(in, opts);

    if (!r.ok) {
        std::fprintf(stderr, "FAIL: remesh returned ok=false, err=\"%s\"\n", r.err.c_str());
        return 1;
    }
    if (r.mesh.positions.size() < 12) {   // at least 4 verts
        std::fprintf(stderr, "FAIL: too few positions (%zu)\n", r.mesh.positions.size());
        return 1;
    }
    if (r.mesh.indices.size() % 3 != 0) {
        std::fprintf(stderr, "FAIL: indices not a multiple of 3 (%zu)\n", r.mesh.indices.size());
        return 1;
    }
    if (r.mesh.indices.empty()) {
        std::fprintf(stderr, "FAIL: no output triangles\n");
        return 1;
    }

    std::printf("OK: %zu verts, %zu tris, elapsed=%.3fs\n",
                r.mesh.positions.size() / 3,
                r.mesh.indices.size() / 3,
                r.elapsed_seconds);
    return 0;
}
