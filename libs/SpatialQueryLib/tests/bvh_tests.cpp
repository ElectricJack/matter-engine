// libs/SpatialQueryLib/tests/bvh_tests.cpp
//
// Headless regression tests for the BLAS/TLAS builders in `src/bvh.cpp` and the
// diagnostic walk in `src/bvh_analyzer.cpp`. No GPU, no window, no files.
//
// Build and run with `make -C libs/SpatialQueryLib test-bvh` (or `test`, which
// runs this suite and the spatial-hash one).
//
// What is pinned here:
//   - the node pool partitions the triangle permutation EXACTLY: each leaf
//     starts where its parent's split says it should, the union of the leaves
//     is [0, triCount) with each slot claimed once, and `triIdx` stays a
//     permutation. That is the invariant `Subdivide`'s partition boundary has
//     to preserve, on the binned path and on the median-rescue path alike.
//   - the depth cap that keeps the 64-entry traversal stack safe.
//   - `TLAS(list, 0)` is a valid empty tree rather than a null dereference.
//   - `AnalyzeBVH` survives a triangle-free mesh, whose root node is marked
//     interior and points at itself, instead of recursing until the stack ends.
//   - `avg_instance_triangles` reports a real number, and the report generators
//     emit real newlines rather than the two characters backslash-n.

#include "../include/bvh.h"
#include "../include/bvh_analyzer.h"

#include <cstdio>
#include <string>
#include <vector>

static int tests_run = 0;
static int tests_passed = 0;
static bool current_ok = true;

#define CHECK(cond, msg) do { \
    if (!(cond)) { current_ok = false; printf("  FAIL: %s\n", (msg)); } \
} while (0)

#define RUN_TEST(name) do { \
    tests_run++; current_ok = true; \
    printf("[TEST] %s\n", #name); \
    name(); \
    if (current_ok) { tests_passed++; printf("  PASS\n"); } \
} while (0)

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Deterministic LCG, re-seeded per fixture, so every run and every platform
// builds the same tree.
static unsigned int rng_state = 0u;
static float rnd01() {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)((rng_state >> 8) & 0xFFFFFFu) / (float)0x1000000u;
}

// A cloud of small triangles scattered through a 10-unit cube: no coincident
// centroids, no axis alignment, so the ordinary binned split path is what runs.
static void fill_scattered(BvhMesh& mesh, int count) {
    rng_state = 0x13579bdfu;
    for (int i = 0; i < count; i++) {
        float ox = rnd01() * 10.0f, oy = rnd01() * 10.0f, oz = rnd01() * 10.0f;
        mesh.tri[i].vertex0 = make_float3(ox, oy, oz);
        mesh.tri[i].vertex1 = make_float3(ox + 0.1f + rnd01() * 0.05f, oy, oz);
        mesh.tri[i].vertex2 = make_float3(ox, oy + 0.1f + rnd01() * 0.05f, oz + 0.02f);
    }
}

// Every triangle identical, so no axis has any centroid extent and
// FindBestSplitPlane can propose nothing. This is the "no valid split" arm.
static void fill_coincident(BvhMesh& mesh, int count) {
    for (int i = 0; i < count; i++) {
        mesh.tri[i].vertex0 = make_float3(1.0f, 2.0f, 3.0f);
        mesh.tri[i].vertex1 = make_float3(2.0f, 2.0f, 3.0f);
        mesh.tri[i].vertex2 = make_float3(1.0f, 3.0f, 3.0f);
    }
}

// Depth-first walk returning the number of triangles under `idx`, asserting as
// it goes that every leaf begins exactly at the offset its ancestors' splits
// imply. The right child's expected start is `expected_first + <left subtree
// size>`, which is precisely the relationship `Subdivide` has to establish when
// it writes the two children's ranges.
static uint walk_ranges(const BVH& bvh, uint idx, uint expected_first, uint depth,
                        std::vector<int>& seen, uint& max_depth) {
    if (depth > max_depth) max_depth = depth;
    if (idx >= bvh.nodesUsed) {
        current_ok = false;
        printf("  FAIL: node index %u past nodesUsed %u\n", idx, bvh.nodesUsed);
        return 0;
    }
    const BVHNode& node = bvh.bvhNode[idx];

    if (node.isLeaf()) {
        CHECK(node.leftFirst == expected_first, "leaf does not start where its parent's split says");
        for (uint i = 0; i < node.triCount; i++) {
            uint slot = node.leftFirst + i;
            if (slot >= seen.size()) {
                current_ok = false;
                printf("  FAIL: leaf range runs past the end of triIdx\n");
                return node.triCount;
            }
            seen[slot]++;
        }
        return node.triCount;
    }

    // Interior: children are the pair (leftFirst, leftFirst + 1) and are
    // bump-allocated after their parent.
    CHECK(node.leftFirst > idx, "interior child index is not after its parent");
    if (node.leftFirst <= idx || node.leftFirst + 1 >= bvh.nodesUsed) return 0;

    uint left = walk_ranges(bvh, node.leftFirst, expected_first, depth + 1, seen, max_depth);
    uint right = walk_ranges(bvh, node.leftFirst + 1, expected_first + left, depth + 1, seen, max_depth);
    return left + right;
}

// Shared assertions for "this build produced a well-formed permutation".
static void expect_well_formed(const BVH& bvh, int N) {
    std::vector<int> seen((size_t)N, 0);
    uint max_depth = 0;
    uint total = walk_ranges(bvh, 0, 0, 0, seen, max_depth);
    CHECK(total == (uint)N, "the leaves do not account for every triangle");

    bool covered = true, duplicated = false;
    for (int i = 0; i < N; i++) {
        if (seen[i] == 0) covered = false;
        if (seen[i] > 1) duplicated = true;
    }
    CHECK(covered, "some triIdx slots are unreachable from any leaf");
    CHECK(!duplicated, "a triIdx slot is claimed by more than one leaf");

    // triIdx itself must stay a permutation of [0, N).
    std::vector<int> used((size_t)N, 0);
    bool in_range = true;
    for (int i = 0; i < N; i++) {
        if (bvh.triIdx[i] >= (uint)N) { in_range = false; break; }
        used[bvh.triIdx[i]]++;
    }
    CHECK(in_range, "triIdx holds an out-of-range triangle index");
    if (in_range) {
        bool once = true;
        for (int i = 0; i < N; i++) if (used[i] != 1) once = false;
        CHECK(once, "triIdx is not a permutation of the triangle range");
    }

    // MAX_DEPTH is 40, and BVH::Intersect walks a 64-entry stack with no
    // overflow check -- that cap is the whole reason the stack is safe.
    CHECK(max_depth <= 40, "tree is deeper than the builder's depth cap");
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_leaf_ranges_tile_the_permutation() {
    const int N = 500;
    BvhMesh mesh(N);
    fill_scattered(mesh, N);
    BVH bvh(&mesh);
    expect_well_formed(bvh, N);
}

static void test_subdiv_to_one_prim_tiles_the_permutation() {
    const int N = 64;
    BvhMesh mesh(N);
    fill_scattered(mesh, N);
    BVH bvh(&mesh, /*subdiv_to_one_prim=*/true);
    expect_well_formed(bvh, N);
}

// All-coincident centroids: no axis has extent, so the root stays a leaf
// holding everything rather than producing an empty or overlapping child.
static void test_coincident_centroids_keep_one_leaf() {
    const int N = 32;
    BvhMesh mesh(N);
    fill_coincident(mesh, N);
    BVH bvh(&mesh);

    CHECK(bvh.bvhNode[0].isLeaf(), "root should stay a leaf when no split plane exists");
    CHECK(bvh.bvhNode[0].triCount == (uint)N, "root leaf lost triangles");
    CHECK(bvh.bvhNode[0].leftFirst == 0, "root leaf does not start at 0");
    expect_well_formed(bvh, N);
}

// A ray fired straight at a stack of triangles must report the nearest one,
// and the packed instPrim must unpack back to it.
static void test_ray_hits_the_nearest_triangle() {
    const int N = 3;
    BvhMesh mesh(N);
    for (int i = 0; i < N; i++) {
        float z = 10.0f * (float)(i + 1);
        mesh.tri[i].vertex0 = make_float3(-1.0f, -1.0f, z);
        mesh.tri[i].vertex1 = make_float3(1.0f, -1.0f, z);
        mesh.tri[i].vertex2 = make_float3(0.0f, 1.0f, z);
    }
    BVH bvh(&mesh);

    BVHRay ray;
    ray.O = make_float3(0.0f, 0.0f, 0.0f);
    ray.D = make_float3(0.0f, 0.0f, 1.0f);
    ray.rD = make_float3(1e30f, 1e30f, 1.0f);
    ray.hit.t = 1e30f;
    ray.hit.instPrim = 0xFFFFFFFFu;
    bvh.Intersect(ray, 0);

    CHECK(ray.hit.t < 1e29f, "ray missed a triangle it points straight at");
    CHECK(ray.hit.t > 9.5f && ray.hit.t < 10.5f, "ray did not report the NEAREST triangle");
    CHECK((ray.hit.instPrim >> 20) == 0u, "instance field is not the one passed in");
    CHECK((ray.hit.instPrim & 0xFFFFFu) == 0u, "primitive field is not the front triangle");
}

// TLAS(list, 0): the constructor floors its allocation at one node, so the
// empty-tree branch of Build() writes into real storage instead of null.
static void test_empty_tlas_is_a_valid_tree() {
    TLAS tlas(nullptr, 0);
    CHECK(tlas.tlasNode != nullptr, "empty TLAS left a null node pool");
    CHECK(tlas.nodesUsed == 1, "empty TLAS should hold exactly one node");
    if (tlas.tlasNode) {
        CHECK(tlas.tlasNode[0].isLeaf(), "empty TLAS root should be the leaf sentinel");
    }

    BVHRay ray;
    ray.O = make_float3(0.0f, 0.0f, -5.0f);
    ray.D = make_float3(0.0f, 0.0f, 1.0f);
    ray.rD = make_float3(1e30f, 1e30f, 1.0f);
    ray.hit.t = 1e30f;
    ray.hit.instPrim = 0xFFFFFFFFu;
    tlas.Intersect(ray);
    CHECK(ray.hit.t == 1e30f, "empty TLAS reported a hit");
}

// Two instances of one BLAS at different depths: the TLAS must reach both and
// report the nearer, carrying that instance's id in the top 12 bits.
static void test_tlas_traces_both_instances() {
    BvhMesh mesh(1);
    mesh.tri[0].vertex0 = make_float3(-1.0f, -1.0f, 0.0f);
    mesh.tri[0].vertex1 = make_float3(1.0f, -1.0f, 0.0f);
    mesh.tri[0].vertex2 = make_float3(0.0f, 1.0f, 0.0f);
    BVH bvh(&mesh);

    std::vector<BVHInstance> instances(2);
    instances[0] = BVHInstance(&bvh, 0);
    instances[0].SetTransform(mat4::Translate(make_float3(0.0f, 0.0f, 9.0f)));
    instances[1] = BVHInstance(&bvh, 1);
    instances[1].SetTransform(mat4::Translate(make_float3(0.0f, 0.0f, 4.0f)));

    TLAS tlas(instances.data(), 2);
    CHECK(tlas.nodesUsed == 3, "two-instance TLAS should be a root plus two leaves");

    BVHRay ray;
    ray.O = make_float3(0.0f, 0.0f, 0.0f);
    ray.D = make_float3(0.0f, 0.0f, 1.0f);
    ray.rD = make_float3(1e30f, 1e30f, 1.0f);
    ray.hit.t = 1e30f;
    ray.hit.instPrim = 0xFFFFFFFFu;
    tlas.Intersect(ray);

    CHECK(ray.hit.t > 3.5f && ray.hit.t < 4.5f, "TLAS did not report the nearer instance");
    CHECK((ray.hit.instPrim >> 20) == 1u, "hit carries the wrong instance index");
}

// AnalyzeBVH over a triangle-free mesh. Build() leaves a root marked interior
// (triCount 0) whose leftFirst is 0 -- a node pointing at itself -- so the walk
// has to refuse it, and none of the derived ratios may divide by zero.
static void test_analyze_empty_mesh_terminates() {
    BvhMesh mesh(0);
    BVH bvh(&mesh);

    BVHTreeAnalysis a = BVHAnalyzer::AnalyzeBVH(&bvh, &mesh, "empty");
    CHECK(a.total_triangles == 0, "empty mesh should report no triangles");
    CHECK(a.total_nodes == 0, "empty mesh should return the all-zero analysis");
    CHECK(a.overall_quality_score == a.overall_quality_score, "quality score is NaN");
    CHECK(a.node_utilization == a.node_utilization, "node utilization is NaN");
    CHECK(a.avg_node_surface_area == a.avg_node_surface_area, "avg node surface area is NaN");
}

// A real mesh still produces a populated, finite analysis and a printable
// report.
static void test_analyze_populated_mesh() {
    const int N = 200;
    BvhMesh mesh(N);
    fill_scattered(mesh, N);
    BVH bvh(&mesh);

    BVHTreeAnalysis a = BVHAnalyzer::AnalyzeBVH(&bvh, &mesh, "cloud");
    CHECK(a.total_triangles == (uint32_t)N, "analysis lost the triangle count");
    CHECK(a.total_nodes == bvh.nodesUsed, "analysis lost the node count");
    CHECK(a.leaf_nodes > 0, "analysis found no leaves");
    CHECK(a.max_depth <= 40, "analysis reports a tree past the depth cap");
    CHECK(a.overall_quality_score == a.overall_quality_score, "quality score is NaN");
    CHECK(a.avg_node_surface_area == a.avg_node_surface_area, "avg node surface area is NaN");

    std::string report = BVHAnalyzer::GenerateReport(a, "cloud");
    CHECK(report.find('\n') != std::string::npos, "report contains no real newline");
    CHECK(report.find("\\n") == std::string::npos, "report still contains a literal backslash-n");
}

// avg_instance_triangles is a real number now that the per-instance count is
// accumulated through BVH::TriangleCount().
static void test_tlas_analysis_reports_instance_triangles() {
    const int N = 8;
    BvhMesh mesh(N);
    fill_scattered(mesh, N);
    BVH bvh(&mesh);

    std::vector<BVHInstance> instances(2);
    instances[0] = BVHInstance(&bvh, 0);
    instances[0].SetTransform(mat4::Translate(make_float3(0.0f, 0.0f, 4.0f)));
    instances[1] = BVHInstance(&bvh, 1);
    instances[1].SetTransform(mat4::Translate(make_float3(0.0f, 0.0f, 9.0f)));
    TLAS tlas(instances.data(), 2);

    TLASAnalysis a = BVHAnalyzer::AnalyzeTLAS(&tlas, "pair");
    CHECK(a.total_instances == 2, "TLAS analysis lost the instance count");
    CHECK(a.blas_analyses.size() == 2, "TLAS analysis lost a per-instance entry");
    CHECK(a.avg_instance_triangles == (float)N, "avg_instance_triangles is not the per-instance count");

    std::string report = BVHAnalyzer::GenerateTLASReport(a, "pair");
    CHECK(report.find('\n') != std::string::npos, "TLAS report contains no real newline");
    CHECK(report.find("\\n") == std::string::npos, "TLAS report still contains a literal backslash-n");
}

int main() {
    printf("=== SpatialQueryLib BVH tests ===\n");

    RUN_TEST(test_leaf_ranges_tile_the_permutation);
    RUN_TEST(test_subdiv_to_one_prim_tiles_the_permutation);
    RUN_TEST(test_coincident_centroids_keep_one_leaf);
    RUN_TEST(test_ray_hits_the_nearest_triangle);
    RUN_TEST(test_empty_tlas_is_a_valid_tree);
    RUN_TEST(test_tlas_traces_both_instances);
    RUN_TEST(test_analyze_empty_mesh_terminates);
    RUN_TEST(test_analyze_populated_mesh);
    RUN_TEST(test_tlas_analysis_reports_instance_triangles);

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
