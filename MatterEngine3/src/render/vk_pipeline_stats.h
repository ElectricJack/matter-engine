#pragma once

#include <cstdlib>
#include <vector>
#include <vulkan/vulkan.h>

#include "matter/log.h"

namespace viewer {

// Opt-in driver statistics. Names and units are vendor supplied; consumers
// must not treat SPIR-V variable counts as register allocation or occupancy.
inline void log_vk_pipeline_stats(VkDevice device, VkPipeline pipeline,
                                  const char* label) {
    if (!std::getenv("MATTER_VK_PIPELINE_STATS")) return;
    MATTER_LOGI("pipeline-stats", "%s: querying driver executable statistics", label);
    auto properties = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
    auto statistics = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));
    auto representations = reinterpret_cast<
        PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(
        vkGetDeviceProcAddr(device,
                            "vkGetPipelineExecutableInternalRepresentationsKHR"));
    if (!properties || !statistics) {
        MATTER_LOGW("pipeline-stats", "%s: executable statistics unavailable", label);
        return;
    }
    VkPipelineInfoKHR pipeline_info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pipeline_info.pipeline = pipeline;
    uint32_t executable_count = 0;
    const VkResult property_result = properties(
        device, &pipeline_info, &executable_count, nullptr);
    if (property_result != VK_SUCCESS || executable_count == 0) {
        MATTER_LOGW("pipeline-stats", "%s: property query result=%d count=%u",
                    label, property_result, executable_count);
        return;
    }
    std::vector<VkPipelineExecutablePropertiesKHR> executables(
        executable_count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    if (properties(device, &pipeline_info, &executable_count,
                   executables.data()) != VK_SUCCESS)
        return;
    for (uint32_t i = 0; i < executable_count; ++i) {
        MATTER_LOGI("pipeline-stats", "%s executable=%s stages=0x%x subgroup=%u",
                    label, executables[i].name, executables[i].stages,
                    executables[i].subgroupSize);
        VkPipelineExecutableInfoKHR info{
            VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        info.pipeline = pipeline;
        info.executableIndex = i;
        uint32_t count = 0;
        if (statistics(device, &info, &count, nullptr) != VK_SUCCESS) continue;
        std::vector<VkPipelineExecutableStatisticKHR> values(
            count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        if (statistics(device, &info, &count, values.data()) != VK_SUCCESS)
            continue;
        for (const auto& value : values) {
            switch (value.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                MATTER_LOGI("pipeline-stats", "%s %s=%u (%s)", label,
                            value.name, value.value.b32, value.description);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                MATTER_LOGI("pipeline-stats", "%s %s=%lld (%s)", label,
                            value.name, static_cast<long long>(value.value.i64),
                            value.description);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                MATTER_LOGI("pipeline-stats", "%s %s=%llu (%s)", label,
                            value.name, static_cast<unsigned long long>(value.value.u64),
                            value.description);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
                MATTER_LOGI("pipeline-stats", "%s %s=%.3f (%s)", label,
                            value.name, value.value.f64, value.description);
                break;
            default: break;
            }
        }
        if (representations) {
            uint32_t ir_count = 0;
            const VkResult ir_result =
                representations(device, &info, &ir_count, nullptr);
            MATTER_LOGI("pipeline-stats", "%s IR query result=%d count=%u",
                        label, ir_result, ir_count);
            if (ir_result == VK_SUCCESS) {
                std::vector<VkPipelineExecutableInternalRepresentationKHR> irs(
                    ir_count,
                    {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
                if (representations(device, &info, &ir_count, irs.data()) == VK_SUCCESS)
                    for (const auto& ir : irs)
                        MATTER_LOGI("pipeline-stats", "%s IR=%s text=%u bytes=%zu",
                                    label, ir.name, ir.isText, ir.dataSize);
            }
        }
    }
}

} // namespace viewer
