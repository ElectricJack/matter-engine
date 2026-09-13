#include "matter/windows_compat.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "matter/log.h"
#include "matter/vulkan_device.h"
#include "render/gpu_meshing/gpu_solid_mesher_vk.h"
#include "script_host.h"
#include "part_asset_v2.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
int failures = 0;
std::atomic<unsigned> validation_errors{0};
#define HOST_CHECK(condition, message)                                                 \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::printf("FAIL solid host: %s\n", message);                             \
            ++failures;                                                                \
        }                                                                              \
    } while (0)
void validation_sink(matter::log::Level, const char *tag, const char *message, void *) {
    if (tag && message && std::string(tag) == "vk" &&
        std::string(message).find("Vulkan validation ERROR") != std::string::npos)
        ++validation_errors;
}
std::string read(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
std::string simple(const std::string &name, const std::string &before = "",
                   const std::string &extra = "") {
    return "class " + name +
           " extends Part { static lodBudgets=[1]; static noImpostor=true; build(){" +
           before +
           "this.fill(8);this.solidSource({version:1,voxelM:0.006,maxVertices:150000," +
           extra +
           "ops:[{shape:'roundedBox',halfExtentsM:[0.14,0.06,0.09],roundingM:0.004,"
           "centerM:[0,0.064,0]}]});}}";
}
void inspect(const script_host::BakeResult &r, const gpu_meshing::SolidJob &job,
             float expected_length, float expected_height, float expected_depth) {
    HOST_CHECK(r.error.ok, r.error.message.c_str());
    if (!r.error.ok)
        return;
    HOST_CHECK(std::filesystem::path(r.written_path).extension() == ".bundle",
               "canonical bundle artifact");
    BLASManager blas;
    TLASManager tlas(65536);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    std::string reason;
    part_asset::PartAssetLoadFailure failure{};
    bool loaded = part_asset::load_v2(r.written_path, r.resolved_hash, blas, tlas,
                                      children, lods, &failure, &reason);
    HOST_CHECK(loaded, reason.c_str());
    if (!loaded)
        return;
    part_asset::LodVariants variants;
    part_asset::StaticLodPlan plan;
    HOST_CHECK(
        part_asset::load_lod_sidecar(r.written_path, r.resolved_hash, variants) &&
            variants.budgets.size() == 1 && variants.hashes.size() == 1 &&
            variants.budgets[0] == 1 && variants.hashes[0] == r.resolved_hash,
        "serialized singleton full-resolution rung policy");
    HOST_CHECK(
        part_asset::load_static_lod_plan(r.written_path, r.resolved_hash, plan) &&
            plan.no_impostor,
        "serialized no-impostor policy");
    HOST_CHECK(children.empty() && blas.get_entries().size() == 1 && lods.size() <= 1,
               "single source body without extra rungs or descendants");
    float minimum[3] = {1e20f, 1e20f, 1e20f}, maximum[3] = {-1e20f, -1e20f, -1e20f};
    double min_dot = 1, max_residual = 0;
    size_t triangles = 0;
    for (const auto &entry : blas.get_entries()) {
        HOST_CHECK(entry->triangles.size() == entry->tri_extra.size(),
                   "serialized normals parallel triangles");
        if (entry->triangles.size() != entry->tri_extra.size())
            continue;
        triangles += entry->triangles.size();
        for (size_t i = 0; i < entry->triangles.size(); ++i) {
            const auto &t = entry->triangles[i];
            const auto &e = entry->tri_extra[i];
            HOST_CHECK(e.materialId == job.material,
                       "serialized current stone material");
            const float3 positions[] = {t.vertex0, t.vertex1, t.vertex2},
                         normals[] = {e.N0, e.N1, e.N2};
            for (int k = 0; k < 3; ++k) {
                const auto &p = positions[k];
                const auto &n = normals[k];
                const float xyz[] = {p.x, p.y, p.z};
                for (int a = 0; a < 3; ++a) {
                    minimum[a] = std::min(minimum[a], xyz[a]);
                    maximum[a] = std::max(maximum[a], xyz[a]);
                }
                auto reference = gpu_meshing::solid_gradient_reference(
                    job, {p.x, p.y, p.z}, job.voxel_m * .02f);
                min_dot =
                    std::min(min_dot, double(reference.x * n.x + reference.y * n.y +
                                             reference.z * n.z));
                max_residual = std::max(
                    max_residual,
                    double(std::abs(gpu_meshing::evaluate_solid_field_reference(
                        job, {p.x, p.y, p.z}))));
            }
        }
    }
    HOST_CHECK(triangles > 0 && min_dot > .995,
               "serialized normals retain same-field gradients");
    HOST_CHECK(max_residual < job.voxel_m * .55,
               "serialized geometry stays on source field");
    const float expected[] = {expected_length, expected_height, expected_depth};
    for (int a = 0; a < 3; ++a)
        HOST_CHECK(std::abs(maximum[a] - minimum[a] - expected[a]) < job.voxel_m * .2f,
                   "physical outer dimension preserved");
    HOST_CHECK(std::abs(minimum[1]) < job.voxel_m * .2f, "brick base remains y=0");
    std::printf(
        "HOST_SOURCE artifact=%s triangles=%zu body_lods=%zu policy_rungs=%zu "
        "normal_min_dot=%.9f residual=%.9f bounds=[%.6f %.6f %.6f]-[%.6f %.6f %.6f]\n",
        r.written_path.c_str(), triangles, lods.size(), variants.budgets.size(),
        min_dot, max_residual, minimum[0], minimum[1], minimum[2], maximum[0],
        maximum[1], maximum[2]);
}
} // namespace
int main() {
    matter::log::add_sink(validation_sink);
#ifdef MATTER_VK_TEST_LAYER_PATH
    SetDllDirectoryA(MATTER_VK_TEST_LAYER_PATH);
    SetEnvironmentVariableA("VK_LAYER_PATH", MATTER_VK_TEST_LAYER_PATH);
#endif
    script_host::BakeOptions opts;
    opts.parts_dir = "build/qa/solid-source-host";
    std::filesystem::create_directories(opts.parts_dir);
    script_host::ScriptHost unavailable;
    auto absent = unavailable.bake_source(simple("UnavailableSolid"), "{}", opts);
    HOST_CHECK(!absent.error.ok && absent.error.code == "solid-source-unavailable" &&
                   absent.written_path.empty(),
               "headless source fails explicitly without artifact");
    if (glfwInit() != GLFW_TRUE) {
        HOST_CHECK(false, "GLFW initialization");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    GLFWwindow *window =
        glfwCreateWindow(800, 450, "Solid source validation: starting", nullptr, nullptr);
    std::string error;
    auto vk = window ? matter::VulkanDevice::create(window, true, error) : nullptr;
    HOST_CHECK(vk != nullptr, error.c_str());
    if (vk) {
        {
            gpu_meshing::GpuSolidMesher service(*vk);
            script_host::ScriptHost host;
            host.set_shared_lib_roots(
                {"projects/world_demo/shared-lib", "MatterEngine3/shared-lib"});
            std::vector<gpu_meshing::SolidOp> captured;
            gpu_meshing::SolidJob captured_job;
            gpu_meshing::SolidStats last_stats;
            unsigned calls = 0;
            auto callback = [&](const gpu_meshing::SolidJob &job,
                                gpu_meshing::MeshResult &result,
                                gpu_meshing::SolidStats &stats, gpu_meshing::Error &e,
                                const gpu_meshing::BuildControl &control) {
                ++calls;
                captured.assign(job.ops, job.ops + job.op_count);
                captured_job = job;
                captured_job.ops = captured.data();
                const bool ok = service.build(job, result, stats, e, control);
                last_stats = stats;
                return ok;
            };
            host.set_solid_source_baker(callback, {}, 42);
            for (const auto &source :
                 {simple("ScaledSolid", "this.scale(2,1,1);"),
                  simple("ReflectedSolid", "this.scale(-1,1,1);"),
                  simple("UnknownSolid", "", "noise:1,"),
                  simple("FractionalVersion")
                      .replace(simple("FractionalVersion").find("version:1"), 9,
                               "version:1.000000001")}) {
                const auto before = calls;
                auto result = host.bake_source(source, "{}", opts);
                HOST_CHECK(!result.error.ok && result.written_path.empty() &&
                               calls == before,
                           "invalid scale/reflection/unsupported semantics rejected "
                           "before GPU");
            }
            const std::string source =
                read("projects/world_demo/objects/CastleStoneSource.js");
            HOST_CHECK(!source.empty(), "real CastleStoneSource consumer exists");
            for (float spacing : {.006f, .003f})
                for (unsigned seed = 0; seed < 8; ++seed) {
                    glfwPollEvents();
                if(glfwWindowShouldClose(window)) {
                    HOST_CHECK(false,"visible source validation closed by user");
                    break;
                }
                const std::string title="Solid source validation: seed "+std::to_string(seed)+
                    " / 7, spacing "+std::to_string(spacing*1000)+" mm";
                glfwSetWindowTitle(window,title.c_str());
                const auto begin = Clock::now();
                    std::string params = "{\"seed\":" + std::to_string(seed) +
                                         ",\"voxelM\":" + std::to_string(spacing) + "}";
                    const bool cold = calls == 0;
                    auto result = host.bake_source(source, params, opts);
                    double total =
                        std::chrono::duration<double, std::milli>(Clock::now() - begin)
                            .count();
                    std::printf(
                        "HOST_SOURCE timing seed=%u voxel=%.3f cold_service=%u "
                        "bake_ms=%.4f service_ms=%.4f gpu_ms=%.4f vertices=%u\n",
                        seed, spacing, cold ? 1u : 0u, total, last_stats.host_ms,
                        last_stats.gpu_ms, last_stats.vertices);
                    inspect(result, captured_job, .30f, .14f, .20f);
                }
            unsigned checks = 0;
            gpu_meshing::BuildControl control;
            control.cancelled = [&]() { return ++checks >= 7; };
            host.set_solid_source_baker(callback, control, 42);
            auto cancelled = host.bake_source(simple("LateCancelledSolid"), "{}", opts);
            HOST_CHECK(!cancelled.error.ok && cancelled.written_path.empty() &&
                           cancelled.error.code == "solid-source-stale",
                       "cancel during host phase prevents bundle publication");
            std::printf("HOST_SOURCE late_cancel_checks=%u\n", checks);
            // Validate the callback boundary itself, independently of the GPU's
            // already-gated output: a malformed backend cannot publish a bundle.
            host.set_solid_source_baker(
                [](const gpu_meshing::SolidJob &job, gpu_meshing::MeshResult &r,
                   gpu_meshing::SolidStats &, gpu_meshing::Error &,
                   const gpu_meshing::BuildControl &) {
                    r.material = job.material;
                    r.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
                    r.normals.resize(9);
                    r.indices = {0, 1, 2};
                    return true;
                });
            auto malformed =
                host.bake_source(simple("MalformedNormalSolid"), "{}", opts);
            HOST_CHECK(!malformed.error.ok &&
                           malformed.error.code == "solid-source-invalid-result" &&
                           malformed.written_path.empty(),
                       "zero-normal callback output rejected");
        }
        vk->wait_idle();
        HOST_CHECK(vk->validation_error_count() == 0,
                   "no validation errors before device teardown");
        vk.reset();
    }
    if (window)
        glfwDestroyWindow(window);
    glfwTerminate();
    HOST_CHECK(validation_errors.load() == 0,
               "no validation errors through device shutdown");
    matter::log::remove_sink(validation_sink);
    std::printf("HOST_SOURCE validation_errors=%u %s failures=%d\n",
                validation_errors.load(), failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
