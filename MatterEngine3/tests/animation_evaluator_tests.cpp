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
RigDefinition two_joint_rig(){RigDefinition r;r.joints.push_back({"root","",tx(0),1,{"t",1,1,"root"}});r.joints.push_back({"tip","root",tx(0),1,{"t",1,1,"tip"}});return r;}
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
void test_cadence_aware_control_sampling(){
 Fixture f;
 f.def.inputs={{AnimationValueType::Number,EvaluationCadence::Fixed},{AnimationValueType::Float3,EvaluationCadence::Fixed},{AnimationValueType::Quaternion,EvaluationCadence::Fixed},{AnimationValueType::Transform,EvaluationCadence::Fixed},{AnimationValueType::Bool,EvaluationCadence::Fixed},{AnimationValueType::Symbol,EvaluationCadence::Fixed},{AnimationValueType::Number,EvaluationCadence::Frame}};
 AnimationValue previous[]={AnimationValue(0.0),AnimationValue(Float3{0,0,0}),AnimationValue(Quaternion{0,0,0,1}),AnimationValue(tx(0)),AnimationValue(false),AnimationValue("idle"),AnimationValue(10.0)};
 AnimationValue current[]={AnimationValue(2.0),AnimationValue(Float3{2,4,6}),AnimationValue(Quaternion{0,0,1,0}),AnimationValue(tx(2)),AnimationValue(true),AnimationValue("walk"),AnimationValue(20.0)};
 AnimationValue frame[]={AnimationValue(0.0),AnimationValue(Float3{}),AnimationValue(Quaternion{}),AnimationValue(tx(0)),AnimationValue(false),AnimationValue(""),AnimationValue(7.0)};
 auto r=request(handle(12),f.def,1,0,.25f);r.fixed_previous={previous,7};r.fixed_current={current,7};r.frame_controls={frame,7}; AnimationValue value;
 CHECK(sample_graph_input(f.def,r,0,value)&&near((float)value.number,.5f),"fixed number controls interpolate at frame alpha");
 CHECK(sample_graph_input(f.def,r,1,value)&&near(value.float3.y,1),"fixed vectors interpolate at frame alpha");
 CHECK(sample_graph_input(f.def,r,2,value)&&std::fabs(value.quaternion.z)>.3f&&std::fabs(value.quaternion.z)<.5f,"fixed quaternions slerp at frame alpha");
 CHECK(sample_graph_input(f.def,r,3,value)&&near(value.transform.translation.x,.5f),"fixed transforms interpolate at frame alpha");
 CHECK(sample_graph_input(f.def,r,4,value)&&!value.boolean,"fixed booleans hold previous value before alpha one");
 CHECK(sample_graph_input(f.def,r,5,value)&&value.symbol=="idle","fixed symbols hold previous value before alpha one");
 CHECK(sample_graph_input(f.def,r,6,value)&&near((float)value.number,7),"frame controls are sampled without fixed interpolation");
 r.accumulator_alpha=1;CHECK(sample_graph_input(f.def,r,4,value)&&value.boolean,"fixed booleans switch at alpha one");CHECK(sample_graph_input(f.def,r,5,value)&&value.symbol=="walk","fixed symbols switch at alpha one");
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
void test_non_looping_clip_clamps(){
 Fixture f; f.def.clips[0].loop=false;f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0}}}; AnimationEvaluator e;
 CHECK(e.evaluate({request(handle(13),f.def,1,2.0f)}),"non-looping clip evaluates beyond duration");
 CHECK(near(e.snapshot(handle(13)).local_pose[0].translation.x,1.0f),"non-looping clip clamps at final key");
}
void test_blend_and_additive(){
 Fixture f; f.def.inputs={{AnimationValueType::Number,EvaluationCadence::Fixed}}; AnimationValue controls[1]={AnimationValue(0.5)};
 f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Clip,{},1},{RuntimeGraphNodeKind::Blend1D,{0,1},UINT16_MAX,0,{0,1}},{RuntimeGraphNodeKind::Output,{2}}}; AnimationEvaluator e;
 auto r=request(handle(4),f.def,1,0);r.fixed_previous={controls,1};r.fixed_current={controls,1};CHECK(e.evaluate({r}),"blend1d evaluates interior");CHECK(near(e.snapshot(handle(4)).local_pose[0].translation.x,0),"zero-time blend has common endpoint");
 r=request(handle(4),f.def,2,.5f);r.fixed_previous={controls,1};r.fixed_current={controls,1};CHECK(e.evaluate({r}),"blend1d evaluates at half clip time");CHECK(near(e.snapshot(handle(4)).local_pose[0].translation.x,1.0f),"blend1d interpolates thresholds and sampled clips");
 AnimationValue previous_speed[1]={AnimationValue(0.0)},current_speed[1]={AnimationValue(1.0)};AnimationEvaluator interpolated;auto interpolated_request=request(handle(24),f.def,1,.5f,.5f);interpolated_request.fixed_previous={previous_speed,1};interpolated_request.fixed_current={current_speed,1};CHECK(interpolated.evaluate({interpolated_request})&&near(interpolated.snapshot(handle(24)).local_pose[0].translation.x,1.0f),"fixed control interpolation is applied through blend graph sampling");
 f.def.inputs[0].cadence=EvaluationCadence::Frame;f.def.nodes[2].cadence=EvaluationCadence::Frame;f.def.nodes[3].cadence=EvaluationCadence::Frame;AnimationValue frame_speed[1]={AnimationValue(.5)};AnimationEvaluator frame;auto frame_request=request(handle(25),f.def,1,.5f,.5f);frame_request.fixed_previous={previous_speed,1};frame_request.fixed_current={current_speed,1};frame_request.frame_controls={frame_speed,1};CHECK(frame.evaluate({frame_request})&&near(frame.snapshot(handle(25)).local_pose[0].translation.x,1.0f),"frame control drives frame-cadence blend without fixed interpolation");
 f.def.nodes[2].cadence=EvaluationCadence::Fixed;f.def.nodes[3].cadence=EvaluationCadence::Fixed;AnimationEvaluator invalid_cadence;CHECK(!invalid_cadence.evaluate({frame_request}),"frame controls cannot feed fixed graph nodes");f.def.inputs[0].cadence=EvaluationCadence::Fixed;
 controls[0]=AnimationValue(-2.0);r=request(handle(4),f.def,3,0);r.fixed_previous={controls,1};r.fixed_current={controls,1};CHECK(e.evaluate({r})&&near(e.snapshot(handle(4)).local_pose[0].translation.x,.5f),"blend1d clamps below first threshold");
 controls[0]=AnimationValue(5.0);r=request(handle(4),f.def,4,0);r.fixed_previous={controls,1};r.fixed_current={controls,1};CHECK(e.evaluate({r})&&near(e.snapshot(handle(4)).local_pose[0].translation.x,1.5f),"blend1d clamps above final threshold");
 f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Clip,{},2},{RuntimeGraphNodeKind::Additive,{0,1},UINT16_MAX,UINT16_MAX,{},.5f},{RuntimeGraphNodeKind::Output,{2}}}; AnimationEvaluator a;CHECK(a.evaluate({request(handle(5),f.def,1,.5f)}),"additive graph evaluates");CHECK(near(a.snapshot(handle(5)).local_pose[0].translation.x,1.0f),"additive clip applies reference-relative delta");
}
void test_budget_order_and_reuse(){
 Fixture f;f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0}}};AnimationEvaluator e({2,0});
 auto first=request(handle(8),f.def,1,0);first.visibility_class=1;auto second=request(handle(1),f.def,1,0);second.visibility_class=0;CHECK(!e.evaluate({first,second}),"node budget reports overflow");CHECK(e.snapshot(handle(1)).local_pose.count==1&&e.snapshot(handle(8)).local_pose.count==0,"visibility then slot order wins budget deterministically");
 CHECK(!e.evaluate({request(handle(1),f.def,2,.5f),request(handle(8),f.def,2,.5f)}),"over-budget repeat remains recoverable");CHECK(near(e.snapshot(handle(1)).local_pose[0].translation.x,.5f),"accepted instance updates without partial skipped snapshot");
}
void test_snapshot_backing_and_priority_controller_budget(){
 Fixture f;f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0}}};AnimationEvaluator e;
 CHECK(e.evaluate({request(handle(14),f.def,1,0)}),"initial immutable snapshot publishes");auto old=e.snapshot(handle(14));const auto old_local=old.local_pose;const auto old_model=old.model_pose;
 AnimationEvaluationDefinition invalid=f.def;invalid.nodes={{RuntimeGraphNodeKind::Output,{}}};CHECK(!e.evaluate({request(handle(14),invalid,2,.5f)}),"invalid graph fails before publish");CHECK(near(old_local[0].translation.x,0)&&near(old_model[0].m[3],0),"failed evaluation leaves retained views stable");
 CHECK(e.evaluate({request(handle(14),f.def,2,.5f)}),"next complete evaluation publishes back buffer");CHECK(near(old_local[0].translation.x,0)&&near(old_model[0].m[3],0),"old ArrayViews remain immutable across one successful swap");CHECK(near(e.snapshot(handle(14)).local_pose[0].translation.x,.5f),"new snapshot advances independently");
 AnimationEvaluator over_budget({2,0});CHECK(over_budget.evaluate({request(handle(26),f.def,1,0)}),"budget test publishes baseline snapshot");const auto retained=over_budget.snapshot(handle(26)).local_pose;auto admitted=request(handle(27),f.def,2,.5f);admitted.visibility_class=0;auto skipped=request(handle(26),f.def,2,.5f);skipped.visibility_class=1;CHECK(!over_budget.evaluate({skipped,admitted}),"over-budget request fails closed");CHECK(near(retained[0].translation.x,0)&&near(over_budget.snapshot(handle(26)).local_pose[0].translation.x,0),"over-budget work leaves front snapshot and retained view stable");
 AnimationEvaluationBudget budget{2,1};AnimationEvaluator priority(budget);auto low=request(handle(20),f.def,1,0);low.explicit_priority=1;auto high=request(handle(21),f.def,1,0);high.explicit_priority=9;CHECK(!priority.evaluate({low,high}),"priority overflow reports skipped work");CHECK(priority.snapshot(handle(21)).local_pose.count==1&&priority.snapshot(handle(20)).local_pose.count==0,"higher explicit priority wins deterministic budget ordering");
 f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::NativeController,{0}},{RuntimeGraphNodeKind::Output,{1}}};AnimationEvaluator controller_limited({3,0});CHECK(!controller_limited.evaluate({request(handle(22),f.def,1,0)}),"native controller nodes consume controller budget");CHECK(controller_limited.snapshot(handle(22)).local_pose.empty(),"controller budget overflow does not publish partial snapshot");AnimationEvaluator controller_ok({3,1});CHECK(controller_ok.evaluate({request(handle(22),f.def,1,0)}),"native controller graph evaluates when controller budget permits");
}
void test_definition_shape_and_graph_contract_rejection(){
 Fixture f;f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0}}};AnimationEvaluator e;
 const auto h=handle(40);CHECK(e.evaluate({request(h,f.def,1,0)}),"shape test publishes a baseline");const auto retained=e.snapshot(h);const auto retained_local=retained.local_pose;
 OzzSkeleton larger_skeleton;OzzAnimation larger_animation;Diagnostics diagnostics;const auto larger_rig=two_joint_rig();CHECK(build_skeleton(larger_rig,larger_skeleton,diagnostics)&&build_clip(larger_rig,clip("larger",1,true),larger_animation,diagnostics),"build larger definition");
 AnimationEvaluationDefinition larger;larger.skeleton=&larger_skeleton;larger.clips={{&larger_animation,1,true,false}};larger.inverse_bind_model={identity(),identity()};larger.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0}}};
 CHECK(!e.evaluate({request(h,larger,2,.5f)}),"a reused handle rejects a different skeleton shape");CHECK(retained_local.count==1&&near(retained_local[0].translation.x,0)&&e.snapshot(h).local_pose.count==1,"shape rejection preserves the published snapshot");
 CHECK(!e.evaluate({request(h,f.def,2,.5f),request(h,larger,2,.5f)}),"conflicting same-batch requests reject before either can publish");CHECK(near(e.snapshot(h).local_pose[0].translation.x,0),"same-batch shape conflict leaves the prior snapshot unchanged");
 CHECK(e.evaluate({request(h,f.def,2,.5f)})&&near(e.snapshot(h).local_pose[0].translation.x,.5f),"a rejected shape does not advance the retained evaluator state");
 AnimationEvaluationDefinition malformed=f.def;malformed.nodes={{static_cast<RuntimeGraphNodeKind>(255),{0}},{RuntimeGraphNodeKind::Output,{0}}};CHECK(!e.evaluate({request(h,malformed,2,.5f)}),"unknown graph node kinds fail closed");
 malformed=f.def;malformed.inputs={{AnimationValueType::Number,EvaluationCadence::Fixed}};malformed.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Clip,{},1},{RuntimeGraphNodeKind::Blend1D,{0,1},UINT16_MAX,0,{0,std::numeric_limits<float>::quiet_NaN()}},{RuntimeGraphNodeKind::Output,{2}}};CHECK(!e.evaluate({request(h,malformed,2,.5f)}),"nonfinite blend thresholds fail closed");
 malformed=f.def;malformed.inputs={{AnimationValueType::Number,EvaluationCadence::Fixed}};malformed.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Clip,{},1},{RuntimeGraphNodeKind::Blend1D,{0,1},UINT16_MAX,0,{0,0}},{RuntimeGraphNodeKind::Output,{2}}};CHECK(!e.evaluate({request(h,malformed,2,.5f)}),"blend thresholds must be strictly increasing");
 malformed=f.def;malformed.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Clip,{},2},{RuntimeGraphNodeKind::Additive,{0,1},UINT16_MAX,UINT16_MAX,{},std::numeric_limits<float>::infinity()},{RuntimeGraphNodeKind::Output,{2}}};CHECK(!e.evaluate({request(h,malformed,2,.5f)}),"nonfinite additive weights fail closed");
 malformed=f.def;malformed.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0,0}}};CHECK(!e.evaluate({request(h,malformed,2,.5f)}),"malformed output arity fails closed");
 malformed=f.def;malformed.inputs={{AnimationValueType::Number,static_cast<EvaluationCadence>(255)}};malformed.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Clip,{},1},{RuntimeGraphNodeKind::Blend1D,{0,1},UINT16_MAX,0,{0,1}},{RuntimeGraphNodeKind::Output,{2}}};CHECK(!e.evaluate({request(h,malformed,2,.5f)}),"invalid input cadence fails before sampling");
 malformed=f.def;malformed.inputs={{AnimationValueType::Number,EvaluationCadence::Frame}};malformed.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Clip,{},1},{RuntimeGraphNodeKind::Blend1D,{0,1},UINT16_MAX,0,{0,1},1,EvaluationCadence::Fixed},{RuntimeGraphNodeKind::Output,{2}}};CHECK(!e.evaluate({request(h,malformed,2,.5f)}),"frame inputs cannot source fixed graph nodes");
 CHECK(near(e.snapshot(h).local_pose[0].translation.x,.5f),"all malformed graph requests preserve the previous snapshot");
 AnimationEvaluationDefinition frame_to_fixed=f.def;AnimationEvaluator cadence_e;const auto cadence_handle=handle(41);CHECK(cadence_e.evaluate({request(cadence_handle,frame_to_fixed,1,0)}),"cadence rejection test publishes baseline");frame_to_fixed.nodes={{RuntimeGraphNodeKind::Clip,{},0,UINT16_MAX,{},1,EvaluationCadence::Frame},{RuntimeGraphNodeKind::Output,{0}}};CHECK(!cadence_e.evaluate({request(cadence_handle,frame_to_fixed,2,.5f)}),"fixed output cannot depend on a frame-cadence node");CHECK(near(cadence_e.snapshot(cadence_handle).local_pose[0].translation.x,0),"frame-to-fixed rejection preserves prior snapshot and evaluator state");
}
void test_graph_root_motion_crosses_loops_and_locks_pose(){
 Fixture f; f.def.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0}}}; f.def.clips[0].rate=10.0f;
 AnimationEvaluator forward; const auto h=handle(50);
 auto first=request(h,f.def,1,.2f); first.root_lock=true;
 CHECK(forward.evaluate({first}),"first graph root sample evaluates");
 auto second=request(h,f.def,2,.2f); second.root_lock=true;
 CHECK(forward.evaluate({second}),"multi-loop graph root sample evaluates");
 DesiredRootMotion motion{}; std::vector<RuntimeGraphClipAdvance> advances;
 CHECK(forward.fixed_root_motion(h,motion)&&near(motion.delta.translation.x,2.0f),"root delta preserves forward multi-loop distance");
 CHECK(forward.fixed_graph_clips(h,advances)&&advances.size()==1&&near(advances[0].previous_time,2.0f)&&near(advances[0].current_time,4.0f),"fixed graph traversal exposes graph clip time rather than descriptor time");
 CHECK(near(forward.snapshot(h).local_pose[0].translation.x,0.0f),"root-locked snapshot removes consumed root translation");
 f.def.clips[0].rate=-10.0f; AnimationEvaluator reverse; const auto r=handle(51);
 CHECK(reverse.evaluate({request(r,f.def,1,.2f)})&&reverse.evaluate({request(r,f.def,2,.2f)}),"reverse graph loop samples evaluate");
 CHECK(reverse.fixed_root_motion(r,motion)&&near(motion.delta.translation.x,-2.0f),"root delta preserves reverse multi-loop distance");
}
void test_root_lock_preserves_authored_root_reference_and_scale(){
 RigDefinition root_rig; AnimationTransform rest{}; rest.translation={3,4,5}; rest.rotation={0,0,.70710678f,.70710678f}; rest.scale={2,3,4};
 root_rig.joints.push_back({"root","",rest,1,{"t",1,1,"root"}});
 AnimationTransform end=rest; end.translation.x=7; end.rotation={0,0,1,0}; end.scale={4,5,6};
 ClipDefinition root_clip; root_clip.name="root-lock"; root_clip.duration=1; root_clip.rate=1; root_clip.loop=false; root_clip.source={"t",1,1,"root-lock"};
 root_clip.tracks.push_back({"root",{{0,rest,{"t",1,1,"start"}},{1,end,{"t",1,1,"end"}}},{"t",1,1,"track"}});
 OzzSkeleton skeleton; OzzAnimation animation; Diagnostics diagnostics;
 CHECK(build_skeleton(root_rig,skeleton,diagnostics)&&build_clip(root_rig,root_clip,animation,diagnostics),"build non-identity root-lock fixture");
 AnimationEvaluationDefinition definition; definition.skeleton=&skeleton; definition.clips={{&animation,1,false,false}}; definition.inverse_bind_model={identity()}; definition.nodes={{RuntimeGraphNodeKind::Clip,{},0},{RuntimeGraphNodeKind::Output,{0}}};
 const auto h=handle(52); AnimationEvaluator evaluator; auto first=request(h,definition,1,.25f); first.root_lock=true;
 auto second=request(h,definition,2,.25f); second.root_lock=true;
 CHECK(evaluator.evaluate({first})&&evaluator.evaluate({second}),"non-identity root-lock samples evaluate");
 const auto pose=evaluator.snapshot(h); DesiredRootMotion motion{};
 CHECK(pose.local_pose.count==1&&near(pose.local_pose[0].translation.x,3)&&near(pose.local_pose[0].translation.y,4)&&near(pose.local_pose[0].rotation.z,.70710678f)&&near(pose.local_pose[0].rotation.w,.70710678f),"root lock retains the authored bind translation and rotation");
 CHECK(near(pose.local_pose[0].scale.x,3)&&near(pose.local_pose[0].scale.y,4)&&near(pose.local_pose[0].scale.z,5),"root lock retains evaluated authored scale");
 CHECK(evaluator.fixed_root_motion(h,motion)&&near(motion.delta.translation.x,1)&&motion.delta.rotation.z>.20f&&motion.delta.rotation.z<.21f,"root motion exposes only the dynamic translation and rotation");
 AnimatorCheckpoint checkpoint{}; checkpoint.instance=h;
 CHECK(evaluator.capture_checkpoint(h,checkpoint),"capture root-locked checkpoint");
 AnimationEvaluator restored;
 CHECK(restored.restore_checkpoint(h,definition,checkpoint),"restore root-locked checkpoint");
 const auto replay=restored.snapshot(h);
 CHECK(replay.local_pose.count==1&&near(replay.local_pose[0].translation.x,3)&&near(replay.local_pose[0].rotation.z,.70710678f)&&near(replay.local_pose[0].scale.x,3),"checkpoint restore retains locked reference root and authored scale");
 auto next=request(h,definition,3,.25f); next.root_lock=true;
 CHECK(restored.evaluate({next})&&restored.fixed_root_motion(h,motion)&&near(motion.delta.translation.x,1),"restored root lock resumes with the same dynamic root delta");
}
}
int main(){test_controls();test_cadence_aware_control_sampling();test_time_interpolation_and_previous();test_wrap_clamp_pause_and_disable();test_non_looping_clip_clamps();test_blend_and_additive();test_budget_order_and_reuse();test_snapshot_backing_and_priority_controller_budget();test_definition_shape_and_graph_contract_rejection();test_graph_root_motion_crosses_loops_and_locks_pose();test_root_lock_preserves_authored_root_reference_and_scale();if(g_failures){std::printf("animation_evaluator_tests: %d failure(s)\n",g_failures);return 1;}std::puts("animation_evaluator_tests: all tests passed");}
