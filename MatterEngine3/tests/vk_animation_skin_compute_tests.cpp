// CPU oracle for shaders_vk/animation_skin.comp. Kept Vulkan-free so the
// deformation equations are regression-tested on every host.
#include "check.h"
#include "render/vk_animation_skinning.h"

#include <cstddef>
#include <cmath>

using namespace viewer;

static VkSkinMatrix translate(float x) {
    VkSkinMatrix result{};
    result.elements[0] = result.elements[5] = result.elements[10] =
        result.elements[15] = 1.0f;
    result.elements[12] = x;
    return result;
}

static void test_single_joint_and_attribute_copy() {
    CHECK(sizeof(VkSkinSourceVertex) == 80 && sizeof(VkSkinVertex) == 96 &&
              offsetof(VkSkinVertex, previous_position) == 16,
          "compute source/output vertex ABI reserves prior deformation position");
    VkSkinSourceVertex source{};
    source.position[0] = 3.0f; source.normal[2] = 1.0f;
    source.tint[1] = 0.5f; source.surface[3] = 1.0f;
    source.material_index = 42;
    VkSkinInfluence influence{}; influence.weight[0] = 65535;
    VkSkinJoint current[1]{}; current[0].position = translate(2); current[0].normal = translate(0);
    VkSkinJoint previous[1]{}; previous[0].position = translate(-2); previous[0].normal = translate(0);
    VkSkinVertex result{};
    CHECK(vk_skin_vertex_cpu(source, influence, current, previous, 1, result),
          "single-joint skinning succeeds");
    CHECK(std::fabs(result.position[0] - 5.0f) < 1e-5f &&
              std::fabs(result.previous_position[0] - 1.0f) < 1e-5f,
          "current and prior palettes produce distinct deformation positions");
    CHECK(result.material_index == source.material_index && result.tint[1] == 0.5f &&
              result.surface[3] == 1.0f,
          "compute preserves material attributes exactly");
}

static void test_normal_normalization_and_fail_closed_inputs() {
    VkSkinSourceVertex source{}; source.normal[0] = 1.0f;
    VkSkinInfluence influence{}; influence.weight[0] = 65535;
    VkSkinJoint palette[1]{}; palette[0].position = translate(0); palette[0].normal = translate(0);
    palette[0].normal.elements[0] = 2.0f;
    VkSkinVertex result{};
    CHECK(vk_skin_vertex_cpu(source, influence, palette, palette, 1, result) &&
              std::fabs(result.normal[0] - 1.0f) < 1e-5f,
          "inverse-transpose normal result is normalized after blend");
    influence.weight[0] = 0;
    CHECK(!vk_skin_vertex_cpu(source, influence, palette, palette, 1, result),
          "zero total weight cannot publish undefined skinned geometry");
    influence.weight[0] = 1; influence.joint[0] = 3;
    CHECK(!vk_skin_vertex_cpu(source, influence, palette, palette, 1, result),
          "nonzero invalid joint fails closed before output is written");
}

int main() {
    test_single_joint_and_attribute_copy();
    test_normal_normalization_and_fail_closed_inputs();
    return check_summary();
}
