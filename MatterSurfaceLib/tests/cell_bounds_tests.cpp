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
#include <vector>
#include "raylib.h"
#include "cell.h"
#include "cluster.h"

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

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
