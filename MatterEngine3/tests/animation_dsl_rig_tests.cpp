#include "check.h"
#include "dsl_state.h"
#include "script_host.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

using matter::AnimationTransform;
using matter::Quaternion;

script_host::BakeResult bake(const char* body, script_host::ScriptHost& host) {
    const std::string source = std::string("class RigPart extends Part { build(p) {\n") + body + "\n} }";
    return host.bake_source(source, "{}", {});
}

bool close(float a, float b) { return std::fabs(a - b) < 1e-5f; }
bool nonzero_location(const script_host::BakeResult& result, const char* object, int line) {
    const std::string needle = std::string("<part>:") + std::to_string(line) + ":";
    return !result.error.source_location.empty() && result.error.source_location.find(needle) != std::string::npos &&
           result.error.source_location.find(std::string("(") + object + ")") != std::string::npos &&
           result.error.code == "rig-dsl";
}

void rotation_matrix(const Quaternion& q, float out[3][3]) {
    const float x=q.x, y=q.y, z=q.z, w=q.w;
    out[0][0]=1-2*(y*y+z*z); out[0][1]=2*(x*y-z*w);   out[0][2]=2*(x*z+y*w);
    out[1][0]=2*(x*y+z*w);   out[1][1]=1-2*(x*x+z*z); out[1][2]=2*(y*z-x*w);
    out[2][0]=2*(x*z-y*w);   out[2][1]=2*(y*z+x*w);   out[2][2]=1-2*(x*x+y*y);
}
float determinant(const float m[3][3]) {
    return m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])-
           m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])+
           m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
}
void check_mirrored_frame(const matter::animation::CanonicalJoint& joint, const Quaternion& source, int axis, const char* label) {
    float expected[3][3], actual[3][3]; rotation_matrix(source, expected); rotation_matrix(joint.local.rotation, actual);
    const float s[3] = {axis == 0 ? -1.0f : 1.0f, axis == 1 ? -1.0f : 1.0f, axis == 2 ? -1.0f : 1.0f};
    for (int row=0; row<3; ++row) for (int col=0; col<3; ++col)
        CHECK(close(actual[row][col], s[row] * expected[row][col] * s[col]), label);
    const auto& q = joint.local.rotation;
    CHECK(close(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w, 1.0f), "mirrored quaternion is normalized");
    CHECK(q.w >= 0.0f, "mirrored quaternion has canonical sign");
    CHECK(close(determinant(actual), 1.0f), "mirrored rotation remains a proper oriented frame");
    CHECK(joint.local.scale.x > 0 && joint.local.scale.y > 0 && joint.local.scale.z > 0 && joint.radius > 0,
          "mirrored scale and radius remain positive");
}

void test_mirror_captures_sockets_and_exact_srs_frames() {
    struct Axis { const char* name; int index; Quaternion rotation; };
    const Axis axes[] = {
        {"x", 0, {0.31f, 0.17f, -0.22f, 0.91f}}, {"y", 1, {-0.19f, 0.37f, 0.28f, 0.86f}}, {"z", 2, {0.23f, -0.34f, 0.18f, 0.88f}},
    };
    for (const Axis& axis : axes) {
        script_host::ScriptHost host;
        const std::string body =
            "this.beginRig('r'); this.radius(.5); this.root('root',[0,0,0]); this.push(); this.radius(.25); "
            "this.bone('leftArm',[1,2,3],[" + std::to_string(axis.rotation.x) + "," + std::to_string(axis.rotation.y) + "," + std::to_string(axis.rotation.z) + "," + std::to_string(axis.rotation.w) + "]); "
            "this.socket('leftGrip',{position:[.2,.3,.4],rotation:[.11,.22,.33,.88],scale:[2,3,4]}); this.bone('leftHand',[.5,0,0]); this.pop(); "
            "this.mirrorBranch('leftArm','rightArm',{axis:'" + axis.name + "',rename:{from:'left',to:'right'}}); this.endRig();";
        const auto result = bake(body.c_str(), host);
        CHECK(result.error.ok, "asymmetric mirrored rig bakes");
        const auto& rig = host.last_animation_rig(); CHECK(rig.has_value(), "canonical rig is retained"); if (!rig) continue;
        CHECK(rig->rig.joints.size() == 5, "complete mirrored subtree is captured");
        CHECK(rig->rig.sockets.size() == 2, "source and mirrored sockets are retained in canonical IR");
        CHECK(rig->rig.sockets[1].name == "rightGrip" && rig->rig.sockets[1].joint == 3, "socket is renamed and remapped");
        const auto& source = rig->rig.joints[1]; const auto& mirrored = rig->rig.joints[3];
        check_mirrored_frame(mirrored, source.local.rotation, axis.index, "mirror rotation is exactly S*R*S");
        const auto& ms = rig->rig.sockets[1];
        CHECK((axis.index != 0 || close(ms.local.translation.x, -.2f)) && (axis.index != 1 || close(ms.local.translation.y, -.3f)) && (axis.index != 2 || close(ms.local.translation.z, -.4f)),
              "socket translation is reflected");
        CHECK(ms.local.scale.x > 0 && ms.local.scale.y > 0 && ms.local.scale.z > 0, "socket scale magnitudes stay positive");
    }
}

void test_mirror_rename_map_and_atomic_preflight() {
    script_host::ScriptHost explicit_host;
    const auto explicit_result = bake(
        "this.beginRig('r'); this.root('root'); this.bone('leftArm',[1,0,0]); this.socket('leftGrip'); "
        "this.mirrorBranch('leftArm','rightArm',{axis:'x',map:{leftGrip:'rightGrip'}}); this.endRig();", explicit_host);
    CHECK(explicit_result.error.ok, "explicit complete name map succeeds");

    struct Case { const char* options; const char* needle; };
    const Case cases[] = {
        {"{axis:'x',map:{leftGrip:'rightGrip'}}", "incomplete"},
        {"{axis:'x',map:{leftHand:'rightHand',leftGrip:'rightGrip',extra:'x'}}", "incomplete"},
        {"{axis:'x',rename:{from:'none',to:'right'}}", "rename token"},
        {"{axis:'x',rename:{from:'left',to:'right'}}", "rename token"},
        {"{axis:'x',rename:{from:'left',to:'right'}}", "rename token"},
    };
    const char* names[] = {"leftArm", "leftArm", "leftArm", "leftleftArm", "leftArm"};
    for (size_t i=0; i<sizeof(cases)/sizeof(cases[0]); ++i) {
        script_host::ScriptHost host;
        const std::string socket = i == 4 ? "leftGripleft" : "leftGrip";
        const std::string child = i == 3 ? "leftleftHand" : "leftHand";
        const std::string body = std::string("this.beginRig('r'); this.root('root'); this.bone('") + names[i] + "',[1,0,0]); this.bone('" + child + "',[1,0,0]); this.atJoint('" + names[i] + "'); this.socket('" + socket + "'); this.mirrorBranch('" + names[i] + "','rightArm'," + cases[i].options + ");";
        const auto result=bake(body.c_str(),host); CHECK(!result.error.ok && result.error.message.find(cases[i].needle)!=std::string::npos, "bad mirror map/token fails closed");
    }
    const char* collisions[] = {
        "this.beginRig('r'); this.root('root'); this.bone('leftArm',[1,0,0]); this.bone('leftHand',[1,0,0]); this.atJoint('root'); this.bone('rightHand',[1,0,0]); this.atJoint('root'); this.mirrorBranch('leftArm','rightArm',{axis:'x',rename:{from:'left',to:'right'}});",
        "this.beginRig('r'); this.root('root'); this.socket('rightArm'); this.bone('leftArm',[1,0,0]); this.mirrorBranch('leftArm','rightArm',{axis:'x',rename:{from:'left',to:'right'}});",
        "this.beginRig('r'); this.root('root'); this.bone('leftArm',[1,0,0]); this.socket('leftGrip'); this.mirrorBranch('leftArm','rightArm',{axis:'x',map:{leftGrip:'rightArm'}});",
    };
    for (const char* body : collisions) { script_host::ScriptHost host; const auto result=bake(body,host); CHECK(!result.error.ok && result.error.message.find("collision")!=std::string::npos, "joint/socket namespace collision is rejected"); }

    dsl::DslState state; state.begin_rig("r"); AnimationTransform identity{}; state.rig_root("root", identity); state.rig_bone("leftArm", identity); state.rig_socket("leftGrip", identity);
    const auto before = state.rig_debug_state();
    state.rig_mirror_branch("leftArm", "rightArm", 0, "", "", {{"leftGrip", "leftArm"}});
    const auto after = state.rig_debug_state();
    CHECK(before.joint_count == after.joint_count && before.socket_count == after.socket_count && before.current_parent == after.current_parent && close(before.radius, after.radius),
          "failed mirror restores exact pre-mirror counts and cursor");
    state.rig_at_joint("root"); state.rig_bone("recovery", identity);
    CHECK(state.rig_debug_state().joint_count == before.joint_count + 1, "valid authoring continues from the preserved cursor after failed mirror");
}

void test_rig_structure_lifecycle_and_validation() {
    script_host::ScriptHost host;
    const auto happy=bake("this.beginRig('r'); this.radius(2); this.root('root',[1,2,3]); this.push(); this.radius(.5); this.bone('child',[1,0,0]); this.pop(); this.bone('sibling',[0,1,0]); this.atJoint('child'); this.bone('grand',[0,0,1]); this.endRig();",host);
    CHECK(happy.error.ok, "root/local parent and atJoint happy branch bake");
    const auto& rig=host.last_animation_rig(); CHECK(rig && rig->rig.joints.size()==4, "all authored joints are canonicalized");
    if (rig) { CHECK(rig->rig.joints[1].parent == 0 && rig->rig.joints[2].parent == 1 && rig->rig.joints[3].parent == 0, "push/pop and atJoint establish parents"); CHECK(close(rig->rig.joints[1].radius,.5f) && close(rig->rig.joints[3].radius,2.0f), "pop restores radius cursor"); }
    struct Case { const char* body; const char* needle; };
    const Case cases[] = {
        {"this.beginRig('r'); this.endRig();", "root"}, {"this.beginRig('r'); this.root('root'); this.push(); this.endRig();", "unbalanced"},
        {"this.beginRig('r'); this.root('root'); this.endRig(); this.endRig();", "outside"}, {"this.beginRig('r'); this.root('root'); this.endRig(); this.beginRig('again');", "only one rig"},
        {"this.beginRig('r'); this.root('root'); this.atJoint('missing');", "unknown"}, {"this.beginRig('r'); this.root('root',[Infinity,0,0]);", "finite"},
        {"this.beginRig('r'); this.root('root',[0,0,0],[0,0,0,0]);", "positive"}, {"this.beginRig('r'); this.root('root'); this.bone('bad',[NaN,0,0]);", "finite"},
        {"this.beginRig('r'); this.root('root'); this.socket('bad',{rotation:[NaN,0,0,1]});", "finite"}, {"this.beginRig('r'); this.root('root'); this.socket('bad',{scale:[0,1,1]});", "positive"},
        {"this.beginRig('r'); this.root('root'); this.socket('bad',{scale:[Infinity,1,1]});", "finite"}, {"this.beginRig('r'); this.radius(Infinity);", "finite"},
    };
    for (const Case& c : cases) { script_host::ScriptHost h; const auto r=bake(c.body,h); CHECK(!r.error.ok && r.error.message.find(c.needle)!=std::string::npos, c.needle); }
    const char* verbs[] = {"this.root('root');", "this.bone('bone',[1,0,0]);", "this.push();", "this.pop();", "this.atJoint('root');", "this.radius(1);", "this.socket('socket');", "this.mirrorBranch('a','b',{axis:'x',map:{}});", "this.endRig();"};
    for (const char* verb : verbs) { script_host::ScriptHost h; const auto r=bake(verb,h); CHECK(!r.error.ok && r.error.message.find("outside an open rig")!=std::string::npos, "every rig verb rejects calls outside a session"); }
}

void test_rig_and_geometry_sessions_are_mutually_exclusive() {
    const char* open_geometry[] = {
        "this.beginVoxels(.1); this.beginRig('r');", "this.beginShape(SHAPE.triangles); this.beginRig('r');", "this.beginShape(SHAPE.polygon); this.beginRig('r');",
        "this.beginShape(SHAPE.polygon); this.vertex(0,0,0); this.vertex(1,0,0); this.vertex(0,1,0); this.beginContour(); this.beginRig('r');", "this.beginModifier(); this.beginRig('r');",
    };
    const char* rig_geometry[] = {"this.beginRig('r'); this.beginVoxels(.1);", "this.beginRig('r'); this.beginShape(SHAPE.triangles);", "this.beginRig('r'); this.beginShape(SHAPE.polygon);", "this.beginRig('r'); this.beginContour();", "this.beginRig('r'); this.beginModifier();"};
    for (const char* body : open_geometry) { script_host::ScriptHost h; const auto r=bake(body,h); CHECK(!r.error.ok && r.error.message.find("open authoring session")!=std::string::npos, "rig rejects every open geometry session"); }
    for (const char* body : rig_geometry) { script_host::ScriptHost h; const auto r=bake(body,h); CHECK(!r.error.ok && r.error.message.find("open rig")!=std::string::npos, "every geometry session rejects an open rig"); }
}

void test_source_aware_failures_and_mirror_spans() {
    struct Case { const char* body; const char* object; int line; };
    const Case cases[] = {
        {"this.beginRig('r');\nthis.root('root');\nthis.bone('plain',[1,0,0]);\nthis.bone('plainChild',[1,0,0]);\nthis.mirrorBranch('plain','right',{axis:'x',rename:{from:'left',to:'right'}});", "mirrorBranch", 6},
        {"this.beginRig('r');\nthis.root('root');\nthis.bone('leftArm',[1,0,0]);\nthis.bone('leftHand',[1,0,0]);\nthis.mirrorBranch('leftArm','right',{axis:'x',map:{}});", "mirrorBranch", 6},
        {"this.beginRig('r');\nthis.root('root');\nthis.radius(0);", "radius", 4},
    };
    for (const Case& c : cases) { script_host::ScriptHost h; const auto r=bake(c.body,h); CHECK(!r.error.ok && nonzero_location(r,c.object,c.line), "multi-line failure reports exact module/line/column/object/code"); }
    script_host::ScriptHost host;
    const auto result=bake("this.beginRig('r');\nthis.root('root');\nthis.bone('leftArm',[1,0,0]);\nthis.socket('leftGrip');\nthis.mirrorBranch('leftArm','rightArm',{axis:'x',rename:{from:'left',to:'right'}});\nthis.endRig();",host);
    CHECK(result.error.ok, "source span mirror fixture bakes"); const auto& rig=host.last_animation_rig(); if (!rig) return;
    CHECK(rig->rig.joints[2].source.object == "mirrorBranch" && rig->rig.joints[2].source.line == 6 && rig->rig.joints[2].source.module == "<part>", "cloned joint source span is the mirror call");
    CHECK(rig->rig.sockets[1].source.object == "mirrorBranch" && rig->rig.sockets[1].source.line == 6 && rig->rig.sockets[1].source.module == "<part>", "cloned socket source span is the mirror call");
}

void test_stable_named_handle_and_one_rig_rule() {
    script_host::ScriptHost host;
    const auto handle=bake("const a=this.beginRig('named'); if(a!==1) throw Error('unstable'); this.root('root'); this.endRig();",host);
    CHECK(handle.error.ok, "beginRig returns the stable opaque bake handle");
    script_host::ScriptHost second; const auto result=bake("this.beginRig('a'); this.root('root'); this.endRig(); this.beginRig('b');",second);
    CHECK(!result.error.ok && result.error.message.find("only one rig") != std::string::npos, "one rig per bake remains enforced");
}

void test_rig_bindings_reject_missing_null_and_coerced_arguments() {
    const char* optional_begin[] = {
        "const h=this.beginRig(); if(h!==1) throw Error('handle'); this.root('root'); this.endRig();",
        "this.beginRig(undefined); this.root('root'); this.endRig();",
        "this.beginRig(null); this.root('root'); this.endRig();",
    };
    for (const char* body : optional_begin) { script_host::ScriptHost host; const auto result=bake(body,host); CHECK(result.error.ok, "beginRig accepts an omitted, undefined, or null optional name"); }
    const char* invalid_begin[] = {"this.beginRig(7);", "this.beginRig({});", "this.beginRig(Symbol('rig'));"};
    for (const char* body : invalid_begin) { script_host::ScriptHost host; const auto result=bake(body,host); CHECK(!result.error.ok && result.error.message.find("beginRig name must be a string") != std::string::npos, "beginRig never coerces a non-string name"); }

    struct NameCase { const char* expression; };
    const NameCase bad_names[] = {{"undefined"}, {"null"}, {"7"}, {"{}"}, {"Symbol('name')"}};
    for (const NameCase& c : bad_names) {
        script_host::ScriptHost root; const std::string root_body="this.beginRig('r'); this.root(" + std::string(c.expression) + ");"; const auto root_result=bake(root_body.c_str(),root);
        CHECK(!root_result.error.ok && root_result.error.message.find("root requires a non-empty string name") != std::string::npos, "root never coerces a required name");
        script_host::ScriptHost bone; const std::string bone_body="this.beginRig('r'); this.root('root'); this.bone(" + std::string(c.expression) + ",[1,0,0]);"; const auto bone_result=bake(bone_body.c_str(),bone);
        CHECK(!bone_result.error.ok && bone_result.error.message.find("bone requires a non-empty string name") != std::string::npos, "bone never coerces a required name");
        script_host::ScriptHost at_joint; const std::string at_joint_body="this.beginRig('r'); this.root('root'); this.atJoint(" + std::string(c.expression) + ");"; const auto at_joint_result=bake(at_joint_body.c_str(),at_joint);
        CHECK(!at_joint_result.error.ok && at_joint_result.error.message.find("atJoint requires a non-empty string name") != std::string::npos, "atJoint never coerces a required name");
        script_host::ScriptHost socket; const std::string socket_body="this.beginRig('r'); this.root('root'); this.socket(" + std::string(c.expression) + ");"; const auto socket_result=bake(socket_body.c_str(),socket);
        CHECK(!socket_result.error.ok && socket_result.error.message.find("socket requires a non-empty string name") != std::string::npos, "socket never coerces a required name");
    }
    const char* bad_radius[] = {"this.beginRig('r'); this.radius();", "this.beginRig('r'); this.radius(null);", "this.beginRig('r'); this.radius('1');", "this.beginRig('r'); this.radius(NaN);", "this.beginRig('r'); this.radius(Infinity);"};
    for (const char* body : bad_radius) { script_host::ScriptHost host; const auto result=bake(body,host); CHECK(!result.error.ok && result.error.message.find("radius requires a finite numeric value") != std::string::npos, "radius requires a present finite number without coercion"); }
    const char* bad_mirror[] = {
        "this.beginRig('r'); this.root('root'); this.mirrorBranch(undefined,'right',{axis:'x'});",
        "this.beginRig('r'); this.root('root'); this.mirrorBranch(7,'right',{axis:'x'});",
        "this.beginRig('r'); this.root('root'); this.mirrorBranch('root',null,{axis:'x'});",
        "this.beginRig('r'); this.root('root'); this.mirrorBranch('root','right',null);",
        "this.beginRig('r'); this.root('root'); this.mirrorBranch('root','right',{axis:7});",
        "this.beginRig('r'); this.root('root'); this.mirrorBranch('root','right',{axis:'x',rename:{from:7,to:'right'}});",
        "this.beginRig('r'); this.root('root'); this.bone('leftArm',[1,0,0]); this.mirrorBranch('leftArm','rightArm',{axis:'x',map:{leftArm:{}}});",
    };
    for (const char* body : bad_mirror) { script_host::ScriptHost host; const auto result=bake(body,host); CHECK(!result.error.ok && result.error.message.find("mirrorBranch requires") != std::string::npos, "mirrorBranch rejects malformed names/options without coercion"); }
}

} // namespace

int main() {
    test_mirror_captures_sockets_and_exact_srs_frames();
    test_mirror_rename_map_and_atomic_preflight();
    test_rig_structure_lifecycle_and_validation();
    test_rig_and_geometry_sessions_are_mutually_exclusive();
    test_source_aware_failures_and_mirror_spans();
    test_stable_named_handle_and_one_rig_rule();
    test_rig_bindings_reject_missing_null_and_coerced_arguments();
    if (g_failures) { std::printf("animation_dsl_rig_tests: %d failure(s)\n", g_failures); return 1; }
    std::printf("animation_dsl_rig_tests: all tests passed\n");
    return 0;
}
