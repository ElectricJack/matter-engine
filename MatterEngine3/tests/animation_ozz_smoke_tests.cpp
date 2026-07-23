#include "check.h"
#include "animation/ozz_adapter.h"

#include <cstdio>
#include <vector>

namespace {

matter::animation::RigDefinition make_rig() {
    using namespace matter;
    using namespace matter::animation;
    AnimationTransform root;
    root.translation = {1.0f, 0.0f, 0.0f};
    AnimationTransform child;
    child.translation = {0.0f, 2.0f, 0.0f};
    RigDefinition rig;
    rig.joints = {{"root", "", root, 1.0f, {}}, {"child", "root", child, 1.0f, {}}};
    return rig;
}

void test_two_joint_rig_round_trip_contract() {
    using namespace matter::animation;
    Diagnostics diagnostics;
    OzzSkeleton rig;
    CHECK(build_skeleton(make_rig(), rig, diagnostics), "adapter builds a two-joint Matter rig");
    CHECK(rig.joint_count() == 2, "adapter preserves two joints");
    CHECK(rig.parent(0) == kInvalidJoint && rig.parent(1) == 0, "adapter preserves parent order");

    const RigDefinition definition = make_rig();
    std::vector<matter::AnimationTransform> locals = {definition.joints[0].local, definition.joints[1].local};
    std::vector<matter::Mat4f> models;
    CHECK(local_to_model(rig, locals, models), "adapter computes bind-pose models");
    CHECK(models[0].m[3] == 1.0f && models[1].m[3] == 1.0f && models[1].m[7] == 2.0f,
          "bind-pose model transforms use Matter row-major matrices");

    std::vector<uint8_t> bytes;
    std::vector<uint8_t> same_bytes;
    OzzSkeleton same_rig;
    CHECK(serialize_skeleton(rig, bytes), "adapter serializes skeleton");
    CHECK(build_skeleton(make_rig(), same_rig, diagnostics) && serialize_skeleton(same_rig, same_bytes) && bytes == same_bytes,
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
