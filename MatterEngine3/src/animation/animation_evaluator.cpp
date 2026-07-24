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
    for(size_t i=0;i<d.nodes.size();++i) {
        const auto& n=d.nodes[i];
        for(uint16_t dep:n.dependencies) if(dep>=i) return false;
        if(n.kind==RuntimeGraphNodeKind::Clip && (n.clip_index>=d.clips.size() || !d.clips[n.clip_index].animation)) return false;
        if(n.kind==RuntimeGraphNodeKind::Blend1D) {
            if(n.input_index>=d.inputs.size() || d.inputs[n.input_index].type!=AnimationValueType::Number) return false;
            if(n.cadence==EvaluationCadence::Fixed && d.inputs[n.input_index].cadence==EvaluationCadence::Frame) return false;
        }
    }
    return d.nodes.back().kind==RuntimeGraphNodeKind::Output;
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

bool sample_graph_input(const AnimationEvaluationDefinition& definition,
                        const AnimationEvaluationRequest& request,
                        uint16_t input_index,
                        AnimationValue& value) {
    if (input_index == kNoIndex || input_index >= definition.inputs.size()) return false;
    const RuntimeGraphInput& input = definition.inputs[input_index];
    if (input.cadence == EvaluationCadence::Frame) {
        if (input_index >= request.frame_controls.count || request.frame_controls[input_index].type != input.type) return false;
        value = request.frame_controls[input_index];
        return true;
    }
    if (input.cadence != EvaluationCadence::Fixed || input_index >= request.fixed_current.count ||
        request.fixed_current[input_index].type != input.type) return false;
    const AnimationValue& current = request.fixed_current[input_index];
    if (input_index >= request.fixed_previous.count || request.fixed_previous[input_index].type != input.type) {
        value = current;
        return true;
    }
    value = interpolate_fixed_control(request.fixed_previous[input_index], current, request.accumulator_alpha);
    return true;
}

struct AnimationEvaluator::State {
    struct PoseBuffer {
        std::vector<AnimationTransform> local;
        std::vector<Mat4f> model;
        std::vector<Mat4f> previous_model;
        std::vector<Mat4f> palette;
        std::vector<Mat4f> previous_palette;
    };
    bool initialized=false, has_snapshot=false;
    uint64_t last_fixed_tick=std::numeric_limits<uint64_t>::max();
    float previous_fixed_time=0, current_fixed_time=0;
    uint8_t front_slot=0;
    PoseBuffer pose[2];
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
        const bool new_fixed=state.last_fixed_tick!=request.fixed_tick;
        float candidate_previous_time=state.previous_fixed_time, candidate_current_time=state.current_fixed_time;
        uint64_t candidate_last_tick=state.last_fixed_tick;
        bool candidate_initialized=state.initialized;
        if(new_fixed) {
            candidate_previous_time=state.current_fixed_time;
            candidate_current_time=state.current_fixed_time;
            if(!request.paused) candidate_current_time+=std::max(0.0f,request.fixed_delta_seconds);
            if(!candidate_initialized) candidate_previous_time=candidate_current_time;
            candidate_last_tick=request.fixed_tick;
            candidate_initialized=true;
        }
        const float alpha=clamp01(request.accumulator_alpha);
        const float sample_time=candidate_previous_time+(candidate_current_time-candidate_previous_time)*alpha;
        std::vector<std::vector<AnimationTransform>> results(def.nodes.size()); std::vector<OzzSampleContext> contexts(def.clips.size());
        bool complete=true;
        for(uint32_t i=0;i<def.nodes.size()&&complete;++i) {
            const RuntimeGraphNode& node=def.nodes[i]; auto& out=results[i];
            if(node.kind==RuntimeGraphNodeKind::Clip) complete=sample(*def.clips[node.clip_index].animation,clip_ratio(def.clips[node.clip_index],sample_time),contexts[node.clip_index],out);
            else if(node.kind==RuntimeGraphNodeKind::Blend1D) {
                if(node.dependencies.empty() || node.thresholds.size()!=node.dependencies.size()) { complete=false; break; }
                AnimationValue control; if(!sample_graph_input(def,request,node.input_index,control) || control.type!=AnimationValueType::Number) { complete=false; break; }
                const float x=static_cast<float>(control.number); size_t hi=0; while(hi+1<node.thresholds.size() && x>node.thresholds[hi+1]) ++hi;
                size_t lo=hi; if(x<=node.thresholds.front()) lo=hi=0; else if(x>=node.thresholds.back()) lo=hi=node.thresholds.size()-1; else ++hi;
                if(lo==hi) out=results[node.dependencies[lo]]; else { const float den=node.thresholds[hi]-node.thresholds[lo]; const float t=den>0?(x-node.thresholds[lo])/den:0; complete=blend(*def.skeleton,{{&results[node.dependencies[lo]],1-clamp01(t)},{&results[node.dependencies[hi]],clamp01(t)}},{},out); }
            } else if(node.kind==RuntimeGraphNodeKind::Additive) {
                if(node.dependencies.size()!=2) { complete=false; break; }
                complete=blend(*def.skeleton,{{&results[node.dependencies[0]],1}},{{&results[node.dependencies[1]],node.weight}},out);
            } else if(node.kind==RuntimeGraphNodeKind::Output) { if(node.dependencies.size()!=1) complete=false; else out=results[node.dependencies[0]]; }
            else { if(node.dependencies.size()!=1) complete=false; else out=results[node.dependencies[0]]; }
        }
        if(!complete || results.back().size()!=def.skeleton->joint_count()) { all=false; continue; }
        const uint8_t back_slot=state.has_snapshot?uint8_t(1u-state.front_slot):state.front_slot;
        State::PoseBuffer& back=state.pose[back_slot];
        back.local=std::move(results.back());
        std::vector<Mat4f> model; if(!local_to_model(*def.skeleton,back.local,model)) { all=false; continue; }
        back.previous_model=state.has_snapshot?state.pose[state.front_slot].model:model;
        back.model=std::move(model); back.palette.resize(back.model.size()); back.previous_palette.resize(back.previous_model.size());
        for(size_t i=0;i<back.model.size();++i) { back.palette[i]=multiply(back.model[i],def.inverse_bind_model[i]); back.previous_palette[i]=multiply(back.previous_model[i],def.inverse_bind_model[i]); }
        state.previous_fixed_time=candidate_previous_time; state.current_fixed_time=candidate_current_time; state.last_fixed_tick=candidate_last_tick; state.initialized=candidate_initialized;
        state.front_slot=back_slot; state.has_snapshot=true;
        state.view={request.instance,request.fixed_tick,request.frame_serial,{back.local.data(),uint32_t(back.local.size())},{back.model.data(),uint32_t(back.model.size())},{back.previous_model.data(),uint32_t(back.previous_model.size())},{back.palette.data(),uint32_t(back.palette.size())},{back.previous_palette.data(),uint32_t(back.previous_palette.size())}};
    }
    return all;
}

AnimationPoseSnapshot AnimationEvaluator::snapshot(AnimatorInstanceHandle instance) const { const auto it=states_.find(key(instance)); return it==states_.end()||!it->second||!it->second->has_snapshot?AnimationPoseSnapshot{}:it->second->view; }
void AnimationEvaluator::forget(AnimatorInstanceHandle instance) { states_.erase(key(instance)); }

} // namespace matter::animation
