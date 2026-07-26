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
#include "render/vk_animation_bounds.h"
#include "render/vk_gi_contract.h"
#include "render/dynamic_instance_slots.h"
#include "matter/scene.h"
#include "shaders_gen/embedded_spirv.h"

#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
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
    change.slot_generation = 9u;
    change.part_hash = 0xABCDEF01ull;
    change.object_to_world = matter::Mat4f{};
    change.casts_shadow = false;
    change.entity_id.value = 42u;

    CHECK(change.kind == matter::render::DynamicSlotChangeKind::Bind,
          "DynamicSlotChange::kind constructs as Bind");
    CHECK(change.slot_index == 3u, "DynamicSlotChange::slot_index round-trips");
    CHECK(change.slot_generation == 9u,
          "DynamicSlotChange carries the full generational slot identity");
    CHECK(change.part_hash == 0xABCDEF01ull,
          "DynamicSlotChange::part_hash round-trips");
    CHECK(change.casts_shadow == false,
          "DynamicSlotChange::casts_shadow round-trips");
    CHECK(change.entity_id.value == 42u,
          "DynamicSlotChange::entity_id.value round-trips");
}

static void test_animation_bounds_cull_shader_contract() {
    printf("\n[test_animation_bounds_cull_shader_contract]\n");
    CHECK(sizeof(viewer::VkAnimationBoundsGpuRecord) == 64,
          "dynamic animation bounds GPU payload preserves slot generation and std430 stride");
    const matter::EmbeddedSpirvView cull = matter::find_spirv("cull.comp.spv");
    CHECK(cull.words != nullptr && cull.word_count != 0,
          "clean embedded shader table contains the generated dynamic-bounds cull shader");
}

// This is deliberately the cull.comp lookup contract, rather than a second
// VkAnimationBounds resolver.  It consumes the std430 records that C3 uploads
// and applies the shader's exact identity/count rules before doing the one
// frustum-plane decision needed by these tests.  A GPU device is not available
// to this CPU-only target; the Makefile prerequisite above still compiles the
// shader into the embedded table used by the renderer.
struct C3CullContractInput {
    viewer::VkAnimationBoundsAabb static_aabb{};
    uint32_t animation_slot = UINT32_MAX;
    uint32_t animation_generation = 0;
    uint32_t cluster_index = 0;
    uint32_t selected_lod = 0;
};

struct C3CullContractResult {
    viewer::VkAnimationBoundsAabb selected_aabb{};
    bool used_dynamic = false;
    bool occlusion_enabled = false;
    bool frustum_visible = false;
};

// Mirrors cull.comp::dynamic_cluster_union(), dynamic_cluster_lod(), and its
// all-corners-outside test for the x >= 0 frustum plane.  `dynamic_count` is
// FrameConstants.counts.w, which is intentionally independent of the backing
// allocation/vector length so a stale tail cannot affect a new frame.
static C3CullContractResult execute_c3_cull_contract(
    const C3CullContractInput& input,
    const std::vector<viewer::VkAnimationBoundsGpuRecord>& records,
    uint32_t dynamic_count) {
    C3CullContractResult result{};
    result.selected_aabb = input.static_aabb;
    const uint32_t inspected = std::min<uint32_t>(
        dynamic_count, static_cast<uint32_t>(records.size()));
    if (input.animation_slot != UINT32_MAX) {
        bool found = false;
        viewer::VkAnimationBoundsAabb unioned{{INFINITY, INFINITY, INFINITY},
                                               {-INFINITY, -INFINITY, -INFINITY}};
        for (uint32_t index = 0; index != inspected; ++index) {
            const auto& value = records[index];
            if (value.instance_slot != input.animation_slot ||
                value.instance_generation != input.animation_generation ||
                value.cluster_index != input.cluster_index)
                continue;
            for (uint32_t axis = 0; axis != 3; ++axis) {
                unioned.min[axis] = std::min(unioned.min[axis], value.aabb_min[axis]);
                unioned.max[axis] = std::max(unioned.max[axis], value.aabb_max[axis]);
            }
            found = true;
        }
        if (found) {
            result.selected_aabb = unioned;
            result.used_dynamic = true;
            // The shader tightens to a selected-LOD record when one exists.
            for (uint32_t index = 0; index != inspected; ++index) {
                const auto& value = records[index];
                if (value.instance_slot != input.animation_slot ||
                    value.instance_generation != input.animation_generation ||
                    value.cluster_index != input.cluster_index ||
                    value.lod != input.selected_lod)
                    continue;
                for (uint32_t axis = 0; axis != 3; ++axis) {
                    result.selected_aabb.min[axis] = value.aabb_min[axis];
                    result.selected_aabb.max[axis] = value.aabb_max[axis];
                }
                result.occlusion_enabled =
                    (value.flags & viewer::kVkAnimationBoundsOcclusionEnabled) != 0;
                break;
            }
        }
    }
    // cull.comp rejects an AABB only when every corner is outside a plane.
    // For plane (+x, 0, 0, 0), at least one x >= 0 corner is visible.
    result.frustum_visible = result.selected_aabb.max[0] >= 0.0f;
    return result;
}

static viewer::VkSkinMatrix test_translate(float x) {
    viewer::VkSkinMatrix matrix{};
    matrix.elements[0] = matrix.elements[5] = matrix.elements[10] =
        matrix.elements[15] = 1.0f;
    matrix.elements[12] = x;
    return matrix;
}

static viewer::VkSkinPose test_pose(float x) {
    viewer::VkSkinPose pose{};
    viewer::VkSkinJoint joint{};
    joint.position = test_translate(x);
    pose.current.push_back(joint);
    return pose;
}

static viewer::VkAnimationBoundsAsset test_c3_bounds_asset(uint64_t key) {
    viewer::VkAnimationBoundsAsset asset{};
    asset.asset_key = key;
    asset.conservative_asset_bound = {{-10.0f, -1.0f, -1.0f},
                                       {10.0f, 1.0f, 1.0f}};
    viewer::VkAnimationBoundsCluster cluster{};
    cluster.cluster_index = 7;
    cluster.lod = 0;
    cluster.joints.push_back({0, {{-1.0f, -1.0f, -1.0f},
                                  {1.0f, 1.0f, 1.0f}}});
    asset.clusters.push_back(cluster);
    return asset;
}

static void test_c3_dynamic_bounds_cull_contract() {
    printf("\n[test_c3_dynamic_bounds_cull_contract]\n");
    CHECK(offsetof(viewer::VkAnimationBoundsGpuRecord, aabb_min) == 0 &&
              offsetof(viewer::VkAnimationBoundsGpuRecord, aabb_max) == 16 &&
              offsetof(viewer::VkAnimationBoundsGpuRecord, instance_slot) == 32 &&
              offsetof(viewer::VkAnimationBoundsGpuRecord, instance_generation) == 36 &&
              offsetof(viewer::VkAnimationBoundsGpuRecord, cluster_index) == 40 &&
              offsetof(viewer::VkAnimationBoundsGpuRecord, lod) == 44 &&
              offsetof(viewer::VkAnimationBoundsGpuRecord, flags) == 48,
          "C3 CPU record offsets match cull.comp DynamicAnimationBound std430 ABI");

    viewer::VkAnimationBounds bounds;
    constexpr uint64_t key = 0xC300u;
    CHECK(bounds.register_asset(test_c3_bounds_asset(key)),
          "register serialized C3 bounds asset");
    C3CullContractInput input{};
    input.static_aabb = {{-12.0f, -1.0f, -1.0f}, {-10.0f, 1.0f, 1.0f}};
    input.animation_slot = 3;
    input.animation_generation = 9;
    input.cluster_index = 7;

    CHECK(bounds.update_instance(3, 9, key, test_pose(2.0f), false),
          "serialize an in-frustum animated pose to the C3 GPU record");
    const auto dynamic_inside = bounds.gpu_records();
    auto shared_mesh_records = dynamic_inside;
    auto bind_peer = dynamic_inside.front();
    bind_peer.instance_slot = 4;
    bind_peer.instance_generation = 1;
    shared_mesh_records.push_back(bind_peer);
    viewer::VkSkinRasterDraw accepted_draw{};
    accepted_draw.instance_slot = 3;
    accepted_draw.instance_generation = 9;
    accepted_draw.lod = 0;
    accepted_draw.first_index = 12;
    accepted_draw.index_count = 6;
    viewer::mark_animation_skin_raster_records(shared_mesh_records,
                                                {accepted_draw});
    CHECK((shared_mesh_records[0].flags &
               viewer::kVkAnimationBoundsSkinRaster) != 0 &&
              (shared_mesh_records[1].flags &
               viewer::kVkAnimationBoundsSkinRaster) == 0,
          "shared mesh marks only the accepted generational instance; bind and static peers stay indirect");
    const auto overridden = execute_c3_cull_contract(
        input, dynamic_inside, static_cast<uint32_t>(dynamic_inside.size()));
    CHECK(overridden.used_dynamic && overridden.frustum_visible,
          "dynamic AABB overrides an outside static AABB before the cull decision");

    input.static_aabb = {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    CHECK(bounds.update_instance(3, 9, key, test_pose(-20.0f), false),
          "serialize an out-of-frustum animated pose to the C3 GPU record");
    const auto dynamic_outside = bounds.gpu_records();
    const auto overridden_outside = execute_c3_cull_contract(
        input, dynamic_outside, static_cast<uint32_t>(dynamic_outside.size()));
    CHECK(overridden_outside.used_dynamic && !overridden_outside.frustum_visible,
          "dynamic AABB also overrides an inside static AABB when animation moves it out");

    // The backing allocation can retain a previous record. FrameConstants.counts.w
    // must be the active count: shrinking to zero must ignore the stale tail.
    auto stale_tail = dynamic_inside;
    // Simulate a capacity-two allocation after this frame shrank to one
    // record. The old second record has a different LOD and is deliberately
    // not selected, so the union itself proves whether it was inspected.
    auto old_tail = dynamic_outside.front();
    old_tail.lod = 1;
    stale_tail.push_back(old_tail);
    input.selected_lod = 2;
    const auto shrunk_count = execute_c3_cull_contract(input, stale_tail, 1);
    CHECK(shrunk_count.used_dynamic && shrunk_count.selected_aabb.min[0] == 1.0f &&
              shrunk_count.selected_aabb.max[0] == 3.0f,
          "a shrunken active count excludes a stale dynamic-bounds allocation tail");
    const auto zero_count = execute_c3_cull_contract(input, stale_tail, 0);
    CHECK(!zero_count.used_dynamic && zero_count.frustum_visible,
          "zero active dynamic bounds ignores an old GPU-buffer tail and uses static culling");
    const auto wrong_generation = execute_c3_cull_contract(
        C3CullContractInput{{{-12.0f, -1.0f, -1.0f}, {-10.0f, 1.0f, 1.0f}},
                            3, 10, 7, 2},
        stale_tail, static_cast<uint32_t>(stale_tail.size()));
    CHECK(!wrong_generation.used_dynamic && !wrong_generation.frustum_visible,
          "same slot with a wrong generation cannot match a previous incarnation's bound");

    // A rejected snapshot replaces the old compact record with the registered
    // conservative asset bound and disables occlusion. It must not keep the
    // previous smaller out-of-frustum record alive for either culling outcome.
    bounds.fail_open_instances({{3, 9, key}});
    const auto fallback_records = bounds.gpu_records();
    input.static_aabb = {{-12.0f, -1.0f, -1.0f}, {-10.0f, 1.0f, 1.0f}};
    input.selected_lod = 0;
    const auto fallback = execute_c3_cull_contract(
        input, fallback_records, static_cast<uint32_t>(fallback_records.size()));
    CHECK(fallback.used_dynamic && fallback.frustum_visible && !fallback.occlusion_enabled,
          "rejected snapshot publishes conservative fail-open culling with occlusion disabled");
    CHECK(fallback.selected_aabb.min[0] == -10.0f && fallback.selected_aabb.max[0] == 10.0f,
          "rejected snapshot cannot retain the prior smaller dynamic AABB");

    // The embedded SPIR-V is rebuilt from cull.comp by this target's shader
    // prerequisite. Keep the source-side descriptor/count contract explicit
    // too, so changing the binding or removing the active-count guard fails
    // this integration contract rather than silently accepting stale records.
    std::ifstream source("../shaders_vk/cull.comp");
    const std::string shader_text((std::istreambuf_iterator<char>(source)),
                                  std::istreambuf_iterator<char>());
    CHECK(source.good() || !shader_text.empty(), "read compiled C3 cull shader source contract");
    CHECK(shader_text.find("layout(set = 1, binding = 8, std430)") != std::string::npos &&
              shader_text.find("frame.counts.w") != std::string::npos &&
              shader_text.find("instance_generation") != std::string::npos &&
              shader_text.find("uses_skin_raster(instance, lod)") !=
                  std::string::npos &&
              shader_text.find("ANIMATION_BOUND_SKIN_RASTER") !=
                  std::string::npos,
          "cull shader excludes skin raster per instance without suppressing its shared command");
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
    test_animation_bounds_cull_shader_contract();
    test_c3_dynamic_bounds_cull_contract();

    printf("\n--- Results: %d/%d passed", g_tests - g_failures, g_tests);
    if (g_failures == 0)
        printf(" --- ALL PASS\n");
    else
        printf(" --- %d FAIL\n", g_failures);

    return g_failures > 0 ? 1 : 0;
}
