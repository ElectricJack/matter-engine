#include "matter/windows_compat.h"
#define GLFW_INCLUDE_NONE
#include "matter/log.h"
#include "render/chart_static_surface.h"
#include "render/vt_surface_topology.h"
#include "vt_module_residency_tests.h"
#include <GLFW/glfw3.h>
#include <cstring>

namespace {

void topology_contract() {
    vt::VtSurfaceTopology topology;
    const vt::VtSurfaceTopology::Region a{1, 10}, b{2, 20};
    std::string error;
    CHECK(topology.replace(a, {{10, 20, 7}, {20, 30, 7}}, error), "topology: initial face pairs");
    const auto initial = topology.revision();
    CHECK(topology.replace(a, {{30, 20, 7}, {20, 10, 7}, {10, 20, 7}}, error) &&
          topology.revision() == initial && topology.pair_count() == 2,
          "topology: duplicate/reordered/reversed authorizations do not change publication");
    CHECK(topology.replace(b, {{20, 10, 7}}, error) && topology.revision() == initial &&
          topology.region_count() == 2,
          "topology: independent regions share a physical pair");
    CHECK(topology.replace(a, {{20, 40, 7}}, error) && topology.pair_count() == 2 &&
          topology.revision() != initial,
          "topology: replacement removes only unsupported pairs");
    std::map<uint64_t, uint32_t> rungs{{10, 2}, {20, 1}};
    std::map<uint64_t, int> queries;
    const auto resolve = [&](uint64_t owner, uint32_t& rung) {
        ++queries[owner];
        const auto found = rungs.find(owner);
        if (found == rungs.end()) return false;
        rung = found->second; return true;
    };
    auto selected = topology.selected_pairs(resolve);
    CHECK(selected.size() == 1 && selected[0].first == 10 && selected[0].second == 20 &&
          selected[0].first_rung == 2 && selected[0].second_rung == 1 &&
          queries[10] == 1 && queries[20] == 1 && queries[40] == 1,
          "topology: current drawn rungs are resolved once; missing destination has no connection");
    topology.erase(b);
    CHECK(topology.pair_count() == 1, "topology: last authorizer removal retires its pair");
    rungs[40] = 0;
    selected = topology.selected_pairs(resolve);
    CHECK(selected.size() == 1 && selected[0].first == 20 && selected[0].second == 40 &&
          selected[0].second_rung == 0, "topology: new strip draw uses its current rung");
    const auto before_invalid = topology.revision();
    CHECK(!topology.replace(a, {{20, 20, 7}}, error) && !error.empty() &&
          !topology.replace(a, {{20, 40, 0}}, error) &&
          topology.revision() == before_invalid && topology.pair_count() == 1,
          "topology: invalid domains/owners preserve the current graph");
    std::vector<vt::VtSurfacePartPair> oversized(vt::VtSurfaceTopology::kMaxRegionPairs + 1, {20, 40, 7});
    CHECK(!topology.replace(a, oversized, error) && topology.revision() == before_invalid,
          "topology: oversized update fails before changing the graph");
    topology.clear();
    CHECK(topology.pair_count() == 0 && topology.region_count() == 0 &&
          topology.selected_pairs(resolve).empty(), "topology: world teardown clears authorizations");
}

viewer::VkScenePart strip(bool folded) {
    viewer::VkScenePart part;
    part.part_hash = 0x57a71c01;
    part.surface_world_anchored = 1;
    part.surface_local_to_world[3] = 2;
    part.surface_local_to_world[7] = 120;
    part.surface_local_to_world[11] = -64;
    for (int row = 0; row <= 8; ++row) {
        for (int side = 0; side < 2; ++side) {
            viewer::VkRasterVertex vertex;
            // A 90-degree bend is strictly outside the production 45-degree
            // chart cone. The old 45-degree fixture sat exactly on its inclusive
            // boundary and legitimately stayed in one chart.
            vertex.position = {side ? .5f : -.5f, float(folded ? std::min(row, 4) : row) - 4,
                               folded && row > 4 ? float(row - 4) : 0};
            vertex.normal = folded && row > 4 ? matter::Float3{0, -1, 0} :
                            folded && row == 4 ? matter::Float3{0, -.70710678f, .70710678f} :
                            matter::Float3{0, 0, 1};
            vertex.material_index = 1;
            vertex.tint = {.3f, .5f, .7f, 0};
            vertex.surface = {0, 0, .8f, 1};
            part.vertices.push_back(vertex);
        }
    }
    for (uint32_t row = 0; row < 8; ++row) {
        const uint32_t a = row * 2;
        part.indices.insert(part.indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
    viewer::VkSceneCluster cluster;
    cluster.aabb_min = {-.5f, -4, 0};
    cluster.aabb_max = {.5f, folded ? 0.f : 4.f, folded ? 4.f : 0.f};
    cluster.radius = 6;
    cluster.lods.push_back({0, static_cast<uint32_t>(part.indices.size()), 0, UINT32_MAX});
    part.clusters.push_back(cluster);
    return part;
}

bool chart_contract() {
    std::string error;
    for (bool folded : {false, true}) {
        auto part = strip(folded);
        const auto before = part;
        if (!viewer::chart_static_surface(part, 32, error)) return false;
        CHECK(part.indices.size() == before.indices.size() &&
              part.clusters[0].lods[0].chart_rung == 0 &&
              part.clusters[0].radius == before.clusters[0].radius,
              "static chart: fixed geometry rung and bounds are preserved");
        if (folded) CHECK(part.lod_charts[0].charts.size() > 1,
                          "static chart: bent strip exercises chart cuts");
        for (size_t i = 0; i < part.indices.size(); ++i) {
            const auto& a = before.vertices[before.indices[i]];
            const auto& b = part.vertices[part.indices[i]];
            CHECK(std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0 &&
                  std::memcmp(&a.normal, &b.normal, sizeof(a.normal)) == 0 &&
                  std::memcmp(&a.tint, &b.tint, sizeof(a.tint)) == 0 &&
                  a.material_index == b.material_index && a.surface.z == b.surface.z &&
                  a.surface.w == b.surface.w,
                  "static chart: triangle winding, positions and non-UV attributes are unchanged");
            CHECK(std::isfinite(b.surface.x) && std::isfinite(b.surface.y) &&
                  b.surface.x >= 0 && b.surface.x <= 1 && b.surface.y >= 0 && b.surface.y <= 1,
                  "static chart: every raster corner has finite atlas UVs");
        }
        part.surface_materials = {1};
        part.surface_tape_hash = 11;
        part.surface_tape_text = "const 1\nmaterial 1 r0\n";
        part.lod_chart_meshes[0].surface_weights.assign(part.vertices.size(), 255);
        const auto retained = viewer::capture_static_surface(part);
        CHECK(retained && retained->owns_context_inputs() &&
              retained->context.surface_local_to_world[3] == 2 &&
              retained->context.surface_local_to_world[7] == 120 &&
              retained->surface->tape_hash == 11 &&
              retained->surface->weights.size() == part.vertices.size(),
              "static chart: retained inputs own the chart, classification, geometry and world frame");
        part = {};
        CHECK(retained && retained->context.triangle_count == before.indices.size() / 3,
              "static chart: temporary raster part can be released");
    }
    auto invalid = strip(false);
    invalid.indices.back() = UINT32_MAX;
    const auto vertices = invalid.vertices;
    CHECK(!viewer::chart_static_surface(invalid, 32, error) && !error.empty() &&
          invalid.lod_charts.empty() && invalid.clusters[0].lods[0].chart_rung == UINT32_MAX &&
          invalid.vertices.size() == vertices.size() &&
          std::memcmp(invalid.vertices.data(), vertices.data(), vertices.size() * sizeof(vertices[0])) == 0,
          "static chart: invalid input leaves the original part intact");
    auto bad_density = strip(false);
    CHECK(!viewer::chart_static_surface(bad_density, std::numeric_limits<float>::quiet_NaN(), error),
          "static chart: nonfinite density rejected");
    return true;
}

std::shared_ptr<const vt::VtPartSnapshot> recipe(
    const vt::VtPartSnapshot& base, float height, uint64_t hash) {
    // A world-space albedo ramp distinguishes the retained frame from local UVs.
    const std::string tape = "input wx\nconst 0.1\nmul r0 r1\nconst 0.2\nadd r2 r3\n"
        "const 0.8\nconst 0\nconst 1\nconst " + std::to_string(height) +
        "\nmaterial 1 r7\nsource 1 r4 r4 r4 r5 r6 r7 r8 -0.08 0\n";
    std::vector<uint8_t> weights(base.context.vertex_count, 255);
    const uint32_t material = 1;
    auto context = base.context;
    context.surface_weights = weights.data();
    context.surface_materials = &material;
    context.surface_material_count = 1;
    context.surface_tape_hash = hash;
    context.surface_tape_text = tape.c_str();
    return base.with_surface(context);
}

bool gpu_contract(matter::VulkanDevice& vk, std::string& error) {
    auto part = strip(false);
    if (!viewer::chart_static_surface(part, 32, error)) return false;
    const auto base = viewer::capture_static_surface(part);
    auto current = recipe(*base, -.03f, 101);
    const auto original = current;
    part = {};
    vt_queue_tests::Budgets budgets;
    vt::VtResidency residency;
    vt_queue_tests::Frames frames(vk);
    if (!frames.valid() || !residency.init(vk, error)) return false;
    auto producer = vt::VtCompositor::create(vk.device(), vk.physical_device(), VK_NULL_HANDLE, error);
    if (!producer) return false;
    vt::VtCompositorMaterial materials[2]{};
    producer->set_materials(materials, 2);
    residency.set_filler(std::move(producer));
    uint64_t serial = 0;
    uint32_t slot = residency.register_variant(current->context.variant_hash, 0,
                                               *current->context.atlas, current->context);
    if (!slot) return false;
    vt_module_residency_tests::Sampler sampler;
    if (!sampler.init(vk, residency, error)) return false;
    const auto advance = [&] { return frames.next(residency, ++serial); };
    const auto check_pixels = [&](float expected_height) {
        const auto& ctx = current->context;
        if (!vt_prepare_tests::until([&] {
                return advance() && bool(residency.surface_boundary_source(slot));
            })) return false;
        for (uint32_t triangle : {1u, 8u, 14u}) {
            float uv[2]{}, local_x = 0;
            for (int corner = 0; corner < 3; ++corner) {
                const uint32_t vertex = ctx.indices[triangle * 3 + corner];
                local_x += ctx.positions[vertex * 3] / 3;
                for (int k = 0; k < 2; ++k) uv[k] += ctx.surface_uvs[vertex * 2 + k] / 3;
            }
            const uint32_t px = static_cast<uint32_t>(uv[0] * ctx.atlas->atlas_w) / chart_atlas::kVtPagePayload;
            const uint32_t py = static_cast<uint32_t>(uv[1] * ctx.atlas->atlas_h) / chart_atlas::kVtPagePayload;
            const vt::VtFeedbackRequest request{slot - 1, 0, px, py};
            residency.inject_feedback_for_test(&request, 1);
            if (!vt_prepare_tests::until([&] {
                    return advance() && residency.resident_page_slot_for_test(slot, {0, px, py}) != UINT32_MAX;
                })) return false;
            vt_material_domain_tests::Probe probe{};
            probe.slots[0] = slot;
            probe.uv[0] = uv[0]; probe.uv[1] = uv[1];
            probe.derivatives[0] = probe.derivatives[3] = .0001f;
            vt_material_domain_tests::Result result{};
            if (!sampler.sample_probe(vk, residency, frames, serial, probe, result, error)) return false;
            const float expected_color = .2f + .1f * (local_x + 2);
            CHECK(std::abs(result.channels[0][0] - expected_color) < .008f &&
                  std::abs(result.channels[2][1] - .8f) < .008f &&
                  std::abs(result.metrics[0] - expected_height) < .00002f,
                  "static chart: GPU color, roughness and metric POM height match the world recipe");
        }
        return true;
    };
    if (!check_pixels(-.03f)) return false;
    current = recipe(*current, -.02f, 102);
    CHECK(current->geometry == original->geometry && current->owns_context_inputs() &&
          original->surface->tape_hash == 101,
          "static chart: recipe edit shares geometry and preserves previous inputs");
    const auto& ctx = current->context;
    if (!residency.update_variant_surface(ctx.variant_hash, 0, ctx.surface_weights,
            ctx.vertex_count, ctx.surface_materials, 1, ctx.surface_tape_hash, ctx.surface_tape_text)) return false;
    residency.invalidate_owners({slot});
    if (!check_pixels(-.02f)) return false;
    const auto old_source = residency.surface_boundary_source(slot);
    residency.release_variant(ctx.variant_hash);
    for (uint32_t i = 0; i < vt::kVtRetireHorizonFrames + 2; ++i) if (!advance()) return false;
    slot = residency.register_variant(ctx.variant_hash, 0, *ctx.atlas, ctx);
    if (!slot || !check_pixels(-.02f)) return false;
    CHECK(!residency.surface_boundary_source_current(*old_source),
          "static chart: retired draw source is rejected after registration from retained inputs");
    std::printf("STATIC_SURFACE_VT geometry=preserved edits=shared_geometry reload=current_recipe samples=9\n");
    return true;
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    topology_contract();
    CHECK(chart_contract(), "static surface chart contract");
    if (g_failures) return check_summary();
#ifdef MATTER_VK_TEST_LAYER_PATH
    SetDllDirectoryA(MATTER_VK_TEST_LAYER_PATH);
    SetEnvironmentVariableA("VK_LAYER_PATH", MATTER_VK_TEST_LAYER_PATH);
#endif
    if (!glfwInit()) { CHECK(false, "GLFW initialization"); return check_summary(); }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(64, 64, "Static surface VT", nullptr, nullptr);
    std::string error;
    auto vk = window ? matter::VulkanDevice::create(window, true, error) : nullptr;
    CHECK(vk != nullptr, error.empty() ? "Vulkan device initialization" : error.c_str());
    if (vk) {
        CHECK(gpu_contract(*vk, error), error.empty() ? "static surface GPU contract" : error.c_str());
        CHECK(vk->validation_error_count() == 0, "zero Vulkan validation errors");
        std::printf("validation errors: %u\n", vk->validation_error_count());
    }
    vk.reset();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return check_summary();
}
