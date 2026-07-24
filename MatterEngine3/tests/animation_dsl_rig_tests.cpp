#include "check.h"
#include "dsl_state.h"
#include "indexed_part_geometry.h"
#include "part_asset_v2.h"
#include "script_host.h"
#include "triangle_emit.hpp"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

void test_rig_transform_arrays_reject_coerced_elements_and_bad_maps() {
    struct Case { const char* body; const char* needle; };
    const Case transforms[] = {
        {"this.beginRig('r'); this.root('root',['0',0,0]);", "root requires finite position and rotation arrays"},
        {"this.beginRig('r'); this.root('root',[0,0,0],[0,false,0,1]);", "root requires finite position and rotation arrays"},
        {"this.beginRig('r'); this.root('root'); this.bone('child',[0,null,0]);", "bone requires finite endpoint and rotation arrays"},
        {"this.beginRig('r'); this.root('root'); this.bone('child',[0,0,0],[0,0,{},1]);", "bone requires finite endpoint and rotation arrays"},
        {"this.beginRig('r'); this.root('root'); this.socket('s',{position:[0,0,'0']});", "socket requires a finite transform object"},
        {"this.beginRig('r'); this.root('root'); this.socket('s',{rotation:[0,0,Symbol('q'),1]});", "socket requires a finite transform object"},
        {"this.beginRig('r'); this.root('root'); this.socket('s',{scale:[1,true,1]});", "socket requires a finite transform object"},
        {"this.beginRig('r'); this.root('root',[NaN,0,0]);", "root requires finite position and rotation arrays"},
    };
    for (const Case& c : transforms) { script_host::ScriptHost host; const auto result=bake(c.body,host); CHECK(!result.error.ok && result.error.message.find(c.needle) != std::string::npos, "transform arrays reject coercive/non-finite elements"); }
    script_host::ScriptHost map_host;
    const auto map_result=bake("this.beginRig('r'); this.root('root'); this.bone('leftArm',[1,0,0]); this.mirrorBranch('leftArm','rightArm',{axis:'x',map:{a:'ok',b:'ok',c:'ok',d:'ok',e:'ok',f:'ok',g:'ok',h:'ok',i:'ok',j:'ok',k:'ok',z:{}}});",map_host);
    CHECK(!map_result.error.ok && map_result.error.message.find("object map with string values") != std::string::npos, "multi-property invalid map fails cleanly without coercion");
}

void test_rig_binding_authoring_captures_skin_segments_and_attachments() {
    script_host::ScriptHost host;
    const std::string source =
        "class BindingPart extends Part { build(p) {\n"
        "this.beginRig('r'); this.radius(.5); this.root('root'); this.bone('arm',[2,0,0]); this.socket('grip'); this.endRig();\n"
        "this.skin('body',{joints:['arm'],radiusScale:1.25,falloffScale:1.5,generate:true,voxelSize:.25});\n"
        "this.segments('armor',{joints:['arm'],decorative:true,offset:{position:[4,5,6]}});\n"
        "this.bind('armor',()=>{this.beginShape(SHAPE.triangles);this.vertex(0,0,0);this.vertex(1,0,0);this.vertex(0,1,0);this.endShape();});\n"
        "this.attach('tool','grip','Tool',{position:[1,2,3]});\n"
        "this.attach('jointTool','arm','Tool',{position:[4,5,6]});\n"
        "} }";
    const uint64_t child_hash = 0x1234u;
    const std::string child_module = "Tool";
    const auto result = host.bake_source(source, "{}", {}, &child_hash, 1, &child_module);
    CHECK(result.error.ok, "rig binding declarations bake with a resolved attachment child");
    const auto& build = host.last_animation_build();
    CHECK(build && build->skin_bindings.size() == 1 && build->skin_bindings[0].name == "body" &&
          build->skin_bindings[0].joints.size() == 1 && build->skin_bindings[0].joints[0] == "arm" &&
          close(build->skin_bindings[0].radius_scale, 1.25f) && close(build->skin_bindings[0].falloff, 1.5f) &&
          close(build->skin_bindings[0].voxel_size, .25f) && build->skin_bindings[0].generated,
          "skin records its selected segment and canonical envelope options in authored animation IR");
    CHECK(build && build->rigid_bindings.size() == 1 && build->rigid_bindings[0].joint == "arm",
          "segments emits one rigid binding record per selected segment");
    CHECK(build && build->rigid_bindings.size() == 1 && close(build->rigid_bindings[0].local.translation.x, 4.0f) &&
          build->rigid_bindings[0].geometry.size() == 1 && build->rigid_bindings[0].geometry[0].triangle_end == 1,
          "segments retains its authored bind offset and selected geometry range");
    CHECK(build && build->attachments.size() == 2 && build->attachments[0].name == "tool" &&
          build->attachments[0].socket == "grip" && build->attachments[0].child_hash == child_hash,
          "attachment captures its socket and declared child hash");
    CHECK(build && build->attachments.size() == 2 && build->attachments[1].name == "jointTool" &&
          build->attachments[1].joint == "arm" && build->attachments[1].socket.empty() &&
          build->attachments[1].child_hash == child_hash && close(build->attachments[1].local.translation.z, 6.0f),
          "attachment accepts a direct joint target and retains its resolved child and local transform");
    const auto& canonical = host.last_animation_rig();
    CHECK(canonical && canonical->authored_state.find("body") != std::string::npos &&
          canonical->authored_state.find("armor") != std::string::npos &&
          canonical->authored_state.find("tool") != std::string::npos,
          "canonical animation state is refreshed after binding authoring");
    CHECK(host.last_buffer().ops.size() >= 3, "generated skin contributes tapered segment and endpoint voxel geometry");
}

void test_rig_binding_authoring_rejects_malformed_or_overlapping_declarations() {
    struct Case { const char* body; const char* needle; };
    const Case cases[] = {
        {"this.beginRig('r');this.root('root');this.bone('arm',[1,0,0]);this.endRig();this.skin('a',{joints:['missing']});", "unknown joint"},
        {"this.beginRig('r');this.root('root');this.bone('arm',[1,0,0]);this.endRig();this.skin('a',{joints:['arm']});this.segments('b',{joints:['arm']});", "already claimed"},
        {"this.beginRig('r');this.root('root');this.bone('arm',[1,0,0]);this.endRig();this.skin('a',{joints:['arm','arm']});", "duplicate"},
        {"this.beginRig('r');this.root('root');this.bone('arm',[1,0,0]);this.endRig();this.skin('a',{falloff:0});", "positive"},
        {"this.beginRig('r');this.root('root');this.bone('arm',[1,0,0]);this.endRig();this.skin('a',{falloff:1,falloffScale:1});", "falloffScale"},
        {"this.beginRig('r');this.root('root');this.socket('grip');this.endRig();this.attach('tool','grip','Missing');", "unresolved child"},
    };
    for (const auto& c : cases) {
        script_host::ScriptHost host;
        const auto result = bake(c.body, host);
        CHECK(!result.error.ok && result.error.message.find(c.needle) != std::string::npos,
              "malformed binding declarations fail closed");
    }
}

void test_scoped_skin_binding_captures_only_its_authored_ranges() {
    script_host::ScriptHost host;
    const auto result = bake(
        "this.beginVoxels(.1); this.sphere([9,0,0],1); this.endVoxels(); "
        "this.beginRig('r'); this.root('root'); this.bone('arm',[1,0,0]); this.endRig(); "
        "this.skin('body',{joints:['arm'],generate:false}); "
        "this.bind('body',()=>{ this.beginVoxels(.1); this.sphere([0,0,0],1); this.endVoxels(); "
        "this.beginShape(SHAPE.triangles); this.vertex(0,0,0); this.vertex(1,0,0); this.vertex(0,1,0); this.endShape(); });", host);
    CHECK(result.error.ok, "scoped bind emits ordinary voxel and indexed geometry");
    const auto& build = host.last_animation_build();
    CHECK(build && build->skin_bindings.size() == 1 && build->skin_bindings[0].geometry.size() == 1,
          "scoped bind retains exactly one authored geometry selection in animation IR");
    if (build && !build->skin_bindings.empty() && !build->skin_bindings[0].geometry.empty()) {
        const auto& range = build->skin_bindings[0].geometry[0];
        CHECK(range.op_begin == 1 && range.op_end == 2,
              "scoped bind excludes preceding static voxel operations");
        CHECK(range.triangle_begin == 0 && range.triangle_end == 1,
              "scoped bind records the exact direct-triangle range");
    }

    const auto unknown = bake(
        "this.beginRig('r'); this.root('root'); this.bone('arm',[1,0,0]); this.endRig(); "
        "this.skin('body',{joints:['arm'],generate:false}); this.bind('missing',()=>{});", host);
    CHECK(!unknown.error.ok && unknown.error.message.find("unknown skin or rigid binding") != std::string::npos,
          "bind rejects a name that is not a declared deformable skin or rigid binding");
    const auto nested = bake(
        "this.beginRig('r'); this.root('root'); this.bone('arm',[1,0,0]); this.endRig(); "
        "this.skin('body',{joints:['arm'],generate:false}); this.bind('body',()=>this.bind('body',()=>{}));", host);
    CHECK(!nested.error.ok && nested.error.message.find("cannot nest") != std::string::npos,
          "bind scopes fail closed rather than silently merging nested selections");
}

void test_scoped_skin_binding_rejects_structural_animation_authoring() {
    const char* escaped_authoring[] = {
        "this.skin('legSkin',{joints:['leg']});",
        "this.segments('legArmor',{joints:['leg'],decorative:true});",
        "this.beginClip('nested',1,30);",
        "this.beginMotion('nested');",
    };
    for (const char* escaped : escaped_authoring) {
        script_host::ScriptHost blocked;
        const std::string body =
            std::string("this.beginRig('r'); this.root('root'); this.bone('arm',[1,0,0]); this.atJoint('root'); this.bone('leg',[0,1,0]); this.endRig(); ") +
            "this.skin('body',{joints:['arm'],generate:false}); this.bind('body',()=>{ " + escaped + " });";
        const auto blocked_result = bake(body.c_str(), blocked);
        CHECK(!blocked_result.error.ok && blocked_result.error.message.find("bind scope") != std::string::npos,
              "bind callbacks reject every structural animation entry point before state can escape the scope");
    }

    script_host::ScriptHost host;
    const auto result = bake(
        "this.beginRig('r'); this.root('root'); this.bone('arm',[1,0,0]); this.atJoint('root'); this.bone('leg',[0,1,0]); this.endRig(); "
        "this.skin('body',{joints:['arm'],generate:false}); "
        "this.bind('body',()=>{ this.skin('legSkin',{joints:['leg']}); "
        "this.beginVoxels(.1); this.sphere([0,0,0],1); this.endVoxels(); });", host);
    CHECK(!result.error.ok && result.error.message.find("bind scope") != std::string::npos,
          "bind callbacks reject structural animation declarations before they can leak into a captured geometry range");
    const auto& build = host.last_animation_build();
    CHECK(!build || (build->skin_bindings.size() == 1 && build->skin_bindings[0].geometry.empty()),
          "a rejected structural callback does not publish a second binding or a captured geometry range");
    CHECK(host.last_buffer().ops.empty(),
          "a rejected structural callback rolls back later voxel geometry instead of leaking it into the failed bake buffer");
}

void test_binding_authoring_requires_owned_geometry_before_publish() {
    script_host::ScriptHost host;
    const auto unbound_skin = bake(
        "this.beginRig('r');this.root('root');this.bone('arm',[1,0,0]);this.endRig();"
        "this.skin('body',{joints:['arm'],generate:false});", host);
    CHECK(!unbound_skin.error.ok && unbound_skin.error.message.find("own geometry") != std::string::npos,
          "explicit skin fails at build end unless a bind scope owns geometry");

    const auto unbound_segments = bake(
        "this.beginRig('r');this.root('root');this.bone('arm',[1,0,0]);this.endRig();"
        "this.segments('armor',{joints:['arm']});", host);
    CHECK(!unbound_segments.error.ok && unbound_segments.error.message.find("own geometry") != std::string::npos,
          "rigid segments fail at build end unless a bind scope owns geometry");

    const auto generated_skin = bake(
        "this.beginRig('r');this.root('root');this.bone('arm',[1,0,0]);this.endRig();"
        "this.skin('body',{joints:['arm'],generate:true,voxelSize:.25});", host);
    CHECK(generated_skin.error.ok,
          "generated skin retains its implicit geometry range without a bind callback");
}

void test_attachment_rejects_child_with_committed_animation_link() {
    const std::filesystem::path root = "animation_dsl_rig_attachment_cache";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "parts", ec);
    const uint64_t child_hash = 0xabcdu;
    BLASManager child_blas;
    TLASManager child_tlas(1);
    const part_asset::PartAnimationLink link{1, 1, child_hash, 4, 5};
    CHECK(part_asset::save_v2((root / part_asset::cache_path_resolved(child_hash)).string(), child_blas,
                              child_tlas, nullptr, 0, {}, {}, link, child_hash),
          "committed animated-child fixture is written with ANLK");

    script_host::ScriptHost host;
    script_host::BakeOptions opts;
    opts.parts_dir = root.string();
    const std::string module = "Tool";
    const auto result = host.bake_source(
        "class Parent extends Part { build(p) { this.beginRig('r');this.root('root');this.socket('grip');this.endRig();this.attach('tool','grip','Tool'); } }",
        "{}", opts, &child_hash, 1, &module);
    CHECK(!result.error.ok && result.error.message.find("nested committed animation") != std::string::npos,
          "v1 attachments reject a resolved child with a committed ANLK animation link");
    {
        std::ofstream corrupt(root / part_asset::cache_path_resolved(child_hash), std::ios::binary | std::ios::trunc);
        corrupt << "corrupt";
    }
    const auto corrupt_result = host.bake_source(
        "class Parent extends Part { build(p) { this.beginRig('r');this.root('root');this.socket('grip');this.endRig();this.attach('tool','grip','Tool'); } }",
        "{}", opts, &child_hash, 1, &module);
    CHECK(!corrupt_result.error.ok && corrupt_result.error.message.find("invalid committed part artifact") != std::string::npos,
          "an existing child part that cannot be preflighted never falls through as a static attachment");
    std::filesystem::remove_all(root, ec);
}

void test_mirrored_bound_geometry_reverses_winding_and_preserves_static_winding() {
    AnimationTransform identity{};
    AnimationTransform child{}; child.translation = {1,0,0};
    dsl::DslState bound;
    bound.begin_rig("r"); bound.rig_root("root", identity); bound.rig_bone("arm", child); bound.end_rig();
    bound.rig_skin("body", {"arm"}, 1.0f, false, .1f);
    CHECK(!bound.has_error() && bound.begin_binding_scope("body"), "a completed skin binding opens a scoped selection");
    bound.scale(-1, 1, 1);
    bound.beginShape(0); bound.vertex(0,0,0); bound.vertex(1,0,0); bound.vertex(0,1,0); bound.endShape();
    CHECK(bound.end_binding_scope(), "a balanced mirrored scoped binding closes successfully");
    const auto* tris = bound.triangle_buffer();
    CHECK(tris && tris->triangles().size() == 1 && tris->tri_extra().size() == 1,
          "mirrored scoped binding emits one direct triangle");
    if (tris && !tris->triangles().empty()) {
        const Tri& t = tris->triangles()[0]; const TriEx& e = tris->tri_extra()[0];
        const float3 face = cross(t.vertex1 - t.vertex0, t.vertex2 - t.vertex0);
        CHECK(face.z > 0.0f && e.N0.z > 0.0f,
              "mirrored bound direct geometry has corrected outward indexed winding and normals");
        const auto indexed = viewer::build_indexed_part_geometry(&t, &e, 1);
        CHECK(indexed.indices.size() == 3 && indexed.vertices[indexed.indices[1]*3+1] > 0.0f,
              "indexed conversion consumes the corrected mirrored corner order");
    }
    const auto* authored = bound.authored_animation();
    CHECK(authored && authored->skin_bindings.size() == 1 && authored->skin_bindings[0].geometry.size() == 1 &&
          authored->skin_bindings[0].geometry[0].triangle_end == 1,
          "the mirrored geometry remains attached to its deformable binding selection");

    tri_emit::TriangleBuildBuffer static_geometry;
    mat4 identity_matrix{}; identity_matrix.cell[0]=identity_matrix.cell[5]=identity_matrix.cell[10]=identity_matrix.cell[15]=1.0f;
    static_geometry.beginShape(tri_emit::ShapeType::TRIANGLES, identity_matrix, 0);
    static_geometry.vertex(make_float3(0,0,0)); static_geometry.vertex(make_float3(1,0,0)); static_geometry.vertex(make_float3(0,1,0)); static_geometry.endShape();
    CHECK(static_geometry.triangles().size() == 1 &&
          static_geometry.triangles()[0].vertex1.x > 0.0f && static_geometry.tri_extra()[0].N0.z > 0.0f,
          "non-mirrored static direct geometry retains its legacy winding and normals");
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
    test_rig_transform_arrays_reject_coerced_elements_and_bad_maps();
    test_rig_binding_authoring_captures_skin_segments_and_attachments();
    test_rig_binding_authoring_rejects_malformed_or_overlapping_declarations();
    test_scoped_skin_binding_captures_only_its_authored_ranges();
    test_scoped_skin_binding_rejects_structural_animation_authoring();
    test_binding_authoring_requires_owned_geometry_before_publish();
    test_attachment_rejects_child_with_committed_animation_link();
    test_mirrored_bound_geometry_reverses_winding_and_preserves_static_winding();
    if (g_failures) { std::printf("animation_dsl_rig_tests: %d failure(s)\n", g_failures); return 1; }
    std::printf("animation_dsl_rig_tests: all tests passed\n");
    return 0;
}
