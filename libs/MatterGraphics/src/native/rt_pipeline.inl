// The ray-tracing set layout, pipeline and shader binding table. Only reached
// when the device reports hardware ray tracing; RT-less devices keep every
// rt_* handle null and record_ray_traced_shadows clears its targets instead.
//
// Nine stages in seven groups: three raygen (shadow / surface-test /
// lighting), two miss (visibility / radiance) and two triangle hit groups
// (visibility with an attenuating any-hit, surface with an alpha-test
// any-hit). The SBT is laid out as three separately base-aligned raygen
// records followed by the miss and hit categories, which is why
// rt_sbt_test_raygen_address_ / rt_sbt_lighting_raygen_address_ are stored
// individually -- a trace picks its entry point by choosing an address, not
// by an index. Adaptive primary optionally appends stage 9 / group 7 and a
// fourth raygen record after all existing SBT regions, preserving their offsets.
// Two signal-specialized copies of lighting follow the optional primary record.
bool VkSceneRenderer::create_ray_tracing_pipeline(std::string& error) {
    const VkDevice device = vulkan_->device();
    // Compile the sample count into the shader: keep the four-sample baseline
    // available while measuring temporal primary soft shadows. Zero selects
    // the conservative history-driven adaptive mode; the default stays four.
    uint32_t primary_area_samples = 4u;
    if (const char* value = std::getenv("MATTER_RT_LOCAL_PRIMARY_SAMPLES")) {
        if (value[0] >= '0' && value[0] <= '4' && value[1] == '\0')
            primary_area_samples = static_cast<uint32_t>(value[0] - '0');
        else
            MATTER_LOGW("rt", "MATTER_RT_LOCAL_PRIMARY_SAMPLES must be 0 (adaptive) or 1..4; using 4");
    }
    rt_primary_adaptive_ = primary_area_samples == 0u;
    const char* primary_only_value = std::getenv("MATTER_RT_PRIMARY_ONLY");
    const bool primary_only = primary_only_value && std::strcmp(primary_only_value, "1") == 0;
    if (primary_only_value && std::strcmp(primary_only_value, "0") &&
        std::strcmp(primary_only_value, "1"))
        MATTER_LOGW("rt", "MATTER_RT_PRIMARY_ONLY must be 0 or 1; using 0");
    rt_separate_primary_ = rt_primary_adaptive_ || primary_light_culling_ ||
                           primary_light_audit_ || primary_only;
    rt_specialize_gi_ = true;
    if (const char* value = std::getenv("MATTER_RT_GI_SPECIALIZE")) {
        if (std::strcmp(value, "0") == 0)
            rt_specialize_gi_ = false;
        else if (std::strcmp(value, "1") != 0)
            MATTER_LOGW("rt", "MATTER_RT_GI_SPECIALIZE must be 0 or 1; using 1");
    }
    MATTER_LOGI("rt", "independent GI shader specialization=%u", rt_specialize_gi_ ? 1u : 0u);
    rt_adaptive_diagnostics_ = false;
    if (const char* value = std::getenv("MATTER_RT_ADAPTIVE_DIAGNOSTICS")) {
        if (std::strcmp(value, "1") == 0)
            rt_adaptive_diagnostics_ = rt_primary_adaptive_;
        else if (std::strcmp(value, "0") != 0)
            MATTER_LOGW("rt", "MATTER_RT_ADAPTIVE_DIAGNOSTICS must be 0 or 1; disabled");
    }
    VkDescriptorSetLayoutBinding bindings[] = {
        descriptor_binding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
        descriptor_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
        descriptor_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
        descriptor_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        // Phase 1 tileset Vulkan port (Task 6): mirrors raster set 1's
        // bindings 6/7. Stage flags must cover every stage whose SPIR-V can
        // statically reference the bindings: tileset_common.glsl is pulled in
        // by rt_surface_common.glsl, which is included by rt_lighting.rgen /
        // rt_surface_test.rgen (raygen), rt_surface.rchit (closest hit),
        // rt_visibility.rahit (any hit), and rt_radiance.rmiss (miss). glslang
        // keeps declared-but-uncalled helper functions (rt_tileset_sample) in
        // the module unless optimized, and SPIR-V 1.4+ lists all globals in
        // the entry-point interface, so be conservative and cover all four
        // stages — extra stage bits on a set layout are harmless.
        descriptor_binding(15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        // WP-G (chart VT in the RT path): 17/18/19 mirror raster set 1's VT
        // bindings 10/11/12 (pool array, indirection buffer, variant table).
        // Same all-four-stages reasoning as 15/16 above: vt_common.glsl is
        // pulled in by rt_surface_common.glsl, so every stage that includes it
        // lists these globals in its entry-point interface whether or not it
        // samples. Binding 13 (feedback) is NOT mirrored — rays never request
        // pages. 18 became a STORAGE_BUFFER with the buffer indirection (it
        // was the R16G16_UINT image array whose 2048-layer format cap forced
        // the redesign).
        descriptor_binding(17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        // RT PBR Phase 1: transmission denoiser aux lane, the storage-image
        // sibling of binding 13 (raw_specular_aux).
        descriptor_binding(20, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(21, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(22, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(23, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(24, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        // Separate traced local-direct lane. It is written by the lighting
        // raygen even when GI is disabled and sampled only when set-2 metadata
        // publishes RayTraced ownership for this frame slot.
        descriptor_binding(26, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                           VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(28, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                           VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(29, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR)};
    bindings[15].descriptorCount =
        kTilesetSourceDescriptors;
    bindings[17].descriptorCount = vt::kVtChannelCount;
    bindings[21].descriptorCount = kWaterFieldBindingSlots;
    bindings[22].descriptorCount = kWaterFieldBindingSlots;
    bindings[23].descriptorCount = kWaterFieldBindingSlots;
    bindings[24].descriptorCount = kWaterFieldBindingSlots;
    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    // Original scene set is identical in both modes. Adaptive history is set 3.
    set_info.bindingCount = static_cast<uint32_t>(std::size(bindings));
    set_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device, &set_info, nullptr,
                                                   &rt_set_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(ray tracing)", result,
                       error);
    if (rt_primary_adaptive_) {
        VkDescriptorSetLayoutBinding history_bindings[8]{};
        for (uint32_t i = 0; i < 8; ++i)
            history_bindings[i] = descriptor_binding(
                i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                VK_SHADER_STAGE_RAYGEN_BIT_KHR);
        VkDescriptorSetLayoutCreateInfo history_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        history_info.bindingCount = 8;
        history_info.pBindings = history_bindings;
        result = vkCreateDescriptorSetLayout(device, &history_info, nullptr,
                                              &rt_adaptive_set_layout_);
        if (result != VK_SUCCESS)
            return fail_vk("vkCreateDescriptorSetLayout(adaptive history)", result, error);
    }
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    push.size = 128;
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const VkDescriptorSetLayout rt_sets[] = {rt_set_layout_,
                                              environment_set_layout_,
                                              local_light_set_layout_,
                                              rt_adaptive_set_layout_};
    layout_info.setLayoutCount = rt_primary_adaptive_ ? 4u : 3u;
    layout_info.pSetLayouts = rt_sets;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    result = vkCreatePipelineLayout(device, &layout_info, nullptr,
                                    &rt_pipeline_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreatePipelineLayout(ray tracing)", result, error);
    const uint32_t diffuse_stage = rt_separate_primary_ ? 10u : 9u;
    const uint32_t diffuse_group = rt_separate_primary_ ? 8u : 7u;
    const uint32_t stage_count = diffuse_stage + 2u;
    const uint32_t group_count = diffuse_group + 2u;
    const char* names[] = {"rt_shadow.rgen.spv", "rt_surface_test.rgen.spv",
                           "rt_lighting.rgen.spv",
                           "rt_visibility.rmiss.spv", "rt_radiance.rmiss.spv",
                           "rt_visibility.rchit.spv",
                           "rt_visibility.rahit.spv",
                           "rt_surface.rchit.spv",
                           "rt_surface.rahit.spv", "rt_primary_adaptive.rgen.spv",
                           "rt_lighting.rgen.spv", "rt_lighting.rgen.spv"};
    // Stage 2 always keeps the original full GI module. Only the optional
    // primary stage includes tile iteration/audit code, so secondary rays do
    // not inherit its larger call graph or compiler resource requirements.
    if (rt_primary_adaptive_) {
        if (primary_light_audit_)
            names[9] = "rt_primary_adaptive_culled_audit.rgen.spv";
        else if (primary_light_culling_)
            names[9] = "rt_primary_adaptive_culled.rgen.spv";
    } else if (rt_separate_primary_) {
        names[9] = primary_light_audit_
            ? "rt_lighting_culled_audit.rgen.spv"
            : (primary_light_culling_ ? "rt_lighting_culled.rgen.spv"
                                      : "rt_primary_fixed.rgen.spv");
    }
    names[diffuse_stage] = names[diffuse_stage + 1u] = "rt_lighting.rgen.spv";
    const VkShaderStageFlagBits stages_bits[] = {
        VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR};
    VkShaderModule modules[12]{};
    VkPipelineShaderStageCreateInfo stages[12]{};
    for (uint32_t i = 0; i < stage_count; ++i) {
        if (!create_shader_module(device, names[i], modules[i], error)) {
            for (VkShaderModule module : modules)
                if (module) vkDestroyShaderModule(device, module, nullptr);
            return false;
        }
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = stages_bits[i];
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    const uint32_t count_lobe_samples = 1u;
#else
    const uint32_t count_lobe_samples = 0u;
#endif
    // RT PBR Phase 1: alpha-tested occluders in the refraction walk
    // (constant_id 1 in rt_lighting.rgen). Default ON: measured on the
    // rt-transmission smoke fixture the two-mask walk costs well under the
    // spec's ~5% RT-budget retreat threshold (see the fixture's gpu-zone
    // print), and correctness -- foliage no longer silhouettes refraction --
    // wins at that price. MATTER_RT_WALK_ALPHA_TEST=0 restores the legacy
    // single OpaqueEXT trace for A/B measurement.
    uint32_t walk_alpha_test = 1u;
    if (const char* walk_env = std::getenv("MATTER_RT_WALK_ALPHA_TEST"))
        walk_alpha_test = std::strtoul(walk_env, nullptr, 10) != 0 ? 1u : 0u;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    uint32_t visibility_counters = 1u;
#else
    uint32_t visibility_counters = 0u;
#endif
    if (const char* value = std::getenv("MATTER_RT_VISIBILITY_COUNTERS")) {
        if ((value[0] == '0' || value[0] == '1') && value[1] == '\0')
            visibility_counters = static_cast<uint32_t>(value[0] - '0');
        else
            MATTER_LOGW("rt", "MATTER_RT_VISIBILITY_COUNTERS must be 0 or 1; retaining default");
    }
    MATTER_LOGI("rt", "local primary area samples=%u (0=adaptive); visibility diagnostic counters=%u",
                primary_area_samples, visibility_counters);
    uint32_t enable_local_selection = local_light_budget_word() != 0u ? 1u : 0u;
    if (const char* value = std::getenv("MATTER_RT_KEEP_LOCAL_SELECTION")) {
        if (std::strcmp(value, "1") == 0)
            enable_local_selection = 1u;
        else if (std::strcmp(value, "0") != 0)
            MATTER_LOGW("rt", "MATTER_RT_KEEP_LOCAL_SELECTION must be 0 or 1; retaining automatic selection policy");
    }
    MATTER_LOGI("rt", "local-light selection code=%u; packed budgets=%u",
                enable_local_selection, local_light_budget_word());
    // The weighted estimator avoids tracing every secondary shadow candidate.
    // Preserve an explicitly configured legacy secondary budget unless the
    // sampling override below requests otherwise.
    uint32_t secondary_light_sampling =
        (local_light_budget_word() & 0xff00u) != 0u ? 0u : 1u;
    if (const char* value = std::getenv("MATTER_RT_SECONDARY_LIGHT_SAMPLING")) {
        if ((value[0] == '0' || value[0] == '1') && value[1] == '\0')
            secondary_light_sampling = static_cast<uint32_t>(value[0] - '0');
        else {
            secondary_light_sampling = 0u;
            MATTER_LOGW("rt", "MATTER_RT_SECONDARY_LIGHT_SAMPLING must be 0 or 1; using exact mode 0");
        }
    }
    MATTER_LOGI("rt", "secondary local-light sampling=%u (0 exact, 1 importance reservoir)",
                secondary_light_sampling);
    const uint32_t lighting_constants[7] = {count_lobe_samples,
                                            walk_alpha_test,
                                            rt_primary_adaptive_ ? 4u : primary_area_samples,
                                            enable_local_selection,
                                            secondary_light_sampling,
                                            primary_light_culling_ ? 1u : 0u,
                                            primary_light_audit_ ? 1u : 0u};
    const VkSpecializationMapEntry lighting_entries[7] = {
        {0, 0, sizeof(uint32_t)},
        {1, sizeof(uint32_t), sizeof(uint32_t)},
        {4, 2 * sizeof(uint32_t), sizeof(uint32_t)},
        {6, 3 * sizeof(uint32_t), sizeof(uint32_t)},
        {7, 4 * sizeof(uint32_t), sizeof(uint32_t)},
        {9, 5 * sizeof(uint32_t), sizeof(uint32_t)},
        {10, 6 * sizeof(uint32_t), sizeof(uint32_t)}};
    const uint32_t lighting_constant_count = primary_light_audit_ ? 7u :
        (primary_light_culling_ ? 6u : 5u);
    // Preserve the baseline stage's original five specialization entries and
    // data size even when the separate primary stage is enabled.
    const VkSpecializationInfo lighting_specialization{
        5u, lighting_entries, 5u * sizeof(uint32_t), lighting_constants};
    stages[2].pSpecializationInfo = &lighting_specialization;
    // Keep all original lighting constants and specialize only ownership.
    // The shared module retains exactly the same random streams and ray work.
    uint32_t signal_constants[2][6]{};
    VkSpecializationMapEntry signal_entries[6]{};
    std::copy_n(lighting_entries, 5, signal_entries);
    signal_entries[5] = {11, 5u * sizeof(uint32_t), sizeof(uint32_t)};
    VkSpecializationInfo signal_specializations[2]{};
    for (uint32_t signal = 0; signal < 2; ++signal) {
        std::copy_n(lighting_constants, 5, signal_constants[signal]);
        signal_constants[signal][5] = signal + 1u;
        signal_specializations[signal] = {
            6u, signal_entries, sizeof(signal_constants[signal]), signal_constants[signal]};
        stages[diffuse_stage + signal].pSpecializationInfo = &signal_specializations[signal];
    }
    const VkSpecializationInfo primary_specialization{
        lighting_constant_count, lighting_entries,
        lighting_constant_count * sizeof(uint32_t), lighting_constants};
    const uint32_t adaptive_constants[] = {
        enable_local_selection, rt_adaptive_diagnostics_ ? 1u : 0u,
        primary_light_culling_ ? 1u : 0u, primary_light_audit_ ? 1u : 0u};
    const VkSpecializationMapEntry adaptive_entries[] = {
        {6, 0, sizeof(uint32_t)}, {8, sizeof(uint32_t), sizeof(uint32_t)},
        {9, 2 * sizeof(uint32_t), sizeof(uint32_t)},
        {10, 3 * sizeof(uint32_t), sizeof(uint32_t)}};
    const uint32_t adaptive_constant_count = primary_light_audit_ ? 4u :
        (primary_light_culling_ ? 3u : 2u);
    const VkSpecializationInfo adaptive_specialization{
        adaptive_constant_count, adaptive_entries,
        adaptive_constant_count * sizeof(uint32_t), adaptive_constants};
    if (rt_separate_primary_)
        stages[9].pSpecializationInfo = rt_primary_adaptive_
            ? &adaptive_specialization : &primary_specialization;
    // Any-hit has its own specialization namespace. Normal runs must not
    // contend on global diagnostic counters for every glass/shadow crossing.
    const VkSpecializationMapEntry visibility_entry{2, 0, sizeof(uint32_t)};
    const VkSpecializationInfo visibility_specialization{
        1, &visibility_entry, sizeof(visibility_counters), &visibility_counters};
    stages[6].pSpecializationInfo = &visibility_specialization;
    uint32_t surface_detail_mode = 0u;
    if (const char* value = std::getenv("MATTER_RT_SURFACE_DETAIL_MODE")) {
        if (value[0] >= '0' && value[0] <= '2' && value[1] == '\0')
            surface_detail_mode = static_cast<uint32_t>(value[0] - '0');
        else
            MATTER_LOGW("rt", "MATTER_RT_SURFACE_DETAIL_MODE must be 0..2; using reference mode 0");
    }
    MATTER_LOGI("rt", "secondary surface detail mode=%u (0 reference, 1 adaptive, 2 normal-only)",
                surface_detail_mode);
    const VkSpecializationMapEntry detail_entry{5, 0, sizeof(uint32_t)};
    const VkSpecializationInfo detail_specialization{
        1, &detail_entry, sizeof(surface_detail_mode), &surface_detail_mode};
    stages[7].pSpecializationInfo = &detail_specialization;
    VkRayTracingShaderGroupCreateInfoKHR groups[10]{};
    for (auto& group : groups) {
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[2].generalShader = 2;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[3].generalShader = 3;
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[4].generalShader = 4;
    groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[5].closestHitShader = 5;
    groups[5].anyHitShader = 6;
    groups[6].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[6].closestHitShader = 7;
    // RT PBR Phase 1: the surface hit group gains an alpha-test any-hit.
    // Only rays traced WITHOUT gl_RayFlagsOpaqueEXT ever run it (today:
    // the refraction walk's non-opaque re-trace).
    groups[6].anyHitShader = 8;
    if (rt_separate_primary_) {
        groups[7].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[7].generalShader = 9;
    }
    for (uint32_t signal = 0; signal < 2; ++signal) {
        groups[diffuse_group + signal].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[diffuse_group + signal].generalShader = diffuse_stage + signal;
    }
    VkRayTracingPipelineCreateInfoKHR create{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    create.stageCount = stage_count;
    create.pStages = stages;
    create.groupCount = group_count;
    create.pGroups = groups;
    create.maxPipelineRayRecursionDepth = 1;
    create.layout = rt_pipeline_layout_;
    const auto create_pipeline = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
    const auto get_handles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR"));
    if (!create_pipeline || !get_handles) {
        error = "native ray tracing pipeline entry points are unavailable";
        result = VK_ERROR_EXTENSION_NOT_PRESENT;
    } else {
        result = profile_pipeline_call("ray_tracing", __func__, __LINE__, 1, &create, [&] {
        return create_pipeline(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
                                 &create, nullptr, &rt_pipeline_);
    });
    }
    for (VkShaderModule module : modules)
        if (module) vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateRayTracingPipelinesKHR", result, error);

    const auto& props = vulkan_->ray_tracing_properties();
    if (props.shader_group_handle_size == 0 ||
        props.shader_group_handle_alignment == 0 ||
        props.shader_group_base_alignment == 0 ||
        props.max_shader_group_stride == 0 ||
        props.max_ray_dispatch_invocation_count == 0) {
        error = "Vulkan device reported unusable ray tracing pipeline limits";
        return false;
    }
    const VkDeviceSize handle_size = props.shader_group_handle_size;
    const VkDeviceSize handle_stride =
        (handle_size + props.shader_group_handle_alignment - 1) /
        props.shader_group_handle_alignment *
        props.shader_group_handle_alignment;
    if (handle_stride > props.max_shader_group_stride) {
        error = "ray tracing SBT stride exceeds maxShaderGroupStride";
        return false;
    }
    std::vector<uint8_t> handles(static_cast<size_t>(group_count * handle_size));
    result = get_handles(device, rt_pipeline_, 0, group_count, handles.size(),
                         handles.data());
    if (result != VK_SUCCESS)
        return fail_vk("vkGetRayTracingShaderGroupHandlesKHR", result, error);
    const VkDeviceSize category_size = 2 * handle_stride;
    const VkDeviceSize category_span =
        (category_size + props.shader_group_base_alignment - 1) /
        props.shader_group_base_alignment * props.shader_group_base_alignment;
    const VkDeviceSize raygen_record_stride =
        (handle_stride + props.shader_group_base_alignment - 1) /
        props.shader_group_base_alignment * props.shader_group_base_alignment;
    const VkDeviceSize raygen_span = 3 * raygen_record_stride;
    // Keep every existing raygen/miss/hit offset unchanged. Optional group 7
    // is a separately base-aligned raygen record appended after the hit span.
    const VkDeviceSize primary_offset = raygen_span + 2 * category_span;
    const VkDeviceSize diffuse_offset = primary_offset +
        (rt_separate_primary_ ? raygen_record_stride : 0);
    const VkDeviceSize sbt_span = diffuse_offset + 2 * raygen_record_stride;
    if (!matter::create_buffer(
            *vulkan_, sbt_span +
                           props.shader_group_base_alignment - 1,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, rt_sbt_, error) ||
        !matter::map_buffer(rt_sbt_, error)) return false;
    rt_sbt_address_ =
        (rt_sbt_.address + props.shader_group_base_alignment - 1) /
        props.shader_group_base_alignment * props.shader_group_base_alignment;
    rt_sbt_stride_ = handle_stride;
    rt_sbt_test_raygen_address_ = rt_sbt_address_ + raygen_record_stride;
    rt_sbt_lighting_raygen_address_ = rt_sbt_address_ + 2 * raygen_record_stride;
    rt_sbt_primary_raygen_address_ = rt_separate_primary_
        ? rt_sbt_address_ + primary_offset : 0;
    rt_sbt_diffuse_raygen_address_ = rt_sbt_address_ + diffuse_offset;
    rt_sbt_reflection_raygen_address_ = rt_sbt_diffuse_raygen_address_ + raygen_record_stride;
    rt_sbt_miss_address_ = rt_sbt_address_ + raygen_span;
    rt_sbt_hit_address_ = rt_sbt_miss_address_ + category_span;
    rt_sbt_miss_size_ = category_size;
    rt_sbt_hit_size_ = category_size;
    const VkDeviceSize mapped_offset = rt_sbt_address_ - rt_sbt_.address;
    std::memset(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset, 0,
                static_cast<size_t>(sbt_span));
    for (uint32_t i = 0; i < 3; ++i) {
        std::memcpy(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset +
                        i * raygen_record_stride,
                    handles.data() + i * handle_size,
                    static_cast<size_t>(handle_size));
    }
    for (uint32_t i = 0; i < 2; ++i) {
        std::memcpy(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset +
                        raygen_span + i * handle_stride,
                    handles.data() + (3 + i) * handle_size,
                    static_cast<size_t>(handle_size));
        std::memcpy(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset +
                        raygen_span + category_span + i * handle_stride,
                    handles.data() + (5 + i) * handle_size,
                    static_cast<size_t>(handle_size));
    }
    if (rt_separate_primary_) {
        std::memcpy(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset +
                        primary_offset,
                    handles.data() + 7 * handle_size,
                    static_cast<size_t>(handle_size));
    }
    for (uint32_t signal = 0; signal < 2; ++signal) {
        std::memcpy(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset +
                        diffuse_offset + signal * raygen_record_stride,
                    handles.data() + (diffuse_group + signal) * handle_size,
                    static_cast<size_t>(handle_size));
    }
    return matter::flush_buffer(rt_sbt_, mapped_offset, sbt_span, error);
}
