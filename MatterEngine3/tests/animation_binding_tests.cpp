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

CanonicalRig three_joint_rig() {
    CanonicalRig rig = two_joint_rig();
    matter::AnimationTransform sibling{};
    sibling.translation = {0.0f, 8.0f, 0.0f};
    rig.joints.push_back({"unselected", 0, sibling, 0.5f, {2, 3}, {}});
    rig.joints[0].subtree.end = 3;
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
    CHECK(matter::animation::build_skin_binding(two_joint_rig(), {1}, {geometry()}, 1.0f, bake), "build skin binding");
    CHECK(bake.lods.size() == 1 && bake.lods[0].influences.size() == 3, "one influence record per final vertex");
    const auto& at_root = bake.lods[0].influences[0];
    uint32_t sum = 0; for (uint16_t v : at_root.weights) sum += v;
    CHECK(sum == 65535, "UNORM16 weights normalize exactly");
    CHECK(at_root.joints[0] == 0 && at_root.weights[0] > at_root.weights[1], "root endpoint favors root joint");
    CHECK(bake.inverse_bind_matrices.size() == 2, "inverse bind matrices cover every joint");
    CHECK(bake.lods[0].clusters.size() == 1 && bake.lods[0].clusters[0].cluster_id == 0 &&
          bake.lods[0].clusters[0].vertex_begin == 0 && bake.lods[0].clusters[0].vertex_end == 3 &&
          !bake.lods[0].clusters[0].joints.empty(),
          "conservative bounds retain a stable per-LOD cluster range");
}

void test_skin_binding_uses_only_the_selected_segments() {
    auto selected = geometry();
    selected.vertices = {0,8,0};
    selected.normals = {0,0,1}; selected.colors = {255,255,255,255};
    selected.texcoords = {0,1}; selected.surface_uvs = {0,0};
    selected.material_ids = {7}; selected.baked_ao = {1};
    selected.indices = {0,0,0}; selected.vertex_count = 1;
    BindingBake bake;
    CHECK(matter::animation::build_skin_binding(three_joint_rig(), {1}, {selected}, 1.0f, bake),
          "a selected segment bakes successfully");
    if (bake.lods.empty() || bake.lods[0].influences.empty()) return;
    const auto& influence = bake.lods[0].influences[0];
    for (size_t i = 0; i < influence.weights.size(); ++i)
        CHECK(!influence.weights[i] || influence.joints[i] == 0 || influence.joints[i] == 1,
              "a selected segment never emits an unselected-joint influence");
}

void test_primary_claims_and_animated_children_fail_closed() {
    matter::animation::BindingClaims claims(two_joint_rig());
    CHECK(!claims.claim_skin({0}, false), "a binding claim cannot select the root");
    CHECK(claims.claim_skin({1}, false), "primary skin claims segments");
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

void test_malformed_rig_hierarchy_fails_before_parent_indexing() {
    CanonicalRig malformed = two_joint_rig();
    malformed.joints[0].parent = 1;
    BindingBake bake;
    CHECK(!matter::animation::build_skin_binding(malformed, {1}, {geometry()}, 1.0f, bake),
          "parent-after-child canonical rig is rejected before binding");
    CHECK(bake.lods.empty() && bake.inverse_bind_matrices.empty(),
          "malformed canonical rig leaves the binding output empty");

    malformed = two_joint_rig();
    malformed.joints[1].parent = 42;
    CHECK(!matter::animation::build_skin_binding(malformed, {1}, {geometry()}, 1.0f, bake),
          "out-of-range canonical rig parent is rejected before binding");
}

void test_binding_payload_retains_lods_matrices_and_cluster_ranges() {
    BindingBake source;
    CHECK(matter::animation::build_skin_binding(two_joint_rig(), {1}, {geometry()}, 1.0f, source),
          "build binding persistence fixture");
    matter::animation::AnimAsset asset;
    asset.sections = {
        {matter::animation::AnimSectionKind::RigSchema, {1}},
        {matter::animation::AnimSectionKind::InputTargetSchemas, {2}},
        {matter::animation::AnimSectionKind::GraphControllerBytecode, {3}},
        {matter::animation::AnimSectionKind::OzzSkeleton, {7}},
        {matter::animation::AnimSectionKind::OzzClips, {8}},
    };
    CHECK(matter::animation::set_anim_binding_bake(asset, source),
          "binding bake serializes into MANM binding sections");
    BindingBake decoded;
    CHECK(matter::animation::get_anim_binding_bake(asset, decoded),
          "binding bake decodes from MANM binding sections");
    CHECK(decoded.lods.size() == 1 && decoded.lods[0].indexed_vertex_signature == source.lods[0].indexed_vertex_signature &&
          decoded.lods[0].influences.size() == source.lods[0].influences.size() &&
          decoded.lods[0].influences[0].joints == source.lods[0].influences[0].joints &&
          decoded.lods[0].influences[0].weights == source.lods[0].influences[0].weights &&
          decoded.inverse_bind_matrices.size() == source.inverse_bind_matrices.size() &&
          decoded.lods[0].clusters.size() == 1 && decoded.lods[0].clusters[0].cluster_id == 0 &&
          decoded.lods[0].clusters[0].vertex_end == source.lods[0].vertex_count,
          "binding payload preserves geometry, inverse binds, and per-LOD cluster bounds");
    for (auto& section : asset.sections) if (section.kind == matter::animation::AnimSectionKind::GeometryBindings) section.bytes[8] = 3;
    CHECK(!matter::animation::get_anim_binding_bake(asset, decoded),
          "truncated binding topology fails closed before allocation");
}

void test_binding_payload_requires_complete_cluster_bounds() {
    BindingBake source;
    CHECK(matter::animation::build_skin_binding(two_joint_rig(), {1}, {geometry()}, 1.0f, source),
          "build binding bounds validation fixture");
    matter::animation::AnimAsset asset;
    auto missing_bounds = source;
    missing_bounds.lods[0].clusters[0].joints.clear();
    CHECK(!matter::animation::set_anim_binding_bake(asset, missing_bounds),
          "empty cluster bounds cannot serialize as a skinned binding");
    auto incomplete_bounds = source;
    incomplete_bounds.lods[0].clusters[0].joints.pop_back();
    CHECK(!matter::animation::set_anim_binding_bake(asset, incomplete_bounds),
          "every influencing joint has a conservative cluster bound");
}

void test_binding_payload_retains_rigid_segments_and_attachments() {
    BindingBake source;
    CHECK(matter::animation::build_skin_binding(two_joint_rig(), {1}, {geometry()}, 1.0f, source),
          "build binding declaration persistence fixture");
    matter::animation::RigidSegmentBake rigid;
    rigid.name = "armor";
    rigid.joint = 1;
    rigid.bind_offset.translation = {2.0f, 3.0f, 4.0f};
    rigid.geometry.push_back({3, 7, 11, 13});
    rigid.lod_geometry.push_back({1, 2});
    source.rigid_segments.push_back(rigid);
    matter::animation::AttachmentBake attachment;
    attachment.name = "tool";
    attachment.target = "arm";
    attachment.target_kind = matter::animation::AttachmentTargetKind::Joint;
    attachment.child_hash = 0x12345678ull;
    attachment.local.translation = {5.0f, 6.0f, 7.0f};
    source.attachments.push_back(attachment);
    matter::animation::AnimAsset asset;
    asset.sections = {
        {matter::animation::AnimSectionKind::RigSchema, {1}},
        {matter::animation::AnimSectionKind::InputTargetSchemas, {2}},
        {matter::animation::AnimSectionKind::GraphControllerBytecode, {3}},
        {matter::animation::AnimSectionKind::OzzSkeleton, {7}},
        {matter::animation::AnimSectionKind::OzzClips, {8}},
    };
    CHECK(matter::animation::set_anim_binding_bake(asset, source),
          "binding declarations serialize into typed MANM sections");
    BindingBake decoded;
    CHECK(matter::animation::get_anim_binding_bake(asset, decoded),
          "typed rigid and attachment declarations decode");
    CHECK(decoded.rigid_segments.size() == 1 && decoded.rigid_segments[0].name == "armor" &&
          decoded.rigid_segments[0].joint == 1 &&
          decoded.rigid_segments[0].geometry.size() == 1 &&
          decoded.rigid_segments[0].geometry[0].op_begin == 3 &&
          decoded.rigid_segments[0].geometry[0].triangle_end == 13 &&
          decoded.rigid_segments[0].bind_offset.translation.x == 2.0f,
          "rigid declaration retains joint, bind offset, and owned geometry range");
    CHECK(decoded.rigid_segments[0].lod_geometry.size() == 1 &&
              decoded.rigid_segments[0].lod_geometry[0].blas_slot == 1 &&
              decoded.rigid_segments[0].lod_geometry[0].triangle_count == 2,
          "rigid declaration persists its exact finalized LOD owner stream");
    CHECK(decoded.attachments.size() == 1 && decoded.attachments[0].name == "tool" &&
          decoded.attachments[0].target == "arm" &&
          decoded.attachments[0].target_kind == matter::animation::AttachmentTargetKind::Joint &&
          decoded.attachments[0].child_hash == attachment.child_hash &&
          decoded.attachments[0].local.translation.z == 7.0f,
          "attachment retains target kind, resolved child hash, and local transform");
    for (auto& section : asset.sections)
        if (section.kind == matter::animation::AnimSectionKind::RigidSegments)
            section.bytes.pop_back();
    CHECK(!matter::animation::get_anim_binding_bake(asset, decoded),
          "truncated typed rigid section fails closed");
}

void test_rigid_only_binding_needs_no_skin_payload() {
    BindingBake source;
    matter::Mat4f identity{}; identity.m[0]=identity.m[5]=identity.m[10]=identity.m[15]=1.0f;
    source.inverse_bind_matrices = {identity, identity};
    matter::animation::RigidSegmentBake rigid;
    rigid.name="separate"; rigid.joint=1; rigid.geometry.push_back({0, 2, 0, 0});
    source.rigid_segments.push_back(rigid);
    matter::animation::AnimAsset asset;
    asset.sections = {
        {matter::animation::AnimSectionKind::RigSchema, {1}},
        {matter::animation::AnimSectionKind::InputTargetSchemas, {2}},
        {matter::animation::AnimSectionKind::GraphControllerBytecode, {3}},
        {matter::animation::AnimSectionKind::OzzSkeleton, {7}},
        {matter::animation::AnimSectionKind::OzzClips, {8}},
    };
    CHECK(matter::animation::set_anim_binding_bake(asset, source),
          "a rigid-only segmented model serializes without synthetic skin weights");
    BindingBake decoded;
    CHECK(matter::animation::get_anim_binding_bake(asset, decoded) && decoded.lods.empty() &&
          decoded.rigid_segments.size()==1 && decoded.rigid_segments[0].joint==1,
          "rigid-only MANM payload retains its segment owner without a skin LOD");
}
}

int main() {
    test_signature_is_stable_and_includes_indices();
    test_weights_are_quantized_and_bind_pose_safe();
    test_skin_binding_uses_only_the_selected_segments();
    test_primary_claims_and_animated_children_fail_closed();
    test_duplicate_segment_claim_fails_closed();
    test_malformed_rig_hierarchy_fails_before_parent_indexing();
    test_binding_payload_retains_lods_matrices_and_cluster_ranges();
    test_binding_payload_requires_complete_cluster_bounds();
    test_binding_payload_retains_rigid_segments_and_attachments();
    test_rigid_only_binding_needs_no_skin_payload();
    return check_summary();
}
