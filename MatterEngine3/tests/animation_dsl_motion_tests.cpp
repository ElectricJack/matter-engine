#include "check.h"
#include "animation/ozz_adapter.h"
#include "script_host.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
script_host::BakeResult bake(const char* body, script_host::ScriptHost& host) {
    return host.bake_source(std::string("class MotionPart extends Part { build(p) {\n") + body + "\n} }", "{}", {});
}
void test_generated_loop_and_controls() {
    script_host::ScriptHost host;
    const auto result=bake("this.beginRig('r'); this.root('root'); this.bone('mid',[1,0,0]); this.bone('tip',[1,0,0]); this.endRig(); const c=this.beginClip(1,'walk',{duration:1,sampleRate:2,loop:true}); this.generate(phase=>{this.at('mid'); this.rotateX(phase); this.at('tip'); this.translate(phase,0,0);}); this.marker(.5,'step'); this.endClip(); this.beginMotion('m'); this.input('speed',{type:'float',cadence:'fixed',default:0}); this.controller('gait','proceduralGait',{cadence:'fixed'}); this.target('foot',{start:'root',end:'tip',driver:{controller:'gait'},cadence:'fixed',pole:[0,0,1],soften:.5,twist:0}); this.clipNode('walkNode','walk'); this.nativeController('gaitNode','gait','walkNode'); this.output('out','gaitNode'); this.endMotion();",host);
    if(!result.error.ok) std::printf("motion error: %s (%s)\n", result.error.message.c_str(), result.error.code.c_str()); CHECK(result.error.ok,"generated motion script bakes");
    const auto& build=host.last_animation_build(); CHECK(build.has_value(),"authored animation build retained"); if(!build)return;
    CHECK(build->clips.size()==1,"one generated clip"); if(build->clips.empty())return;
    const auto& clip=build->clips[0]; CHECK(clip.loop,"loop flag retained"); CHECK(clip.markers.size()==1&&clip.markers[0].time==.5f,"normalized marker retained");
    CHECK(!build->ozz_skeleton_blob.empty()&&!clip.ozz_blob.empty(),"compiled Ozz skeleton and clip bytes retained");
    matter::animation::OzzSkeleton skeleton; matter::animation::OzzAnimation animation; matter::animation::Diagnostics diagnostics;
    CHECK(matter::animation::deserialize_skeleton(build->ozz_skeleton_blob.data(),build->ozz_skeleton_blob.size(),skeleton,diagnostics),"retained skeleton archive round-trips");
    CHECK(matter::animation::deserialize_animation(clip.ozz_blob.data(),clip.ozz_blob.size(),animation,diagnostics),"retained clip archive round-trips");
    std::vector<uint8_t> skeleton_bytes, animation_bytes;
    CHECK(matter::animation::serialize_skeleton(skeleton,skeleton_bytes)&&skeleton_bytes==build->ozz_skeleton_blob,"skeleton archive bytes are stable");
    CHECK(matter::animation::serialize_animation(animation,animation_bytes)&&animation_bytes==clip.ozz_blob,"clip archive bytes are stable");
    CHECK(!clip.tracks.empty()&&clip.tracks[0].keys.size()==3,"loop samples use ceil segments and closure");
    if(!clip.tracks.empty()&&clip.tracks[0].keys.size()==3) CHECK(std::memcmp(&clip.tracks[0].keys.front().value,&clip.tracks[0].keys.back().value,sizeof(matter::AnimationTransform))==0,"loop closure is byte exact");
    CHECK(build->inputs.size()==1&&build->inputs[0].default_value.type==matter::AnimationValueType::Number,"typed input schema retained"); CHECK(build->targets.size()==1&&build->targets[0].driver==matter::animation::TargetDriverKind::Controller,"controller target ownership retained");
}
void test_validation_rejects_bad_motion_graph() {
    script_host::ScriptHost host; const auto result=bake("this.beginRig('r'); this.root('root'); this.bone('mid',[1,0,0]); this.bone('tip',[1,0,0]); this.endRig(); this.beginMotion(); this.output('out'); this.endMotion();",host); if(result.error.ok) std::printf("bad graph unexpectedly succeeded\\n"); else std::printf("bad graph diagnostic: %s (%s)\\n",result.error.message.c_str(),result.error.code.c_str()); CHECK(!result.error.ok,"bad output graph fails closed");
}
void test_collinear_target_requires_pole() {
    script_host::ScriptHost host; const auto result=bake("this.beginRig('r'); this.root('root'); this.bone('mid',[1,0,0]); this.bone('tip',[1,0,0]); this.endRig(); this.beginClip('idle',{duration:1,sampleRate:1}); this.key('root',0,{}); this.endClip(); this.beginMotion(); this.target('foot',{start:'root',end:'tip',driver:'external'}); this.clipNode('idleNode','idle'); this.output('out','idleNode'); this.endMotion();",host);
    if (result.error.ok) std::printf("collinear diagnostic: bake unexpectedly succeeded\\n");
    else std::printf("collinear diagnostic: %s (%s)\\n", result.error.message.c_str(), result.error.code.c_str());
    CHECK(!result.error.ok && result.error.message.find("explicit pole")!=std::string::npos,"collinear omitted pole fails closed");
}
void test_generate_cannot_author_geometry() {
    script_host::ScriptHost host; const auto result=bake("this.beginRig('r'); this.root('root'); this.bone('mid',[1,0,0]); this.bone('tip',[1,0,0]); this.endRig(); this.beginClip('bad',{duration:1,sampleRate:1}); this.generate(phase=>{this.fill(2); this.tint(1,0,0,1); this.placeChild('missing'); this.emitVolume({radius:1,length:1,dir:[1,0,0]}); this.beginVoxels(.1); this.sphere([0,0,0],1); this.endVoxels();}); this.endClip();",host);
    CHECK(!result.error.ok && result.error.message.find("geometry authoring is forbidden")!=std::string::npos,"generate rejects structural geometry authoring");
}
void test_generate_cannot_mutate_terrain_or_join_cursor() {
    script_host::ScriptHost terrain_host;
    const auto terrain = bake("this.beginRig('r'); this.root('root'); this.endRig(); this.beginClip('bad',{duration:1,sampleRate:1}); this.generate(phase=>{this.terrainVolume(0,0,0);}); this.endClip();", terrain_host);
    CHECK(!terrain.error.ok && terrain.error.message.find("geometry authoring is forbidden") != std::string::npos,
          "generate rejects terrain output before terrain world validation");
    script_host::ScriptHost join_host;
    const auto join = bake("this.beginRig('r'); this.root('root'); this.endRig(); this.beginClip('bad',{duration:1,sampleRate:1}); this.generate(phase=>{this.joinType(1);}); this.endClip();", join_host);
    CHECK(!join.error.ok && join.error.message.find("geometry authoring is forbidden") != std::string::npos,
          "generate rejects join cursor mutation");
}
void test_motion_options_fail_closed() {
    script_host::ScriptHost type_host;
    const auto unknown_type = bake("this.beginRig('r'); this.root('root'); this.endRig(); this.beginMotion(); this.input('x',{type:'not-a-type',default:0}); this.output('out','missing'); this.endMotion();", type_host);
    CHECK(!unknown_type.error.ok && unknown_type.error.message.find("type") != std::string::npos,
          "unknown input type fails instead of becoming number");
    script_host::ScriptHost value_host;
    const auto bad_value = bake("this.beginRig('r'); this.root('root'); this.endRig(); this.beginMotion(); this.input('x',{type:'vec3',default:[1,'bad',3]}); this.output('out','missing'); this.endMotion();", value_host);
    CHECK(!bad_value.error.ok && bad_value.error.message.find("default") != std::string::npos,
          "ill-typed input default fails instead of becoming a zero vector");
    script_host::ScriptHost cadence_host;
    const auto bad_cadence = bake("this.beginRig('r'); this.root('root'); this.endRig(); this.beginMotion(); this.input('x',{type:'float',cadence:'tick',default:0}); this.output('out','missing'); this.endMotion();", cadence_host);
    CHECK(!bad_cadence.error.ok && bad_cadence.error.message.find("cadence") != std::string::npos,
          "unknown input cadence fails instead of becoming fixed");
    script_host::ScriptHost dependency_host;
    const auto bad_dependency = bake("this.beginRig('r'); this.root('root'); this.endRig(); this.beginClip('idle',{duration:1,sampleRate:1}); this.key('root',0,{}); this.endClip(); this.beginMotion(); this.input('speed',{type:'float',cadence:'frame',default:0}); this.clipNode('idle','idle'); this.blend1D('blend','speed',[[0,'idle'],[1,'idle']]); this.output('out','blend'); this.endMotion();", dependency_host);
    CHECK(!bad_dependency.error.ok && bad_dependency.error.message.find("frame") != std::string::npos,
          "frame input cannot feed a fixed blend graph node");
}
void test_motion_source_spans_are_preserved() {
    script_host::ScriptHost host; const auto input=bake("this.beginRig('r');\n this.root('root');\n this.bone('mid',[1,0,0]);\n this.bone('tip',[1,0,0]);\n this.endRig();\n this.beginMotion();\n this.input('speed',{type:'symbol',default:''});\n this.output('out');\n this.endMotion();",host);
    if(!input.error.ok) std::printf("input span: %s (%s)\\n",input.error.source_location.c_str(),input.error.message.c_str()); CHECK(!input.error.ok && !input.error.source_location.empty(),"input diagnostic preserves declaration span");
    script_host::ScriptHost marker_host; const auto marker=bake("this.beginRig('r'); this.root('root'); this.bone('mid',[1,0,0]); this.bone('tip',[1,0,0]); this.endRig(); this.beginClip('bad',{duration:1,sampleRate:1}); this.marker(1,'bad'); this.endClip();",marker_host);
    CHECK(!marker.error.ok && marker.error.source_location.find("marker")!=std::string::npos,"marker diagnostic preserves declaration object");
}
void test_imported_motion_source_span_preserves_module() {
    const auto root = std::filesystem::temp_directory_path() / "matter_animation_motion_span";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    {
        std::ofstream helper(root / "motion_helper.js", std::ios::binary);
        helper << "export function declareBad(part) {\n"
                  "  part.input('speed',{type:'symbol',default:''});\n"
                  "}\n";
    }
    script_host::ScriptHost host;
    host.set_shared_lib_root(root.string());
    const auto result = host.bake_source(
        "import { declareBad } from 'shared-lib/motion_helper';\n"
        "class ImportedMotionPart extends Part { build(p) {\n"
        "  this.beginRig('r'); this.root('root'); this.endRig();\n"
        "  this.beginClip('idle',{duration:1,sampleRate:1}); this.key('root',0,{}); this.endClip();\n"
        "  this.beginMotion(); declareBad(this); this.clipNode('idle','idle'); this.output('out','idle'); this.endMotion();\n"
        "} }", "{}", {});
    CHECK(!result.error.ok && result.error.source_location.find("shared-lib/motion_helper") != std::string::npos,
          "imported motion diagnostic preserves its module specifier");
    std::filesystem::remove_all(root, ec);
}
void test_animated_rig_gallery_source_bakes() {
    const std::filesystem::path source_path =
        std::filesystem::path("..") / "examples" / "world_demo" / "objects" / "AnimatedRigGallery.js";
    std::ifstream input(source_path, std::ios::binary);
    CHECK(input.good(), "animated gallery source is present beside the world-demo objects");
    if (!input.good()) return;
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    script_host::ScriptHost host;
    const uint64_t crate_hash = 0xC4A11Eull;
    const std::string crate_module = "Crate";
    const auto result = host.bake_source(source, "{}", {}, &crate_hash, 1, &crate_module);
    if (!result.error.ok) std::printf("gallery bake error: %s (%s)\n", result.error.message.c_str(), result.error.code.c_str());
    CHECK(result.error.ok, "animated gallery remains a deterministic bake-time DSL source");
    const auto& build = host.last_animation_build();
    CHECK(build && build->rig.joints.size() < 128 && build->targets.size() <= 8 &&
          build->graph.nodes.size() <= 32,
          "gallery stays inside the declared v1 joints/targets/graph budgets");
    CHECK(build && build->clips.size() == 2 && build->skin_bindings.size() == 1 &&
          build->rigid_bindings.size() == 3 && build->attachments.size() == 1,
          "gallery contains generated clips, deformable skin, rigid segments, and socket attachment");
}
}
int main(){test_generated_loop_and_controls();test_validation_rejects_bad_motion_graph();test_collinear_target_requires_pole();test_generate_cannot_author_geometry();test_generate_cannot_mutate_terrain_or_join_cursor();test_motion_options_fail_closed();test_motion_source_spans_are_preserved();test_imported_motion_source_span_preserves_module();test_animated_rig_gallery_source_bakes();if(g_failures){std::printf("animation_dsl_motion_tests: %d failure(s)\n",g_failures);return 1;}std::printf("animation_dsl_motion_tests: all tests passed\n");return 0;}
