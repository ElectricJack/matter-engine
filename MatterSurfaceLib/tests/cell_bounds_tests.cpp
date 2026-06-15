// Regression tests for Cell geometric bounds vs. the cluster's cell-coordinate
// convention.
//
// The cluster addresses cells with a CORNER convention: a point at local
// position p belongs to cell index C = floor(p / cell_size), so cell C occupies
// the box [C*s, (C+1)*s] and its center is (C+0.5)*s. This is what
// Cluster::get_cell_coordinates and the cell spatial-hash key both use.
//
// Cell::calculate_bounds must agree, otherwise a particle that lands in the
// upper part of its coordinate cell is not actually covered by that cell's
// mesh-generation box -> the marching-cubes volume is shifted half a cell and
// spheres render as partial blobs (the "partial spheres when adding particles"
// bug, which reproduces even with simplification disabled at ratio 1.0).

#include <cstdio>
#include <cmath>
#include <map>
#include <vector>
#include "raylib.h"
#include "cell.h"
#include "cluster.h"
#include "material_registry.h"

static int g_failures = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        printf("  FAIL: %s\n", msg);
        ++g_failures;
    } else {
        printf("  ok:   %s\n", msg);
    }
}

// Mirror of Cluster::get_cell_coordinates (src/cluster.cpp:163-169).
static Vector3 owning_coords(Vector3 p, float cell_size) {
    return Vector3{ floorf(p.x / cell_size), floorf(p.y / cell_size), floorf(p.z / cell_size) };
}

// Materials that share a mergeGroup must bucket into ONE entry of the cell's
// material_particle_indices map; materials in a different group must occupy a
// separate bucket. materialId 8 (stone_light) and 9 (stone_dark) share
// GROUP_STONE; materialId 4 (glass) is its own group.
//
// Cell::add_particle_index keys material_particle_indices by
// MaterialMergeGroup(material_id) (cell.cpp), so we can assert on the bucket
// count directly -- no GL context / UploadMesh / rebuild_meshes needed.
static void test_shades_merge_one_mesh() {
    printf("--- materials sharing a merge group share one bucket ---\n");

    const float smallest = 4.0f;  // size_power 0 -> actual_size == 4.0
    const int   size_pow = 0;
    const float s = smallest;

    // Cell (0,0,0) spans [0,4]^3, centered at (2,2,2).
    Cell cell(Vector3{0, 0, 0}, size_pow, smallest);

    // Two stone shades, close together near the cell center.
    std::vector<StaticParticle> particles;
    particles.push_back(StaticParticle(Vector3{0.45f * s, 0.5f * s, 0.5f * s}, 0.8f, 8)); // stone_light
    particles.push_back(StaticParticle(Vector3{0.55f * s, 0.5f * s, 0.5f * s}, 0.8f, 9)); // stone_dark

    cell.add_particle_index(0, particles[0].materialId);
    cell.add_particle_index(1, particles[1].materialId);

    check(cell.material_particle_indices.size() == 1,
          "stone_light(8) + stone_dark(9) share a single merge-group bucket");

    // Add a glass particle (materialId 4, GROUP_GLASS) -> distinct group.
    particles.push_back(StaticParticle(Vector3{0.5f * s, 0.5f * s, 0.5f * s}, 0.8f, 4)); // glass
    cell.add_particle_index(2, particles[2].materialId);

    check(cell.material_particle_indices.size() == 2,
          "adding glass(4, different group) yields a second bucket (2 total)");
}

// Task 3.3: transparency-gated foreign clip-set construction.
//
// build_clip_particles (cell.h) gathers the OTHER merge groups' particles in a
// cell into the clip set used to carve THIS group's surface, but ONLY when the
// carve is transparency-relevant: G carves iff G is transparent OR the foreign
// group F is transparent. opaque<->opaque pairs are skipped (no carve).
//
// This is the GL-free extraction of the logic generate_mesh_for_group uses, so
// the production path and the tested path are the same code. Voxel-derived
// cull/vis radii mirror generate_mesh_for_group's taper.
static void test_clip_set_transparency_gating() {
    printf("--- clip-set construction is transparency-gated ---\n");

    // LOD radii small enough that every particle below survives the taper.
    const float cull_radius = 0.01f;
    const float vis_radius  = 0.05f;

    // Bucket map keyed by MERGE GROUP (as the cell builds it). glass(4) and
    // metal(3) are distinct groups; stone(8) is its own group.
    auto bucket_of = [](int matId) { return (uint32_t)MaterialMergeGroup(matId); };

    // --- Case A: glass group adjacent to metal group (glass is transparent). ---
    {
        std::vector<StaticParticle> cluster;
        cluster.push_back(StaticParticle(Vector3{0, 0, 0}, 0.8f, 4)); // glass
        cluster.push_back(StaticParticle(Vector3{1, 0, 0}, 0.8f, 3)); // metal
        std::map<uint32_t, std::vector<uint32_t>> buckets;
        buckets[bucket_of(4)].push_back(0);
        buckets[bucket_of(3)].push_back(1);

        // Clip set for the glass group: foreign metal is relevant (glass transparent).
        std::vector<Particle> glass_clip =
            build_clip_particles(bucket_of(4), buckets, cluster,
                                 /*group_transparent=*/true, cull_radius, vis_radius);
        check(!glass_clip.empty(),
              "glass group gets a non-empty clip (foreign metal carves transparent glass)");

        // Clip set for the metal group: foreign glass is transparent -> still relevant.
        std::vector<Particle> metal_clip =
            build_clip_particles(bucket_of(3), buckets, cluster,
                                 /*group_transparent=*/false, cull_radius, vis_radius);
        check(!metal_clip.empty(),
              "metal group gets a non-empty clip (foreign glass is transparent -> carve)");
    }

    // --- Case B: two OPAQUE groups (stone 8 vs metal 3): no carve either way. ---
    {
        std::vector<StaticParticle> cluster;
        cluster.push_back(StaticParticle(Vector3{0, 0, 0}, 0.8f, 8)); // stone (opaque)
        cluster.push_back(StaticParticle(Vector3{1, 0, 0}, 0.8f, 3)); // metal (opaque)
        std::map<uint32_t, std::vector<uint32_t>> buckets;
        buckets[bucket_of(8)].push_back(0);
        buckets[bucket_of(3)].push_back(1);

        std::vector<Particle> stone_clip =
            build_clip_particles(bucket_of(8), buckets, cluster,
                                 /*group_transparent=*/false, cull_radius, vis_radius);
        check(stone_clip.empty(),
              "opaque stone vs opaque metal: stone clip is EMPTY (no carve)");

        std::vector<Particle> metal_clip =
            build_clip_particles(bucket_of(3), buckets, cluster,
                                 /*group_transparent=*/false, cull_radius, vis_radius);
        check(metal_clip.empty(),
              "opaque metal vs opaque stone: metal clip is EMPTY (no carve)");
    }
}

// Task 3.3: the clip set built by build_clip_particles actually carves the
// surface end-to-end. GenerateMesh is GL-free (returns a CPU-side Mesh with
// .vertices), so we mesh the glass group WITH and WITHOUT the clip set built by
// the helper and assert the glass extent TOWARD the metal is pulled inward.
static void test_built_clip_carves_surface() {
    printf("--- clip set from build_clip_particles carves the surface ---\n");

    const float cull_radius = 0.01f;
    const float vis_radius  = 0.05f;
    const float blendWidth  = 0.10f; // small non-zero -> realistic blended path

    // Glass at origin, metal foreign neighbor on +x.
    std::vector<StaticParticle> cluster;
    cluster.push_back(StaticParticle(Vector3{0.0f, 0, 0}, 1.0f, 4)); // glass
    cluster.push_back(StaticParticle(Vector3{1.2f, 0, 0}, 1.0f, 3)); // metal

    auto bucket_of = [](int matId) { return (uint32_t)MaterialMergeGroup(matId); };
    std::map<uint32_t, std::vector<uint32_t>> buckets;
    buckets[bucket_of(4)].push_back(0);
    buckets[bucket_of(3)].push_back(1);

    // The glass group's own surface particle.
    Particle g[1];
    g[0].position = Vector3{0.0f, 0, 0};
    g[0].radius   = 1.0f;
    g[0].materialId = 4;

    // Clip set from the helper (foreign metal, glass is transparent -> non-empty).
    std::vector<Particle> clip =
        build_clip_particles(bucket_of(4), buckets, cluster,
                             /*group_transparent=*/true, cull_radius, vis_radius);
    check(!clip.empty(), "helper produced a non-empty clip for the glass group");

    Bounds b;
    b.center = Vector3{0, 0, 0};
    b.size   = Vector3{5, 5, 5};
    b.divisionPow = 4;

    Mesh open   = GenerateMesh(g, 1.0f, 1, b, blendWidth, NULL, 0, NULL, 0, 0.0f);
    Mesh carved = GenerateMesh(g, 1.0f, 1, b, blendWidth,
                               clip.empty() ? NULL : clip.data(), (int)clip.size(), NULL, 0, 0.0f);

    float maxOpen = -1e9f, maxCarved = -1e9f;
    for (int i = 0; i < open.vertexCount; ++i)
        maxOpen = fmaxf(maxOpen, open.vertices[i * 3]);
    for (int i = 0; i < carved.vertexCount; ++i)
        maxCarved = fmaxf(maxCarved, carved.vertices[i * 3]);

    char buf[160];
    snprintf(buf, sizeof(buf),
             "clip carves +x extent inward (open=%.3f carved=%.3f)", maxOpen, maxCarved);
    check(maxCarved < maxOpen - 0.05f, buf);
}

static void test_choose_division_pow() {
    printf("--- choose_division_pow derives resolution from detail ---\n");
    const float S = 0.8f;          // base (tier-0) spacing
    const int base_pow = 4, max_pow = 6;
    check(choose_division_pow(S,        S, base_pow, max_pow) == 4, "tier 0 (detail==S) -> pow 4");
    check(choose_division_pow(S * 0.5f, S, base_pow, max_pow) == 5, "tier 1 (detail==S/2) -> pow 5");
    check(choose_division_pow(S * 0.25f,S, base_pow, max_pow) == 6, "tier 2 (detail==S/4) -> pow 6");
    check(choose_division_pow(S * 0.125f,S, base_pow, max_pow) == 6, "tier 3 clamps to max_pow 6");
    check(choose_division_pow(0.0f,     S, base_pow, max_pow) == 4, "absent detail (0) -> base pow");
    check(choose_division_pow(S * 2.0f, S, base_pow, max_pow) == 4, "coarser-than-base detail -> base pow");
}

int main() {
    const float smallest = 4.0f;  // size_power 0 -> actual_size == 4.0
    const int   size_pow = 0;
    const float s = smallest;

    printf("--- a particle's owning cell must contain the particle ---\n");
    // Sweep positions across several cells, including the upper half of each
    // cell where the center-vs-corner mismatch bites.
    for (float p = -10.0f; p <= 10.0f; p += 0.5f) {
        Vector3 pos{p, p, p};
        Vector3 c = owning_coords(pos, s);
        Cell cell(c, size_pow, smallest);
        char buf[128];
        snprintf(buf, sizeof(buf), "pos=%.1f -> coord=%.0f bounds=[%.1f,%.1f] contains pos",
                 p, c.x, cell.min_bound.x, cell.max_bound.x);
        check(cell.contains_point(pos), buf);
    }

    printf("--- a sphere smaller than a cell, centered in its owning cell, is fully inside that cell ---\n");
    // Place a particle at the corner-convention center of cell (0,0,0): (2,2,2).
    // radius 1.5 < s/2, so the whole sphere lies within [0,4]^3 -> exactly one
    // cell needs meshing. With the bug the cell box is [-2,2]^3 and the sphere
    // pokes out to 3.5, so part of it has no covering cell.
    {
        Vector3 center{0.5f * s, 0.5f * s, 0.5f * s}; // (2,2,2)
        float r = 1.5f;
        Vector3 coord = owning_coords(center, s);
        Cell cell(coord, size_pow, smallest);
        bool fully_inside =
            cell.min_bound.x <= center.x - r && center.x + r <= cell.max_bound.x &&
            cell.min_bound.y <= center.y - r && center.y + r <= cell.max_bound.y &&
            cell.min_bound.z <= center.z - r && center.z + r <= cell.max_bound.z;
        check(fully_inside, "sphere at owning-cell center stays within that cell's bounds");
    }

    test_shades_merge_one_mesh();
    test_clip_set_transparency_gating();
    test_built_clip_carves_surface();
    test_choose_division_pow();

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
