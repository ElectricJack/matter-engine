"""CPU check of production RT group selection/remapping; no Vulkan execution."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "libs/MatterGraphics/src/native"


def test_shadow_and_surface_pipeline_groups(tmp_path):
    source = (NATIVE / "rt_pipeline.inl").read_text()
    groups = source[source.index("    VkRayTracingShaderGroupCreateInfoKHR groups[10]"):
                    source.index("    VkRayTracingPipelineCreateInfoKHR create{")]
    selections = []
    for name in ["rt_shadow_pipeline_", "rt_surface_test_pipeline_"]:
        match = re.search(r"create_trace_pipeline\(" + name + r", (\{[^}]+\}), (\d+),", source)
        assert match, name
        selections.append(match.groups())
    fixture = r'''
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <vector>
#include "rt_pipeline_subset.inl"
int main() {
  for (bool rt_separate_primary_ : {false, true}) {
    const uint32_t diffuse_stage = rt_separate_primary_ ? 10u : 9u;
    const uint32_t diffuse_group = rt_separate_primary_ ? 8u : 7u;
    VkPipelineShaderStageCreateInfo stages[12]{};
    VkSpecializationInfo constants[12]{};
    for (uint32_t i = 0; i < 12; ++i) {
      stages[i].module = (VkShaderModule)(uintptr_t)(i + 1);
      stages[i].pSpecializationInfo = &constants[i];
    }
    // GROUPS
    const auto shadow = rt_pipeline_subset(stages, groups, SHADOW_GROUPS);
    const auto surface = rt_pipeline_subset(stages, groups, SURFACE_GROUPS);
    assert(shadow.groups.size() == 1 + 2 * SHADOW_COUNT);
    assert(surface.groups.size() == 1 + 2 * SURFACE_COUNT);
    assert(shadow.stages.size() == 4 && shadow.groups.size() == 3);
    assert(surface.stages.size() == 7 && surface.groups.size() == 5);
    const auto check_stages = [&](const RtPipelineSubset& subset,
                                   std::initializer_list<uint32_t> original) {
      size_t i = 0;
      for (uint32_t old : original) {
        assert(subset.stages[i].module == stages[old].module);
        assert(subset.stages[i].pSpecializationInfo == &constants[old]);
        ++i;
      }
      for (const auto& group : subset.groups) {
        assert(group.intersectionShader == VK_SHADER_UNUSED_KHR);
        if (group.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR) {
          assert(group.closestHitShader == VK_SHADER_UNUSED_KHR);
          assert(group.anyHitShader == VK_SHADER_UNUSED_KHR);
        } else {
          assert(group.generalShader == VK_SHADER_UNUSED_KHR);
        }
      }
    };
    // No lighting (stage 2 or >=9), and shadow excludes surface-hit shaders.
    check_stages(shadow, {0, 3, 5, 6});
    check_stages(surface, {1, 3, 4, 5, 6, 7, 8});
    assert(shadow.groups[0].generalShader == 0);
    assert(shadow.groups[1].generalShader == 1);
    assert(shadow.groups[2].closestHitShader == 2);
    assert(shadow.groups[2].anyHitShader == 3);
    // Radiance miss and surface hit retain SBT index one within their categories.
    assert(surface.groups[2].generalShader == 2);
    assert(surface.groups[3].closestHitShader == 3);
    assert(surface.groups[3].anyHitShader == 4);
    assert(surface.groups[4].closestHitShader == 5);
    assert(surface.groups[4].anyHitShader == 6);
    // Lighting's source groups remain untouched, including specialization groups.
    assert(groups[2].generalShader == 2 && groups[6].closestHitShader == 7);
    assert(groups[diffuse_group].generalShader == diffuse_stage);
    // Shared stages are remapped once even when multiple groups reference them.
    const auto shared = rt_pipeline_subset(stages, groups, {5, 5});
    assert(shared.stages.size() == 2 && shared.groups.size() == 2);
    assert(shared.groups[0].closestHitShader == shared.groups[1].closestHitShader);
    assert(shared.groups[0].anyHitShader == shared.groups[1].anyHitShader);
  }
}
'''
    fixture = fixture.replace("// GROUPS", groups)
    for label, (indices, count) in zip(["SHADOW", "SURFACE"], selections):
        fixture = fixture.replace(label + "_GROUPS", indices).replace(label + "_COUNT", count)
    cpp = tmp_path / "pipeline.cpp"
    cpp.write_text(fixture)
    exe = tmp_path / "pipeline"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(ROOT / "third_party/Vulkan-Headers/include"),
                    "-I" + str(NATIVE), str(cpp), "-o", str(exe)],
                   check=True, capture_output=True, text=True, timeout=60)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=10)
