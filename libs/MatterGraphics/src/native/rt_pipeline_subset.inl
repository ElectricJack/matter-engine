// Internal pipeline assembly: preserve group order (the shader's SBT indices),
// while copying only the stages referenced by those groups. Some drivers lower
// every raygen in a pipeline into one shader, even for a single selected raygen.
struct RtPipelineSubset {
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
};

static RtPipelineSubset rt_pipeline_subset(
    const VkPipelineShaderStageCreateInfo* stages,
    const VkRayTracingShaderGroupCreateInfoKHR* groups,
    std::initializer_list<uint32_t> group_indices) {
    RtPipelineSubset subset;
    std::vector<uint32_t> original_stages;
    const auto remap = [&](uint32_t& index) {
        if (index == VK_SHADER_UNUSED_KHR) return;
        const auto found = std::find(original_stages.begin(),
                                     original_stages.end(), index);
        if (found != original_stages.end()) {
            index = static_cast<uint32_t>(found - original_stages.begin());
        } else {
            original_stages.push_back(index);
            subset.stages.push_back(stages[index]);
            index = static_cast<uint32_t>(subset.stages.size() - 1);
        }
    };
    for (uint32_t index : group_indices) {
        auto group = groups[index];
        remap(group.generalShader);
        remap(group.closestHitShader);
        remap(group.anyHitShader);
        remap(group.intersectionShader);
        subset.groups.push_back(group);
    }
    return subset;
}
