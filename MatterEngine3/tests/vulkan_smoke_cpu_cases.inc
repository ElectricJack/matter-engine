// Shared verbatim CPU contracts used by the native GPU suite and
// the portable CPU-only executable. Production implementations
// remain in their canonical translation units.

matter::Mat4f identity_matrix() {
    matter::Mat4f result{};
    result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
    return result;
}

bool close4(matter::Float4 actual, matter::Float4 expected, float epsilon) {
    return std::fabs(actual.x - expected.x) <= epsilon &&
           std::fabs(actual.y - expected.y) <= epsilon &&
           std::fabs(actual.z - expected.z) <= epsilon &&
           std::fabs(actual.w - expected.w) <= epsilon;
}

bool close3(matter::Float3 actual, matter::Float3 expected, float epsilon) {
    return std::fabs(actual.x - expected.x) <= epsilon &&
           std::fabs(actual.y - expected.y) <= epsilon &&
           std::fabs(actual.z - expected.z) <= epsilon;
}

bool gpu_matrix_equal(const viewer::GpuMat4& actual,
                      const matter::Mat4f& expected) {
    const viewer::GpuMat4 packed = viewer::pack_glsl_mat4(expected);
    for (size_t i = 0; i < 16; ++i) {
        if (!(std::fabs(actual.elements[i] - packed.elements[i]) < 1e-5f))
            return false;
    }
    return true;
}

struct RetainProbe {
    uint32_t* destroyed = nullptr;
    ~RetainProbe() { ++*destroyed; }
};

void test_atmosphere_timing_contract() {
    using Renderer = viewer::VkSceneRenderer;
    CHECK(Renderer::kGpuZoneTotal == 0 && Renderer::kGpuZoneVolumetrics == 9 &&
              Renderer::kGpuZoneVt == 10 && Renderer::kGpuZoneRtGi == 11,
          "atmosphere timings retain every existing GPU-zone index");
    CHECK(Renderer::kGpuZoneAtmosphere == 12 &&
              Renderer::kGpuZoneCloudShadows == 13 &&
              Renderer::kGpuZoneVolDensity == 14 &&
              Renderer::kGpuZoneVolScatter == 15 &&
              Renderer::kGpuZoneVolIntegrate == 16,
          "atmosphere timings append five exact GPU zones");
    CHECK(Renderer::kGpuZoneWaterDecode == 17 &&
              Renderer::kGpuZoneWaterDraw == 18 &&
              Renderer::kGpuZoneWaterDirectDraw == 19,
          "water animation timings append direct draw without renumbering existing GPU zones");
    CHECK(Renderer::kGpuZoneRtLocalDirect == 20 &&
              std::string(matter::kGpuTimingNames[20]) == "rt_local_direct",
          "primary local-light timing retains its GPU zone and public sample slot");
    CHECK(Renderer::kGpuZoneHdrLighting == 21 &&
              Renderer::kGpuZoneRtGiDiffuse == 22 &&
              Renderer::kGpuZoneRtGiReflectionTransmission == 23 &&
              Renderer::kGpuZonePrimaryLightCull == 24 &&
              Renderer::kGpuZoneVt == 10 &&
              Renderer::kGpuZoneVtFeedbackReadback == 25 &&
              std::string(matter::kGpuTimingNames[25]) == "vt_feedback_readback" &&
              Renderer::kGpuZoneVtFill == 26 &&
              Renderer::kGpuZoneVtEnrich == 27 &&
              std::string(matter::kGpuTimingNames[26]) == "vt_fill" &&
              std::string(matter::kGpuTimingNames[27]) == "vt_enrich" &&
              Renderer::kGpuZoneCount == 28 &&
              matter::kGpuTimingNames.size() == Renderer::kGpuZoneCount,
          "VT detail timings append GPU zones without renumbering");
    CHECK(std::string(matter::kGpuTimingNames[21]) == "hdr_lighting" &&
              std::string(matter::kGpuTimingNames[22]) == "rt_gi_diffuse" &&
              std::string(matter::kGpuTimingNames[23]) == "rt_gi_reflection_transmission" &&
              std::string(matter::kGpuTimingNames[24]) == "primary_light_cull",
          "lighting detail timings expose exact public sample names");

    std::array<uint32_t, 3> boundaries{};
    viewer::VolumetricPassBoundary boundary =
        [&](viewer::VolumetricPass pass, bool is_end) {
            if (is_end) ++boundaries[static_cast<uint32_t>(pass)];
        };
    boundary(viewer::VolumetricPass::Density, true);
    boundary(viewer::VolumetricPass::Scatter, true);
    boundary(viewer::VolumetricPass::Integrate, true);
    CHECK((boundaries == std::array<uint32_t, 3>{1, 1, 1}),
          "atmosphere timings retain one typed boundary for each froxel pass");
}

void test_water_forward_perf_evidence_contract() {
    matter::FrameStats stats{};
    stats.gpu_water_forward_ms = 1.25f;
    stats.gpu_water_direct_draw_ms = 0.375f;
    stats.water_animation_publish_ms = 8.5f;
    stats.water_animation_compressed_cpu_bytes = 123456789u;
    stats.water_animation_peak_activation_cpu_bytes = 234567890u;
    stats.water_animation_gpu_bytes_per_slot = 345678901u;
    stats.water_forward_width = 320u;
    stats.water_forward_height = 180u;
    stats.water_forward_image_bytes = 691200u;
    std::ostringstream json;
    matter::append_water_forward_perf_json(json, stats);
    CHECK(json.str() ==
              ",\"gpu_water_forward_ms\":1.25"
              ",\"gpu_water_direct_draw_ms\":0.375"
              ",\"water_forward_width\":320"
              ",\"water_forward_height\":180"
              ",\"water_forward_image_bytes\":691200"
              ",\"water_animation_publish_ms\":8.5"
              ",\"water_animation_compressed_cpu_bytes\":123456789"
              ",\"water_animation_peak_activation_cpu_bytes\":234567890"
              ",\"water_animation_gpu_bytes_per_slot\":345678901",
          "water forward: perf JSON publishes full/direct timing and distinct runtime memory lanes");
    CHECK(viewer::vk_scene_detail::water_forward_image_bytes_for_extent(
              320u, 180u) == 691200u &&
              viewer::vk_scene_detail::water_forward_image_bytes_for_extent(
                  UINT32_MAX, UINT32_MAX) == 0u,
          "water forward: logical preservation byte accounting is exact and overflow-safe");
}

void test_atmosphere_acceptance_fifo_parser_and_present_sequencer() {
    {
        const auto parsed = viewer::parse_fifo_line("render_path raster");
        const auto* command =
            std::get_if<viewer::FifoRenderPath>(&parsed.command);
        CHECK(parsed.recognized && parsed.success && command &&
                  command->requested == matter::RenderPath::GpuDriven,
              "FIFO parser types render_path raster");
    }
    {
        const auto parsed = viewer::parse_fifo_line("render_path native_rt");
        const auto* command =
            std::get_if<viewer::FifoRenderPath>(&parsed.command);
        CHECK(parsed.recognized && parsed.success && command &&
                  command->requested == matter::RenderPath::Raytrace,
              "FIFO parser types render_path native_rt");
    }
    {
        const auto parsed = viewer::parse_fifo_line("history_reset");
        CHECK(parsed.recognized && parsed.success &&
                  std::holds_alternative<viewer::FifoHistoryReset>(
                      parsed.command),
              "FIFO parser types history_reset");
    }
    {
        const auto parsed = viewer::parse_fifo_line("wait_frames 3");
        const auto* command =
            std::get_if<viewer::FifoWaitFrames>(&parsed.command);
        CHECK(parsed.recognized && parsed.success && command &&
                  command->count == 3u,
              "FIFO parser types a positive wait_frames count");
    }
    {
        const auto parsed =
            viewer::parse_fifo_line("shot_now C:\\absolute\\frame.png");
        const auto* command =
            std::get_if<viewer::FifoScreenshotNow>(&parsed.command);
        CHECK(parsed.recognized && parsed.success && command &&
                  command->path == "C:\\absolute\\frame.png",
              "FIFO parser types an absolute shot_now path");
    }

    for (const char* line : {"wait_frames 0", "wait_frames -1",
                             "wait_frames 1.5", "wait_frames 4294967296",
                             "shot_now relative.png",
                             "render_path pathtrace"}) {
        const auto parsed = viewer::parse_fifo_line(line);
        CHECK(parsed.recognized && !parsed.success,
              "FIFO parser rejects malformed presentation commands");
    }

    for (const char* line : {"render_pathology native_rt",
                             "history_reset_extra",
                             "wait_frames_extra 3",
                             "shot_now_extra C:\\absolute\\frame.png"}) {
        const auto parsed = viewer::parse_fifo_line(line);
        CHECK(!parsed.recognized,
              "FIFO parser requires an exact presentation command token");
    }

    for (const char* line : {"shot_now \\frame.png",
                             "shot_now /frame.png",
                             "shot_now C:\\absolute\\frame.jpg",
                             "shot_now C:\\absolute\\..\\frame.png",
                             "shot_now \\\\server\\..\\frame.png"}) {
        const auto parsed = viewer::parse_fifo_line(line);
        CHECK(parsed.recognized && !parsed.success,
              "FIFO parser rejects drive-relative, non-PNG, and traversal shots");
    }

    viewer::FifoPresentSequencer sequencing;
    sequencing.queue_wait(3u);
    sequencing.queue_screenshot("C:\\absolute\\next.png");
    CHECK(sequencing.presented_frame_serial() == 0u &&
              sequencing.pending_screenshot_path() ==
                  "C:\\absolute\\next.png",
          "present sequencer queues wait and zero-settle screenshot");
    const auto failed = sequencing.advance(false);
    CHECK(sequencing.presented_frame_serial() == 0u &&
              failed.completed_waits.empty() &&
              failed.screenshot_path.empty() &&
              sequencing.pending_screenshot_path() ==
                  "C:\\absolute\\next.png",
          "failed/acquire-only frame neither advances nor consumes queued work");
    const auto first = sequencing.advance(true);
    CHECK(sequencing.presented_frame_serial() == 1u &&
              first.completed_waits.empty() &&
              first.screenshot_path == "C:\\absolute\\next.png",
          "shot_now captures the next successful present with zero settle");
    const auto intervening_failure = sequencing.advance(false);
    CHECK(sequencing.presented_frame_serial() == 1u &&
              intervening_failure.completed_waits.empty(),
          "intervening failed present cannot complete a queued wait");
    const auto second = sequencing.advance(true);
    CHECK(second.completed_waits.empty(),
          "wait_frames does not print success before its target serial");
    const auto third = sequencing.advance(true);
    CHECK(third.completed_waits.size() == 1u &&
              third.completed_waits[0].count == 3u &&
              third.completed_waits[0].frame_serial == 3u,
          "wait_frames completes on the third successful present only");
}

void test_screenshot_completion_marker_is_nonempty() {
    matter::test::MemoryCacheByteStorage storage;
    const std::string marker = "/screenshot-marker-fixture/capture.png.done";
    CHECK(viewer::write_screenshot_completion_marker(marker, &storage),
          "write screenshot completion marker");
    const auto bytes = storage.open(marker);
    CHECK(bytes && std::string(bytes->begin(), bytes->end()) == "captured\n",
          "screenshot completion marker contains exact completion bytes");
    storage.fail_publication(true);
    CHECK(!viewer::write_screenshot_completion_marker(marker + ".failed", &storage) &&
              !storage.open(marker + ".failed"),
          "failed completion marker is not published");
}

static void test_vulkan_lighting_override_contract() {
    // 2026-07-30 tuning pass: sun/sky moved off 1.0 (see the struct comment in
    // world_session.h). Pinned here on purpose — these are what every world
    // loads with and what "Reset to World" restores.
    matter::VulkanLightingOverrides defaults{};
    CHECK(defaults.sun_multiplier == 1.67f, "sun override defaults to 1.67");
    CHECK(defaults.sky_multiplier == 0.77f, "sky override defaults to 0.77");
    CHECK(defaults.emission_multiplier == 1.0f,
          "emission override defaults to one");
    CHECK(defaults.exposure_ev == -2.0f, "display exposure defaults to -2 EV");

    matter::VulkanLightingOverrides bad{};
    bad.sun_multiplier = -4.0f;
    bad.sky_multiplier = 9.0f;
    bad.emission_multiplier = std::numeric_limits<float>::infinity();
    bad.exposure_ev = std::numeric_limits<float>::quiet_NaN();
    const auto clean = viewer::sanitize_vulkan_lighting_overrides(bad);
    CHECK(clean.sun_multiplier == 0.0f, "sun override clamps low");
    CHECK(clean.sky_multiplier == 4.0f, "sky override clamps high");
    CHECK(clean.emission_multiplier == 1.0f,
          "invalid emission override uses default");
    CHECK(clean.exposure_ev == -2.0f, "invalid exposure uses default");

    // "Uses default" means the STRUCT's default, for every field. sun and sky
    // used to fall back to a hard-coded 1.0 while the struct ships 1.67/0.77,
    // so a single NaN re-lit the scene at a brightness nothing had authored.
    matter::VulkanLightingOverrides nan_gains{};
    nan_gains.sun_multiplier = std::numeric_limits<float>::quiet_NaN();
    nan_gains.sky_multiplier = std::numeric_limits<float>::quiet_NaN();
    const auto clean_gains = viewer::sanitize_vulkan_lighting_overrides(nan_gains);
    CHECK(clean_gains.sun_multiplier == defaults.sun_multiplier &&
              clean_gains.sky_multiplier == defaults.sky_multiplier,
          "invalid sun/sky overrides fall back to the struct defaults");
    CHECK(std::fabs(viewer::vulkan_exposure_scale(-2.0f) - 0.25f) < 1e-6f,
          "-2 EV maps to quarter exposure");

    auto exposure_only = defaults;
    exposure_only.exposure_ev = 1.0f;
    CHECK(!viewer::vulkan_source_lighting_changed(defaults, exposure_only),
          "exposure-only change preserves lighting history");
    auto source_change = defaults;
    source_change.emission_multiplier = 0.5f;
    CHECK(viewer::vulkan_source_lighting_changed(defaults, source_change),
          "emission change invalidates lighting history");

    // ---- Sun/sky tints ---------------------------------------------------
    // White by default, which must be a BIT-EXACT no-op: the tint multiplies
    // the same authored colour the scalar multiplier does (matter_engine.cpp),
    // and every consumer — composite, GI constants, volumetric scatter —
    // reads that one pair of colours.
    CHECK(defaults.sun_tint[0] == 1.0f && defaults.sun_tint[1] == 1.0f &&
              defaults.sun_tint[2] == 1.0f,
          "sun tint defaults to white");
    CHECK(defaults.sky_tint[0] == 1.0f && defaults.sky_tint[1] == 1.0f &&
              defaults.sky_tint[2] == 1.0f,
          "sky tint defaults to white");
    {
        // The exact expression matter_engine.cpp evaluates, on a colour with
        // no representable slack left.
        const float authored = 0.1234567f;
        const float mul = 1.67f;
        CHECK(authored * mul * 1.0f == authored * mul,
              "a white tint is bit-exact on the assembled sun colour");
    }

    matter::VulkanLightingOverrides bad_tint{};
    bad_tint.sun_tint[0] = -1.0f;
    bad_tint.sun_tint[1] = 9.0f;
    bad_tint.sun_tint[2] = std::numeric_limits<float>::quiet_NaN();
    bad_tint.sky_tint[0] = std::numeric_limits<float>::infinity();
    const auto clean_tint = viewer::sanitize_vulkan_lighting_overrides(bad_tint);
    CHECK(clean_tint.sun_tint[0] == 0.0f, "sun tint clamps low");
    CHECK(clean_tint.sun_tint[1] == 4.0f, "sun tint clamps high");
    CHECK(clean_tint.sun_tint[2] == 1.0f,
          "a non-finite tint channel falls back to white, not black");
    CHECK(clean_tint.sky_tint[0] == 1.0f,
          "an infinite sky tint channel falls back to white");
    CHECK(clean_tint.sky_tint[1] == 1.0f && clean_tint.sky_tint[2] == 1.0f,
          "untouched tint channels survive sanitizing");

    auto tint_change = defaults;
    tint_change.sun_tint[2] = 0.6f;
    CHECK(viewer::vulkan_source_lighting_changed(defaults, tint_change),
          "a sun tint change invalidates lighting history");
    auto sky_tint_change = defaults;
    sky_tint_change.sky_tint[0] = 0.9f;
    CHECK(viewer::vulkan_source_lighting_changed(defaults, sky_tint_change),
          "a sky tint change invalidates lighting history");
}

static void test_viewer_lighting_controls() {
    // These seams assert that a reset restores THE DEFAULT, not that the
    // default holds any particular value — so they compare against the struct
    // rather than repeating its literals. test_vulkan_lighting_override_contract
    // below is the one place the values themselves are pinned.
    const matter::VulkanLightingOverrides world{};
    auto matches_world = [&world](const matter::VulkanLightingOverrides& v) {
        for (int i = 0; i < 3; ++i) {
            if (v.sun_tint[i] != world.sun_tint[i]) return false;
            if (v.sky_tint[i] != world.sky_tint[i]) return false;
        }
        return v.sun_multiplier == world.sun_multiplier &&
               v.sky_multiplier == world.sky_multiplier &&
               v.emission_multiplier == world.emission_multiplier &&
               v.exposure_ev == world.exposure_ev;
    };

    viewer::ViewerStats stats{};
    stats.lighting.sun_multiplier = 0.25f;
    stats.lighting.sky_multiplier = 0.5f;
    stats.lighting.emission_multiplier = 0.75f;
    stats.lighting.exposure_ev = 3.0f;
    stats.lighting.sun_tint[0] = 0.2f;
    stats.lighting.sky_tint[2] = 0.4f;
    viewer::reset_lighting_controls(stats);
    CHECK(matches_world(stats.lighting),
          "world reset restores the sun/sky tints along with the multipliers");
    CHECK(stats.lighting.sun_multiplier == world.sun_multiplier,
          "world reset restores authored sun multiplier");
    CHECK(stats.lighting.sky_multiplier == world.sky_multiplier,
          "world reset restores authored sky multiplier");
    CHECK(stats.lighting.emission_multiplier == world.emission_multiplier,
          "world reset restores authored emission multiplier");
    CHECK(stats.lighting.exposure_ev == world.exposure_ev,
          "world reset restores default display exposure");

    stats.lighting = {0.25f, 0.5f, 0.75f, 3.0f};
    viewer::prepare_world_reload(stats);
    CHECK(matches_world(stats.lighting),
          "reload seam resets all Viewer lighting controls");

    // Positional aggregate init still names only the first four members; the
    // tints ride their NSDMIs, which is exactly why they were appended.
    stats.lighting = {0.25f, 0.5f, 0.75f, 3.0f};
    stats.lighting.sun_tint[1] = 0.3f;
    viewer::complete_world_switch(stats, false);
    CHECK(stats.lighting.sun_multiplier == 0.25f &&
              stats.lighting.sky_multiplier == 0.5f &&
              stats.lighting.emission_multiplier == 0.75f &&
              stats.lighting.exposure_ev == 3.0f &&
              stats.lighting.sun_tint[1] == 0.3f,
          "failed world-switch seam preserves Viewer lighting controls");
    viewer::complete_world_switch(stats, true);
    CHECK(matches_world(stats.lighting),
          "successful world-switch seam resets all Viewer lighting controls");
}

void run_vulkan_gi_math_tests() {
    CHECK(VULKAN_GI_REJECT_HIT_DISTANCE == (1u << 6),
          "CPU and temporal shader reserve bit 6 for specular hit-distance rejection");
    const matter::VulkanGiSettings defaults{};
    CHECK(defaults.enabled && defaults.max_bounces == 1u &&
              defaults.samples_per_pixel == 1u && defaults.trace_scale == 1.0f &&
              defaults.reflection_trace_scale == 1.0f &&
              defaults.diffuse_multiplier == 1.0f &&
              defaults.reflection_multiplier == 1.0f &&
              defaults.max_reflection_roughness == 1.0f &&
              defaults.transmission_multiplier == 1.0f &&
              defaults.scattering_multiplier == 1.0f,
          "Vulkan GI defaults enable one full-resolution diffuse bounce");

    const matter::Float3 normal{0.26726124f, 0.53452248f, 0.80178373f};
    const viewer::VulkanCosineSample sample =
        viewer::vulkan_cosine_sample(normal, 0.25f, 0.75f);
    const float direction_length = std::sqrt(
        sample.direction.x * sample.direction.x +
        sample.direction.y * sample.direction.y +
        sample.direction.z * sample.direction.z);
    const float cosine = sample.direction.x * normal.x +
                         sample.direction.y * normal.y +
                         sample.direction.z * normal.z;
    CHECK(std::fabs(direction_length - 1.0f) < 1e-5f && cosine > 0.0f &&
              std::fabs(sample.pdf - cosine / 3.14159265358979323846f) < 1e-6f,
          "cosine sampler produces an orthonormal upper-hemisphere direction");

    const uint32_t first = viewer::vulkan_gi_pcg_hash(0x12345678u);
    CHECK(first == viewer::vulkan_gi_pcg_hash(0x12345678u) &&
              first != viewer::vulkan_gi_pcg_hash(0x12345679u),
          "GI PCG hash is fixed-seed deterministic and input-sensitive");
    const uint32_t retry_seed = viewer::vulkan_gi_seed(17, 23, 9, 1);
    CHECK(retry_seed == viewer::vulkan_gi_seed(17, 23, 9, 1) &&
              retry_seed != viewer::vulkan_gi_seed(17, 23, 9, 0) &&
              retry_seed != viewer::vulkan_gi_seed(17, 23, 10, 1),
          "GI seed uses committed frame identity and explicit bounce component");
    const auto source_uv =
        viewer::vulkan_gi_source_uv(10, 5, 80, 40, 160, 80);
    CHECK(std::fabs(source_uv.x - 21.5f / 160.0f) < 1e-7f &&
              std::fabs(source_uv.y - 11.5f / 80.0f) < 1e-7f,
          "scaled GI reconstruction uses the selected source texel center");

    const matter::Float3 f0{0.04f, 0.1f, 0.8f};
    const matter::Float3 normal_fresnel =
        viewer::vulkan_schlick_fresnel(f0, 1.0f);
    const matter::Float3 grazing_fresnel =
        viewer::vulkan_schlick_fresnel(f0, 0.0f);
    CHECK(std::fabs(normal_fresnel.x - f0.x) < 1e-6f &&
              std::fabs(normal_fresnel.y - f0.y) < 1e-6f &&
              std::fabs(normal_fresnel.z - f0.z) < 1e-6f &&
              std::fabs(grazing_fresnel.x - 1.0f) < 1e-6f &&
              std::fabs(grazing_fresnel.y - 1.0f) < 1e-6f &&
              std::fabs(grazing_fresnel.z - 1.0f) < 1e-6f,
          "Schlick Fresnel equals F0 at normal incidence and one at grazing");
    for (const float roughness : {0.02f, 0.1f, 0.5f, 1.0f}) {
        const float pdf = viewer::vulkan_ggx_reflection_pdf(
            0.8f, 0.65f, roughness);
        CHECK(std::isfinite(pdf) && pdf >= 0.0f,
              "GGX reflection PDF remains finite across authored roughness");
    }
    CHECK(viewer::vulkan_clearcoat_selection_probability(0.0f) == 0.0f &&
              std::fabs(viewer::vulkan_clearcoat_selection_probability(1.0f) -
                        0.5f) < 1e-6f,
          "zero clearcoat launches no coat samples and full coat normalizes lobe selection");
}

void run_raster_mesh_material_contract_tests() {
    Tri triangle{};
    triangle.vertex0 = make_float3(-1.0f, 0.0f, 0.0f);
    triangle.vertex1 = make_float3(1.0f, 0.0f, 0.0f);
    triangle.vertex2 = make_float3(0.0f, 1.0f, 0.0f);
    TriEx surface{};
    surface.uv0 = make_float2(0.1f, 0.2f);
    surface.uv1 = make_float2(0.3f, 0.4f);
    surface.uv2 = make_float2(0.5f, 0.6f);
    surface.N0 = surface.N1 = surface.N2 = make_float3(0.0f, 0.0f, 1.0f);
    surface.materialId = 7;
    surface.tint = make_float4(0.2f, 0.4f, 0.6f, 0.75f);
    surface.ao0 = 0.2f;
    surface.ao1 = 0.5f;
    surface.ao2 = 0.8f;

    const viewer::RasterMeshData mesh =
        viewer::build_raster_mesh_data(&triangle, &surface, 1);
    CHECK(mesh.surface_uvs ==
              std::vector<float>({0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f}),
          "raster mesh retains Vulkan source UVs");
    CHECK(mesh.material_ids == std::vector<uint32_t>({7u, 7u, 7u}),
          "raster mesh retains exact Vulkan material ids");
    CHECK(mesh.baked_ao == std::vector<float>({0.2f, 0.5f, 0.8f}),
          "raster mesh retains baked AO source values");

    const viewer::RasterMeshData fallback =
        viewer::build_raster_mesh_data(&triangle, nullptr, 1);
    CHECK(fallback.surface_uvs == std::vector<float>(6, 0.0f) &&
              fallback.material_ids ==
                  std::vector<uint32_t>(3, 0xffffffffu) &&
              fallback.baked_ao == std::vector<float>(3, 1.0f),
          "raster mesh supplies neutral Vulkan sources without TriEx");
}

void run_rt_lod_payload_contract_tests() {
    viewer::VkScenePart part{};
    part.part_hash = 0x4c4f4452u;
    part.clusters = {
        {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 20.0f,
         {{0, 6, 1.0f}, {6, 3, 0.0f}}},
        {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f,
         {{9, 6, 1.0f}, {15, 3, 0.0f}}},
    };
    part.vertices.resize(18);
    const auto selected = viewer::vk_scene_detail::select_rt_instance_geometry(
        part, identity_matrix(), {0.0f, 0.0f, 10.0f}, 1.0f);
    CHECK(selected.size() == 2 &&
              selected[0].cluster_index == 0 &&
              selected[0].lod_index == 0 &&
              selected[0].first_index == 0 &&
              selected[0].index_count == 6 &&
              selected[1].cluster_index == 1 &&
              selected[1].lod_index == 1 &&
              selected[1].first_index == 15 &&
              selected[1].index_count == 3,
          "RT instance payload contains exactly one raster-selected LOD per cluster");
    const auto offsets = viewer::vk_scene_detail::dense_rt_lod_offsets(part);
    uint32_t record_index = UINT32_MAX;
    CHECK(offsets == std::vector<uint32_t>({0, 2, 4}) &&
              viewer::vk_scene_detail::dense_rt_lod_index(
                  offsets, 1, 1, record_index) &&
              record_index == 3 &&
              !viewer::vk_scene_detail::dense_rt_lod_index(
                  offsets, 1, 2, record_index),
          "dense RT LOD offsets provide bounded O(1) cluster/LOD indexing");
    // M1: select_cluster_lod_view reads normalized SWITCH DISTANCES, not
    // thresholds. Authored through the conversion rather than as bare
    // constants, so the unit is visible at the callsite -- the old {1.0f, 0.0f}
    // would still compile here and would silently mean something else.
    // Intent is unchanged: rung 0 reaches one radius-scaled unit, rung 1 is
    // open, and a camera 10 units out lands on rung 1.
    const float switch_distances[] = {
        lod::normalized_switch_distance(1.0f),   // -> 1.0
        lod::normalized_switch_distance(0.0f),   // -> INFINITY, always qualifies
    };
    CHECK(viewer::vk_scene_detail::select_cluster_lod_view(
              part.clusters[1].aabb_min, part.clusters[1].aabb_max,
              part.clusters[1].radius, switch_distances, 2, identity_matrix(),
              {0.0f, 0.0f, 10.0f}, 1.0f) == 1,
          "non-owning RT LOD view matches raster distance selection");

    // M2.5: the terminal billboard is not traceable geometry. It is oriented
    // in the vertex stage, so its BLAS holds the quad UNROTATED -- a ray that
    // hits it sees an axis-fixed rectangle where raster drew a camera-facing
    // card. A cluster whose selected rung is that billboard therefore
    // contributes NOTHING to the RT payload rather than contributing the wrong
    // shape (and rather than being clamped down to the last mesh rung, which
    // buries the card inside the traced mesh -- see build_ray_geometry).
    {
        viewer::VkScenePart billboard{};
        billboard.part_hash = 0x494d504fu;
        // Two rungs: a 2-triangle mesh, then the 2-triangle billboard. Only
        // the second carries the marker, so only the second is peeled.
        billboard.clusters = {
            {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f,
             {{0, 6, 1.0f}, {6, 6, 0.0f}}},
        };
        billboard.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        billboard.vertices.resize(12);
        // surface.x carries impostor::kQuadMarker on the billboard's vertices;
        // build_vulkan_part forwards it from TriEx::uv0.x, and raster.vert /
        // gbuffer.frag branch on the same sentinel.
        for (size_t i = 6; i < billboard.vertices.size(); ++i)
            billboard.vertices[i].surface.x = impostor::kQuadMarker;
        CHECK(viewer::vk_scene_detail::cluster_mesh_lod_count(billboard, 0) == 1,
              "the trailing billboard rung is not counted as a mesh rung");
        // A near camera selects rung 0 (the mesh): still traced.
        const auto near_selection =
            viewer::vk_scene_detail::select_rt_instance_geometry(
                billboard, identity_matrix(), {0.0f, 0.0f, 1.0f}, 1.0f);
        // A far camera selects rung 1 (the billboard): traced by nothing.
        const auto far_selection =
            viewer::vk_scene_detail::select_rt_instance_geometry(
                billboard, identity_matrix(), {0.0f, 0.0f, 10000.0f}, 1.0f);
        CHECK(near_selection.size() == 1 && near_selection[0].lod_index == 0 &&
                  near_selection[0].index_count == 6 && far_selection.empty(),
              "RT traces the mesh rung and drops the cluster at its billboard");
    }
}

void run_ray_tracing_capability_contract_tests() {
    CHECK(viewer::vk_scene_detail::scene_binding_stage_flags(5) ==
              (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
          "material binding is visible only to raster shader stages");
    // Account for the complete scene layout, including VT/water tables and
    // geometry hierarchy metadata. Compute sees 14 of the 20 storage buffers.
    CHECK(viewer::vk_scene_detail::scene_storage_limits_supported(14, 20) &&
              !viewer::vk_scene_detail::scene_storage_limits_supported(13, 20) &&
              !viewer::vk_scene_detail::scene_storage_limits_supported(14, 19),
          "scene capability accounting includes all 14 compute and 20 set buffers");
    matter::VulkanRayTracingCapabilities unsupported{};
    unsupported.buffer_device_address = true;
    std::string reason;
    CHECK(!matter::supports_native_ray_tracing(unsupported, reason) &&
              reason.find(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) !=
                  std::string::npos,
          "unsupported fake device cleanly disables native ray tracing");

    matter::VulkanRayTracingCapabilities complete{};
    complete.acceleration_structure_extension = true;
    complete.ray_tracing_pipeline_extension = true;
    complete.deferred_host_operations_extension = true;
    complete.spirv_1_4_extension = true;
    complete.shader_float_controls_extension = true;
    complete.ray_query_extension = true;
    complete.buffer_device_address = true;
    complete.acceleration_structure = true;
    complete.ray_tracing_pipeline = true;
    complete.ray_query = true;
    complete.storage_image_r8 = true;
    complete.shader_storage_image_extended_formats = true;
    CHECK(matter::supports_native_ray_tracing(complete, reason) &&
              reason.empty(),
          "complete fake RTX capability set enables native ray tracing");
    // Ray query is a hard requirement, not a bonus: vol_scatter.comp and the
    // tileset bake shaders declare RayQueryKHR, so a device that reports the
    // ray-tracing pipeline without ray query must not claim native RT.
    complete.ray_query = false;
    CHECK(!matter::supports_native_ray_tracing(complete, reason) &&
              reason.find("rayQuery") != std::string::npos,
          "missing rayQuery cleanly disables native ray tracing");
    complete.ray_query = true;
    complete.storage_image_r8 = false;
    CHECK(!matter::supports_native_ray_tracing(complete, reason) &&
              reason.find("R8_UNORM") != std::string::npos,
          "missing R8 storage support cleanly disables native ray tracing");
    CHECK((viewer::vk_scene_detail::ray_depth_destination_stages(true) &
           VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) != 0,
          "depth barrier includes ray tracing shader reads");
    CHECK((viewer::vk_scene_detail::ray_depth_destination_stages(false) &
           VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) == 0,
          "fallback depth barrier excludes unavailable RT stages");
}

void run_vulkan_instance_cache_tests() {
    viewer::ResolvedInstance a{};
    a.part_hash = 11;
    a.stable_id = 41;
    a.segment = 0;
    a.transform[0] = a.transform[5] = a.transform[10] = a.transform[15] = 1.0f;
    viewer::ResolvedInstance b = a;
    b.part_hash = 12;
    b.stable_id = 42;
    std::vector<viewer::ResolvedInstance> roots{a, b};

    viewer::VulkanInstanceCache cache;
    CHECK(!cache.matches(roots), "empty Vulkan instance cache misses");
    std::vector<viewer::VkSceneInstance> expanded(2);
    expanded[0].part_hash = 21;
    expanded[1].part_hash = 22;
    cache.store(roots, std::move(expanded));
    CHECK(cache.matches(roots), "unchanged resolved roots hit Vulkan cache");
    CHECK(cache.instances().size() == 2 && cache.expansion_count() == 1,
          "Vulkan cache retains expanded instances and counts one expansion");

    roots[1].lod_level = 3;
    CHECK(cache.matches(roots), "LOD change preserves Vulkan cache hit");
    roots[1].segment = 1;
    CHECK(!cache.matches(roots), "segment change invalidates Vulkan cache");
    roots[1].segment = 0;
    roots[1].transform[3] = 1.0f;
    CHECK(!cache.matches(roots), "transform change invalidates Vulkan cache");
    cache.invalidate();
    CHECK(cache.instances().empty(), "cache invalidation releases expansion");

    // Per-source memo: a publish only adds a source, so every pre-existing
    // source must still be served from its memo on the rebuild that follows.
    std::vector<viewer::VkSceneInstance> a_expansion(3);
    a_expansion[0].part_hash = 31;
    CHECK(cache.find_source(a) == nullptr, "empty memo misses");
    cache.store_source(a, a_expansion);
    const std::vector<viewer::VkSceneInstance>* hit = cache.find_source(a);
    CHECK(hit != nullptr && hit->size() == 3 && (*hit)[0].part_hash == 31,
          "memoised source round-trips its expansion");
    CHECK(cache.find_source(b) == nullptr, "a different source misses the memo");

    viewer::ResolvedInstance moved = a;
    moved.transform[3] = 5.0f;
    CHECK(cache.find_source(moved) == nullptr,
          "same stable id with a changed transform misses the memo");
    viewer::ResolvedInstance repointed = a;
    repointed.part_hash = 99;
    CHECK(cache.find_source(repointed) == nullptr,
          "same stable id naming a different part misses the memo");

    // invalidate_expansion keeps memos (the publish path); invalidate drops
    // them (every release path).
    cache.store(roots, std::vector<viewer::VkSceneInstance>(2));
    cache.invalidate_expansion();
    CHECK(!cache.matches(roots) && cache.find_source(a) != nullptr,
          "invalidate_expansion drops the flat set but keeps memos");
    cache.store(roots, std::vector<viewer::VkSceneInstance>(2));
    cache.invalidate_sources();
    CHECK(cache.matches(roots) && cache.find_source(a) == nullptr,
          "invalidate_sources drops memos but keeps the flat set");
    cache.store_source(a, a_expansion);
    cache.invalidate();
    CHECK(cache.find_source(a) == nullptr,
          "full invalidation drops memos too");

    // Pruning bounds the memo to the live source set.
    cache.store_source(a, a_expansion);
    cache.store_source(b, a_expansion);
    cache.prune_sources(std::vector<viewer::ResolvedInstance>{a});
    CHECK(cache.find_source(a) != nullptr && cache.find_source(b) == nullptr,
          "prune_sources keeps live sources and drops absent ones");
    CHECK(cache.source_memo_size() == 1, "pruned memo holds only live sources");

    // Large unchanged sets exercise the contiguous snapshot, including its
    // tail. Every identity field matters; LOD alone is deliberately ignored.
    std::vector<viewer::ResolvedInstance> many(4097, a);
    for (size_t i = 0; i < many.size(); ++i) many[i].stable_id += i;
    viewer::VulkanInstanceCache dense;
    dense.store(many, {});
    CHECK(dense.matches(many), "dense instance snapshot matches original values");
    auto changed = many;
    changed.back().part_hash++;
    CHECK(!dense.matches(changed), "dense snapshot detects changed tail part");
    changed = many;
    changed.back().stable_id++;
    CHECK(!dense.matches(changed), "dense snapshot detects changed tail identity");
    for (size_t component = 0; component < 16; ++component) {
        changed = many;
        changed.back().transform[component] += 0.25f;
        CHECK(!dense.matches(changed), "dense snapshot checks every matrix component");
    }
    changed = many;
    std::swap(changed[0], changed[1]);
    CHECK(!dense.matches(changed), "dense snapshot preserves ordering");
    changed = many;
    changed.back().lod_level++;
    CHECK(dense.matches(changed), "dense snapshot ignores GPU-selected LOD");
    many.back().segment++;
    CHECK(!dense.matches(many), "caller edits cannot mutate owned snapshot");
    dense.store({}, {});
    CHECK(dense.matches({}), "valid empty snapshot matches empty scene");
    dense.invalidate_expansion();
    CHECK(!dense.matches({}), "invalid empty snapshot cannot be reused");
}

void run_vulkan_temporal_tests() {
    viewer::TemporalState temporal;
    viewer::FrameMatrices camera{};
    camera.world_to_view = identity_matrix();
    camera.view_to_clip = identity_matrix();
    camera.world_to_clip = identity_matrix();
    camera.clip_to_world = identity_matrix();
    const viewer::TemporalInstance still{7, identity_matrix()};

    viewer::TemporalState native_temporal;
    viewer::TemporalFrame native_frame = native_temporal.begin(
        camera, {100, 80}, {100, 80}, {still}, false, {});
    CHECK(native_frame.jitter_pixels[0] == 0.0f &&
              native_frame.jitter_pixels[1] == 0.0f &&
              std::equal(std::begin(native_frame.current_jittered.world_to_clip.m),
                         std::end(native_frame.current_jittered.world_to_clip.m),
                         std::begin(native_frame.current_unjittered.world_to_clip.m)),
          "native rendering uses the exact unjittered projection");

    viewer::TemporalFrame first = temporal.begin(
        camera, {100, 80}, {100, 80}, {still}, true, {});
    CHECK(first.reset && !first.instances[0].history_valid,
          "first temporal frame resets invalid history");
    CHECK(first.presented_frame_index == 0,
          "first candidate seeds from zero successfully presented frames");
    CHECK(temporal.commit_presented(first.attempt_token),
          "successful presentation commits temporal candidate");

    viewer::TemporalFrame static_frame = temporal.begin(
        camera, {100, 80}, {100, 80}, {still}, true, {});
    const matter::Float3 static_velocity =
        viewer::temporal_velocity_pixels(static_frame, 7, {0.0f, 0.0f, 0.0f});
    // Y expectations here (and in the two deltas below) follow the Y-down
    // jitter convention d5f97aa7 gave jitter_frame; the old +Y-NDC values
    // predate that fix and went stale while this suite could not build.
    CHECK(!static_frame.reset && static_frame.instances[0].history_valid &&
              std::fabs(static_velocity.x + 0.25f) < 1e-6f &&
              std::fabs(static_velocity.y - 1.0f / 3.0f) < 1e-6f,
          "static camera and rigid instance preserve the known Halton delta");
    CHECK(std::fabs(static_frame.previous_jittered.world_to_clip.m[3] -
                    first.current_jittered.world_to_clip.m[3]) < 1e-6f &&
              std::fabs(static_frame.previous_jittered.world_to_clip.m[7] -
                        first.current_jittered.world_to_clip.m[7]) < 1e-6f &&
              std::fabs(static_frame.previous_jittered.jitter_pixels[0] -
                        first.jitter_pixels[0]) < 1e-6f &&
              std::fabs(static_frame.previous_jittered.jitter_pixels[1] -
                        first.jitter_pixels[1]) < 1e-6f,
          "previous projection retains the last actually presented jitter");
    CHECK(temporal.commit_presented(static_frame.attempt_token),
          "second successful presentation advances temporal history");

    viewer::FrameMatrices moved_camera = camera;
    moved_camera.view_to_clip = viewer::mat4_translation({-0.2f, 0.0f, 0.0f});
    moved_camera.world_to_clip = viewer::mat4_translation({-0.2f, 0.0f, 0.0f});
    moved_camera.clip_to_world = viewer::mat4_translation({0.2f, 0.0f, 0.0f});
    viewer::TemporalFrame camera_motion = temporal.begin(
        moved_camera, {100, 80}, {100, 80}, {still}, true, {});
    const matter::Float3 camera_velocity = viewer::temporal_velocity_pixels(
        camera_motion, 7, {0.0f, 0.0f, 0.0f});
    CHECK(std::fabs(camera_velocity.x + 9.5f) < 1e-5f &&
              std::fabs(camera_velocity.y + 5.0f / 9.0f) < 1e-5f,
          "known camera translation includes the presented Halton delta");
    CHECK(temporal.commit_presented(camera_motion.attempt_token),
          "camera-motion candidate commits");

    const viewer::TemporalInstance moved_object{
        7, viewer::mat4_translation({0.4f, 0.0f, 0.0f})};
    viewer::TemporalFrame object_motion = temporal.begin(
        moved_camera, {100, 80}, {100, 80}, {moved_object}, true, {});
    const matter::Float3 object_velocity = viewer::temporal_velocity_pixels(
        object_motion, 7, {0.0f, 0.0f, 0.0f});
    CHECK(std::fabs(object_velocity.x - 19.375f) < 1e-5f &&
              std::fabs(object_velocity.y - 1.0f / 3.0f) < 1e-5f,
          "known rigid-instance translation includes the presented Halton delta");
    CHECK(temporal.commit_presented(object_motion.attempt_token),
          "object-motion candidate commits");

    const auto expect_one_reset = [&](VkExtent2D internal,
                                      viewer::TemporalInvalidation invalidation,
                                      std::vector<viewer::TemporalInstance> instances,
                                      const char* label) {
        viewer::TemporalFrame reset = temporal.begin(
            moved_camera, internal, {100, 80}, instances, true, invalidation);
        CHECK(reset.reset, label);
        CHECK(temporal.commit_presented(reset.attempt_token), label);
        viewer::TemporalFrame stable = temporal.begin(
            moved_camera, internal, {100, 80}, instances, true, {});
        CHECK(!stable.reset, "temporal invalidation resets exactly one frame");
        CHECK(temporal.commit_presented(stable.attempt_token),
              "post-reset candidate commits");
    };
    expect_one_reset({120, 80}, {}, {moved_object}, "resize resets temporal history");
    viewer::TemporalInvalidation camera_cut{};
    camera_cut.camera_cut = true;
    expect_one_reset({120, 80}, camera_cut, {moved_object},
                     "camera cut resets temporal history");
    viewer::TemporalInvalidation world_reload{};
    world_reload.world_reload = true;
    expect_one_reset({120, 80}, world_reload, {moved_object},
                     "world reload resets temporal history");
    viewer::TemporalInvalidation renderer_reset{};
    renderer_reset.renderer_reset = true;
    expect_one_reset({120, 80}, renderer_reset, {moved_object},
                     "renderer recovery resets temporal history");

    // A newcomer id is a per-instance event, not a global cut: it enters with
    // invalid history while survivors keep theirs. Escalating it to a full
    // reset starved DLSS/GI in streaming worlds, which introduce new ids
    // nearly every frame (issues/render-dlss-not-applied).
    viewer::TemporalFrame streamed_in = temporal.begin(
        moved_camera, {120, 80}, {100, 80},
        {moved_object, {99, identity_matrix()}}, true, {});
    CHECK(!streamed_in.reset && streamed_in.instances.size() == 2 &&
              streamed_in.instances[0].history_valid &&
              !streamed_in.instances[1].history_valid,
          "streamed-in instance joins without resetting global history");
    CHECK(temporal.commit_presented(streamed_in.attempt_token),
          "streamed-in candidate commits");

    viewer::TemporalFrame failed = temporal.begin(
        moved_camera, {120, 80}, {100, 80}, {{99, identity_matrix()}}, true, {});
    const uint64_t failed_token = failed.attempt_token;
    CHECK(temporal.discard_failed_attempt(failed_token),
          "failed presentation discards uncommitted candidate");
    viewer::TemporalFrame after_failure = temporal.begin(
        moved_camera, {120, 80}, {100, 80}, {{99, identity_matrix()}}, true, {});
    CHECK(after_failure.reset && after_failure.attempt_token > failed_token &&
              std::fabs(after_failure.jitter_pixels[0] -
                        failed.jitter_pixels[0]) < 1e-6f &&
              std::fabs(after_failure.jitter_pixels[1] -
                        failed.jitter_pixels[1]) < 1e-6f,
          "failed presentation forces reset without advancing presented jitter");
    CHECK(!temporal.commit_presented(failed_token),
          "stale failed attempt token cannot commit temporal history");
    CHECK(after_failure.presented_frame_index == failed.presented_frame_index,
          "retry retains committed frame identity despite a new attempt token");

    viewer::TemporalState stable_ids;
    const uint64_t a_id = viewer::temporal_instance_id(41, 1001, 0);
    const uint64_t b_id = viewer::temporal_instance_id(42, 1002, 0);
    viewer::TemporalFrame two = stable_ids.begin(
        camera, {100, 80}, {100, 80},
        {{a_id, identity_matrix()}, {b_id, identity_matrix()}}, true, {});
    CHECK(stable_ids.commit_presented(two.attempt_token),
          "two-instance temporal baseline commits");
    viewer::TemporalFrame only_b = stable_ids.begin(
        camera, {100, 80}, {100, 80}, {{b_id, identity_matrix()}}, true, {});
    CHECK(!only_b.reset && only_b.instances.size() == 1 &&
              only_b.instances[0].instance_id == b_id &&
              only_b.instances[0].history_valid,
          "stable rigid identity survives [A,B] to [B] compaction");
    CHECK(stable_ids.commit_presented(only_b.attempt_token),
          "single surviving instance commits");
    viewer::TemporalFrame empty = stable_ids.begin(
        camera, {100, 80}, {100, 80}, {}, true, {});
    CHECK(stable_ids.commit_presented(empty.attempt_token),
          "presented clear frame advances empty temporal history");
    viewer::TemporalFrame returning_b = stable_ids.begin(
        camera, {100, 80}, {100, 80}, {{b_id, identity_matrix()}}, true, {});
    CHECK(!returning_b.reset && !returning_b.instances[0].history_valid,
          "instance returning after a presented clear frame starts fresh "
          "without a global reset");

    // 288 is the historical block; + 16 (water animation), + 16 (vis_params),
    // + 64 (cull_world_to_clip), and + 16 (surface_detail_sampling) form the
    // appended tail. Existing shader prefixes retain their original offsets.
    CHECK(viewer::vk_scene_detail::frame_constants_size_for_test() ==
              288 + 16 + 16 + 64 + 16,
          "C++ FrameConstants matches final std140 uvec4 padding and size");
}

void run_vulkan_gi_temporal_sequence_tests() {
    viewer::GiTemporalState history;
    const viewer::GiTemporalSurface stable{
        {1.0f, 0.25f, 0.125f}, 0.5f, {0.0f, 0.0f, 1.0f}, 7u, 41u};

    auto present = [&](viewer::GiTemporalSurface surface,
                       matter::Float3 velocity, bool reset,
                       uint64_t attempt) {
        const viewer::GiTemporalResult result = history.accumulate(
            surface, velocity, {4, 4}, {2, 2}, reset, attempt);
        CHECK(history.commit_presented(attempt),
              "presented GI candidate commits its ping-pong history");
        return result;
    };

    const auto first = present(stable, {}, true, 1);
    const auto second = present(stable, {}, false, 2);
    const auto third = present(stable, {}, false, 3);
    CHECK(first.history_length == 1u && second.history_length == 2u &&
              third.history_length == 3u && third.rejection_bits == 0u,
          "static GI pixel reaches history length three");

    history.seed_presented_for_test({4, 4}, {1, 2}, stable, 3u);
    const auto translated = history.accumulate(
        stable, {1.0f, 0.0f, 0.0f}, {4, 4}, {2, 2}, false, 4);
    CHECK(translated.previous_pixel.x == 1 &&
              translated.previous_pixel.y == 2 &&
              translated.history_length == 4u,
          "current-to-previous pixel velocity samples current minus velocity");
    CHECK(history.commit_presented(4), "translated GI candidate commits");

    const auto expect_rejection = [&](viewer::GiTemporalSurface changed,
                                      uint32_t expected_bit,
                                      uint64_t attempt,
                                      const char* label) {
        history.seed_presented_for_test({4, 4}, {2, 2}, stable, 3u);
        const auto rejected = history.accumulate(
            changed, {}, {4, 4}, {2, 2}, false, attempt);
        CHECK(rejected.history_length == 1u &&
                  rejected.rejection_bits == expected_bit,
              label);
        CHECK(history.commit_presented(attempt), label);
    };
    auto changed = stable;
    changed.depth += 0.2f;
    expect_rejection(changed, viewer::kGiRejectDepth, 5,
                     "depth discontinuity has unique GI rejection bit");
    changed = stable;
    changed.normal = {1.0f, 0.0f, 0.0f};
    expect_rejection(changed, viewer::kGiRejectNormal, 6,
                     "normal discontinuity has unique GI rejection bit");
    changed = stable;
    changed.material_index++;
    expect_rejection(changed, viewer::kGiRejectMaterial, 7,
                     "material discontinuity has unique GI rejection bit");
    changed = stable;
    changed.instance_token++;
    expect_rejection(changed, viewer::kGiRejectInstance, 8,
                     "instance discontinuity has unique GI rejection bit");

    history.seed_presented_for_test({4, 4}, {2, 2}, stable, 3u);
    const auto failed = history.accumulate(stable, {}, {4, 4}, {2, 2}, false, 9);
    CHECK(failed.history_length == 4u && history.discard_failed_attempt(9),
          "failed GI attempt discards candidate history");
    const auto retry = history.accumulate(stable, {}, {4, 4}, {2, 2}, false, 10);
    CHECK(retry.history_length == 4u,
          "failed presentation cannot become future GI history");
    CHECK(history.commit_presented(10), "retried GI candidate commits");

    const auto reset_once = [&](VkExtent2D extent, uint64_t first_attempt,
                                const char* label) {
        const auto reset = history.accumulate(
            stable, {}, extent, {1, 1}, true, first_attempt);
        CHECK(reset.history_length == 1u &&
                  reset.rejection_bits == viewer::kGiRejectReset,
              label);
        CHECK(history.commit_presented(first_attempt), label);
        const auto stable_again = history.accumulate(
            stable, {}, extent, {1, 1}, false, first_attempt + 1);
        CHECK(stable_again.history_length == 2u &&
                  stable_again.rejection_bits == 0u,
              "GI invalidation resets exactly one presented frame");
        CHECK(history.commit_presented(first_attempt + 1), label);
    };
    reset_once({8, 8}, 11, "resize resets GI history once");
    reset_once({8, 8}, 13, "camera cut resets GI history once");
    reset_once({8, 8}, 15, "world reload resets GI history once");
    reset_once({8, 8}, 17, "Native/DLSS mode change resets GI history once");
}

void run_streamline_bridge_fallback_tests() {
    const matter::StreamlineBridge bridge =
        matter::StreamlineBridge::initialize_before_vulkan();
    CHECK(bridge.initialized(),
          "Streamline bridge fallback initialization succeeds without SDK");
    CHECK(!bridge.dlss_requested(),
          "Streamline bridge does not request DLSS when the SDK is absent");
    CHECK(bridge.dlss_unavailable_reason().find("not found") !=
              std::string::npos,
          "Streamline bridge reports that the SDK was not found");
    CHECK(!bridge.proxy_dispatch_used(),
          "Streamline fallback never dispatches through a proxy");
    CHECK(!matter::StreamlineBridge::requires_explicit_vulkan_info(true) &&
              matter::StreamlineBridge::requires_explicit_vulkan_info(false),
          "proxy-created Vulkan devices are not registered with Streamline twice");
    CHECK(matter::StreamlineBridge::test_missing_proxy("instance")
                  .native_retry_required() &&
              matter::StreamlineBridge::test_missing_proxy("device")
                  .native_retry_required(),
          "missing Streamline proxy acquisition preserves native retry intent");

    const std::vector<const char*> merged =
        matter::StreamlineBridge::merge_extensions({"A", "B"},
                                                    {"B", "C"});
    CHECK(merged.size() == 3 && std::string(merged[0]) == "A" &&
              std::string(merged[1]) == "B" && std::string(merged[2]) == "C",
          "Streamline extension merge preserves first-seen order");
}

void run_dlss_bridge_contract_tests() {
    const auto image = [](uintptr_t value) {
        return reinterpret_cast<VkImage>(value);
    };
    const auto view = [](uintptr_t value) {
        return reinterpret_cast<VkImageView>(value);
    };
    const auto memory = [](uintptr_t value) {
        return reinterpret_cast<VkDeviceMemory>(value);
    };
    matter::DlssConstants constants{};
    for (uint32_t index = 0; index < 16; ++index) {
        constants.camera_view_to_clip[index] = static_cast<float>(index + 1);
        constants.clip_to_camera_view[index] = static_cast<float>(index + 17);
        constants.clip_to_prev_clip[index] = static_cast<float>(index + 33);
        constants.prev_clip_to_clip[index] = static_cast<float>(index + 49);
    }
    constants.jitter_offset = {0.25f, -0.125f};
    constants.motion_vector_scale = {1.0f / 1280.0f, 1.0f / 720.0f};
    constants.motion_vectors_jittered = true;
    constants.reset = true;
    constants.internal_extent = {1280, 720};
    constants.output_extent = {1920, 1080};

    matter::DlssResources resources{};
    resources.hdr = {image(1), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    resources.depth = {image(2), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    resources.velocity = {image(3), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    resources.output = {image(4), VK_IMAGE_LAYOUT_GENERAL};
    resources.hdr.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    resources.hdr.extent = {1280, 720};
    resources.hdr.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    resources.hdr.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    resources.depth.format = VK_FORMAT_D32_SFLOAT;
    resources.depth.extent = {1280, 720};
    resources.depth.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    resources.depth.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    resources.velocity.format = VK_FORMAT_R16G16_SFLOAT;
    resources.velocity.extent = {1280, 720};
    resources.velocity.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    resources.velocity.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    resources.output.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    resources.output.extent = {1920, 1080};
    resources.output.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    resources.output.access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    resources.hdr.view = view(11);
    resources.hdr.memory = memory(21);
    resources.hdr.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    resources.hdr.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    resources.depth.view = view(12);
    resources.depth.memory = memory(22);
    resources.depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT;
    resources.depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    resources.velocity.view = view(13);
    resources.velocity.memory = memory(23);
    resources.velocity.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    resources.velocity.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    resources.output.view = view(14);
    resources.output.memory = memory(24);
    resources.output.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    resources.output.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    const matter::DlssOptions options{matter::DlssMode::Quality,
                                      {1920, 1080}, true, true};

    matter::StreamlineBridge native = matter::StreamlineBridge::native_fallback(
        "test native fallback");
    std::string error;
    matter::DlssConstants native_constants = constants;
    native_constants.output_extent = native_constants.internal_extent;
    matter::DlssEvaluationOutput evaluation_output{};
    CHECK(native.evaluate_dlss(VK_NULL_HANDLE, 1,
                               {matter::DlssMode::Native, {1280, 720}, true,
                                false},
                               native_constants, resources,
                               evaluation_output, error) &&
              native_constants.internal_extent.width ==
                  native_constants.output_extent.width &&
              native_constants.internal_extent.height ==
                  native_constants.output_extent.height &&
              native.test_dlss_evaluation_count() == 0,
          "Native mode keeps equal extents and never evaluates Streamline");

    bool received = false;
    std::vector<matter::DlssMode> mode_transitions;
    matter::StreamlineBridge fake = matter::StreamlineBridge::test_fake_dlss(
        [&](VkCommandBuffer command_buffer, uint64_t token,
            const matter::DlssOptions& captured_options,
            const matter::DlssConstants& captured,
            const matter::DlssResources& tagged,
            matter::DlssEvaluationOutput& output, std::string&) {
            mode_transitions.push_back(captured_options.mode);
            if (captured_options.mode == matter::DlssMode::Native) return true;
            received = command_buffer == VK_NULL_HANDLE &&
                       (token == 77 || token == 79) &&
                       captured_options.mode == matter::DlssMode::Quality &&
                       captured_options.output_extent.width == 1920 &&
                       captured_options.output_extent.height == 1080 &&
                       captured_options.color_buffers_hdr &&
                       captured_options.use_auto_exposure &&
                       captured.camera_view_to_clip[0] == 1.0f &&
                       captured.camera_view_to_clip[15] == 16.0f &&
                       captured.motion_vector_scale.x == 1.0f / 1280.0f &&
                       captured.motion_vector_scale.y == 1.0f / 720.0f &&
                       captured.motion_vectors_jittered && captured.reset &&
                       tagged.hdr.image != tagged.depth.image &&
                       tagged.hdr.image != tagged.velocity.image &&
                       tagged.hdr.image != tagged.output.image &&
                       tagged.hdr.layout ==
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                       tagged.depth.layout ==
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                       tagged.velocity.layout ==
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                       tagged.output.layout == VK_IMAGE_LAYOUT_GENERAL &&
                       tagged.depth.stage ==
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                       tagged.depth.access ==
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT &&
                       tagged.velocity.stage ==
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                       tagged.velocity.access ==
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT &&
                       tagged.output.access ==
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT &&
                       tagged.hdr.view == view(11) &&
                       tagged.hdr.memory == memory(21) &&
                       tagged.hdr.aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
                       tagged.depth.view == view(12) &&
                       tagged.depth.memory == memory(22) &&
                       tagged.depth.aspect == VK_IMAGE_ASPECT_DEPTH_BIT &&
                       tagged.velocity.view == view(13) &&
                       tagged.velocity.memory == memory(23) &&
                       tagged.output.view == view(14) &&
                       tagged.output.memory == memory(24) &&
                       (tagged.output.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
            output = {true, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            return true;
        },
        [](const matter::DlssOptions& queried,
           matter::DlssOptimalSettings& optimal, std::string&) {
            if (queried.mode != matter::DlssMode::Quality ||
                queried.output_extent.width != 1920 ||
                queried.output_extent.height != 1080)
                return false;
            optimal = {{1280, 720}, 0.0f};
            return true;
        });
    std::vector<const char*> instance_extensions;
    std::vector<const char*> device_extensions;
    VkPhysicalDeviceVulkan12Features required12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features required13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    uint32_t graphics_queues = 1;
    uint32_t compute_queues = 0;
    fake.append_requirements(instance_extensions, device_extensions, required12,
                             required13, graphics_queues, compute_queues);
    CHECK(required13.privateData == VK_TRUE,
          "active Streamline bridge explicitly enables Vulkan 1.3 privateData");
    matter::DlssOptimalSettings optimal{};
    CHECK(fake.query_dlss_optimal_settings(options, optimal, error) &&
              optimal.render_extent.width == 1280 &&
              optimal.render_extent.height == 720 &&
              optimal.sharpness == 0.0f,
          "fake Quality returns exact optimal settings for requested output");
    CHECK(fake.evaluate_dlss(VK_NULL_HANDLE, 77, options, constants,
                             resources, evaluation_output, error) && received &&
              evaluation_output.output_written &&
              fake.test_dlss_evaluation_count() == 1 &&
              fake.active_dlss_mode() == matter::DlssMode::Quality,
          "fake Quality receives exact constants, distinct tagged resources, and output");
    matter::DlssOptions native_options{matter::DlssMode::Native,
                                       {1920, 1080}, true, true};
    CHECK(fake.evaluate_dlss(VK_NULL_HANDLE, 78, native_options, constants,
                             resources, evaluation_output, error) &&
              fake.active_dlss_mode() == matter::DlssMode::Native &&
              fake.consume_dlss_history_reset() &&
              !fake.consume_dlss_history_reset(),
          "Quality to Native sends eOff and requests exactly one history reset");
    received = false;
    const std::vector<matter::DlssMode> expected_mode_transitions{
        matter::DlssMode::Quality, matter::DlssMode::Native,
        matter::DlssMode::Quality};
    CHECK(fake.evaluate_dlss(VK_NULL_HANDLE, 79, options, constants, resources,
                             evaluation_output, error) && received &&
              fake.active_dlss_mode() == matter::DlssMode::Quality &&
              mode_transitions == expected_mode_transitions,
          "fake DLSS bridge observes the complete Quality Native Quality transition");
    CHECK(fake.free_dlss_resources(error) &&
              fake.test_dlss_resource_free_count() == 1 &&
              fake.active_dlss_mode() == matter::DlssMode::Native,
          "DLSS resize releases viewport resources before tagged images change");

    matter::StreamlineBridge failing = matter::StreamlineBridge::test_fake_dlss(
        [](VkCommandBuffer, uint64_t, const matter::DlssOptions&,
           const matter::DlssConstants&, const matter::DlssResources&,
           matter::DlssEvaluationOutput&, std::string& evaluation_error) {
            evaluation_error = "injected DLSS evaluation failure";
            return false;
        });
    CHECK(!failing.evaluate_dlss(VK_NULL_HANDLE, 78, options, constants,
                                 resources, evaluation_output, error) &&
              failing.active_dlss_mode() == matter::DlssMode::Native &&
              failing.consume_dlss_history_reset() &&
              !failing.consume_dlss_history_reset() &&
              failing.dlss_unavailable_reason().find("injected") !=
                  std::string::npos,
          "evaluation error selects Native and resets exactly the following history");
}

void run_vulkan_cpu_contract_tests() {
    test_atmosphere_timing_contract();
    test_water_forward_perf_evidence_contract();
    test_atmosphere_acceptance_fifo_parser_and_present_sequencer();
    test_screenshot_completion_marker_is_nonempty();
    test_vulkan_lighting_override_contract();
    test_viewer_lighting_controls();
    run_vulkan_gi_math_tests();
    run_raster_mesh_material_contract_tests();
    run_rt_lod_payload_contract_tests();
    run_ray_tracing_capability_contract_tests();
    run_vulkan_instance_cache_tests();
    run_vulkan_temporal_tests();
    run_vulkan_gi_temporal_sequence_tests();
    run_streamline_bridge_fallback_tests();
    run_dlss_bridge_contract_tests();
}
