#include "shader_source.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static std::string read_shader(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

int main() {
    std::string text, err;
    // 1. embedded lookup works and matches the on-disk source.
    // shaders_gpu/cull.comp (the GL GpuCuller compute shader) is gone --
    // its only readers were deleted in Phase 5a (tech-debt.md §6) and it was
    // dropped from SHADER_LOGICAL in MatterEngine3/Makefile -- so this
    // round-trip check uses shaders/bvh_tlas_common.glsl, a still-embedded
    // fixture, instead.
    assert(matter::shader_text("shaders/bvh_tlas_common.glsl", text, err));
    assert(text.find("layout") != std::string::npos);
    // 2. unknown path fails with a useful error
    std::string t2;
    assert(!matter::shader_text("shaders/nope.fs", t2, err));
    assert(err.find("nope.fs") != std::string::npos);
    // 3. override dir wins: write a marker file, point the override at it
    const std::filesystem::path override_dir =
        std::filesystem::absolute("build/shader_override_test");
    std::filesystem::create_directories(override_dir / "shaders");
    std::ofstream override_file(override_dir / "shaders/bvh_tlas_common.glsl");
    override_file << "// OVERRIDE MARKER\n";
    override_file.close();
    matter::set_shader_override_dir(override_dir.string().c_str());
    std::string t3;
    assert(matter::shader_text("shaders/bvh_tlas_common.glsl", t3, err));
    assert(t3.find("OVERRIDE MARKER") != std::string::npos);
    matter::set_shader_override_dir(nullptr);

    // Task 7: every production lighting consumer must use the shared physical
    // environment path; a procedural fallback would make raster/RT/fog diverge.
    const char* entries[] = {"../shaders_vk/composite.frag",
                             "../shaders_vk/rt_lighting.rgen",
                             "../shaders_vk/vol_scatter.comp"};
    for (const char* entry : entries) {
        const std::string production = read_shader(entry);
        assert(!production.empty());
        assert(production.find("procedural_sky") == std::string::npos);
        assert(production.find("sky_with_sun") == std::string::npos);
        assert(production.find("#include \"sky_common.glsl\"") == std::string::npos);
    }
    // sky_view stores azimuth relative to the sun, while its SH output is
    // evaluated later with world-space normals. The producer must rotate its
    // quadrature direction into world space before evaluating the basis.
    const std::string irradiance =
        read_shader("../shaders_vk/atmosphere_irradiance.comp");
    assert(irradiance.find("vec3 world_direction") != std::string::npos);
    assert(irradiance.find("sh(int(coefficient),world_direction)") !=
           std::string::npos);
    const std::string environment =
        read_shader("../shaders_vk/environment_common.glsl");
    assert(environment.find("layout(set = 1, binding = 0) uniform sampler2D atmosphere_sky_view") !=
           std::string::npos);
    assert(environment.find("layout(set = 1, binding = 6, std140) uniform EnvironmentBlock") !=
           std::string::npos);
    assert(environment.find("sample_sky_irradiance") != std::string::npos &&
           environment.find("environment.cloud_state.x == 0.0") !=
               std::string::npos);
    const std::string renderer = read_shader("../src/render/vk_scene_renderer.cpp");
    assert(renderer.find("VK_FORMAT_R16_SFLOAT") != std::string::npos &&
           renderer.find("record_neutral_cloud_clear") != std::string::npos &&
           renderer.find("update_environment_descriptor(selected)") !=
               std::string::npos &&
           renderer.find("volumetrics_->invalidate_history()") !=
               std::string::npos);
    const std::string atmosphere_host = read_shader("../src/render/vk_atmosphere.cpp");
    assert(atmosphere_host.find("VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR") !=
           std::string::npos);
    printf("shader_source_tests: all passed\n");
    return 0;
}
