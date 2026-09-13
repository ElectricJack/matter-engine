#include "matter/windows_compat.h"
#define GLFW_INCLUDE_NONE
#include "matter/log.h"
#include "matter/vulkan_device.h"
#include "render/gpu_meshing/gpu_solid_face_projector_vk.h"
#include "script_host.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
using namespace gpu_meshing;
namespace {
int failures = 0;
std::atomic<unsigned> validation_errors{0};
#define CHECK(x, m)                                                                                \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::printf("FAIL face GPU: %s\n", m);                                                 \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
void sink(matter::log::Level, const char *tag, const char *message, void *) {
    if (tag && message && std::string(tag) == "vk" &&
        std::string(message).find("Vulkan validation ERROR") != std::string::npos)
        ++validation_errors;
}
std::string read(const char *path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
struct Source {
    script_host::EvaluatedSolidSource recipe;
    FacePatch front, back;
};
FaceJob face(const Source &s, bool back) {
    FaceJob j;
    j.source = s.recipe.job();
    j.source_identity = s.recipe.resolved_hash;
    j.frame.origin_m = {0, .07f, 0};
    if (back) {
        j.frame.u = {-1, 0, 0};
        j.frame.n = {0, 0, -1};
    }
    j.u_min_m = -.16f;
    j.u_max_m = .16f;
    j.v_min_m = -.08f;
    j.v_max_m = .08f;
    j.height_min_m = -.12f;
    j.height_max_m = .12f;
    j.pixel_m = .003f;
    return j;
}
void compare(const FacePatch &gpu, const FacePatch &cpu) {
    CHECK(gpu.texels.size() == cpu.texels.size(), "oracle dimensions");
    if (gpu.texels.size() != cpu.texels.size())
        return;
    double max_height = 0, min_dot = 1;
    unsigned coverage_errors = 0;
    for (size_t i = 0; i < gpu.texels.size(); ++i) {
        auto &a = gpu.texels[i];
        auto &b = cpu.texels[i];
        if (a.coverage != b.coverage) {
            ++coverage_errors;
            continue;
        }
        if (!a.coverage)
            continue;
        max_height = std::max(max_height, double(std::abs(a.height_m - b.height_m)));
        min_dot = std::min(min_dot, double(a.normal_uvn.x * b.normal_uvn.x +
                                           a.normal_uvn.y * b.normal_uvn.y +
                                           a.normal_uvn.z * b.normal_uvn.z));
    }
    std::printf(
        "FACE oracle pixels=%zu coverage_mismatch=%u max_height_error_m=%.9g min_normal_dot=%.9g\n",
        gpu.texels.size(), coverage_errors, max_height, min_dot);
    CHECK(coverage_errors == 0, "CPU/GPU finite coverage agreement");
    CHECK(max_height < .00003, "CPU/GPU height agreement");
    CHECK(min_dot > .999, "CPU/GPU same-field normal agreement");
}
} // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    matter::log::add_sink(sink);
#ifdef MATTER_VK_TEST_LAYER_PATH
    SetDllDirectoryA(MATTER_VK_TEST_LAYER_PATH);
    SetEnvironmentVariableA("VK_LAYER_PATH", MATTER_VK_TEST_LAYER_PATH);
#endif
    if (!glfwInit())
        return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    auto *window =
        glfwCreateWindow(800, 450, "Finite brick face projection: starting", nullptr, nullptr);
    std::string message;
    auto vk = window ? matter::VulkanDevice::create(window, true, message) : nullptr;
    CHECK(vk != nullptr, message.c_str());
    if (vk) {
        GpuSolidFaceProjector service(*vk);
        script_host::ScriptHost host;
        host.set_shared_lib_roots({"projects/world_demo/shared-lib", "MatterEngine3/shared-lib"});
        Source sources[8];
        const auto source = read("projects/world_demo/objects/CastleStoneSource.js");
        CHECK(!source.empty(), "actual recipe source available");
        for (unsigned seed = 0; seed < 8; ++seed) {
            script_host::BakeError error;
            bool ok = host.evaluate_solid_source(
                source, "{\"seed\":" + std::to_string(seed) + ",\"voxelM\":0.003}",
                sources[seed].recipe, error);
            CHECK(ok, error.message.c_str());
            if (!ok)
                break;
        }
        // Analytic grazing corner and later-union guard run through the same
        // native service before actual recipes. Keep all original precision and
        // iteration limits; the correction proves misses, not looser hits.
        SolidOp grazing_ops[2];
        grazing_ops[0].kind[0] = 1;
        grazing_ops[0].shape = {.1475f, .0675f, .0975f, .0025f};
        FaceJob grazing;
        grazing.source.ops = grazing_ops;
        grazing.source.op_count = 1;
        grazing.source.voxel_m = .003f;
        grazing.u_min_m = .14930f; grazing.u_max_m = .14940f;
        grazing.v_min_m = .06930f; grazing.v_max_m = .06940f;
        grazing.height_min_m = -.12f; grazing.height_max_m = .12f;
        grazing.pixel_m = .0001f;
        FacePatch corner;
        FaceStats corner_stats;
        Error corner_error;
        CHECK(service.project(grazing, corner, corner_stats, corner_error), corner_error.message.c_str());
        CHECK(corner_stats.covered_pixels == 0 && corner_stats.max_steps_used == 0,
              "GPU rounded corner interval proves miss");
        grazing_ops[1].kind[0] = 2;
        // Keep the union surface near ray entry: this tests proof admission,
        // not acceleration of the deliberately unchanged general marcher.
        grazing_ops[1].shape = {.11f, 0, 0, 0};
        grazing_ops[1].row0[3] = -.14935f;
        grazing_ops[1].row1[3] = -.06935f;
        grazing.source.op_count = 2;
        CHECK(service.project(grazing, corner, corner_stats, corner_error), corner_error.message.c_str());
        CHECK(corner_stats.covered_pixels == corner.texels.size() && corner_stats.covered_pixels > 0,
              "GPU later union disables base-only proof");
        BuildControl control;
        control.cancelled = [&] {
            glfwPollEvents();
            return glfwWindowShouldClose(window) != 0;
        };
        // Timed GPU batch first: no CPU projection, serialization or compression in these spans.
        for (unsigned seed = 0; seed < 8; ++seed)
            for (unsigned side = 0; side < 2; ++side) {
                if (sources[seed].recipe.source.ops.empty())
                    continue;
                auto j = face(sources[seed], side != 0);
                auto &patch = side ? sources[seed].back : sources[seed].front;
                FaceStats stats;
                Error e;
                std::string title = "Finite brick face GPU: seed " + std::to_string(seed) +
                                    (side ? " back" : " front");
                glfwSetWindowTitle(window, title.c_str());
                bool ok = service.project(j, patch, stats, e, control);
                CHECK(ok, e.message.c_str());
                if (!ok)
                    continue;
                std::printf(
                    "FACE seed=%u side=%u digest=%016llx dimensions=%ux%u pitch_m=%.9g,%.9g "
                    "covered=%u steps=%u host_ms=%.4f prepare_ms=%.4f submit_ms=%.4f "
                    "decode_ms=%.4f gpu_ms=%.4f readback_flags=%u resident_bytes=%llu\n",
                    seed, side, (unsigned long long)patch.recipe_digest, patch.layout.width,
                    patch.layout.height, patch.layout.pitch_u_m, patch.layout.pitch_v_m,
                    stats.covered_pixels, stats.max_steps_used, stats.host_ms, stats.prepare_ms,
                    stats.submit_wait_ms, stats.decode_ms, stats.gpu_ms,
                    stats.readback_memory_flags, (unsigned long long)stats.resident_bytes);
            }
        for (unsigned seed = 0; seed < 8; ++seed)
            for (unsigned side = 0; side < 2; ++side) {
                auto &patch = side ? sources[seed].back : sources[seed].front;
                if (sources[seed].recipe.source.ops.empty())
                    continue;
                std::string title = "Finite brick face CPU oracle: seed " + std::to_string(seed) +
                                    (side ? " back" : " front");
                glfwSetWindowTitle(window, title.c_str());
                auto j = face(sources[seed], side != 0);
                FacePatch cpu;
                FaceStats stats;
                Error e;
                bool ok = project_solid_face_reference(j, cpu, stats, e, control);
                CHECK(ok, e.message.c_str());
                if (ok) {
                    if (!patch.texels.empty()) compare(patch, cpu);
                    std::printf("FACE oracle_host_ms=%.4f (excluded from GPU timings)\n",
                                stats.host_ms);
                }
            }
        if (!sources[0].front.texels.empty()) {
            auto j = face(sources[0], false);
            FacePatch preserved = sources[0].front;
            const auto digest = preserved.recipe_digest;
            FaceStats stats;
            Error e;
            j.max_steps = 1;
            CHECK(!service.project(j, preserved, stats, e, control) &&
                      e.code == ErrorCode::LimitExceeded,
                  "GPU exhausted march explicitly fails");
            CHECK(preserved.recipe_digest == digest, "failed GPU request preserves patch");
            j = face(sources[0], false);
            CHECK(!service.project(j, preserved, stats, e, {[] { return true; }, {}}) &&
                      e.code == ErrorCode::Cancelled,
                  "GPU cancellation");
            CHECK(!service.project(j, preserved, stats, e,
                                   {{}, [](std::uint64_t) { return false; }}) &&
                      e.code == ErrorCode::StaleGeneration,
                  "GPU stale generation");
        }
    }
    vk.reset();
    if (window)
        glfwDestroyWindow(window);
    glfwTerminate();
    CHECK(validation_errors.load() == 0, "zero Vulkan validation errors");
    std::printf("solid_face_projection_gpu_tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
