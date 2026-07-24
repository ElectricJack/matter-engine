#include "animation/animation_evaluator.h"
#include "check.h"

#include <cmath>
#include <cstdio>

using namespace matter;
using namespace matter::animation;
namespace {
bool near(float a,float b){return std::fabs(a-b)<1e-3f;}
AnimatorInstanceHandle handle(uint32_t slot){return {slot,1,UINT32_MAX,static_cast<AnimationValueType>(0xff),AnimationCadence::Invalid};}
AnimationTransform tx(float x){AnimationTransform t;t.translation.x=x;return t;}
Mat4f identity(){Mat4f m{};m.m[0]=m.m[5]=m.m[10]=m.m[15]=1;return m;}
RigDefinition rig(){RigDefinition r;r.joints.push_back({"root","",tx(0),1,{"t",1,1,"root"}});return r;}
ClipDefinition clip(const char* name,float end,bool loop=false){ClipDefinition c;c.name=name;c.duration=1;c.rate=30;c.loop=loop;c.source={"t",1,1,name};c.tracks.push_back({"root",{{0,tx(0),{"t",1,1,"a"}},{1,tx(end),{"t",1,1,"b"}}},{"t",1,1,"track"}});return c;}
struct Fixture {
 OzzSkeleton skeleton; OzzAnimation a,b,add; AnimationEvaluationDefinition def; Diagnostics d;
 Fixture(){CHECK(build_skeleton(rig(),skeleton,d),"build evaluator skeleton");CHECK(build_clip(rig(),clip("a",1,true),a,d)&&build_clip(rig(),clip("b",3,true),b,d)&&build_clip(rig(),clip("add",2,true),add,d),"build evaluator clips");def.skeleton=&skeleton;def.clips={{&a,1,true,false},{&b,1,true,false},{&add,1,true,true}};def.inverse_bind_model={identity()};}
};
AnimationEvaluationRequest request(AnimatorInstanceHandle h,const AnimationEvaluationDefinition& d,uint64_t tick,float delta,float alpha=1){AnimationEvaluationRequest r;r.instance=h;r.definition=&d;r.fixed_tick=tick;r.frame_serial=tick;r.fixed_delta_seconds=delta;r.accumulator_alpha=alpha;return r;}
void test_controls(){
 AnimationValue p(0.0),c(2.0);CHECK(near(interpolate_fixed_control(p,c,.25f).number,.5f),"numbers interpolate");
 AnimationValue b0(false),b1(true);CHECK(!interpolate_fixed_control(b0,b1,.99f).boolean&&interpolate_fixed_control(b0,b1,1).boolean,"booleans hold until final alpha");
 Quaternion q0{},q1{0,0,1,0};CHECK(std::fabs(interpolate_fixed_control(AnimationValue(q0),AnimationValue(q1),.5f).quaternion.z)>.7f,"quaternion shortest-path slerp");
}
void test_time_interpolation_and_previous(){
 Fixture f; f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0}}}; AnimationEvaluator e;
 CHECK(e.evaluate({request(handle(2),f.def,1,0)}),"initial fixed pose publishes");
 CHECK(e.evaluate({request(handle(2),f.def,2,.5f,.5f)}),"advanced fixed pose publishes"); auto s=e.snapshot(handle(2));
 CHECK(s.local_pose.count==1&&near(s.local_pose[0].translation.x,.25f),"frame snapshot interpolates previous and current fixed locals");
 CHECK(near(s.model_pose[0].m[3],.25f)&&near(s.previous_model_pose[0].m[3],0),"current and previous model palettes retained");
 CHECK(near(s.skin_palette[0].m[3],.25f)&&near(s.previous_skin_palette[0].m[3],0),"skin palettes use model times inverse bind");
}
void test_wrap_clamp_pause_and_disable(){
 Fixture f; f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0}}}; AnimationEvaluator e;
 CHECK(e.evaluate({request(handle(3),f.def,1,1.25f)}),"looping clip evaluates");CHECK(near(e.snapshot(handle(3)).local_pose[0].translation.x,.25f),"loop clips wrap graph time");
 auto r=request(handle(3),f.def,2,.75f);r.paused=true;CHECK(e.evaluate({r}),"paused instance publishes unchanged pose");CHECK(near(e.snapshot(handle(3)).local_pose[0].translation.x,.25f),"paused graph time does not advance");
 r=request(handle(3),f.def,3,1);r.enabled=false;CHECK(!e.evaluate({r}),"disabled instance is not evaluated");CHECK(near(e.snapshot(handle(3)).local_pose[0].translation.x,.25f),"disabled instance retains last complete snapshot");
}
void test_blend_and_additive(){
 Fixture f; AnimationValue controls[1]={AnimationValue(0.5)};
 f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Clip,{},1},{RuntimeGraphNodeKind::Blend1D,{0,1},UINT16_MAX,0,{0,1}},{RuntimeGraphNodeKind::Output,{2}}}; AnimationEvaluator e;
 auto r=request(handle(4),f.def,1,0);r.fixed_current={controls,1};CHECK(e.evaluate({r}),"blend1d evaluates interior");CHECK(near(e.snapshot(handle(4)).local_pose[0].translation.x,0),"zero-time blend has common endpoint");
 r=request(handle(4),f.def,2,.5f);r.fixed_current={controls,1};CHECK(e.evaluate({r}),"blend1d evaluates at half clip time");CHECK(near(e.snapshot(handle(4)).local_pose[0].translation.x,1.0f),"blend1d interpolates thresholds and sampled clips");
 f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Clip,{},2},{RuntimeGraphNodeKind::Additive,{0,1},UINT16_MAX,UINT16_MAX,{},.5f},{RuntimeGraphNodeKind::Output,{2}}}; AnimationEvaluator a;CHECK(a.evaluate({request(handle(5),f.def,1,.5f)}),"additive graph evaluates");CHECK(near(a.snapshot(handle(5)).local_pose[0].translation.x,1.0f),"additive clip applies reference-relative delta");
}
void test_budget_order_and_reuse(){
 Fixture f;f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0}}};AnimationEvaluator e({2,0});
 auto first=request(handle(8),f.def,1,0);first.visibility_class=1;auto second=request(handle(1),f.def,1,0);second.visibility_class=0;CHECK(!e.evaluate({first,second}),"node budget reports overflow");CHECK(e.snapshot(handle(1)).local_pose.count==1&&e.snapshot(handle(8)).local_pose.count==0,"visibility then slot order wins budget deterministically");
 CHECK(!e.evaluate({request(handle(1),f.def,2,.5f),request(handle(8),f.def,2,.5f)}),"over-budget repeat remains recoverable");CHECK(near(e.snapshot(handle(1)).local_pose[0].translation.x,.5f),"accepted instance updates without partial skipped snapshot");
}
}
int main(){test_controls();test_time_interpolation_and_previous();test_wrap_clamp_pause_and_disable();test_blend_and_additive();test_budget_order_and_reuse();if(g_failures){std::printf("animation_evaluator_tests: %d failure(s)\n",g_failures);return 1;}std::puts("animation_evaluator_tests: all tests passed");}
