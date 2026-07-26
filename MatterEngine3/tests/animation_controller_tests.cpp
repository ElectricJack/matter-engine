#include "check.h"
#include "animation/animation_controllers.h"

#include <cmath>
#include <cstring>
using namespace matter; using namespace matter::animation;
namespace {
class Ground final: public AnimationWorldQueries { public: bool enabled=true; float height=0.0f; float normal_y=1.0f; mutable std::vector<Float3> origins; bool ray_cast(const Float3&o,const Float3&,float,uint64_t,WorldRayHit&out)const override {origins.push_back(o);if(!enabled)return false;out={7,{o.x,height,o.z},{0,normal_y,0},o.y-height};return true;} };
void test_registry_and_gait(){ auto r=NativeControllerRegistry::with_v1_controllers();NativeControllerLayout l{};NativeControllerDescriptor bad{kGaitControllerTypeId,{1},EvaluationCadence::Fixed};CHECK(!r.create(bad,l),"schema mismatch rejects before allocation");GaitControllerParameters p{};p.left_target=0;p.right_target=1;std::vector<uint8_t>b(sizeof(p));std::memcpy(b.data(),&p,sizeof(p));auto c=r.create({kGaitControllerTypeId,b,EvaluationCadence::Fixed},l);CHECK(c&&l.fixed_state_bytes>0,"stable gait factory validates blob and declares state");Ground ground;NativeControllerContext x{};x.fixed_delta_seconds=.1;x.world_queries=&ground;x.inputs={AnimationValue(2.0)};CHECK(c->fixed_update(x)&&x.writes.size()==2,"gait receives declared fixed input and emits ordered writes");std::vector<uint8_t> saved;CHECK(c->checkpoint(saved),"controller checkpoints deterministic state");NativeControllerContext y{};y.fixed_delta_seconds=.1;y.world_queries=&ground;y.inputs={AnimationValue(2.0)};CHECK(c->fixed_update(y),"input-driven gait advances deterministically");CHECK(c->restore(saved),"controller restores matching state bytes");NativeControllerContext replay{};replay.fixed_delta_seconds=.1;replay.world_queries=&ground;replay.inputs={AnimationValue(2.0)};CHECK(c->fixed_update(replay)&&replay.writes[0].transform.translation.x==y.writes[0].transform.translation.x,"checkpoint restore replays input-driven gait output");ground.enabled=false;NativeControllerContext z{};z.fixed_delta_seconds=.1;z.world_queries=&ground;CHECK(c->fixed_update(z)&&z.writes.size()==2,"missing ground releases without nondeterministic failure");}
void test_smoothing(){ CanonicalTarget t{};t.cadence=EvaluationCadence::Frame;t.position_half_life=1;t.rotation_half_life=1;t.weight_half_life=1;AnimationTargetState s{};s.desired.translation.x=1;s.desired_weight=1;s.evaluated_weight=0;CHECK(smooth_animation_target(t,s,1,EvaluationCadence::Frame)&&s.evaluated.translation.x>.49f&&s.evaluated.translation.x<.51f,"half-life uses exp2 convergence");s.enabled=false;CHECK(smooth_animation_target(t,s,1,EvaluationCadence::Frame)&&s.evaluated_weight<.5f,"disable fades solver weight");s.snap_requested=true;s.enabled=true;s.desired.translation.x=3;CHECK(!smooth_animation_target(t,s,0,EvaluationCadence::Fixed)&&s.snap_requested&&s.evaluated.translation.x!=3,"wrong cadence neither reports success nor consumes snap");CHECK(smooth_animation_target(t,s,0,EvaluationCadence::Frame)&&s.evaluated.translation.x==3,"snap occurs at cadence boundary");}
void test_gait_queries_use_full_scaled_entity_world_transform(){
 GaitControllerParameters p{};p.left_target=0;p.right_target=1;p.left_predicted={1,2,3};p.right_predicted={-1,-2,-3};
 std::vector<uint8_t>b(sizeof(p));std::memcpy(b.data(),&p,sizeof(p));NativeControllerLayout l{};auto c=create_gait_controller(b.data(),b.size(),l);
 Ground ground;NativeControllerContext x{};x.fixed_delta_seconds=.1;x.world_queries=&ground;x.has_entity_world=true;
 x.entity_world.m[0]=0;x.entity_world.m[1]=-3;x.entity_world.m[2]=0;x.entity_world.m[3]=10;
 x.entity_world.m[4]=2;x.entity_world.m[5]=0;x.entity_world.m[6]=0;x.entity_world.m[7]=20;
 x.entity_world.m[8]=0;x.entity_world.m[9]=0;x.entity_world.m[10]=4;x.entity_world.m[11]=30;x.entity_world.m[15]=1;
 CHECK(c&&c->fixed_update(x)&&ground.origins.size()==2&&std::fabs(ground.origins[0].x-4)<1e-4f&&std::fabs(ground.origins[0].y-22.25f)<1e-4f&&std::fabs(ground.origins[0].z-42)<1e-4f,
       "gait predicted point composes entity translation, rotation, and nonuniform scale before querying world");
}
void test_gait_releases_contacts_on_swing_and_rejects_bad_ground(){
 GaitControllerParameters p{};p.left_target=0;p.right_target=1;p.left_predicted={0,0,0};p.right_predicted={0,0,0};p.stride_seconds=1.0f;p.step_height=.25f;p.min_ground_normal_y=.5f;
 std::vector<uint8_t>b(sizeof(p));std::memcpy(b.data(),&p,sizeof(p));NativeControllerLayout l{};auto c=create_gait_controller(b.data(),b.size(),l);Ground ground;NativeControllerContext x{};x.fixed_delta_seconds=.5;x.world_queries=&ground;
 CHECK(c&&c->fixed_update(x)&&std::fabs(x.writes[0].transform.translation.y)<1e-4f,"gait plants on a flat stance hit");
 x.writes.clear();CHECK(c->fixed_update(x)&&std::fabs(x.writes[0].transform.translation.y)<1e-4f,"gait clears a valid planted contact when the foot enters swing");
 auto stepped_parameters=p;stepped_parameters.step_height=1.25f;std::vector<uint8_t> stepped_bytes(sizeof(stepped_parameters));std::memcpy(stepped_bytes.data(),&stepped_parameters,sizeof(stepped_parameters));auto stepped=create_gait_controller(stepped_bytes.data(),stepped_bytes.size(),l);ground.height=1.0f;NativeControllerContext step{};step.fixed_delta_seconds=.1;step.world_queries=&ground;
 CHECK(stepped&&stepped->fixed_update(step)&&std::fabs(step.writes[0].transform.translation.y-1.0f)<1e-4f,"gait plants on stepped ground");
 auto too_high=create_gait_controller(b.data(),b.size(),l);NativeControllerContext high{};high.fixed_delta_seconds=.1;high.world_queries=&ground;
 CHECK(too_high&&too_high->fixed_update(high)&&std::fabs(high.writes[0].transform.translation.y)<1e-4f,"gait rejects a stance hit above the predicted-foot step cap");
 auto slope=create_gait_controller(b.data(),b.size(),l);ground.height=0.0f;ground.normal_y=.25f;NativeControllerContext rejected{};rejected.fixed_delta_seconds=.1;rejected.world_queries=&ground;
 CHECK(slope&&slope->fixed_update(rejected)&&std::fabs(rejected.writes[0].transform.translation.y)<1e-4f,"gait rejects a slope below the authored walkability threshold");
 auto miss=create_gait_controller(b.data(),b.size(),l);ground.normal_y=1.0f;ground.enabled=true;NativeControllerContext missed{};missed.fixed_delta_seconds=1.0f;missed.world_queries=&ground;
 CHECK(miss&&miss->fixed_update(missed),"gait establishes a contact before a ray miss");ground.enabled=false;missed.writes.clear();
 CHECK(miss->fixed_update(missed)&&std::fabs(missed.writes[0].transform.translation.y)<1e-4f,"gait releases a planted contact after a ground-query miss");
}
}
int main(){test_registry_and_gait();test_smoothing();test_gait_queries_use_full_scaled_entity_world_transform();test_gait_releases_contacts_on_swing_and_rejects_bad_ground();return check_summary();}
