#include "check.h"
#include "animation/animation_binding_bake.h"
#include "indexed_part_geometry.h"

#include <array>
#include <cstdio>

namespace {
using matter::Float3;
using matter::animation::BindingBake;
using matter::animation::CanonicalJoint;
using matter::animation::CanonicalRig;
using matter::animation::JointIndex;

CanonicalRig two_joint_rig() {
    CanonicalRig rig;
    rig.joints.push_back({"root", matter::animation::kInvalidJoint, {}, 1.0f, {0, 2}, {}});
    matter::AnimationTransform child{};
    child.translation = {2.0f, 0.0f, 0.0f};
    rig.joints.push_back({"tip", 0, child, 0.5f, {1, 2}, {}});
    return rig;
}

viewer::IndexedPartGeometry geometry() {
    viewer::IndexedPartGeometry g;
    g.vertices = {0,0,0, 1,0,0, 0,1,0};
    g.normals = {0,0,1, 0,0,1, 0,0,1};
    g.colors = {255,255,255,255, 255,255,255,255, 255,255,255,255};
    g.texcoords = {0,1, 0,1, 0,1};
    g.surface_uvs = {0,0, 1,0, 0,1};
    g.material_ids = {7,7,7};
    g.baked_ao = {1,1,1};
    g.indices = {0,1,2};
    g.vertex_count = 3;
    return g;
}

void test_signature_is_stable_and_includes_indices() {
    auto g = geometry();
    const uint64_t first = viewer::indexed_part_geometry_signature(g, 0);
    CHECK(first != 0, "indexed geometry has a non-zero semantic signature");
    CHECK(first == viewer::indexed_part_geometry_signature(g, 0), "signature is deterministic");
    std::swap(g.indices[1], g.indices[2]);
    CHECK(first != viewer::indexed_part_geometry_signature(g, 0), "signature includes indexed winding/order");
}

void test_weights_are_quantized_and_bind_pose_safe() {
    BindingBake bake;
    CHECK(matter::animation::build_skin_binding(two_joint_rig(), {geometry()}, 1.0f, bake), "build skin binding");
    CHECK(bake.lods.size() == 1 && bake.lods[0].influences.size() == 3, "one influence record per final vertex");
    const auto& at_root = bake.lods[0].influences[0];
    uint32_t sum = 0; for (uint16_t v : at_root.weights) sum += v;
    CHECK(sum == 65535, "UNORM16 weights normalize exactly");
    CHECK(at_root.joints[0] == 0 && at_root.weights[0] > at_root.weights[1], "root endpoint favors root joint");
    CHECK(bake.inverse_bind_matrices.size() == 2, "inverse bind matrices cover every joint");
    CHECK(!bake.lods[0].cluster_bounds.empty(), "conservative joint-local bounds are emitted");
}

void test_primary_claims_and_animated_children_fail_closed() {
    matter::animation::BindingClaims claims(2);
    CHECK(claims.claim_skin({0, 1}, false), "primary skin claims segments");
    CHECK(!claims.claim_rigid({1}, false), "overlapping primary binding is rejected");
    CHECK(claims.claim_rigid({1}, true), "decorative overlap is explicit");
    CHECK(!matter::animation::validate_attachment(false, true), "nested committed animation is rejected");
    CHECK(matter::animation::validate_attachment(true, false), "static resolved child is accepted");
}

void test_duplicate_segment_claim_fails_closed() {
    matter::animation::BindingClaims claims(2);
    CHECK(!claims.claim_skin({1, 1}, false),
          "a primary binding cannot claim the same segment twice");
    CHECK(claims.claim_rigid({1}, false),
          "a rejected duplicate claim leaves the segment unclaimed");
}
}

int main() {
    test_signature_is_stable_and_includes_indices();
    test_weights_are_quantized_and_bind_pose_safe();
    test_primary_claims_and_animated_children_fail_closed();
    test_duplicate_segment_claim_fails_closed();
    return check_summary();
}
