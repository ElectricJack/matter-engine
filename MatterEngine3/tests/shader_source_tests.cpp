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

    // Baked water animation stays packed until a selected 30 Hz frame reaches
    // its Vulkan slot. The compute ABI and the 28-byte raster specialization
    // must remain explicit source/inventory entries.
    const std::string water_decode =
        read_shader("../shaders_vk/water_animation_decode.comp");
    const std::string raster_vertex =
        read_shader("../shaders_vk/raster.vert");
    const std::string root_cmake = read_shader("../../CMakeLists.txt");
    const std::string engine_makefile = read_shader("../Makefile");
    assert(water_decode.find("local_size_x = 64") != std::string::npos);
    assert(water_decode.find("uint words[]") != std::string::npos);
    assert(water_decode.find("WaterAnimationVertex") != std::string::npos);
    assert(water_decode.find("decoded_vertices.vertices") !=
           std::string::npos);
    assert(raster_vertex.find("MATTER_WATER_ANIMATION_VERTEX_INPUT") !=
           std::string::npos);
    const std::string cull_shader = read_shader("../shaders_vk/cull.comp");
    assert(cull_shader.find("if (instance.water_pad0 != 0u) return;") !=
           std::string::npos);
    assert(root_cmake.find(
               "raster_water.vert|raster.vert|MATTER_WATER_ANIMATION_VERTEX_INPUT") !=
           std::string::npos);
    assert(engine_makefile.find("water_animation_decode.comp.spv") !=
               std::string::npos &&
           engine_makefile.find("raster_water.vert.spv") !=
               std::string::npos);

    // River presentation Task 8: raster and RT must share one bounded water
    // evaluator. Private copies inevitably drift at reset boundaries and make
    // the static mesh appear different in reflections than in the G-buffer.
    const std::string water_common =
        read_shader("../shaders_vk/water_surface.glsl");
    const std::string water_raster =
        read_shader("../shaders_vk/gbuffer.frag");
    const std::string water_rt =
        read_shader("../shaders_vk/rt_lighting.rgen");
    const std::string water_visibility =
        read_shader("../shaders_vk/rt_visibility.rahit");
    assert(!water_common.empty());
    assert(water_raster.find("#include \"water_surface.glsl\"") !=
           std::string::npos);
    assert(water_rt.find("#include \"water_surface.glsl\"") !=
           std::string::npos);
    assert(count_occurrences(water_common, "vec2 water_backtrace_rk2(") == 1u);
    assert(count_occurrences(water_raster, "water_backtrace_rk2(") == 0u);
    assert(count_occurrences(water_rt, "water_backtrace_rk2(") == 0u);
    assert(water_common.find("const int WATER_BACKTRACE_STEPS = 3;") !=
           std::string::npos);
    assert(water_common.find("const int WATER_WAVE_BAND_COUNT = 3;") !=
           std::string::npos);
    assert(water_common.find("const int WATER_PHASE_COUNT = 2;") !=
           std::string::npos);
    assert(water_common.find(
               "for (int step = 0; step < WATER_BACKTRACE_STEPS; ++step)") !=
           std::string::npos);
    assert(water_common.find(
               "for (int band = 0; band < WATER_WAVE_BAND_COUNT; ++band)") !=
           std::string::npos);
    assert(water_common.find("uniform sampler2D water_field_d") !=
           std::string::npos);
    assert(water_common.find("optics_shallow") != std::string::npos &&
           water_common.find("foam_controls") != std::string::npos &&
           water_common.find("water_breakup_noise") != std::string::npos);
    assert(water_raster.find("out_reactivity") != std::string::npos);
    // A closed water mesh contributes an entry and exit any-hit to a sun
    // shadow ray.  Its dark display albedo is not an absorption coefficient:
    // using it as the tint twice blackens the riverbed even when the authored
    // shallow optics are almost clear.  Water shadows use the material's
    // optical absorption lane (with the same black-means-clear compatibility
    // rule as the refraction walk); ordinary colored glass stays unchanged.
    assert(water_visibility.find("WATER_SURFACE_MATERIAL_FLAG") !=
           std::string::npos);
    assert(water_visibility.find("material.absorption_pad.rgb") !=
           std::string::npos);
    assert(water_visibility.find("water_shadow_absorption") !=
           std::string::npos);
    const std::string gi_temporal =
        read_shader("../shaders_vk/gi_temporal.comp");
    const std::string gi_atrous =
        read_shader("../shaders_vk/gi_atrous.comp");
    assert(gi_temporal.find("reactivityTex") != std::string::npos &&
           gi_temporal.find("float alpha = mix") != std::string::npos);
    assert(gi_atrous.find("reactivityTex") != std::string::npos &&
           gi_atrous.find("float wr = exp") != std::string::npos);

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
           renderer.find("update_environment_descriptor(selected, error)") !=
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
    assert(sky_tail.find("sampled_shader_stages_") != std::string::npos);
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
    // 2026-08-14: MatterEditor/Makefile used to keep a second, independently
    // maintained copy of the VK_SPV list and every .glsl dependency edge,
    // regenerating the SAME shaders_gen/embedded_spirv.h -- the two drifted,
    // and this test used to guard the drift directly by asserting both
    // copies had this one edge. MatterEngine3/Makefile is now the single
    // source of truth and MatterEditor delegates to its `vulkan-spirv`
    // target instead, so the guard now is that the delegation exists and
    // the old duplicate edge is gone (a duplicate reappearing here is
    // exactly the drift hazard this test exists to catch).
    assert(editor_make.find("embedded-spirv") != std::string::npos &&
           editor_make.find("$(MAKE) -C $(ME3_DIR) vulkan-spirv") != std::string::npos);
    assert(editor_make.find("build/shaders_vk/vol_scatter.comp.spv: $(ME3_DIR)/shaders_vk/environment_common.glsl") ==
           std::string::npos);
    printf("shader_source_tests: all passed\n");
    return 0;
}
