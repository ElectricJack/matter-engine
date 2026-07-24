#include "animation/animation_evaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace matter::animation {
namespace {
constexpr uint16_t kNoIndex = UINT16_MAX;

uint64_t key(AnimatorInstanceHandle h) { return (uint64_t(h.slot_index) << 32u) | h.generation; }
float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }
Float3 lerp(Float3 a, Float3 b, float t) { return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t}; }
Quaternion normalize(Quaternion q) { const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w); return n>1e-8f?Quaternion{q.x/n,q.y/n,q.z/n,q.w/n}:Quaternion{}; }
Quaternion slerp(Quaternion a, Quaternion b, float t) {
    a=normalize(a); b=normalize(b); float d=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;
    if(d<0){d=-d;b={-b.x,-b.y,-b.z,-b.w};}
    if(d>.9995f) return normalize({a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t});
    const float theta=std::acos(std::max(-1.0f,std::min(1.0f,d))), s=std::sin(theta);
    const float x=std::sin((1-t)*theta)/s,y=std::sin(t*theta)/s;
    return {a.x*x+b.x*y,a.y*x+b.y*y,a.z*x+b.z*y,a.w*x+b.w*y};
}
AnimationTransform lerp(AnimationTransform a, AnimationTransform b, float t) { return {lerp(a.translation,b.translation,t),slerp(a.rotation,b.rotation,t),lerp(a.scale,b.scale,t)}; }
Mat4f multiply(const Mat4f& a,const Mat4f& b) { Mat4f r{}; for(int y=0;y<4;++y)for(int x=0;x<4;++x)for(int k=0;k<4;++k)r.m[y*4+x]+=a.m[y*4+k]*b.m[k*4+x]; return r; }
bool valid(const AnimationEvaluationDefinition& d) {
    if(!d.skeleton || d.skeleton->joint_count()==0 || d.inverse_bind_model.size()!=d.skeleton->joint_count() || d.nodes.empty()) return false;
    for(size_t i=0;i<d.nodes.size();++i) { const auto& n=d.nodes[i]; for(uint16_t dep:n.dependencies) if(dep>=i) return false; if(n.kind==RuntimeGraphNodeKind::Clip && (n.clip_index>=d.clips.size() || !d.clips[n.clip_index].animation)) return false; }
    return d.nodes.back().kind==RuntimeGraphNodeKind::Output;
}
float number(const AnimationEvaluationRequest& r,uint16_t index) {
    if(index==kNoIndex) return 0.0f;
    const ArrayView<AnimationValue>& f=r.fixed_current;
    if(index<f.count && f[index].type==AnimationValueType::Number) return f[index].number;
    if(index<r.frame_controls.count && r.frame_controls[index].type==AnimationValueType::Number) return r.frame_controls[index].number;
    return 0.0f;
}
float clip_ratio(const RuntimeGraphClip& clip,float time) {
    if(clip.duration<=0) return 0;
    if(clip.loop) { time=std::fmod(time,clip.duration); if(time<0) time+=clip.duration; }
    else time=std::max(0.0f,std::min(clip.duration,time));
    return time/clip.duration;
}
} // namespace

AnimationValue interpolate_fixed_control(const AnimationValue& previous,const AnimationValue& current,float alpha) {
    alpha=clamp01(alpha); if(previous.type!=current.type) return alpha>=1.0f?current:previous;
    switch(current.type) {
        case AnimationValueType::Number: return AnimationValue(previous.number+(current.number-previous.number)*alpha);
        case AnimationValueType::Float3: return AnimationValue(lerp(previous.float3,current.float3,alpha));
        case AnimationValueType::Quaternion: return AnimationValue(slerp(previous.quaternion,current.quaternion,alpha));
        case AnimationValueType::Transform: return AnimationValue(lerp(previous.transform,current.transform,alpha));
        case AnimationValueType::Bool: case AnimationValueType::Symbol: return alpha>=1.0f?current:previous;
    }
    return current;
}

struct AnimationEvaluator::State {
    bool initialized=false, has_snapshot=false;
    uint64_t last_fixed_tick=std::numeric_limits<uint64_t>::max();
    float graph_time=0;
    std::vector<AnimationTransform> prior_fixed, current_fixed, front_local;
    std::vector<Mat4f> front_model, front_previous_model, front_palette, front_previous_palette;
    AnimationPoseSnapshot view{};
};

AnimationEvaluator::AnimationEvaluator(AnimationEvaluationBudget budget) : budget_(budget) {}
AnimationEvaluator::~AnimationEvaluator() = default;

bool AnimationEvaluator::evaluate(std::vector<AnimationEvaluationRequest> requests) {
    std::stable_sort(requests.begin(),requests.end(),[](const auto&a,const auto&b){ if(a.visibility_class!=b.visibility_class)return a.visibility_class<b.visibility_class; if(a.explicit_priority!=b.explicit_priority)return a.explicit_priority>b.explicit_priority; return a.instance.slot_index<b.instance.slot_index; });
    uint32_t graph_used=0, controller_used=0; bool all=true;
    for(const auto& request:requests) {
        if(!request.instance.valid() || !request.enabled || !request.definition || !valid(*request.definition)) { all=false; continue; }
        uint32_t graph_count=0, controller_count=0; for(const auto& n:request.definition->nodes) { ++graph_count; if(n.kind==RuntimeGraphNodeKind::NativeController) ++controller_count; }
        if(graph_count>budget_.graph_nodes-graph_used || controller_count>budget_.controller_nodes-controller_used) { all=false; continue; }
        graph_used+=graph_count; controller_used+=controller_count;
        auto& owned=states_[key(request.instance)]; if(!owned) owned=std::make_unique<State>(); State& state=*owned; const auto& def=*request.definition;
        if(!state.initialized) { state.initialized=true; state.current_fixed.resize(def.skeleton->joint_count()); state.prior_fixed.resize(def.skeleton->joint_count()); }
        const bool new_fixed=state.last_fixed_tick!=request.fixed_tick;
        if(new_fixed) {
            if(!request.paused) state.graph_time+=std::max(0.0f,request.fixed_delta_seconds);
            std::vector<std::vector<AnimationTransform>> results(def.nodes.size()); std::vector<OzzSampleContext> contexts(def.clips.size());
            bool complete=true;
            for(uint32_t i=0;i<def.nodes.size()&&complete;++i) {
                const RuntimeGraphNode& node=def.nodes[i]; auto& out=results[i];
                if(node.kind==RuntimeGraphNodeKind::Clip) complete=sample(*def.clips[node.clip_index].animation,clip_ratio(def.clips[node.clip_index],state.graph_time),contexts[node.clip_index],out);
                else if(node.kind==RuntimeGraphNodeKind::Blend1D) {
                    if(node.dependencies.empty() || node.thresholds.size()!=node.dependencies.size()) { complete=false; break; }
                    const float x=number(request,node.input_index); size_t hi=0; while(hi+1<node.thresholds.size() && x>node.thresholds[hi+1]) ++hi;
                    size_t lo=hi; if(x<=node.thresholds.front()) lo=hi=0; else if(x>=node.thresholds.back()) lo=hi=node.thresholds.size()-1; else ++hi;
                    if(lo==hi) out=results[node.dependencies[lo]]; else { const float den=node.thresholds[hi]-node.thresholds[lo]; const float t=den>0?(x-node.thresholds[lo])/den:0; complete=blend(*def.skeleton,{{&results[node.dependencies[lo]],1-clamp01(t)},{&results[node.dependencies[hi]],clamp01(t)}},{},out); }
                } else if(node.kind==RuntimeGraphNodeKind::Additive) {
                    if(node.dependencies.size()!=2) { complete=false; break; }
                    complete=blend(*def.skeleton,{{&results[node.dependencies[0]],1}},{{&results[node.dependencies[1]],node.weight}},out);
                } else if(node.kind==RuntimeGraphNodeKind::Output) { if(node.dependencies.size()!=1) complete=false; else out=results[node.dependencies[0]]; }
                else { if(node.dependencies.size()!=1) complete=false; else out=results[node.dependencies[0]]; }
            }
            if(!complete || results.back().size()!=def.skeleton->joint_count()) { all=false; continue; }
            state.prior_fixed=state.current_fixed; state.current_fixed=std::move(results.back()); if(state.last_fixed_tick==std::numeric_limits<uint64_t>::max()) state.prior_fixed=state.current_fixed; state.last_fixed_tick=request.fixed_tick;
        }
        const float alpha=clamp01(request.accumulator_alpha); state.front_local.resize(state.current_fixed.size()); for(size_t i=0;i<state.front_local.size();++i) state.front_local[i]=lerp(state.prior_fixed[i],state.current_fixed[i],alpha);
        std::vector<Mat4f> model; if(!local_to_model(*def.skeleton,state.front_local,model)) { all=false; continue; }
        const std::vector<Mat4f> previous=state.has_snapshot?state.front_model:model; std::vector<Mat4f> palette(model.size()), previous_palette(model.size());
        for(size_t i=0;i<model.size();++i) { palette[i]=multiply(model[i],def.inverse_bind_model[i]); previous_palette[i]=multiply(previous[i],def.inverse_bind_model[i]); }
        state.front_previous_model=previous; state.front_previous_palette=previous_palette; state.front_model=std::move(model); state.front_palette=std::move(palette); state.has_snapshot=true;
        state.view={request.instance,request.fixed_tick,request.frame_serial,{state.front_local.data(),uint32_t(state.front_local.size())},{state.front_model.data(),uint32_t(state.front_model.size())},{state.front_previous_model.data(),uint32_t(state.front_previous_model.size())},{state.front_palette.data(),uint32_t(state.front_palette.size())},{state.front_previous_palette.data(),uint32_t(state.front_previous_palette.size())}};
    }
    return all;
}

AnimationPoseSnapshot AnimationEvaluator::snapshot(AnimatorInstanceHandle instance) const { const auto it=states_.find(key(instance)); return it==states_.end()||!it->second||!it->second->has_snapshot?AnimationPoseSnapshot{}:it->second->view; }
void AnimationEvaluator::forget(AnimatorInstanceHandle instance) { states_.erase(key(instance)); }

} // namespace matter::animation
