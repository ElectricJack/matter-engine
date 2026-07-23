#include "check.h"
#include "animation/ozz_adapter.h"
#include "script_host.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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
}
int main(){test_generated_loop_and_controls();test_validation_rejects_bad_motion_graph();test_collinear_target_requires_pole();if(g_failures){std::printf("animation_dsl_motion_tests: %d failure(s)\n",g_failures);return 1;}std::printf("animation_dsl_motion_tests: all tests passed\n");return 0;}
