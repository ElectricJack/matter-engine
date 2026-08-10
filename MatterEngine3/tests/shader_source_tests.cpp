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

static size_t count_occurrences(const std::string& text,
                                const std::string& needle) {
    size_t count = 0;
    for (size_t offset = 0;
         (offset = text.find(needle, offset)) != std::string::npos;
         offset += needle.size())
        ++count;
    return count;
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
    assert(environment.find("fract(azimuth_u)") != std::string::npos);
    assert(environment.find("clamp(v, 0.5 / 108.0, 107.5 / 108.0)") !=
           std::string::npos);
    // Task 2: visible sky, post-9SH irradiance, direct-world sunlight and the
    // analytic disc are four independent CPU-resolved lanes in one UBO.  None
    // of the three consumers may retain its former private sun/sky colours or
    // reconstruct an elevation curve in GLSL.
    for (const char* name : {"vec4 direct_world_sun_ratio;",
                             "vec4 sun_disc_reserved;",
                             "vec4 sky_display_reserved;",
                             "vec4 sky_irradiance_ambient_ratio;"})
        assert(environment.find(name) != std::string::npos);
    const size_t sh_loop = environment.find(
        "for (int coefficient = 0; coefficient < 9; ++coefficient)");
    const size_t irradiance_modifier = environment.find(
        "environment.sky_irradiance_ambient_ratio.rgb", sh_loop);
    assert(sh_loop != std::string::npos && irradiance_modifier != std::string::npos &&
           irradiance_modifier > sh_loop);
    assert(environment.find("environment.sky_display_reserved.rgb") !=
           std::string::npos);
    for (const char* entry : entries) {
        const std::string production = read_shader(entry);
        for (const char* forbidden : {"lighting.sky_color", "lighting.sun_color",
                                      "constants.sky_color", "constants.sun_color",
                                      "pc.sky_color", "pc.sun_color",
                                      "smoothstep(-6", "smoothstep(0,5",
                                      "elevation_deg"})
            assert(production.find(forbidden) == std::string::npos);
    }
    const std::string composite = read_shader("../shaders_vk/composite.frag");
    const std::string rt = read_shader("../shaders_vk/rt_lighting.rgen");
    const std::string volume = read_shader("../shaders_vk/vol_scatter.comp");
    assert(composite.find("environment.direct_world_sun_ratio.rgb") !=
               std::string::npos &&
           composite.find("environment.sun_disc_reserved.rgb") !=
               std::string::npos);
    assert(rt.find("environment.direct_world_sun_ratio.rgb") !=
               std::string::npos &&
           rt.find("environment.sun_disc_reserved.rgb") !=
               std::string::npos &&
           rt.find("environment.sky_display_reserved.rgb") !=
               std::string::npos);
    assert(volume.find("environment.direct_world_sun_ratio.rgb") !=
               std::string::npos &&
           volume.find("sample_sky_irradiance") != std::string::npos);
    // Task 12: one superset scatter shader specializes the enhanced cloud
    // path away for Current cost, reads the R16F detail grid only when live,
    // and consumes the post-adjustment environment lighting ABI.
    for (const char* required : {
             "layout(constant_id = 0) const bool ENHANCED_CLOUD_LIGHTING",
             "uniform sampler3D vol_cloud_density",
             "world_to_froxel_uvw",
             "tau_local_full + tau_remaining_coarse"})
        assert(volume.find(required) != std::string::npos);
    const std::string volume_common =
        read_shader("../shaders_vk/vol_common.glsl");
    assert(volume_common.find(
               "0.8 * hg_phase(mu, 0.85 * anisotropy_scale)") !=
               std::string::npos &&
           volume_common.find(
               "0.2 * hg_phase(mu, -0.30 * anisotropy_scale)") !=
               std::string::npos);
    // Terrain occlusion for cloud-bearing froxels extends the existing ray;
    // it must not silently add a second per-froxel query or lengthen the
    // fog-only/Current path.  Ordering pins the density decision before the
    // one real ray and its sanitized value before cloud scattering consumes it.
    const size_t cloud_sample = volume.find(
        "cloud_extinction = texture(vol_cloud_density, uvw).r");
    const size_t ray_query = volume.find("rayQueryInitializeEXT");
    const size_t cloud_scattering = volume.find(
        "vec3 cloud_scattering = vec3(0.99) * cloud_extinction");
    assert(volume_common.find(
               "const float VOL_CLOUD_TERRAIN_SHADOW_FAR = VOL_FROXEL_FAR") !=
               std::string::npos);
    assert(volume.find("cloud_extinction > 1e-6") != std::string::npos);
    assert(volume.find("VOL_CLOUD_TERRAIN_SHADOW_FAR") != std::string::npos);
    assert(count_occurrences(volume, "rayQueryInitializeEXT") == 1);
    assert(cloud_sample != std::string::npos && ray_query != std::string::npos &&
           cloud_scattering != std::string::npos && cloud_sample < ray_query &&
           ray_query < cloud_scattering);
    assert(volume.find("pc.sun_color") == std::string::npos &&
           volume.find("pc.sky_color") == std::string::npos &&
           volume.find("sun_intensity") == std::string::npos);
    const std::string volumetrics_header =
        read_shader("../src/render/vk_volumetrics.h");
    assert(volumetrics_header.find(
               "static_assert(sizeof(ScatterConstants) == 240)") !=
               std::string::npos);
    for (const char* offset : {"offsetof(ScatterConstants, camera_pos) == 128",
                               "offsetof(ScatterConstants, local_sun_march_steps) == 156",
                               "offsetof(ScatterConstants, camera_fwd) == 192",
                               "offsetof(ScatterConstants, local_march_distance_m) == 236"})
        assert(volumetrics_header.find(offset) != std::string::npos);
    assert(volumetrics_header.find("sun_intensity") == std::string::npos);
    const std::string volumetrics_host =
        read_shader("../src/render/vk_volumetrics.cpp");
    assert(volumetrics_host.find(
               "volumetric_camera_eye(matrices.world_to_view)") !=
           std::string::npos);
    assert(volume.find("view_depth + 1.0e-5 < pc.camera_near") !=
           std::string::npos);
    const std::string sky_view =
        read_shader("../shaders_vk/atmosphere_sky_view.comp");
    assert(sky_view.find("(float(pixel.x) + 0.5) / 192.0") !=
           std::string::npos);
    assert(sky_view.find("float(pixel.x) / 191.0") == std::string::npos);
    const std::string display =
        read_shader("../shaders_vk/display_transform.frag");
    const std::string ranks =
        "37,12,54,1,46,27,61,8,18,43,5,58,31,50,14,40,"
        "63,22,35,10,48,3,56,29,16,45,7,60,25,52,11,38,"
        "33,0,47,20,57,15,42,30,9,53,24,62,4,36,19,51,"
        "41,13,55,28,59,6,44,21,26,49,2,39,17,34,23,32";
    assert(display.find(ranks) != std::string::npos);
    for (const char* required : {"gl_FragCoord", "linear_to_srgb",
                                 "srgb_to_linear", "code_dithered"})
        assert(display.find(required) != std::string::npos);
    for (const char* forbidden : {"frame_index", "jitter", "time",
                                  "random", "pcg"})
        assert(display.find(forbidden) == std::string::npos);
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
    // Review regressions: sky-view must be visible to every Task 7 consumer,
    // and the environment UBO must be mapped before its neutral std140 bytes
    // are initialized. These source contracts complement the real raster
    // readback because neither property is uniquely observable in one pixel.
    const size_t sky_transition = atmosphere_host.find(
        "record_image_transition(command_buffer, sky_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL");
    assert(sky_transition != std::string::npos);
    const std::string sky_tail = atmosphere_host.substr(sky_transition, 520);
    assert(sky_tail.find("VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |") !=
           std::string::npos &&
           sky_tail.find("VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR") !=
               std::string::npos);
    assert(renderer.find("matter::map_buffer(frame.environment_constants, error)") !=
           std::string::npos &&
           renderer.find("environment_constants.mapped == nullptr") ==
               std::string::npos);
    const size_t neutral_environment_block = renderer.find("float block[56]{};");
    assert(neutral_environment_block != std::string::npos);
    const std::string neutral_environment_tail =
        renderer.substr(neutral_environment_block, 3200);
    assert(neutral_environment_tail.find("block[0] = block[5] = block[10] = block[15] = 1.0f;") !=
               std::string::npos &&
           neutral_environment_tail.find("block[16] = block[21] = block[26] = block[31] = 1.0f;") !=
               std::string::npos &&
           neutral_environment_tail.find("std::memcpy(frame.environment_constants.mapped") !=
               std::string::npos &&
           neutral_environment_tail.find("matter::flush_buffer(frame.environment_constants,") !=
               std::string::npos);
    const std::string engine_make = read_shader("../Makefile");
    const std::string editor_make = read_shader("../../MatterEditor/Makefile");
    assert(engine_make.find("build/shaders_vk/vol_scatter.comp.spv: shaders_vk/environment_common.glsl") !=
           std::string::npos);
    assert(editor_make.find("build/shaders_vk/vol_scatter.comp.spv: $(ME3_DIR)/shaders_vk/environment_common.glsl") !=
           std::string::npos);
    printf("shader_source_tests: all passed\n");
    return 0;
}
