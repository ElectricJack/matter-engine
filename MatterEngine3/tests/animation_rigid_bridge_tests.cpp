#include "check.h"
#include "render/animation_rigid_bridge.h"

#include <vector>

namespace {

matter::Mat4f matrix(float x) {
    matter::Mat4f value{};
    value.m[0] = value.m[5] = value.m[10] = value.m[15] = 1.0f;
    value.m[3] = x;
    return value;
}

matter::AnimatorInstanceHandle handle() {
    return {7, 3, UINT32_MAX, static_cast<matter::AnimationValueType>(0xff),
            matter::AnimationCadence::Invalid};
}

void publish_pose(matter::animation::AnimationPoseSnapshotStore& store) {
    std::vector<matter::AnimationTransform> local(1);
    std::vector<matter::Mat4f> current{matrix(2.0f)};
    std::vector<matter::Mat4f> previous{matrix(1.0f)};
    matter::animation::AnimationPoseSnapshot snapshot{
        handle(), 4, 9,
        {local.data(), 1}, {current.data(), 1}, {previous.data(), 1},
        {current.data(), 1}, {previous.data(), 1}};
    CHECK(store.publish(snapshot), "pose snapshot publishes");
}

void test_rigid_and_socket_expand_in_serialized_order() {
    matter::animation::AnimationPoseSnapshotStore snapshots;
    publish_pose(snapshots);
    matter::animation::BindingBake bindings;
    bindings.rigid_segments.push_back({"arm", 0, {}, false, {{0, 1, 0, 1}}});
    bindings.attachments.push_back({"tool", "hand", matter::animation::AttachmentTargetKind::Socket,
                                    0x222, {}});
    matter::animation::CanonicalRig rig;
    matter::animation::CanonicalJoint root{};
    root.name = "root";
    rig.joints.push_back(root);
    matter::animation::CanonicalSocket hand{};
    hand.name = "hand";
    hand.joint = 0;
    hand.local.translation = {3.0f, 0.0f, 0.0f};
    rig.sockets.push_back(hand);
    matter::render::AnimationRigidAsset asset{0x111, 2, &bindings, &rig, {0x111}};
    matter::render::AnimationRigidBinding binding{handle(), &asset, 2, true};
    matter::render::AnimationRigidExpansion input{{42, 5, 0}, matrix(10.0f), matrix(8.0f), binding};
    std::vector<matter::render::DynamicInstanceInput> out;
    matter::render::AnimationRigidBridge bridge(&snapshots);
    CHECK(bridge.expand(input, out), "rigid bridge accepts complete immutable asset");
    CHECK(out.size() == 2, "one rigid and one attachment produce two dynamic records");
    if (out.size() == 2) {
        CHECK(out[0].key.binding_index == 1 && out[0].part_hash == 0x111,
              "rigid record uses binding index one and resolved part hash");
        CHECK(out[0].object_to_world.m[3] == 12.0f && out[0].previous_object_to_world.m[3] == 9.0f,
              "rigid record composes current and matching previous transforms");
        CHECK(out[1].key.binding_index == 2 && out[1].part_hash == 0x222,
              "attachment follows rigid serialized order");
        CHECK(out[1].object_to_world.m[3] == 15.0f && out[1].previous_object_to_world.m[3] == 12.0f,
              "socket local composes after its joint");
    }
}

void test_missing_snapshot_and_stale_asset_reject_without_append() {
    matter::animation::BindingBake bindings;
    bindings.rigid_segments.push_back({"arm", 0, {}, false, {{0, 1, 0, 1}}});
    matter::animation::CanonicalRig rig;
    matter::animation::CanonicalJoint root{};
    root.name = "root";
    rig.joints.push_back(root);
    matter::render::AnimationRigidAsset asset{0x111, 2, &bindings, &rig, {0x111}};
    matter::render::AnimationRigidBinding binding{handle(), &asset, 1, true};
    matter::render::AnimationRigidExpansion input{{42, 5, 0}, matrix(1), matrix(1), binding};
    std::vector<matter::render::DynamicInstanceInput> out;
    matter::animation::AnimationPoseSnapshotStore snapshots;
    matter::render::AnimationRigidBridge bridge(&snapshots);
    CHECK(!bridge.expand(input, out) && out.empty(), "stale asset generation rejects before mutation");
    binding.asset_generation = 2;
    input.binding = binding;
    CHECK(!bridge.expand(input, out) && out.empty(), "missing snapshot does not produce stale records");
}

} // namespace

int main() {
    test_rigid_and_socket_expand_in_serialized_order();
    test_missing_snapshot_and_stale_asset_reject_without_append();
    return check_summary();
}
