// Tests for the Taubin lambda/mu smoothing pass (mesh_smooth.hpp).
#include "mesh_smooth.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

namespace {

// Signed volume of a closed mesh: sum over tris of dot(v0, cross(v1, v2)) / 6.
double signed_volume(const MeshIndexed& m) {
    double vol = 0.0;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        float3 a = m.positions[m.indices[t]];
        float3 b = m.positions[m.indices[t + 1]];
        float3 c = m.positions[m.indices[t + 2]];
        double cx = (double)b.y * c.z - (double)b.z * c.y;
        double cy = (double)b.z * c.x - (double)b.x * c.z;
        double cz = (double)b.x * c.y - (double)b.y * c.x;
        vol += a.x * cx + a.y * cy + a.z * cz;
    }
    return vol / 6.0;
}

// Unit octahedron subdivided `levels` times, midpoints projected to the unit
// sphere. Closed, manifold, no boundary.
MeshIndexed make_sphere(int levels) {
    MeshIndexed m;
    m.positions = {
        make_float3( 1, 0, 0), make_float3(-1, 0, 0),
        make_float3( 0, 1, 0), make_float3( 0,-1, 0),
        make_float3( 0, 0, 1), make_float3( 0, 0,-1),
    };
    m.indices = { 0,2,4, 2,1,4, 1,3,4, 3,0,4,
                  2,0,5, 1,2,5, 3,1,5, 0,3,5 };
    for (int l = 0; l < levels; ++l) {
        std::map<std::pair<uint32_t,uint32_t>, uint32_t> mid;
        auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
            auto key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            float3 pa = m.positions[a], pb = m.positions[b];
            float3 p = make_float3((pa.x+pb.x)*0.5f, (pa.y+pb.y)*0.5f, (pa.z+pb.z)*0.5f);
            float len = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
            p = make_float3(p.x/len, p.y/len, p.z/len);
            uint32_t idx = (uint32_t)m.positions.size();
            m.positions.push_back(p);
            mid[key] = idx;
            return idx;
        };
        std::vector<uint32_t> out;
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            uint32_t a = m.indices[t], b = m.indices[t+1], c = m.indices[t+2];
            uint32_t ab = midpoint(a,b), bc = midpoint(b,c), ca = midpoint(c,a);
            uint32_t tri[12] = { a,ab,ca, b,bc,ab, c,ca,bc, ab,bc,ca };
            out.insert(out.end(), tri, tri + 12);
        }
        m.indices = out;
    }
    return m;
}

// Open (n+1)x(n+1) grid in the XY plane — has a boundary ring.
MeshIndexed make_grid(int n) {
    MeshIndexed m;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(make_float3((float)x, (float)y, 0.0f));
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            uint32_t a = (uint32_t)(y*(n+1)+x), b = a+1,
                     c = a+(uint32_t)(n+1),     d = c+1;
            uint32_t tri[6] = { a,b,d, a,d,c };
            m.indices.insert(m.indices.end(), tri, tri + 6);
        }
    return m;
}

void test_volume_preserved() {
    MeshIndexed m = make_sphere(2);
    double v0 = signed_volume(m);
    SmoothOptions opts; opts.iterations = 5;
    SmoothResult r = smooth(m, opts);
    assert(r.ok);
    double v1 = signed_volume(r.mesh);
    assert(std::fabs(v1 - v0) / std::fabs(v0) < 0.05);  // Taubin is shrink-free
    printf("  volume %.6f -> %.6f\n", v0, v1);
}

void test_counts_unchanged_no_nans() {
    MeshIndexed m = make_sphere(1);
    SmoothResult r = smooth(m, {});
    assert(r.ok);
    assert(r.mesh.positions.size() == m.positions.size());
    assert(r.mesh.indices == m.indices);   // connectivity untouched
    bool moved = false;
    for (size_t i = 0; i < m.positions.size(); ++i) {
        const float3& p = r.mesh.positions[i];
        assert(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        if (p.x != m.positions[i].x || p.y != m.positions[i].y || p.z != m.positions[i].z)
            moved = true;
    }
    assert(moved);  // it actually did something
}

void test_determinism() {
    MeshIndexed m = make_sphere(2);
    SmoothResult a = smooth(m, {});
    SmoothResult b = smooth(m, {});
    assert(a.ok && b.ok);
    assert(a.mesh.positions.size() == b.mesh.positions.size());
    for (size_t i = 0; i < a.mesh.positions.size(); ++i) {
        assert(a.mesh.positions[i].x == b.mesh.positions[i].x);
        assert(a.mesh.positions[i].y == b.mesh.positions[i].y);
        assert(a.mesh.positions[i].z == b.mesh.positions[i].z);
    }
}

void test_boundary_fixed() {
    const int n = 4;
    MeshIndexed m = make_grid(n);
    SmoothResult r = smooth(m, {});
    assert(r.ok);
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            if (!(x == 0 || y == 0 || x == n || y == n)) continue;
            size_t i = (size_t)y*(n+1)+x;
            assert(r.mesh.positions[i].x == m.positions[i].x);
            assert(r.mesh.positions[i].y == m.positions[i].y);
            assert(r.mesh.positions[i].z == m.positions[i].z);
        }
}

void test_triex_normals_recomputed() {
    MeshIndexed m = make_sphere(1);
    m.triex.resize(m.indices.size() / 3);
    for (TriEx& e : m.triex) { e = TriEx{}; e.materialId = 3; }
    SmoothResult r = smooth(m, {});
    assert(r.ok);
    assert(r.mesh.triex.size() == m.triex.size());
    for (const TriEx& e : r.mesh.triex) {
        assert(e.materialId == 3);  // non-normal TriEx fields untouched
        float len = std::sqrt(e.N0.x*e.N0.x + e.N0.y*e.N0.y + e.N0.z*e.N0.z);
        assert(std::fabs(len - 1.0f) < 1e-3f);  // unit, non-NaN
    }
}

void test_error_paths() {
    MeshIndexed empty;
    assert(!smooth(empty, {}).ok);

    MeshIndexed m = make_sphere(0);
    SmoothOptions bad;
    bad.iterations = 0;            assert(!smooth(m, bad).ok);
    bad = {}; bad.lambda = -0.1f;  assert(!smooth(m, bad).ok);
    bad = {}; bad.mu = 0.1f;       assert(!smooth(m, bad).ok);

    MeshIndexed oob = m; oob.indices[0] = 9999;
    assert(!smooth(oob, {}).ok);

    MeshIndexed ragged = m; ragged.indices.pop_back();
    assert(!smooth(ragged, {}).ok);

    MeshIndexed badex = m; badex.triex.resize(1);  // wrong parallel size
    assert(!smooth(badex, {}).ok);
}

} // namespace

int main() {
    printf("mesh_smooth_tests\n");
    test_volume_preserved();
    test_counts_unchanged_no_nans();
    test_determinism();
    test_boundary_fixed();
    test_triex_normals_recomputed();
    test_error_paths();
    printf("all mesh_smooth tests passed\n");
    return 0;
}
