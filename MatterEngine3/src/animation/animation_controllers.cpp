#include "animation/animation_controllers.h"

#include <cmath>
#include <cstring>

namespace matter::animation {
namespace {
bool finite(float v){return std::isfinite(v);}

bool finite3(Float3 v){return finite(v.x)&&finite(v.y)&&finite(v.z);}

bool valid(const GaitControllerParameters&p){
    return p.left_target != UINT16_MAX
        && p.right_target != UINT16_MAX
        && p.left_target != p.right_target
        && finite3(p.left_predicted)
        && finite3(p.right_predicted)
        && finite(p.stride_seconds) && p.stride_seconds > 0
        && finite(p.swing_height) && p.swing_height >= 0
        && finite(p.ray_distance) && p.ray_distance > 0
        && finite(p.step_height) && p.step_height >= 0
        && finite(p.min_ground_normal_y)
        && p.min_ground_normal_y >= -1
        && p.min_ground_normal_y <= 1;
}

Float3 transform_point(const NativeControllerContext& c,Float3 v){
    if (!c.has_entity_world) return v;
    const Mat4f& m = c.entity_world;
    return {m.m[0]*v.x + m.m[1]*v.y + m.m[2]*v.z  + m.m[3],
            m.m[4]*v.x + m.m[5]*v.y + m.m[6]*v.z  + m.m[7],
            m.m[8]*v.x + m.m[9]*v.y + m.m[10]*v.z + m.m[11]};
}
struct Foot { bool planted=false; Float3 position{}; uint64_t ground=0; };
class GaitController final: public NativeController {
public: explicit GaitController(GaitControllerParameters p):p_(p){}
 NativeControllerTypeId type()const noexcept override{return kGaitControllerTypeId;} NativeControllerLayout layout()const noexcept override{return {sizeof(State),0};}
 bool fixed_update(NativeControllerContext& c) override {
  if(!c.world_queries||!std::isfinite(c.fixed_delta_seconds)||c.fixed_delta_seconds<0)return false;

  double speed=1.0;
  if(!c.inputs.empty()){
   if(c.inputs[0].type!=AnimationValueType::Number||!std::isfinite(c.inputs[0].number)||c.inputs[0].number<0)return false;
   speed=c.inputs[0].number;
  }

  // Both feet solve against a scratch copy so a mid-step failure leaves the
  // committed state untouched.
  State next=state_;
  std::vector<ControllerTargetWrite> writes;
  if(!foot(c,next,writes,0,p_.left_target,p_.left_predicted)||!foot(c,next,writes,1,p_.right_target,p_.right_predicted)) return false;

  next.time+=c.fixed_delta_seconds*speed;
  if(!std::isfinite(next.time)) return false;

  c.writes.insert(c.writes.end(),writes.begin(),writes.end());
  state_=next;
  return true;
 }
 bool checkpoint(std::vector<uint8_t>&out)const override{out.resize(sizeof(state_));std::memcpy(out.data(),&state_,sizeof(state_));return true;} bool restore(const std::vector<uint8_t>&in)override{if(in.size()!=sizeof(state_))return false;State s{};std::memcpy(&s,in.data(),sizeof(s));if(!std::isfinite(s.time))return false;state_=s;return true;}
private: struct State{double time=0;Foot feet[2]{};};
 bool foot(NativeControllerContext&c,
           State& next,
           std::vector<ControllerTargetWrite>& writes,
           int index,
           uint16_t target,
           Float3 predicted){
  // Right foot is half a stride out of phase; first half of the cycle is stance.
  const float phase=std::fmod(static_cast<float>(next.time/p_.stride_seconds)+(index?0.5f:0.f),1.f);
  const bool stance=phase<0.5f;
  Foot&f=next.feet[index];

  WorldRayHit hit{};
  const Float3 predicted_world=transform_point(c,predicted);
  const Float3 from{predicted_world.x,
                    predicted_world.y+p_.step_height,
                    predicted_world.z};
  const bool got=c.world_queries->ray_cast(from,{0,-1,0},p_.ray_distance,0,hit);
  const bool walkable=got
                   && finite3(hit.position)
                   && finite3(hit.normal)
                   && hit.normal.y>=p_.min_ground_normal_y
                   && std::fabs(hit.position.y-predicted_world.y)<=p_.step_height;

  // A foot contact belongs only to its stance half-cycle.  Keeping the old
  // contact through a successful swing ray made an airborne foot snap back to
  // its previous plant instead of following its predicted swing trajectory.
  if(stance&&walkable){f.planted=true;f.position=hit.position;f.ground=hit.entity;} else f.planted=false;

  AnimationTransform result{};
  result.translation=f.planted?f.position:predicted_world;
  if(!stance){
    // Smoothstep arc over the swing half: 3x^2 - 2x^3 on x in [0,1].
    const float x=(phase-.5f)*2.f;
    result.translation.y+=p_.swing_height*(3*x*x-2*x*x*x);
  }
  if(!finite3(result.translation)) return false;
  writes.push_back({target,result,1});
  return true;
 }
 GaitControllerParameters p_{};State state_{};
};
}
bool NativeControllerRegistry::register_factory(NativeControllerTypeId id,Factory f){return id!=0&&f&&factories_.emplace(id,f).second;}
std::unique_ptr<NativeController> NativeControllerRegistry::create(const NativeControllerDescriptor&d,NativeControllerLayout&layout)const{if(d.cadence!=EvaluationCadence::Fixed&&d.cadence!=EvaluationCadence::Frame)return {};auto it=factories_.find(d.type);return it==factories_.end()?nullptr:it->second(d.parameters.data(),d.parameters.size(),layout);}
NativeControllerRegistry NativeControllerRegistry::with_v1_controllers(){NativeControllerRegistry r;r.register_factory(kGaitControllerTypeId,&create_gait_controller);return r;}
std::unique_ptr<NativeController> create_gait_controller(const uint8_t*b,size_t n,NativeControllerLayout&layout){if(!b||n!=sizeof(GaitControllerParameters))return {};GaitControllerParameters p{};std::memcpy(&p,b,sizeof(p));if(!valid(p))return {};auto result=std::make_unique<GaitController>(p);layout=result->layout();return result;}
} // namespace matter::animation
