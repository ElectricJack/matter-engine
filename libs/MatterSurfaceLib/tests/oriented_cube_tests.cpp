#include "oriented_cube_algorithm.h"
#include "raylib.h"   // MemFree
#include <cstdio>
#include <cmath>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static MeshContext make_ctx(const std::vector<Particle>& ps, const std::vector<float4>& tints) {
    MeshContext ctx{ps, tints, 1.0f,
                    Bounds{}, CellBounds{}, 1.0f,
                    0.0f, nullptr, 0, nullptr, 0, 0.0f, 1.0f,
                    nullptr, 7u};
    return ctx;
}

int main() {
    OrientedCubeAlgorithm algo;

    Particle p; p.position = Vector3{2.0f, -1.0f, 0.5f}; p.radius = 0.5f; p.materialId = 13;
    std::vector<Particle> ps{p};
    std::vector<float4> tints{make_float4(1.0f, 1.0f, 1.0f, 0.0f)};

    GroupMeshResult r = algo.generate(make_ctx(ps, tints));

    CHECK(r.group_id == 7u, "group_id passes through");
    CHECK(r.mesh.vertexCount == 24, "one cube => 24 vertices");
    CHECK(r.mesh.triangleCount == 12, "one cube => 12 triangles");
    CHECK(r.triangles.size() == 12, "one cube => 12 Tri");
    CHECK(r.triangle_normals.size() == 12, "one cube => 12 TriEx");

    bool tagged = true, unit_normals = true, flat = true;
    for (size_t t = 0; t < r.triangle_normals.size(); ++t) {
        const TriEx& ex = r.triangle_normals[t];
        if (ex.materialId != 13) tagged = false;
        float len = sqrtf(ex.N0.x*ex.N0.x + ex.N0.y*ex.N0.y + ex.N0.z*ex.N0.z);
        if (fabsf(len - 1.0f) > 1e-3f) unit_normals = false;
        if (ex.N0.x != ex.N1.x || ex.N0.y != ex.N1.y || ex.N0.z != ex.N1.z ||
            ex.N0.x != ex.N2.x || ex.N0.y != ex.N2.y || ex.N0.z != ex.N2.z) flat = false;
    }
    CHECK(tagged, "all triangles tagged with source material 13");
    CHECK(unit_normals, "all face normals are unit length");
    CHECK(flat, "each triangle's three vertex normals equal its face normal");

    // Vertices are within the cube's circumradius of the particle center.
    float circum = p.radius * 1.7321f + 1e-3f; // half-edge*sqrt(3)
    bool in_range = true;
    for (int v = 0; v < r.mesh.vertexCount; ++v) {
        float dx = r.mesh.vertices[v*3+0] - p.position.x;
        float dy = r.mesh.vertices[v*3+1] - p.position.y;
        float dz = r.mesh.vertices[v*3+2] - p.position.z;
        if (sqrtf(dx*dx+dy*dy+dz*dz) > circum) in_range = false;
    }
    CHECK(in_range, "all vertices within cube circumradius of center");

    // Determinism: a second build of the same particle is byte-identical.
    GroupMeshResult r2 = algo.generate(make_ctx(ps, tints));
    bool deterministic = (r2.mesh.vertexCount == r.mesh.vertexCount);
    for (int i = 0; deterministic && i < r.mesh.vertexCount*3; ++i) {
        if (r.mesh.vertices[i] != r2.mesh.vertices[i]) deterministic = false;
    }
    CHECK(deterministic, "orientation is stable across re-meshes (deterministic)");

    // Empty group yields an empty result (vertexCount 0 => commit skips it).
    std::vector<Particle> none; std::vector<float4> none_t;
    GroupMeshResult empty = algo.generate(make_ctx(none, none_t));
    CHECK(empty.mesh.vertexCount == 0, "empty group => empty mesh");

    MemFree(r.mesh.vertices);  MemFree(r.mesh.normals);  MemFree(r.mesh.colors);  MemFree(r.mesh.indices);
    MemFree(r2.mesh.vertices); MemFree(r2.mesh.normals); MemFree(r2.mesh.colors); MemFree(r2.mesh.indices);

    if (failures == 0) printf("All oriented_cube tests passed\n");
    return failures == 0 ? 0 : 1;
}
