#include "check.h"
#include "animation/animation_controllers.h"

#include <cmath>
#include <cstring>
using namespace matter; using namespace matter::animation;
namespace {
class Ground final: public AnimationWorldQueries { public: bool enabled=true; mutable std::vector<Float3> origins; bool ray_cast(const Float3&o,const Float3&,float,uint64_t,WorldRayHit&out)const override {origins.push_back(o);if(!enabled)return false;out={7,{o.x,0,o.z},{0,1,0},o.y};return true;} };
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
}
int main(){test_registry_and_gait();test_smoothing();test_gait_queries_use_full_scaled_entity_world_transform();return check_summary();}
