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

static size_t matching_closing_brace(const std::string& text,
                                     size_t opening_brace) {
    if (opening_brace == std::string::npos ||
        opening_brace >= text.size() || text[opening_brace] != '{')
        return std::string::npos;
    int depth = 0;
    for (size_t offset = opening_brace; offset < text.size(); ++offset) {
        if (text[offset] == '{')
            ++depth;
        else if (text[offset] == '}' && --depth == 0)
            return offset;
    }
    return std::string::npos;
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

    // Baked water animation stays packed through the GPU upload. The water
    // vertex specialization decodes the 12-byte file ABI directly, avoiding
    // a full-frame compute expansion and its device-memory round trip.
    const std::string raster_vertex =
        read_shader("../shaders_vk/raster.vert");
    const std::string renderer_header =
        read_shader("../src/render/vk_scene_renderer.h");
    const std::string renderer_source =
        read_shader("../src/render/vk_scene_renderer.cpp");
    const std::string water_animation_header =
        read_shader("../src/render/water_animation_gpu.h");
    const std::string root_cmake = read_shader("../../CMakeLists.txt");
    const std::string engine_makefile = read_shader("../Makefile");
    assert(raster_vertex.find("MATTER_WATER_ANIMATION_VERTEX_INPUT") !=
           std::string::npos);
    assert(raster_vertex.find("uvec3 in_water_packed") != std::string::npos);
    assert(raster_vertex.find("decode_water_octahedral") != std::string::npos);
    assert(raster_vertex.find("debug_push.water_bounds_min") !=
           std::string::npos);
    const std::string cull_shader = read_shader("../shaders_vk/cull.comp");
    assert(cull_shader.find("if (instance.water_pad0 != 0u) return;") !=
           std::string::npos);
    assert(root_cmake.find(
               "raster_water.vert|raster.vert|MATTER_WATER_ANIMATION_VERTEX_INPUT") !=
           std::string::npos);
    assert(engine_makefile.find("water_animation_decode.comp.spv") ==
               std::string::npos);
    assert(engine_makefile.find("water_animation_rt_decode.comp.spv") ==
               std::string::npos);
    assert(renderer_source.find("prepare_water_animation_blas") ==
               std::string::npos);
    assert(renderer_header.find("water_animation_current_blas_") ==
               std::string::npos);
    assert(water_animation_header.find("WaterAnimationRtDecodeHeader") ==
               std::string::npos);
    assert(engine_makefile.find("raster_water.vert.spv") !=
           std::string::npos);
    const size_t water_forward_inventory = engine_makefile.find(
        "build/shaders_vk/water_forward.frag.spv");
    assert(water_forward_inventory != std::string::npos);
    const size_t water_forward_dependencies = engine_makefile.find(
        "build/shaders_vk/water_forward.frag.spv:");
    assert(water_forward_dependencies != std::string::npos);
    const std::string water_forward_dependency_rule =
        engine_makefile.substr(water_forward_dependencies, 320);
    for (const char* dependency : {"shaders_vk/material_common.glsl",
                                   "shaders_vk/water_surface.glsl",
                                   "shaders_vk/water_screen_space.glsl",
                                   "shaders_vk/environment_common.glsl",
                                   "shaders_vk/cloud_shadow_common.glsl"})
        assert(water_forward_dependency_rule.find(dependency) !=
               std::string::npos);
    const std::string water_forward =
        read_shader("../shaders_vk/water_forward.frag");
    const std::string water_screen =
        read_shader("../shaders_vk/water_screen_space.glsl");
    assert(water_forward.find("water_refract_scene") != std::string::npos);
    assert(water_forward.find("water_reflect_scene") != std::string::npos);
    assert(water_forward.find("water_evaluate_optics") != std::string::npos);
    assert(water_screen.find("const int WATER_REFLECTION_STEPS = 24;") !=
           std::string::npos);
    assert(water_screen.find(
               "const int WATER_REFLECTION_REFINEMENT_STEPS = 4;") !=
           std::string::npos);
    assert(water_screen.find(
               "step < WATER_REFLECTION_STEPS") != std::string::npos);
    assert(water_screen.find(
               "refinement < WATER_REFLECTION_REFINEMENT_STEPS") !=
           std::string::npos);
    const size_t reflection_march = water_screen.find(
        "for (int step = 0; step < WATER_REFLECTION_STEPS; ++step)");
    const size_t reflection_march_open =
        water_screen.find('{', reflection_march);
    const size_t reflection_march_close =
        matching_closing_brace(water_screen, reflection_march_open);
    const size_t reflection_refinement = water_screen.find(
        "for (int refinement = 0;");
    // Brace-scope the march rather than merely counting loop tokens: the
    // single refinement phase must be after the march has exited, so a failed
    // refined candidate cannot resume marching and refine a second bracket.
    assert(reflection_march != std::string::npos &&
           reflection_march_close != std::string::npos &&
           reflection_refinement != std::string::npos &&
           reflection_refinement > reflection_march_close);
    assert(count_occurrences(water_screen,
                             "for (int refinement = 0;") == 1u);
    const size_t missing_bracket_miss = water_screen.find(
        "if (!reflection_bracket_valid)", reflection_march_close);
    const size_t invalid_refinement_miss = water_screen.find(
        "if (!refinement_valid ||", reflection_refinement);
    const size_t accepted_reflection = water_screen.find(
        "result.valid = true;", reflection_refinement);
    assert(missing_bracket_miss > reflection_march_close &&
           missing_bracket_miss < reflection_refinement &&
           invalid_refinement_miss > reflection_refinement &&
           accepted_reflection > invalid_refinement_miss);
    const size_t refraction_helper =
        water_screen.find("vec2 water_refraction_uv(");
    const size_t refraction_helper_open =
        water_screen.find('{', refraction_helper);
    const size_t refraction_helper_close =
        matching_closing_brace(water_screen, refraction_helper_open);
    assert(refraction_helper != std::string::npos &&
           refraction_helper_close != std::string::npos);
    const std::string refraction_contract = water_screen.substr(
        refraction_helper, refraction_helper_close - refraction_helper);
    assert(water_screen.find(
               "water_refraction_uv(result.uv, normal.xz,") !=
           std::string::npos);
    assert(refraction_contract.find(
               "normal_xz * max(optical_distance_m, 0.0)") !=
           std::string::npos);
    assert(refraction_contract.find("water_clamp_pixel_offset(") !=
               std::string::npos &&
           refraction_contract.find(
               "water_forward.viewport_refraction.z") !=
               std::string::npos);
    assert(refraction_contract.find(
               "source_uv + offset_px / viewport") !=
           std::string::npos);
    assert(water_screen.find("water_world + normal") ==
           std::string::npos);
    assert(water_forward.find("sample_physical_sky") != std::string::npos);
    for (const char* forbidden : {"visibility_texture", "raw_diffuse",
                                  "raw_specular", "raw_transmission",
                                  "accelerationStructureEXT", "rayQuery",
                                  "traceRayEXT", "topLevelAS", "tlas",
                                  "sample_cloud_transmittance",
                                  "GL_EXT_ray_query",
                                  "GL_EXT_ray_tracing"}) {
        assert(water_forward.find(forbidden) == std::string::npos);
        assert(water_screen.find(forbidden) == std::string::npos);
    }
    assert(renderer_header.find("kGpuZoneWaterDecode") != std::string::npos);
    assert(renderer_header.find("kGpuZoneWaterDraw") != std::string::npos);
    assert(renderer_source.find(
               "water_forward.timing_zone = kGpuZoneWaterDraw") !=
           std::string::npos);
    assert(renderer_source.find("kGpuZoneWaterDecode") ==
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
    const std::string primary_shadow =
        read_shader("../shaders_vk/rt_shadow.rgen");
    assert(!water_common.empty());
    assert(water_raster.find("#include \"water_surface.glsl\"") !=
           std::string::npos);
    assert(renderer_header.find("uint32_t diagnostics[4]") !=
           std::string::npos);
    assert(water_screen.find("uvec4 diagnostics;") != std::string::npos);
    assert(raster_vertex.find(
               "layout(location = 17) flat out uint out_water_diagnostic_identity") !=
           std::string::npos);
    assert(water_raster.find(
               "layout(location = 17) flat in uint in_water_diagnostic_identity") !=
           std::string::npos);
    assert(water_forward.find(
               "layout(location = 17) flat in uint in_water_diagnostic_identity") !=
           std::string::npos);
    assert(raster_vertex.find("uint water_diagnostic_identity;") !=
           std::string::npos);
    assert(water_raster.find("uint water_diagnostic_identity;") !=
           std::string::npos);
    assert(water_forward.find("WATER_DIAGNOSTIC_IDENTITY") !=
               std::string::npos &&
           water_forward.find("WATER_DIAGNOSTIC_GEOMETRY_NORMAL") !=
               std::string::npos &&
           water_forward.find("WATER_DIAGNOSTIC_FOAM_DRIVER") !=
               std::string::npos);
    const size_t diagnostic_branch = water_forward.find(
        "if (diagnostic_view != WATER_DIAGNOSTIC_NONE)");
    const size_t normal_optics_branch = water_forward.find(
        "else if (surface_valid)", diagnostic_branch);
    const size_t first_optics_sample = water_forward.find(
        "water_refract_scene", diagnostic_branch);
    assert(diagnostic_branch != std::string::npos &&
           normal_optics_branch != std::string::npos &&
           first_optics_sample > normal_optics_branch);
    assert(water_forward.find(
               "normalize(in_normal) * 0.5 + 0.5") !=
           std::string::npos);
    assert(water_forward.find(
               "surface_valid ? water_foam_driver_heatmap(surface.foam.coverage) : vec3(0.0)") !=
           std::string::npos);
    assert(water_common.find("vec3 water_apply_foam_radiance") !=
           std::string::npos);
    assert(water_forward.find(
               "water_apply_foam_radiance(color, surface.foam.coverage)") !=
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
    assert(water_common.find("const int WATER_FIELD_FRINGE_RADIUS = 2;") !=
           std::string::npos);
    assert(water_common.find("water_field_record_matches") !=
           std::string::npos);
    assert(water_common.find("water_default_fringe_sample") !=
           std::string::npos);
    assert(water_common.find("water_field_a[nonuniformEXT") !=
           std::string::npos);
    assert(water_common.find("water_field_c[nonuniformEXT") !=
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
    assert(water_forward.find("bool surface_valid") != std::string::npos);
    assert(water_forward.find("else if (surface_valid)") !=
           std::string::npos);
    const size_t material_lookup = water_common.find(
        "bool water_evaluate_surface_for_material(");
    const size_t material_lookup_open =
        water_common.find('{', material_lookup);
    const size_t material_lookup_close =
        matching_closing_brace(water_common, material_lookup_open);
    const std::string material_lookup_body =
        material_lookup != std::string::npos &&
                material_lookup_close != std::string::npos
            ? water_common.substr(material_lookup,
                                  material_lookup_close - material_lookup + 1u)
            : std::string{};
    const size_t material_wet_support = material_lookup_body.find(
        "water_sample_field(slot, record.extent_generation.z, material_id");
    const size_t material_surface_evaluation = material_lookup_body.find(
        "water_evaluate_surface(");
    assert(material_wet_support != std::string::npos &&
           material_surface_evaluation != std::string::npos &&
           material_wet_support < material_surface_evaluation);
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
    // Transparent water has its own RT transmission walk, which shades the
    // terrain below the surface (including sun visibility).  The primary
    // shadow target must stay neutral for water receivers or the same caster
    // appears once on the surface and again on the riverbed.
    assert(primary_shadow.find("is_water_receiver") != std::string::npos);
    assert(primary_shadow.find(
               "if (is_water_receiver) {\n"
               "        imageStore(visibility_image, pixel, vec4(1.0));\n"
               "        return;\n"
               "    }") != std::string::npos);
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
    assert(environment.find("#ifndef ENVIRONMENT_SET") !=
           std::string::npos);
    assert(environment.find("#define ENVIRONMENT_SET 0") !=
           std::string::npos);
    assert(environment.find("#define ENVIRONMENT_SET 1") !=
           std::string::npos);
    assert(environment.find("layout(set = ENVIRONMENT_SET, binding = 0) uniform sampler2D atmosphere_sky_view") !=
           std::string::npos);
    assert(environment.find("layout(set = ENVIRONMENT_SET, binding = 6, std140) uniform EnvironmentBlock") !=
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
    const std::string local_lighting =
        read_shader("../shaders_vk/local_lighting.glsl");
    assert(!local_lighting.empty());
    for (const char* binding : {
             "layout(set = 2, binding = 0, std430)",
             "layout(set = 2, binding = 1, std430)",
             "layout(set = 2, binding = 2, std430)",
             "layout(set = 2, binding = 3, std430)",
             "layout(set = 2, binding = 4, std430)"})
        assert(local_lighting.find(binding) != std::string::npos);
    assert(local_lighting.find("0x8da6b343u") != std::string::npos &&
           local_lighting.find("0xd8163841u") != std::string::npos &&
           local_lighting.find("0xcb1ab31fu") != std::string::npos);
    assert(local_lighting.find("uvec3 reserved;") == std::string::npos &&
           local_lighting.find("uint reserved0;") != std::string::npos &&
           local_lighting.find("uint reserved1;") != std::string::npos &&
           local_lighting.find("uint reserved2;") != std::string::npos);
    assert(local_lighting.find("normalized_delta = delta / range") !=
               std::string::npos &&
           local_lighting.find("cutoff * cutoff") != std::string::npos &&
           local_lighting.find("light.kind == LOCAL_LIGHT_SPOT") !=
               std::string::npos);
    assert(local_lighting.find("vec3 metal_f") == std::string::npos &&
           local_lighting.find(
               "mix(vec3(0.04), albedo, clamp(metallic, 0.0, 1.0))") !=
               std::string::npos);
    assert(composite.find("#include \"local_lighting.glsl\"") !=
               std::string::npos &&
           composite.find("local_light_indices[offset + candidate]") !=
               std::string::npos &&
           composite.find("candidate < local_light_counts.z") !=
               std::string::npos &&
           composite.find("candidate < local_light_counts.x") ==
               std::string::npos);
    assert(rt.find("#include \"local_lighting.glsl\"") !=
               std::string::npos &&
           rt.find("layout(set = 0, binding = 26, rgba16f)") !=
               std::string::npos &&
           rt.find("local_light_indices[offset + candidate]") !=
               std::string::npos &&
           rt.find("local_light_oversized_indices[candidate]") !=
               std::string::npos);
    assert(rt.find("distance_to_sample - constants.bias") !=
               std::string::npos &&
           rt.find("gl_RayFlagsTerminateOnFirstHitEXT |") !=
               std::string::npos &&
           rt.find("0x01, 0, 0, 0, origin") != std::string::npos &&
           rt.find("0x02, 0, 0, 0,") != std::string::npos);
    assert(rt.find("surface.position, shading_normal, surface.normal") !=
               std::string::npos &&
           rt.find("hit.surface.position, hit_shading_normal,") !=
               std::string::npos);
    assert(composite.find("layout(set = 0, binding = 11) uniform sampler2D "
                          "local_direct_texture") != std::string::npos &&
           composite.find("local_light_counts.w == LOCAL_DIRECT_RAY_TRACED") !=
               std::string::npos &&
           composite.find("texture(local_direct_texture, in_uv).rgb") !=
               std::string::npos);
    assert(composite.find("raw_diffuse + local_direct.diffuse") ==
               std::string::npos &&
           composite.find("specular + local_direct.specular") ==
               std::string::npos);
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
    assert(!atmosphere_host.empty());
    // Review regressions: sky-view must be visible to every Task 7 consumer,
    // and the environment UBO must be mapped before its neutral std140 bytes
    // are initialized. These source contracts complement the real raster
    // readback because neither property is uniquely observable in one pixel.
    //
    // 2026-08-20: ray tracing is an OPTIONAL device feature and
    // VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR is ILLEGAL in a barrier
    // stage mask on a device that did not enable it
    // (VUID-VkImageMemoryBarrier2-dstStageMask-07946, ten of them per frame in
    // the vt-enrich-nort smoke mode). The "readable by every shader stage"
    // transitions therefore fold the stage in through
    // matter::ray_tracing_shader_stage(ray_tracing_available()), which yields 0
    // on an RT-less device -- so the bit is no longer named literally here and
    // these assertions pin the SAME property through that helper instead. What
    // must still hold: the RT stage is derived from the live device query, and
    // the sky-view SHADER_READ_ONLY transition's DESTINATION stage mask still
    // carries it alongside COMPUTE and FRAGMENT. Dropping ray tracing from that
    // mask, or hard-wiring the helper's argument, still fails.
    assert(atmosphere_host.find("VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR") ==
           std::string::npos);
    const size_t rt_stage_binding =
        atmosphere_host.find("VkPipelineStageFlags2 ray_tracing_stage =");
    assert(rt_stage_binding != std::string::npos);
    const std::string rt_stage_tail = atmosphere_host.substr(rt_stage_binding, 200);
    assert(rt_stage_tail.find("matter::ray_tracing_shader_stage(") !=
               std::string::npos &&
           rt_stage_tail.find("ray_tracing_available()") != std::string::npos);
    const size_t sky_transition = atmosphere_host.find(
        "record_image_transition(command_buffer, sky_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL");
    assert(sky_transition != std::string::npos &&
           rt_stage_binding < sky_transition);
    const std::string sky_tail = atmosphere_host.substr(sky_transition, 520);
    const size_t sky_destination_mask = sky_tail.find(
        "VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |");
    assert(sky_destination_mask != std::string::npos);
    // The gated ray-tracing stage has to be OR'd into that same destination
    // mask, i.e. before the destination access mask closes the argument list.
    const size_t sky_ray_tracing =
        sky_tail.find("ray_tracing_stage", sky_destination_mask);
    const size_t sky_destination_access =
        sky_tail.find("VK_ACCESS_2_SHADER_SAMPLED_READ_BIT", sky_destination_mask);
    assert(sky_ray_tracing != std::string::npos &&
           sky_destination_access != std::string::npos &&
           sky_ray_tracing < sky_destination_access);
    // The same gating covers the cloud-shadow volumes, which are transitioned
    // "readable by every shader stage" for the same reason.
    const std::string cloud_shadow_host =
        read_shader("../src/render/vk_cloud_shadows.cpp");
    assert(!cloud_shadow_host.empty());
    assert(cloud_shadow_host.find("VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR") ==
               std::string::npos &&
           cloud_shadow_host.find("matter::ray_tracing_shader_stage(") !=
               std::string::npos &&
           cloud_shadow_host.find("ray_tracing_available()") != std::string::npos);
    // The helper itself must stay conditional: a version that always returns
    // the bit would re-introduce the very validation error it exists to avoid.
    const std::string vk_resources_header =
        read_shader("../src/render/vk_resources.h");
    const size_t rt_stage_helper =
        vk_resources_header.find("ray_tracing_shader_stage(");
    assert(rt_stage_helper != std::string::npos);
    const std::string rt_stage_helper_tail =
        vk_resources_header.substr(rt_stage_helper, 260);
    assert(rt_stage_helper_tail.find("ray_tracing_available") != std::string::npos &&
           rt_stage_helper_tail.find("VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR") !=
               std::string::npos &&
           rt_stage_helper_tail.find("VkPipelineStageFlags2{0}") != std::string::npos);
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
    assert(engine_make.find(
               "build/shaders_vk/composite.frag.spv build/shaders_vk/rt_lighting.rgen.spv: \\") !=
               std::string::npos &&
           engine_make.find("    shaders_vk/local_lighting.glsl") !=
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
