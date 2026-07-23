#include "check.h"
#include "script_host.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

script_host::BakeResult bake(const char* body, script_host::ScriptHost& host) {
    const std::string source = std::string("class RigPart extends Part { build(p) { ") + body + " } }";
    return host.bake_source(source, "{}", {});
}

void test_stateful_rig_and_mirror_are_captured() {
    script_host::ScriptHost host;
    const auto result = bake(
        "this.beginRig(); this.radius(.5); this.root('root',[0,0,0]);"
        "this.push(); this.radius(.25); this.bone('leftArm',[1,0,0],[0,.70710678,0,.70710678]);"
        "this.socket('leftGrip',{position:[.2,0,0]}); this.bone('leftHand',[.5,0,0]); this.pop();"
        "this.mirrorBranch('leftArm','rightArm',{axis:'x',rename:{from:'left',to:'right'}}); this.endRig();",
        host);
    CHECK(result.error.ok, "stateful rig source bakes");
    const auto& rig = host.last_animation_rig();
    CHECK(rig.has_value(), "endRig exposes canonical rig to ScriptHost");
    if (!rig) return;
    CHECK(rig->rig.joints.size() == 5, "root, source branch, and mirrored branch captured");
    CHECK(rig->rig.joints[0].name == "root", "root is first canonical joint");
    CHECK(rig->rig.joints[3].name == "rightArm" && rig->rig.joints[4].name == "rightHand", "mirror renames full descendant subtree");
    CHECK(std::fabs(rig->rig.joints[3].local.translation.x + 1.0f) < 1e-5f, "mirror reflects local translation");
    CHECK(rig->rig.joints[3].local.rotation.w >= 0.0f, "mirrored quaternion has canonical sign");
    const auto& q = rig->rig.joints[3].local.rotation;
    CHECK(std::fabs(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w - 1.0f) < 1e-5f, "mirrored rotation remains proper and normalized");
    CHECK(rig->rig.joints[3].radius == .25f, "radius is captured by next joint");
}

void test_rig_state_errors_are_fail_closed() {
    struct Case { const char* body; const char* needle; };
    const Case cases[] = {
        {"this.root('root');", "outside an open rig"},
        {"this.beginRig(); this.root('root'); this.root('other');", "multiple roots"},
        {"this.beginRig(); this.root('root'); this.bone('root',[1,0,0]);", "duplicate joint"},
        {"this.beginRig(); this.root('root'); this.pop();", "without matching push"},
        {"this.beginRig(); this.root('root'); this.push(); this.endRig();", "stack left unbalanced"},
        {"this.beginRig(); this.root('root'); this.bone('bad',[1/0,0,0]);", "finite"},
        {"this.beginRig(); this.root('root'); this.radius(0);", "positive"},
    };
    for (const Case& c : cases) {
        script_host::ScriptHost host;
        const auto result = bake(c.body, host);
        CHECK(!result.error.ok, c.needle);
        CHECK(result.error.message.find(c.needle) != std::string::npos, c.needle);
    }
}

void test_reviewed_session_handle_and_source_contracts() {
    script_host::ScriptHost host;
    auto rig_in_voxels = bake("this.beginVoxels(.1); this.beginRig('blocked');", host);
    CHECK(!rig_in_voxels.error.ok && rig_in_voxels.error.message.find("open authoring session") != std::string::npos,
          "beginRig rejects open voxel session");
    script_host::ScriptHost host2;
    auto voxels_in_rig = bake("this.beginRig('r'); this.beginVoxels(.1);", host2);
    CHECK(!voxels_in_rig.error.ok && voxels_in_rig.error.message.find("open rig") != std::string::npos,
          "voxel session rejects open rig");
    script_host::ScriptHost host_shape;
    auto shape_in_rig = bake("this.beginRig('r'); this.beginShape(SHAPE.triangles);", host_shape);
    CHECK(!shape_in_rig.error.ok && shape_in_rig.error.message.find("open rig") != std::string::npos,
          "shape session rejects open rig");
    script_host::ScriptHost host_modifier;
    auto modifier_in_rig = bake("this.beginRig('r'); this.beginModifier();", host_modifier);
    CHECK(!modifier_in_rig.error.ok && modifier_in_rig.error.message.find("open rig") != std::string::npos,
          "modifier session rejects open rig");
    script_host::ScriptHost host_polygon;
    auto rig_in_polygon = bake("this.beginShape(SHAPE.polygon); this.beginRig('r');", host_polygon);
    CHECK(!rig_in_polygon.error.ok && rig_in_polygon.error.message.find("open authoring session") != std::string::npos,
          "beginRig rejects an open polygon");
    script_host::ScriptHost host3;
    auto second = bake("const a=this.beginRig('named'); this.root('root'); this.endRig(); const b=this.beginRig('again');", host3);
    CHECK(!second.error.ok && second.error.message.find("only one rig") != std::string::npos,
          "a second rig is rejected after endRig");
    script_host::ScriptHost host_handle;
    auto handle = bake("const a=this.beginRig('named'); if(a!==1) throw Error('unstable'); this.root('root'); this.endRig();", host_handle);
    CHECK(handle.error.ok, "beginRig returns the stable opaque bake handle");
    script_host::ScriptHost host4;
    auto source = bake("this.beginRig('named');\nthis.root('root');\nthis.atJoint('missing');", host4);
    CHECK(!source.error.ok && source.error.source_location.find(":3:") != std::string::npos,
          "rig error records the actual DSL call line");
}

} // namespace

int main() {
    test_stateful_rig_and_mirror_are_captured();
    test_rig_state_errors_are_fail_closed(); test_reviewed_session_handle_and_source_contracts();
    if (g_failures) { std::printf("animation_dsl_rig_tests: %d failure(s)\n", g_failures); return 1; }
    std::printf("animation_dsl_rig_tests: all tests passed\n");
    return 0;
}
