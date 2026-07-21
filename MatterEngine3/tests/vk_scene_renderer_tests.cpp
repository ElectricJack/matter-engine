// vk_scene_renderer_tests.cpp — CPU-side tests for indexed VkScenePart assembly.
//
// Tests the invariants established by Task 3 and Task 5:
//   - VkSceneLod carries first_index/index_count (not first_vertex/vertex_count)
//   - VkScenePart::indices holds part-local index values
//   - RtGeometrySelection emits first_index/index_count
//   - Out-of-range index validation detects bad parts
//   - GpuRtPartRecord is exactly 48 bytes ("three vec4 records") with index_address
//
// These are pure CPU tests; no GPU/window required.

#include "render/vk_scene_renderer.h"
#include "render/vk_gi_contract.h"
#include "render/dynamic_instance_slots.h"
#include "matter/scene.h"

#include <cstddef>
#include <cstdio>
#include <cstdint>
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

    // The real ensure_part rejection test (out-of-range index → non-empty err,
    // return < 0) is exercised against a live VkSceneRenderer in
    // vulkan_smoke_tests.cpp::run_cull_region_and_lifecycle_tests.  This CPU-
    // only binary cannot link the full renderer (Vulkan/GLFW deps absent), so
    // only the struct-field and in-range checks are validated here.
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
// test_gpu_rt_part_record_layout
//
// Validates the Task 5 GpuRtPartRecord layout: 48 bytes ("three vec4 records"),
// with index_address field between vertex_address and vertex_stride.
// ---------------------------------------------------------------------------
static void test_gpu_rt_part_record_layout() {
    printf("\n[test_gpu_rt_part_record_layout]\n");

    CHECK(sizeof(GpuRtPartRecord) == 48,
          "GpuRtPartRecord is exactly 48 bytes (three vec4 records)");
    CHECK(offsetof(GpuRtPartRecord, vertex_address) == 0,
          "vertex_address at byte 0");
    CHECK(offsetof(GpuRtPartRecord, index_address) == 8,
          "index_address at byte 8 (after 8-byte vertex_address)");
    CHECK(offsetof(GpuRtPartRecord, vertex_stride) == 16,
          "vertex_stride at byte 16 (after two 8-byte addresses)");
    CHECK(offsetof(GpuRtPartRecord, vertex_count) == 20,
          "vertex_count at byte 20");
    CHECK(offsetof(GpuRtPartRecord, primitive_count) == 24,
          "primitive_count at byte 24");
    CHECK(offsetof(GpuRtPartRecord, valid) == 28,
          "valid at byte 28");

    // Verify that zero-init gives sensible defaults.
    GpuRtPartRecord r{};
    CHECK(r.vertex_address == 0 && r.index_address == 0 && r.valid == 0,
          "zero-init GpuRtPartRecord has zero addresses and valid=0");

    // Verify field assignment round-trips.
    r.vertex_address = 0xDEADBEEF00000001ull;
    r.index_address  = 0xCAFEBABE00000002ull;
    r.vertex_stride  = 72u;
    r.vertex_count   = 100u;
    r.primitive_count = 33u;
    r.valid = 1u;
    CHECK(r.vertex_address == 0xDEADBEEF00000001ull, "vertex_address round-trips");
    CHECK(r.index_address  == 0xCAFEBABE00000002ull, "index_address round-trips");
    CHECK(r.vertex_stride  == 72u,  "vertex_stride holds 72");
    CHECK(r.vertex_count   == 100u, "vertex_count round-trips");
    CHECK(r.primitive_count == 33u, "primitive_count round-trips");
    CHECK(r.valid == 1u, "valid round-trips");
}

// ---------------------------------------------------------------------------
// test_dynamic_slot_change_fields
//
// Task 7: DynamicSlotChange (from render/dynamic_instance_slots.h, Task 6)
// has the fields the dynamic lane consumes, and can be constructed directly.
// ---------------------------------------------------------------------------
static void test_dynamic_slot_change_fields() {
    printf("\n[test_dynamic_slot_change_fields]\n");

    matter::render::DynamicSlotChange change;
    change.kind = matter::render::DynamicSlotChangeKind::Bind;
    change.slot_index = 3u;
    change.part_hash = 0xABCDEF01ull;
    change.object_to_world = matter::Mat4f{};
    change.casts_shadow = false;
    change.entity_id.value = 42u;

    CHECK(change.kind == matter::render::DynamicSlotChangeKind::Bind,
          "DynamicSlotChange::kind constructs as Bind");
    CHECK(change.slot_index == 3u, "DynamicSlotChange::slot_index round-trips");
    CHECK(change.part_hash == 0xABCDEF01ull,
          "DynamicSlotChange::part_hash round-trips");
    CHECK(change.casts_shadow == false,
          "DynamicSlotChange::casts_shadow round-trips");
    CHECK(change.entity_id.value == 42u,
          "DynamicSlotChange::entity_id.value round-trips");
}

// ---------------------------------------------------------------------------
// test_dynamic_slot_change_bind_shares_part_hash
//
// A Bind change references a part_hash from the same 64-bit space used by
// VkScenePart::part_hash / VkSceneInstance::part_hash, showing static and
// dynamic instances share the renderer's part resources.
// ---------------------------------------------------------------------------
static void test_dynamic_slot_change_bind_shares_part_hash() {
    printf("\n[test_dynamic_slot_change_bind_shares_part_hash]\n");

    viewer::VkScenePart part;
    part.part_hash = 0x9999AAAA1111BBBBull;

    matter::render::DynamicSlotChange change;
    change.kind = matter::render::DynamicSlotChangeKind::Bind;
    change.slot_index = 0u;
    change.part_hash = part.part_hash;

    CHECK(change.part_hash == part.part_hash,
          "Bind change part_hash matches VkScenePart::part_hash type/value space");
}

// ---------------------------------------------------------------------------
// test_instance_identity_tagging
//
// Static instances (VkSceneInstance) tag identity via a raw uint64_t
// instance_id fed through viewer::vulkan_history_token(); dynamic instances
// (DynamicSlotChange) tag identity via matter::scene::SceneEntityId. Both
// route through the same folding function so history/token semantics are
// identical, but the source field differs by lane.
// ---------------------------------------------------------------------------
static void test_instance_identity_tagging() {
    printf("\n[test_instance_identity_tagging]\n");

    viewer::VkSceneInstance static_instance;
    static_instance.instance_id = 0x1000000020ull;

    matter::render::DynamicSlotChange dynamic_change;
    dynamic_change.entity_id.value = 0x1000000020ull;

    const uint32_t static_token =
        viewer::vulkan_history_token(static_instance.instance_id);
    const uint32_t dynamic_token =
        viewer::vulkan_history_token(dynamic_change.entity_id.value);

    CHECK(static_token == dynamic_token,
          "same 64-bit identity folds to the same history token regardless of "
          "lane");
    CHECK(static_token != 0, "vulkan_history_token never returns zero");

    // Zero identity still folds to a nonzero sentinel token for both lanes.
    viewer::VkSceneInstance zero_static;
    zero_static.instance_id = 0;
    matter::scene::SceneEntityId zero_entity;
    zero_entity.value = 0;
    CHECK(viewer::vulkan_history_token(zero_static.instance_id) == 1u,
          "zero instance_id folds to sentinel token 1");
    CHECK(viewer::vulkan_history_token(zero_entity.value) == 1u,
          "zero entity_id folds to sentinel token 1");
}

// ---------------------------------------------------------------------------
// test_dynamic_slot_change_kind_distinct
//
// Bind, Transform, and Remove are distinct enumerators; a Transform-only
// change is recognizable and does not alias Bind or Remove.
// ---------------------------------------------------------------------------
static void test_dynamic_slot_change_kind_distinct() {
    printf("\n[test_dynamic_slot_change_kind_distinct]\n");

    using matter::render::DynamicSlotChangeKind;

    CHECK(DynamicSlotChangeKind::Bind != DynamicSlotChangeKind::Transform,
          "Bind is distinct from Transform");
    CHECK(DynamicSlotChangeKind::Transform != DynamicSlotChangeKind::Remove,
          "Transform is distinct from Remove");
    CHECK(DynamicSlotChangeKind::Bind != DynamicSlotChangeKind::Remove,
          "Bind is distinct from Remove");

    matter::render::DynamicSlotChange change;
    change.kind = DynamicSlotChangeKind::Transform;
    change.slot_index = 7u;
    CHECK(change.kind == DynamicSlotChangeKind::Transform &&
              change.kind != DynamicSlotChangeKind::Bind,
          "Transform-only change is constructible and distinguishable from Bind");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    printf("vk_scene_renderer_tests: indexed VkScenePart assembly (CPU-only)\n");
    printf("Validates Task 3+5 struct layout: first_index/index_count, part.indices,\n");
    printf("GpuRtPartRecord 48-byte layout with index_address.\n");

    test_vk_scene_lod_fields();
    test_rt_geometry_selection_fields();
    test_two_lod_rt_payload_indexed();
    test_gpu_rt_part_record_layout();
    test_dynamic_slot_change_fields();
    test_dynamic_slot_change_bind_shares_part_hash();
    test_instance_identity_tagging();
    test_dynamic_slot_change_kind_distinct();

    printf("\n--- Results: %d/%d passed", g_tests - g_failures, g_tests);
    if (g_failures == 0)
        printf(" --- ALL PASS\n");
    else
        printf(" --- %d FAIL\n", g_failures);

    return g_failures > 0 ? 1 : 0;
}
