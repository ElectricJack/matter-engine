#include "animation/animation_controllers.h"

#include <cmath>
#include <cstring>

namespace matter::animation {
namespace {
bool finite(float v){return std::isfinite(v);} bool valid(const GaitControllerParameters&p){return p.left_target!=UINT16_MAX&&p.right_target!=UINT16_MAX&&p.left_target!=p.right_target&&finite(p.stride_seconds)&&p.stride_seconds>0&&finite(p.swing_height)&&p.swing_height>=0&&finite(p.ray_distance)&&p.ray_distance>0&&finite(p.step_height)&&p.step_height>=0&&finite(p.min_ground_normal_y)&&p.min_ground_normal_y>=-1&&p.min_ground_normal_y<=1;}
struct Foot { bool planted=false; Float3 position{}; uint64_t ground=0; };
class GaitController final: public NativeController {
public: explicit GaitController(GaitControllerParameters p):p_(p){}
 NativeControllerTypeId type()const noexcept override{return kGaitControllerTypeId;} NativeControllerLayout layout()const noexcept override{return {sizeof(State),0};}
 bool fixed_update(NativeControllerContext& c) override { if(!c.world_queries||!std::isfinite(c.fixed_delta_seconds)||c.fixed_delta_seconds<0)return false;state_.time+=c.fixed_delta_seconds;return foot(c,0,p_.left_target,p_.left_predicted)&&foot(c,1,p_.right_target,p_.right_predicted); }
 bool checkpoint(std::vector<uint8_t>&out)const override{out.resize(sizeof(state_));std::memcpy(out.data(),&state_,sizeof(state_));return true;} bool restore(const std::vector<uint8_t>&in)override{if(in.size()!=sizeof(state_))return false;State s{};std::memcpy(&s,in.data(),sizeof(s));if(!std::isfinite(s.time))return false;state_=s;return true;}
private: struct State{double time=0;Foot feet[2]{};};
 bool foot(NativeControllerContext&c,int index,uint16_t target,Float3 predicted){ const float phase=std::fmod(static_cast<float>(state_.time/p_.stride_seconds)+(index?0.5f:0.f),1.f); const bool stance=phase<0.5f; Foot&f=state_.feet[index];WorldRayHit hit{};const Float3 from{predicted.x,predicted.y+p_.step_height,predicted.z};const bool got=c.world_queries->ray_cast(from,{0,-1,0},p_.ray_distance,0,hit);const bool walkable=got&&hit.normal.y>=p_.min_ground_normal_y;
  if(stance&&walkable){if(!f.planted||f.ground!=hit.entity||std::fabs(f.position.y-hit.position.y)>p_.step_height){f.planted=true;f.position=hit.position;f.ground=hit.entity;}} else if(!walkable)f.planted=false;
  AnimationTransform result{}; result.translation=f.planted?f.position:predicted; if(!stance){const float x=(phase-.5f)*2.f;result.translation.y+=p_.swing_height*(3*x*x-2*x*x*x);} c.writes.push_back({target,result,1});return true; }
 GaitControllerParameters p_{};State state_{};
};
}
bool NativeControllerRegistry::register_factory(NativeControllerTypeId id,Factory f){return id!=0&&f&&factories_.emplace(id,f).second;}
std::unique_ptr<NativeController> NativeControllerRegistry::create(const NativeControllerDescriptor&d,NativeControllerLayout&layout)const{if(d.cadence!=EvaluationCadence::Fixed&&d.cadence!=EvaluationCadence::Frame)return {};auto it=factories_.find(d.type);return it==factories_.end()?nullptr:it->second(d.parameters.data(),d.parameters.size(),layout);}
NativeControllerRegistry NativeControllerRegistry::with_v1_controllers(){NativeControllerRegistry r;r.register_factory(kGaitControllerTypeId,&create_gait_controller);return r;}
std::unique_ptr<NativeController> create_gait_controller(const uint8_t*b,size_t n,NativeControllerLayout&layout){if(!b||n!=sizeof(GaitControllerParameters))return {};GaitControllerParameters p{};std::memcpy(&p,b,sizeof(p));if(!valid(p))return {};auto result=std::make_unique<GaitController>(p);layout=result->layout();return result;}
} // namespace matter::animation
