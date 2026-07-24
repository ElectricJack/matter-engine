#include "animation/animation_evaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

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
bool valid_cadence(EvaluationCadence cadence) { return cadence==EvaluationCadence::Fixed || cadence==EvaluationCadence::Frame; }
bool valid_value_type(AnimationValueType type) {
    switch(type) {
        case AnimationValueType::Number: case AnimationValueType::Float3: case AnimationValueType::Quaternion:
        case AnimationValueType::Transform: case AnimationValueType::Bool: case AnimationValueType::Symbol: return true;
    }
    return false;
}
bool valid(const AnimationEvaluationDefinition& d) {
    if(!d.skeleton || d.skeleton->joint_count()==0 || d.inverse_bind_model.size()!=d.skeleton->joint_count() || d.nodes.empty()) return false;
    for(const RuntimeGraphInput& input:d.inputs) if(!valid_value_type(input.type) || !valid_cadence(input.cadence)) return false;
    for(const RuntimeGraphClip& clip:d.clips) if(!clip.animation || !std::isfinite(clip.duration) || clip.duration<0.0f) return false;
    for(size_t i=0;i<d.nodes.size();++i) {
        const auto& n=d.nodes[i];
        if(!valid_cadence(n.cadence)) return false;
        for(uint16_t dep:n.dependencies) {
            if(dep>=i) return false;
            if(d.nodes[dep].cadence==EvaluationCadence::Frame && n.cadence==EvaluationCadence::Fixed) return false;
        }
        switch(n.kind) {
            case RuntimeGraphNodeKind::Clip:
                if(!n.dependencies.empty() || n.clip_index>=d.clips.size() || n.input_index!=kNoIndex || !n.thresholds.empty()) return false;
                break;
            case RuntimeGraphNodeKind::Blend1D:
                if(n.dependencies.size()<2 || n.thresholds.size()!=n.dependencies.size() || n.clip_index!=kNoIndex || n.input_index>=d.inputs.size()) return false;
                if(d.inputs[n.input_index].type!=AnimationValueType::Number || (n.cadence==EvaluationCadence::Fixed && d.inputs[n.input_index].cadence==EvaluationCadence::Frame)) return false;
                for(size_t threshold=0;threshold<n.thresholds.size();++threshold) if(!std::isfinite(n.thresholds[threshold]) || (threshold>0 && !(n.thresholds[threshold]>n.thresholds[threshold-1]))) return false;
                break;
            case RuntimeGraphNodeKind::Additive:
                if(n.dependencies.size()!=2 || n.clip_index!=kNoIndex || n.input_index!=kNoIndex || !n.thresholds.empty() || !std::isfinite(n.weight) || n.weight<0.0f || n.weight>1.0f) return false;
                break;
            case RuntimeGraphNodeKind::NativeController:
                if(n.dependencies.size()!=1 || n.clip_index!=kNoIndex || n.input_index!=kNoIndex || !n.thresholds.empty()) return false;
                break;
            case RuntimeGraphNodeKind::Output:
                if(i+1!=d.nodes.size() || n.dependencies.size()!=1 || n.clip_index!=kNoIndex || n.input_index!=kNoIndex || !n.thresholds.empty()) return false;
                break;
            default: return false;
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

void emit_crossed_markers(AnimatorInstanceHandle instance,
                          ArrayView<RuntimeClipMarker> markers,
                          float duration, bool loop,
                          float previous_time, float current_time,
                          std::vector<AnimationMarkerEvent>& out) {
    if (!instance.valid() || !std::isfinite(previous_time) || !std::isfinite(current_time) ||
        !std::isfinite(duration) || duration <= 0.0f || previous_time == current_time) return;
    struct Crossing { float absolute_time; RuntimeClipMarker marker; };
    std::vector<Crossing> crossings;
    constexpr size_t kMaxMarkerEmissionsPerAdvance = 4096;
    const bool forward = current_time > previous_time;
    for (uint32_t i = 0; i < markers.count && crossings.size() < kMaxMarkerEmissionsPerAdvance; ++i) {
        const RuntimeClipMarker marker = markers[i];
        if (!std::isfinite(marker.time) || marker.time < 0.0f || marker.time > duration) continue;
        if (!loop) {
            const bool crossed = forward ? (marker.time > previous_time && marker.time <= current_time)
                                         : (marker.time >= current_time && marker.time < previous_time);
            if (crossed) crossings.push_back({marker.time, marker});
            continue;
        }
        const int first = static_cast<int>(std::floor(std::min(previous_time, current_time) / duration)) - 1;
        const int last = static_cast<int>(std::ceil(std::max(previous_time, current_time) / duration)) + 1;
        for (int cycle = first; cycle <= last && crossings.size() < kMaxMarkerEmissionsPerAdvance; ++cycle) {
            const float absolute = marker.time + cycle * duration;
            const bool crossed = forward ? (absolute > previous_time && absolute <= current_time)
                                         : (absolute >= current_time && absolute < previous_time);
            if (crossed) crossings.push_back({absolute, marker});
        }
    }
    std::stable_sort(crossings.begin(), crossings.end(), [forward](const Crossing& a, const Crossing& b) {
        if (a.absolute_time != b.absolute_time) return forward ? a.absolute_time < b.absolute_time : a.absolute_time > b.absolute_time;
        if (a.marker.time != b.marker.time) return forward ? a.marker.time < b.marker.time : a.marker.time > b.marker.time;
        return a.marker.marker_index < b.marker.marker_index;
    });
    for (const Crossing& crossing : crossings) out.push_back({instance, crossing.marker.marker_index, crossing.marker.time});
}

AnimationTransform root_motion_delta(const AnimationTransform& previous,
                                     const AnimationTransform& current) {
    const Quaternion inverse_previous{-previous.rotation.x, -previous.rotation.y,
                                      -previous.rotation.z, previous.rotation.w};
    const Quaternion& rotation = current.rotation;
    AnimationTransform delta{};
    delta.translation = {current.translation.x - previous.translation.x,
                         current.translation.y - previous.translation.y,
                         current.translation.z - previous.translation.z};
    delta.rotation = normalize({rotation.w * inverse_previous.x + rotation.x * inverse_previous.w + rotation.y * inverse_previous.z - rotation.z * inverse_previous.y,
                                rotation.w * inverse_previous.y - rotation.x * inverse_previous.z + rotation.y * inverse_previous.w + rotation.z * inverse_previous.x,
                                rotation.w * inverse_previous.z + rotation.x * inverse_previous.y - rotation.y * inverse_previous.x + rotation.z * inverse_previous.w,
                                rotation.w * inverse_previous.w - rotation.x * inverse_previous.x - rotation.y * inverse_previous.y - rotation.z * inverse_previous.z});
    return delta;
}

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
    struct DefinitionShape {
        const AnimationEvaluationDefinition* definition = nullptr;
        const OzzSkeleton* skeleton = nullptr;
        uint32_t joint_count = 0;
    } shape;
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
    const auto shape_for=[](const AnimationEvaluationRequest& request) { return State::DefinitionShape{request.definition,request.definition->skeleton,uint32_t(request.definition->skeleton->joint_count())}; };
    const auto same_shape=[](const State::DefinitionShape& a,const State::DefinitionShape& b) { return a.definition==b.definition && a.skeleton==b.skeleton && a.joint_count==b.joint_count; };
    std::map<uint64_t,State::DefinitionShape> batch_shapes;
    std::set<uint64_t> conflicting_instances;
    bool all=true;
    for(const auto& request:requests) {
        if(!request.instance.valid() || !request.enabled || !request.definition || !valid(*request.definition)) { all=false; continue; }
        const uint64_t instance_key=key(request.instance); const State::DefinitionShape shape=shape_for(request);
        const auto [it,inserted]=batch_shapes.emplace(instance_key,shape);
        if(!inserted && !same_shape(it->second,shape)) conflicting_instances.insert(instance_key);
    }
    uint32_t graph_used=0, controller_used=0;
    for(const auto& request:requests) {
        if(!request.instance.valid() || !request.enabled || !request.definition || !valid(*request.definition)) { all=false; continue; }
        const uint64_t instance_key=key(request.instance);
        if(conflicting_instances.count(instance_key)!=0) { all=false; continue; }
        uint32_t graph_count=0, controller_count=0; for(const auto& n:request.definition->nodes) { ++graph_count; if(n.kind==RuntimeGraphNodeKind::NativeController) ++controller_count; }
        if(graph_count>budget_.graph_nodes-graph_used || controller_count>budget_.controller_nodes-controller_used) { all=false; continue; }
        graph_used+=graph_count; controller_used+=controller_count;
        const auto state_it=states_.find(instance_key); State::DefinitionShape shape=shape_for(request);
        if(state_it!=states_.end() && !same_shape(state_it->second->shape,shape)) { all=false; continue; }
        std::unique_ptr<State> candidate_state;
        State* state_ptr=nullptr;
        if(state_it!=states_.end()) state_ptr=state_it->second.get();
        else { candidate_state=std::make_unique<State>(); state_ptr=candidate_state.get(); }
        State& state=*state_ptr; const auto& def=*request.definition;
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
            else if(node.kind==RuntimeGraphNodeKind::NativeController) { if(node.dependencies.size()!=1) complete=false; else out=results[node.dependencies[0]]; }
            else complete=false;
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
        state.shape=shape;
        state.view={request.instance,request.fixed_tick,request.frame_serial,{back.local.data(),uint32_t(back.local.size())},{back.model.data(),uint32_t(back.model.size())},{back.previous_model.data(),uint32_t(back.previous_model.size())},{back.palette.data(),uint32_t(back.palette.size())},{back.previous_palette.data(),uint32_t(back.previous_palette.size())}};
        if(state_it==states_.end()) states_.emplace(instance_key,std::move(candidate_state));
    }
    return all;
}

AnimationPoseSnapshot AnimationEvaluator::snapshot(AnimatorInstanceHandle instance) const { const auto it=states_.find(key(instance)); return it==states_.end()||!it->second||!it->second->has_snapshot?AnimationPoseSnapshot{}:it->second->view; }
void AnimationEvaluator::forget(AnimatorInstanceHandle instance) { states_.erase(key(instance)); }

} // namespace matter::animation
