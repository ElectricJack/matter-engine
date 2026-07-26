#include <cstdio>
#include <vector>
#include <array>
#include <chrono>
#include <cmath>

static const float kPI = 3.14159265358979323846f;

#include "raylib.h"
#include "mesh_simplifier.hpp"

static Mesh makeMesh(const std::vector<float>& v, const std::vector<unsigned short>& idx) {
    Mesh m = {0};
    m.vertexCount = (int)(v.size() / 3);
    m.triangleCount = (int)(idx.size() / 3);
    m.vertices = (float*)MemAlloc(sizeof(float) * v.size());
    for (size_t i = 0; i < v.size(); ++i) m.vertices[i] = v[i];
    m.indices = (unsigned short*)MemAlloc(sizeof(unsigned short) * idx.size());
    for (size_t i = 0; i < idx.size(); ++i) m.indices[i] = idx[i];
    return m;
}

static Mesh makeGrid(int n, float span) {
    std::vector<float> v;
    std::vector<unsigned short> idx;
    int side = n + 1;
    for (int j = 0; j < side; ++j)
        for (int i = 0; i < side; ++i) {
            v.push_back(span * (float)i / (float)n);
            v.push_back(span * (float)j / (float)n);
            v.push_back(0.0f);
        }
    auto vid = [&](int i, int j) { return (unsigned short)(j*side + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            idx.push_back(vid(i, j));   idx.push_back(vid(i+1, j));   idx.push_back(vid(i+1, j+1));
            idx.push_back(vid(i, j));   idx.push_back(vid(i+1, j+1)); idx.push_back(vid(i, j+1));
        }
    return makeMesh(v, idx);
}

// Closed UV sphere (triangle soup, unwelded) — like a marching-cubes blob:
// decimating it creates high-valence hub vertices, the worst case for the
// incidence-list bloat.
static Mesh makeSphere(int rings, int sectors, float radius) {
    std::vector<float> verts; // ring/sector grid of positions
    auto P = [&](int r, int s) {
        float phi   = kPI * (float)r / (float)rings;       // 0..pi
        float theta = 2.0f * kPI * (float)s / (float)sectors;
        float x = radius * sinf(phi) * cosf(theta);
        float y = radius * cosf(phi);
        float z = radius * sinf(phi) * sinf(theta);
        return std::array<float,3>{x, y, z};
    };
    std::vector<float> v;
    auto push = [&](std::array<float,3> p) { v.push_back(p[0]); v.push_back(p[1]); v.push_back(p[2]); };
    std::vector<unsigned short> idx;
    int next = 0;
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < sectors; ++s) {
            auto a = P(r, s), b = P(r+1, s), c = P(r+1, s+1), d = P(r, s+1);
            push(a); push(b); push(c); idx.push_back(next++); idx.push_back(next++); idx.push_back(next++);
            push(a); push(c); push(d); idx.push_back(next++); idx.push_back(next++); idx.push_back(next++);
        }
    return makeMesh(v, idx);
}

int main() {
    printf("--- flat grid, ratio 0.5 ---\n");
    for (int n : {20, 40, 60, 80, 120}) {
        Mesh in = makeGrid(n, 10.0f);
        SimplifyOptions o; o.target_ratio = 0.5f;
        auto t0 = std::chrono::steady_clock::now();
        Mesh out = simplify_mesh(in, o);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("n=%-4d inTris=%-6d outTris=%-6d  %.1f ms\n",
               n, in.triangleCount, out.triangleCount, ms);
        fflush(stdout);
        UnloadMesh(in); UnloadMesh(out);
    }
    printf("--- UV sphere, aggressive ratio 0.1 (high-valence hubs) ---\n");
    for (int r : {30, 50, 70, 90}) {
        Mesh in = makeSphere(r, r*2, 5.0f);
        SimplifyOptions o; o.target_ratio = 0.1f;
        auto t0 = std::chrono::steady_clock::now();
        Mesh out = simplify_mesh(in, o);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("rings=%-3d inTris=%-6d outTris=%-6d  %.1f ms\n",
               r, in.triangleCount, out.triangleCount, ms);
        fflush(stdout);
        UnloadMesh(in); UnloadMesh(out);
    }
    return 0;
}
