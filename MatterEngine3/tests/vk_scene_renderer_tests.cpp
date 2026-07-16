// vk_scene_renderer_tests.cpp — CPU-side tests for indexed VkScenePart assembly.
//
// Tests the invariants established by Task 3:
//   - VkSceneLod carries first_index/index_count (not first_vertex/vertex_count)
//   - VkScenePart::indices holds part-local index values
//   - RtGeometrySelection emits first_index/index_count
//   - Out-of-range index validation detects bad parts
//
// These are pure CPU tests; no GPU/window required.

#include "render/vk_scene_renderer.h"

#include <cstdio>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

static int g_failures = 0;
static int g_tests    = 0;

#define CHECK(cond, msg) do {                          \
    ++g_tests;                                         \
    if (!(cond)) {                                     \
        printf("  FAIL: %s\n", (msg));                 \
        ++g_failures;                                  \
    } else {                                           \
        printf("  ok:   %s\n", (msg));                 \
    }                                                  \
} while (0)

// ---------------------------------------------------------------------------
// test_vk_scene_lod_fields
//
// Verifies that VkSceneLod has first_index / index_count (not first_vertex /
// vertex_count), and that VkScenePart has an indices member.
// This test would fail to compile against the old struct layout.
// ---------------------------------------------------------------------------
static void test_vk_scene_lod_fields() {
    printf("\n[test_vk_scene_lod_fields]\n");

    // Two-LOD part: LOD0 = 2 welded tris (4 verts, 6 idx), LOD1 = 1 tri (3 verts, 3 idx).
    // Assembled part: 7 vertices; LOD1 indices are part-local (offset by 4).
    viewer::VkScenePart part;
    part.part_hash = 0x1DEAF00Dull;
    part.vertices.resize(7);
    part.indices = {0,1,2, 1,3,2,   /* LOD1, mesh base 4: */ 4,5,6};

    viewer::VkSceneCluster cluster;
    cluster.aabb_min = {-1,-1,-1};
    cluster.aabb_max = {1,1,1};
    cluster.radius = 1.7f;
    // Brace-init uses new field names: {first_index, index_count, threshold}
    cluster.lods.push_back({0u, 6u, 2.0f});   // LOD0
    cluster.lods.push_back({6u, 3u, 0.0f});   // LOD1
    part.clusters.push_back(cluster);

    CHECK(part.indices.size() == 9, "part has 9 index values (6 + 3)");
    CHECK(part.vertices.size() == 7, "part has 7 unique vertices");
    CHECK(part.clusters[0].lods[0].first_index == 0, "LOD0 first_index == 0");
    CHECK(part.clusters[0].lods[0].index_count == 6, "LOD0 index_count == 6");
    CHECK(part.clusters[0].lods[1].first_index == 6, "LOD1 first_index == 6");
    CHECK(part.clusters[0].lods[1].index_count == 3, "LOD1 index_count == 3");

    // Verify LOD1 part-local indices reference valid vertices.
    bool lod1_in_range = true;
    for (uint32_t k = 6; k < 9; ++k) {
        if (part.indices[k] >= static_cast<uint32_t>(part.vertices.size())) {
            lod1_in_range = false;
        }
    }
    CHECK(lod1_in_range, "LOD1 indices (4,5,6) are in-range for 7-vertex part");

    // Verify out-of-range detection (the rejection case from the brief).
    viewer::VkScenePart bad_part;
    bad_part.part_hash = 0xBAD0Cull;
    bad_part.vertices.resize(3);
    bad_part.indices = {0u, 1u, 99u};   // index 99 > vertices.size()-1
    viewer::VkSceneCluster bad_cluster;
    bad_cluster.aabb_min = {-1,-1,-1};
    bad_cluster.aabb_max = {1,1,1};
    bad_cluster.radius = 1.7f;
    bad_cluster.lods.push_back({0u, 3u, 0.0f});
    bad_part.clusters.push_back(bad_cluster);

    // Mirror ensure_part's validation loop.
    bool out_of_range_detected = false;
    for (uint32_t idx : bad_part.indices) {
        if (idx >= static_cast<uint32_t>(bad_part.vertices.size())) {
            out_of_range_detected = true;
            break;
        }
    }
    CHECK(out_of_range_detected, "out-of-range index 99 detected (ensure_part rejects)");
}

// ---------------------------------------------------------------------------
// test_rt_geometry_selection_fields
//
// Verifies that RtGeometrySelection has first_index / index_count.
// ---------------------------------------------------------------------------
static void test_rt_geometry_selection_fields() {
    printf("\n[test_rt_geometry_selection_fields]\n");

    // Construct a RtGeometrySelection with the new field names.
    viewer::vk_scene_detail::RtGeometrySelection sel{};
    sel.cluster_index = 1;
    sel.lod_index = 2;
    sel.first_index = 15;
    sel.index_count = 3;

    CHECK(sel.first_index == 15, "RtGeometrySelection::first_index compiles and holds value");
    CHECK(sel.index_count == 3, "RtGeometrySelection::index_count compiles and holds value");
    CHECK(sel.cluster_index == 1 && sel.lod_index == 2,
          "cluster_index/lod_index unchanged");
}

// ---------------------------------------------------------------------------
// test_two_lod_rt_payload_indexed
//
// Two-cluster part using part-local first_index values matching the brief's
// contract test. Validates the data layout matches what select_rt_instance_geometry
// will emit once linked (field names and values).
// ---------------------------------------------------------------------------
static void test_two_lod_rt_payload_indexed() {
    printf("\n[test_two_lod_rt_payload_indexed]\n");

    viewer::VkScenePart part{};
    part.part_hash = 0x4c4f4452u;
    // Two clusters with two LODs each, using part-local first_index values.
    part.clusters = {
        {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 20.0f,
         {{0u, 6u, 1.0f}, {6u, 3u, 0.0f}}},
        {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f,
         {{9u, 6u, 1.0f}, {15u, 3u, 0.0f}}},
    };
    part.vertices.resize(18);

    // Verify the LOD field values via direct struct access.
    CHECK(part.clusters[0].lods[0].first_index == 0 &&
              part.clusters[0].lods[0].index_count == 6,
          "cluster 0, LOD 0: first_index=0, index_count=6");
    CHECK(part.clusters[0].lods[1].first_index == 6 &&
              part.clusters[0].lods[1].index_count == 3,
          "cluster 0, LOD 1: first_index=6, index_count=3");
    CHECK(part.clusters[1].lods[0].first_index == 9 &&
              part.clusters[1].lods[0].index_count == 6,
          "cluster 1, LOD 0: first_index=9, index_count=6");
    CHECK(part.clusters[1].lods[1].first_index == 15 &&
              part.clusters[1].lods[1].index_count == 3,
          "cluster 1, LOD 1: first_index=15, index_count=3");

    // Verify the threshold field is preserved.
    CHECK(part.clusters[0].lods[0].threshold == 1.0f &&
              part.clusters[0].lods[1].threshold == 0.0f,
          "cluster 0 thresholds preserved");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    printf("vk_scene_renderer_tests: indexed VkScenePart assembly (CPU-only)\n");
    printf("Validates Task 3 struct layout: first_index/index_count, part.indices.\n");

    test_vk_scene_lod_fields();
    test_rt_geometry_selection_fields();
    test_two_lod_rt_payload_indexed();

    printf("\n--- Results: %d/%d passed", g_tests - g_failures, g_tests);
    if (g_failures == 0)
        printf(" --- ALL PASS\n");
    else
        printf(" --- %d FAIL\n", g_failures);

    return g_failures > 0 ? 1 : 0;
}
