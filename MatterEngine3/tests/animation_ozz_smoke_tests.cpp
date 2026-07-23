#include "check.h"
#include "animation/ozz_adapter.h"

#include <cstdio>
#include <vector>

namespace {

bool matrix_has_translation(const matter::Mat4f& matrix,
                            float x,
                            float y,
                            float z) {
    return matrix.m[3] == x && matrix.m[7] == y && matrix.m[11] == z;
}

void test_two_joint_rig_round_trip_contract() {
    using matter::animation::OzzAdapter;
    using matter::animation::RigJoint;

    const std::vector<RigJoint> joints = {
        {"root", -1, {1.0f, 0.0f, 0.0f}},
        {"child", 0, {0.0f, 2.0f, 0.0f}},
    };

    const auto rig = OzzAdapter::make_rig(joints);
    const auto same_rig = OzzAdapter::make_rig(joints);

    CHECK(rig.joint_count() == 2, "adapter preserves two joints");
    CHECK(rig.parent(0) == -1 && rig.parent(1) == 0,
          "adapter preserves parent order");
    CHECK(matrix_has_translation(rig.bind_pose_model(0), 1.0f, 0.0f, 0.0f),
          "root bind-pose model transform is Matter row-major");
    CHECK(matrix_has_translation(rig.bind_pose_model(1), 1.0f, 2.0f, 0.0f),
          "child bind-pose model transform composes its parent");
    CHECK(rig.serialized_bytes() == same_rig.serialized_bytes(),
          "adapter serialization bytes are deterministic");
}

} // namespace

int main() {
    test_two_joint_rig_round_trip_contract();
    if (g_failures != 0) {
        std::printf("animation_ozz_smoke_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("animation_ozz_smoke_tests: all tests passed\n");
    return 0;
}
