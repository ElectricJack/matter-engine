// Determinism harness: build_cell_meshes output must be identical regardless of
// MeshWorkerPool worker count. GL-free (no UploadMesh / no commit).
#include "../include/cell.h"
#include "../include/cluster.h"        // StaticParticle
#include "../include/mesh_worker_pool.h"
#include "../include/surface.h"        // CreateSurfaceScratch / DestroySurfaceScratch
#include <cstdio>
#include <cmath>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++g_failures; } } while(0)

// Build one cell's worth of particle indices for a fixed scene.
static void make_scene(Cell& cell, std::vector<StaticParticle>& particles) {
    // A small cluster of same-material particles inside the cell, plus a couple
    // offset ones, so meshing produces a non-trivial multi-triangle surface.
    const float r = 0.6f;
    Vector3 c = cell.center;
    auto add = [&](float dx, float dy, float dz, uint32_t mat) {
        uint32_t idx = (uint32_t)particles.size();
        particles.push_back(StaticParticle(Vector3{c.x+dx, c.y+dy, c.z+dz}, r, mat));
        cell.add_particle_index(idx, mat);
    };
    add(-0.4f, 0.0f, 0.0f, 0);
    add( 0.4f, 0.0f, 0.0f, 0);
    add( 0.0f, 0.4f, 0.0f, 0);
    add( 0.0f,-0.4f, 0.0f, 0);
    add( 0.0f, 0.0f, 0.5f, 0);
}

// Run build_cell_meshes for the cell across `workers` threads; return the result.
static CellMeshResult run_build(Cell& cell, const std::vector<StaticParticle>& particles,
                                int workers, float simplification_ratio) {
    MeshWorkerPool pool(workers);
    std::vector<CellJob> jobs;
    CellJob job;
    job.cell = &cell;
    job.simplification_ratio = simplification_ratio;
    job.base_detail = 0.0f;
    job.max_pow = 6;
    job.uniform_detail = 0.0f;
    jobs.push_back(job);

    std::vector<CellMeshResult> results;
    pool.run(jobs, results, [&](const CellJob& j, SurfaceScratch* scratch, CellMeshResult& out) {
        out = j.cell->build_cell_meshes(particles, scratch, j.simplification_ratio,
                                        j.base_detail, j.max_pow, j.uniform_detail, nullptr, 0);
    });
    return std::move(results[0]);
}

static void compare(const CellMeshResult& a, const CellMeshResult& b) {
    CHECK(a.groups.size() == b.groups.size(), "group count differs between W=1 and W=N");
    if (a.groups.size() != b.groups.size()) return;
    for (size_t g = 0; g < a.groups.size(); ++g) {
        const GroupMeshResult& ga = a.groups[g];
        const GroupMeshResult& gb = b.groups[g];
        CHECK(ga.group_id == gb.group_id, "group_id differs");
        CHECK(ga.mesh.vertexCount == gb.mesh.vertexCount, "vertexCount differs");
        CHECK(ga.mesh.triangleCount == gb.mesh.triangleCount, "triangleCount differs");
        CHECK(ga.triangles.size() == gb.triangles.size(), "triangle array size differs");
        if (ga.mesh.vertexCount != gb.mesh.vertexCount) continue;
        if (!ga.mesh.vertices || !gb.mesh.vertices) { CHECK(false, "null vertices"); continue; }
        for (int v = 0; v < ga.mesh.vertexCount * 3; ++v) {
            if (ga.mesh.vertices[v] != gb.mesh.vertices[v]) {
                printf("FAIL: vertex float %d differs (%.9g vs %.9g)\n", v, ga.mesh.vertices[v], gb.mesh.vertices[v]);
                ++g_failures;
                break;
            }
        }
    }
}

// Build the same fixed scene at W=1 and W=4 for a given simplification ratio and
// assert byte-identical CPU geometry. Exercises the worker mesh path; ratio<1.0
// also drives the in-worker simplify branch (simplify_mesh + unload_cpu_mesh).
static size_t check_ratio(float ratio) {
    const float cell_size = 4.0f;

    Cell cell_a(Vector3{0,0,0}, 0, cell_size);
    std::vector<StaticParticle> particles_a;
    make_scene(cell_a, particles_a);

    Cell cell_b(Vector3{0,0,0}, 0, cell_size);
    std::vector<StaticParticle> particles_b;
    make_scene(cell_b, particles_b);

    CellMeshResult serial   = run_build(cell_a, particles_a, 1, ratio);
    CellMeshResult parallel = run_build(cell_b, particles_b, 4, ratio);

    CHECK(!serial.groups.empty(), "serial build produced no groups (scene meshed empty)");
    compare(serial, parallel);
    return serial.groups.size();
}

// A single sand particle (material 13) must be meshed by the oriented-cube
// algorithm rather than marching cubes: exactly one group of 12 triangles,
// each tagged with the source material. Exercises material-driven dispatch
// through the real cell pipeline.
static void test_oriented_cube_material_path() {
    Cell cell(Vector3{0,0,0}, 0, 1.0f);
    std::vector<StaticParticle> particles;
    StaticParticle sp{};
    sp.position = Vector3{0.5f, 0.5f, 0.5f};
    sp.radius = 0.3f;
    sp.materialId = 13;
    sp.tint = Vector4{1,1,1,0};
    sp.detail_size = 0.0f;
    particles.push_back(sp);
    cell.add_particle_index(0, 13);

    SurfaceScratch* scratch = CreateSurfaceScratch();
    CellMeshResult res = cell.build_cell_meshes(particles, scratch,
                                                1.0f, 1.0f, 6, 0.0f, nullptr, 0);
    DestroySurfaceScratch(scratch);

    CHECK(res.groups.size() == 1, "sand cell should produce exactly one group");
    if (res.groups.size() == 1) {
        const GroupMeshResult& g = res.groups[0];
        CHECK(g.triangle_normals.size() == 12, "one sand cube => 12 triangles");
        bool tagged = true;
        for (const TriEx& ex : g.triangle_normals) if (ex.materialId != 13) tagged = false;
        CHECK(tagged, "cube triangles tagged with material 13");
    }
}

int main() {
    size_t groups_full   = check_ratio(1.0f);   // no simplification
    size_t groups_simpl  = check_ratio(0.5f);   // drives worker simplify path
    test_oriented_cube_material_path();          // material-driven cube dispatch

    if (g_failures == 0) {
        printf("PARALLEL DETERMINISM: PASS (groups=%zu, simplified groups=%zu)\n",
               groups_full, groups_simpl);
        return 0;
    }
    printf("PARALLEL DETERMINISM: FAIL (%d failures)\n", g_failures);
    return 1;
}
