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

} // namespace

int main() {
    test_stateful_rig_and_mirror_are_captured();
    test_rig_state_errors_are_fail_closed();
    if (g_failures) { std::printf("animation_dsl_rig_tests: %d failure(s)\n", g_failures); return 1; }
    std::printf("animation_dsl_rig_tests: all tests passed\n");
    return 0;
}
