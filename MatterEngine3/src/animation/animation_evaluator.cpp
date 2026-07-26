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
Quaternion multiply(Quaternion a, Quaternion b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
            a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
Float3 rotate(Quaternion rotation, Float3 value) {
    const Quaternion q=normalize(rotation);
    const Quaternion rotated=multiply(multiply(q,{value.x,value.y,value.z,0}),{-q.x,-q.y,-q.z,q.w});
    return {rotated.x,rotated.y,rotated.z};
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
    if(!d.skeleton || d.skeleton->joint_count()==0 || d.skeleton->joint_count()>AnimationBudgetConfig::kHardMaxJointsPerAsset || d.inverse_bind_model.size()!=d.skeleton->joint_count() || d.nodes.empty() || d.nodes.size()>AnimationBudgetConfig::kHardMaxGraphNodes) return false;
    for(const RuntimeGraphInput& input:d.inputs) if(!valid_value_type(input.type) || !valid_cadence(input.cadence)) return false;
    for(const RuntimeGraphClip& clip:d.clips) {
        if(!clip.animation || !std::isfinite(clip.duration) || clip.duration<0.0f || !std::isfinite(clip.rate)) return false;
        for(const RuntimeClipMarker& marker:clip.markers)
            if(!std::isfinite(marker.time) || marker.time<0.0f || marker.time>clip.duration) return false;
    }
    uint32_t controller_count = 0;
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
                ++controller_count;
                if(n.dependencies.size()!=1 || n.clip_index!=kNoIndex || n.input_index!=kNoIndex || !n.thresholds.empty()) return false;
                break;
            case RuntimeGraphNodeKind::Output:
                if(i+1!=d.nodes.size() || n.dependencies.size()!=1 || n.clip_index!=kNoIndex || n.input_index!=kNoIndex || !n.thresholds.empty()) return false;
                break;
            default: return false;
        }
    }
    // Ozz stores additive clips as bind-relative deltas.  A delta is not a
    // normal pose, so prove every graph path keeps the two representations
    // separate before any sampler sees it.
    enum class PoseKind : uint8_t { Normal, Additive };
    std::vector<PoseKind> kinds(d.nodes.size(), PoseKind::Normal);
    for (size_t i=0;i<d.nodes.size();++i) {
        const RuntimeGraphNode& node=d.nodes[i];
        switch (node.kind) {
            case RuntimeGraphNodeKind::Clip:
                kinds[i]=d.clips[node.clip_index].additive ? PoseKind::Additive : PoseKind::Normal;
                break;
            case RuntimeGraphNodeKind::Blend1D:
                kinds[i]=kinds[node.dependencies.front()];
                for (uint16_t dependency:node.dependencies)
                    if (kinds[dependency]!=kinds[i]) return false;
                break;
            case RuntimeGraphNodeKind::Additive:
                if (kinds[node.dependencies[0]]!=PoseKind::Normal || kinds[node.dependencies[1]]!=PoseKind::Additive) return false;
                kinds[i]=PoseKind::Normal;
                break;
            case RuntimeGraphNodeKind::NativeController:
            case RuntimeGraphNodeKind::Output:
                if (kinds[node.dependencies[0]]!=PoseKind::Normal) return false;
                kinds[i]=PoseKind::Normal;
                break;
            default: return false;
        }
    }
    return controller_count<=AnimationBudgetConfig::kHardMaxControllerNodes && d.nodes.back().kind==RuntimeGraphNodeKind::Output;
}
bool within_budget(const AnimationEvaluationDefinition& d,
                   const AnimationBudgetConfig& budget) {
    if (!d.skeleton || d.skeleton->joint_count() > budget.max_joints_per_asset ||
        d.nodes.size() > budget.max_graph_nodes) return false;
    uint32_t controllers = 0;
    for (const RuntimeGraphNode& node : d.nodes)
        if (node.kind == RuntimeGraphNodeKind::NativeController) ++controllers;
    return controllers <= budget.max_controller_nodes;
}
float clip_ratio(const RuntimeGraphClip& clip,float time) {
    if(clip.duration<=0) return 0;
    if(clip.loop) { time=std::fmod(time,clip.duration); if(time<0) time+=clip.duration; }
    else time=std::max(0.0f,std::min(clip.duration,time));
    return time/clip.duration;
}

AnimationTransform sample_root(const RuntimeGraphClip& clip, float time, bool boundary_end = false) {
    AnimationTransform root{};
    if (clip.duration <= 0.0f) return root;
    float ratio = 0.0f;
    if (clip.loop) {
        ratio = std::fmod(time, clip.duration);
        if (ratio < 0.0f) ratio += clip.duration;
        if (boundary_end && std::fabs(ratio) < 1e-5f && std::fabs(time) > 1e-5f) ratio = clip.duration;
    } else ratio = std::max(0.0f, std::min(clip.duration, time));
    std::vector<AnimationTransform> locals;
    OzzSampleContext context;
    if (!sample(*clip.animation, ratio / clip.duration, context, locals) || locals.empty()) return root;
    return locals[0];
}

AnimationTransform inverse_delta(const AnimationTransform& value) {
    AnimationTransform inverse{};
    inverse.translation = {-value.translation.x, -value.translation.y, -value.translation.z};
    inverse.rotation = {-value.rotation.x, -value.rotation.y, -value.rotation.z, value.rotation.w};
    return inverse;
}

AnimationTransform forward_clip_root_delta(const RuntimeGraphClip& clip, float previous, float current) {
    AnimationTransform accumulated{};
    if (current <= previous || clip.duration <= 0.0f) return accumulated;
    if (!clip.loop) return root_motion_delta(sample_root(clip, previous), sample_root(clip, current));
    float cursor = previous;
    constexpr uint32_t kMaxSegments = 4096;
    for (uint32_t segment = 0; cursor < current && segment < kMaxSegments; ++segment) {
        const float boundary = (std::floor(cursor / clip.duration) + 1.0f) * clip.duration;
        const float next = std::min(current, boundary);
        const AnimationTransform delta = root_motion_delta(sample_root(clip, cursor),
                                                            sample_root(clip, next, next == boundary));
        accumulated.translation.x += delta.translation.x;
        accumulated.translation.y += delta.translation.y;
        accumulated.translation.z += delta.translation.z;
        accumulated.rotation = normalize({delta.rotation.w*accumulated.rotation.x + delta.rotation.x*accumulated.rotation.w + delta.rotation.y*accumulated.rotation.z - delta.rotation.z*accumulated.rotation.y,
                                           delta.rotation.w*accumulated.rotation.y - delta.rotation.x*accumulated.rotation.z + delta.rotation.y*accumulated.rotation.w + delta.rotation.z*accumulated.rotation.x,
                                           delta.rotation.w*accumulated.rotation.z + delta.rotation.x*accumulated.rotation.y - delta.rotation.y*accumulated.rotation.x + delta.rotation.z*accumulated.rotation.w,
                                           delta.rotation.w*accumulated.rotation.w - delta.rotation.x*accumulated.rotation.x - delta.rotation.y*accumulated.rotation.y - delta.rotation.z*accumulated.rotation.z});
        cursor = next;
    }
    return accumulated;
}

AnimationTransform clip_root_delta(const RuntimeGraphClip& clip, float previous, float current) {
    return current >= previous ? forward_clip_root_delta(clip, previous, current)
                               : inverse_delta(forward_clip_root_delta(clip, current, previous));
}

AnimationTransform weighted_delta(const AnimationTransform& a, const AnimationTransform& b, float t) {
    return {lerp(a.translation, b.translation, t), slerp(a.rotation, b.rotation, t), {1.0f, 1.0f, 1.0f}};
}
} // namespace

bool valid_animation_evaluation_definition(const AnimationEvaluationDefinition& definition) { return valid(definition); }

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
    DesiredRootMotion fixed_root_motion{};
    std::vector<RuntimeGraphClipAdvance> fixed_clips;
    PoseBuffer pose[2];
    AnimationPoseSnapshot view{};
};

AnimationEvaluator::AnimationEvaluator(AnimationEvaluationBudget budget) : budget_(std::move(budget)) {
    if (!budget_.limits.valid()) budget_.limits = {};
    budget_.graph_nodes = std::min(budget_.graph_nodes, budget_.limits.max_graph_nodes);
    budget_.controller_nodes = std::min(budget_.controller_nodes, budget_.limits.max_controller_nodes);
}
AnimationEvaluator::~AnimationEvaluator() = default;

bool AnimationEvaluator::set_budget_config(const AnimationBudgetConfig& config) {
    if (!config.valid() || !states_.empty()) return false;
    budget_.limits = config;
    budget_.graph_nodes = config.max_graph_nodes;
    budget_.controller_nodes = config.max_controller_nodes;
    return true;
}

bool AnimationEvaluator::evaluate(std::vector<AnimationEvaluationRequest> requests) {
    std::stable_sort(requests.begin(),requests.end(),[](const auto&a,const auto&b){ if(a.visibility_class!=b.visibility_class)return a.visibility_class<b.visibility_class; if(a.explicit_priority!=b.explicit_priority)return a.explicit_priority>b.explicit_priority; return a.instance.slot_index<b.instance.slot_index; });
    const auto shape_for=[](const AnimationEvaluationRequest& request) { return State::DefinitionShape{request.definition,request.definition->skeleton,uint32_t(request.definition->skeleton->joint_count())}; };
    const auto same_shape=[](const State::DefinitionShape& a,const State::DefinitionShape& b) { return a.definition==b.definition && a.skeleton==b.skeleton && a.joint_count==b.joint_count; };
    std::map<uint64_t,State::DefinitionShape> batch_shapes;
    std::set<uint64_t> conflicting_instances;
    bool all=true;
    for(const auto& request:requests) {
        if(!request.instance.valid() || !request.enabled || !request.definition || !valid(*request.definition) || !within_budget(*request.definition,budget_.limits)) { all=false; continue; }
        const uint64_t instance_key=key(request.instance); const State::DefinitionShape shape=shape_for(request);
        const auto [it,inserted]=batch_shapes.emplace(instance_key,shape);
        if(!inserted && !same_shape(it->second,shape)) conflicting_instances.insert(instance_key);
    }
    uint32_t graph_used=0, controller_used=0;
    for(const auto& request:requests) {
        if(!request.instance.valid() || !request.enabled || !request.definition) { all=false; stats_.record_fallback(AnimationFallbackReason::InvalidEvaluationRequest); continue; }
        if(!valid(*request.definition) || !within_budget(*request.definition,budget_.limits)) { all=false; stats_.record_fallback(AnimationFallbackReason::AssetLimit); continue; }
        const uint64_t instance_key=key(request.instance);
        if(conflicting_instances.count(instance_key)!=0) { all=false; stats_.record_fallback(AnimationFallbackReason::InvalidEvaluationRequest); continue; }
        uint32_t graph_count=0, controller_count=0; for(const auto& n:request.definition->nodes) { ++graph_count; if(n.kind==RuntimeGraphNodeKind::NativeController) ++controller_count; }
        if(graph_count>budget_.graph_nodes-graph_used || controller_count>budget_.controller_nodes-controller_used) { all=false; stats_.record_fallback(AnimationFallbackReason::EvaluationBudget); continue; }
        graph_used+=graph_count; controller_used+=controller_count;
        const auto state_it=states_.find(instance_key); State::DefinitionShape shape=shape_for(request);
        if(state_it!=states_.end() && !same_shape(state_it->second->shape,shape)) { all=false; stats_.record_fallback(AnimationFallbackReason::InvalidEvaluationRequest); continue; }
        if(state_it==states_.end() && states_.size()>=budget_.limits.max_runtime_instances) { all=false; stats_.record_fallback(AnimationFallbackReason::RuntimeInstanceLimit); continue; }
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
        std::vector<AnimationTransform> root_deltas(def.nodes.size());
        std::vector<RuntimeGraphClipAdvance> fixed_clips;
        bool complete=true;
        for(uint32_t i=0;i<def.nodes.size()&&complete;++i) {
            const RuntimeGraphNode& node=def.nodes[i]; auto& out=results[i];
            if(node.kind==RuntimeGraphNodeKind::Clip) {
                const RuntimeGraphClip& clip=def.clips[node.clip_index];
                complete=sample(*clip.animation,clip_ratio(clip,sample_time*clip.rate),contexts[node.clip_index],out);
                root_deltas[i]=clip_root_delta(clip,candidate_previous_time*clip.rate,candidate_current_time*clip.rate);
                if(new_fixed) fixed_clips.push_back({uint16_t(i),node.clip_index,candidate_previous_time*clip.rate,candidate_current_time*clip.rate});
            }
            else if(node.kind==RuntimeGraphNodeKind::Blend1D) {
                if(node.dependencies.empty() || node.thresholds.size()!=node.dependencies.size()) { complete=false; break; }
                AnimationValue control; if(!sample_graph_input(def,request,node.input_index,control) || control.type!=AnimationValueType::Number) { complete=false; break; }
                const float x=static_cast<float>(control.number); size_t hi=0; while(hi+1<node.thresholds.size() && x>node.thresholds[hi+1]) ++hi;
                size_t lo=hi; if(x<=node.thresholds.front()) lo=hi=0; else if(x>=node.thresholds.back()) lo=hi=node.thresholds.size()-1; else ++hi;
                if(lo==hi) { out=results[node.dependencies[lo]]; root_deltas[i]=root_deltas[node.dependencies[lo]]; }
                else { const float den=node.thresholds[hi]-node.thresholds[lo]; const float t=den>0?(x-node.thresholds[lo])/den:0; const float weight=clamp01(t); complete=blend(*def.skeleton,{{&results[node.dependencies[lo]],1-weight},{&results[node.dependencies[hi]],weight}},{},out); root_deltas[i]=weighted_delta(root_deltas[node.dependencies[lo]],root_deltas[node.dependencies[hi]],weight); }
            } else if(node.kind==RuntimeGraphNodeKind::Additive) {
                if(node.dependencies.size()!=2) { complete=false; break; }
                complete=blend(*def.skeleton,{{&results[node.dependencies[0]],1}},{{&results[node.dependencies[1]],node.weight}},out);
                const AnimationTransform additive=weighted_delta(AnimationTransform{},root_deltas[node.dependencies[1]],node.weight);
                root_deltas[i]=root_deltas[node.dependencies[0]];
                root_deltas[i].translation.x+=additive.translation.x; root_deltas[i].translation.y+=additive.translation.y; root_deltas[i].translation.z+=additive.translation.z;
                root_deltas[i].rotation=normalize({additive.rotation.w*root_deltas[i].rotation.x + additive.rotation.x*root_deltas[i].rotation.w + additive.rotation.y*root_deltas[i].rotation.z - additive.rotation.z*root_deltas[i].rotation.y,
                                                    additive.rotation.w*root_deltas[i].rotation.y - additive.rotation.x*root_deltas[i].rotation.z + additive.rotation.y*root_deltas[i].rotation.w + additive.rotation.z*root_deltas[i].rotation.x,
                                                    additive.rotation.w*root_deltas[i].rotation.z + additive.rotation.x*root_deltas[i].rotation.y - additive.rotation.y*root_deltas[i].rotation.x + additive.rotation.z*root_deltas[i].rotation.w,
                                                    additive.rotation.w*root_deltas[i].rotation.w - additive.rotation.x*root_deltas[i].rotation.x - additive.rotation.y*root_deltas[i].rotation.y - additive.rotation.z*root_deltas[i].rotation.z});
            } else if(node.kind==RuntimeGraphNodeKind::Output) { if(node.dependencies.size()!=1) complete=false; else { out=results[node.dependencies[0]]; root_deltas[i]=root_deltas[node.dependencies[0]]; } }
            else if(node.kind==RuntimeGraphNodeKind::NativeController) { if(node.dependencies.size()!=1) complete=false; else { out=results[node.dependencies[0]]; root_deltas[i]=root_deltas[node.dependencies[0]]; } }
            else complete=false;
        }
        if(!complete || results.back().size()!=def.skeleton->joint_count()) { all=false; stats_.record_fallback(AnimationFallbackReason::EvaluationFailure); continue; }
        const uint8_t back_slot=state.has_snapshot?uint8_t(1u-state.front_slot):state.front_slot;
        State::PoseBuffer& back=state.pose[back_slot];
        back.local=std::move(results.back());
        // The ECS root is the sole root-motion authority.  Keep the renderer
        // pose in-place after deriving the graph delta so no skinning path can
        // apply the same translation/rotation a second time.  The Ozz tracks
        // contain absolute local transforms, so identity is not the correct
        // in-place reference for an authored non-identity root: retain the
        // skeleton rest translation/rotation and only remove the dynamic root
        // components consumed by ECS.  Scale is deliberately retained from
        // the evaluated track; root motion never consumes scale.
        if(request.root_lock) {
            AnimationTransform root_reference{};
            if(!def.skeleton->rest_local(0,root_reference)) { all=false; stats_.record_fallback(AnimationFallbackReason::EvaluationFailure); continue; }
            back.local[0].translation=root_reference.translation;
            back.local[0].rotation=root_reference.rotation;
        }
        std::vector<Mat4f> model; if(!local_to_model(*def.skeleton,back.local,model)) { all=false; stats_.record_fallback(AnimationFallbackReason::EvaluationFailure); continue; }
        back.previous_model=state.has_snapshot?state.pose[state.front_slot].model:model;
        back.model=std::move(model); back.palette.resize(back.model.size()); back.previous_palette.resize(back.previous_model.size());
        for(size_t i=0;i<back.model.size();++i) { back.palette[i]=multiply(back.model[i],def.inverse_bind_model[i]); back.previous_palette[i]=multiply(back.previous_model[i],def.inverse_bind_model[i]); }
        state.previous_fixed_time=candidate_previous_time; state.current_fixed_time=candidate_current_time; state.last_fixed_tick=candidate_last_tick; state.initialized=candidate_initialized;
        state.front_slot=back_slot; state.has_snapshot=true;
        state.shape=shape;
        if(new_fixed) {
            // The root track's delta is authored in root-bind coordinates,
            // while DesiredRootMotion is consumed by the ECS entity.  Express
            // both its translation and rotation in that entity basis before
            // root lock replaces the visual root with the bind pose.
            AnimationTransform root_reference{};
            if (!def.skeleton->rest_local(0,root_reference)) { all=false; stats_.record_fallback(AnimationFallbackReason::EvaluationFailure); continue; }
            AnimationTransform ecs_delta=root_deltas.back();
            ecs_delta.translation=rotate(root_reference.rotation,ecs_delta.translation);
            const Quaternion bind=normalize(root_reference.rotation);
            ecs_delta.rotation=normalize(multiply(multiply(bind,ecs_delta.rotation),{-bind.x,-bind.y,-bind.z,bind.w}));
            state.fixed_root_motion={ecs_delta,true}; state.fixed_clips=std::move(fixed_clips);
        }
        state.view={request.instance,request.fixed_tick,request.frame_serial,{back.local.data(),uint32_t(back.local.size())},{back.model.data(),uint32_t(back.model.size())},{back.previous_model.data(),uint32_t(back.previous_model.size())},{back.palette.data(),uint32_t(back.palette.size())},{back.previous_palette.data(),uint32_t(back.previous_palette.size())}};
        ++stats_.evaluated_pose_count;
        stats_.evaluated_joint_count += back.local.size();
        if(state_it==states_.end()) states_.emplace(instance_key,std::move(candidate_state));
    }
    return all;
}

AnimationPoseSnapshot AnimationEvaluator::snapshot(AnimatorInstanceHandle instance) const { const auto it=states_.find(key(instance)); return it==states_.end()||!it->second||!it->second->has_snapshot?AnimationPoseSnapshot{}:it->second->view; }

bool AnimationEvaluator::fixed_clock(AnimatorInstanceHandle instance, float& previous, float& current) const {
    previous=current=0.0f;
    const auto it=states_.find(key(instance));
    if (!instance.valid() || it==states_.end() || !it->second || !it->second->initialized) return false;
    previous=it->second->previous_fixed_time;
    current=it->second->current_fixed_time;
    return std::isfinite(previous) && std::isfinite(current);
}

bool AnimationEvaluator::seed_presentation_clock(AnimatorInstanceHandle instance,
                                                  const AnimationEvaluationDefinition& definition,
                                                  uint64_t fixed_tick, float previous, float current) {
    if (!instance.valid() || !valid(definition) || !within_budget(definition,budget_.limits) ||
        !std::isfinite(previous) || !std::isfinite(current)) return false;
    const uint64_t instance_key=key(instance);
    const State::DefinitionShape shape{&definition,definition.skeleton,uint32_t(definition.skeleton->joint_count())};
    auto it=states_.find(instance_key);
    if (it==states_.end()) {
        if (states_.size()>=budget_.limits.max_runtime_instances) return false;
        auto state=std::make_unique<State>();
        state->shape=shape; state->initialized=true; state->last_fixed_tick=fixed_tick;
        state->previous_fixed_time=previous; state->current_fixed_time=current;
        states_.emplace(instance_key,std::move(state));
        return true;
    }
    State& state=*it->second;
    if (state.shape.definition!=shape.definition || state.shape.skeleton!=shape.skeleton || state.shape.joint_count!=shape.joint_count) return false;
    state.initialized=true; state.last_fixed_tick=fixed_tick;
    state.previous_fixed_time=previous; state.current_fixed_time=current;
    return true;
}

bool AnimationEvaluator::begin_presentation(AnimatorInstanceHandle instance,
                                             const AnimationEvaluationDefinition& definition,
                                             const AnimationPoseSnapshot& previous_fixed_pose,
                                             const AnimationPoseSnapshot& current_fixed_pose,
                                             float interpolation_alpha,
                                             uint64_t frame_serial) {
    if (!instance.valid() || !previous_fixed_pose.instance.valid() ||
        !current_fixed_pose.instance.valid() || key(previous_fixed_pose.instance) != key(instance) ||
        key(current_fixed_pose.instance) != key(instance) ||
        !std::isfinite(interpolation_alpha)) {
        stats_.record_fallback(AnimationFallbackReason::InvalidEvaluationRequest);
        return false;
    }
    if (!valid(definition) || !within_budget(definition, budget_.limits)) {
        stats_.record_fallback(AnimationFallbackReason::AssetLimit);
        return false;
    }
    const uint32_t joints = static_cast<uint32_t>(definition.skeleton->joint_count());
    const auto complete_pose=[joints](const AnimationPoseSnapshot& pose) { return pose.local_pose.count==joints && pose.model_pose.count==joints &&
        pose.previous_model_pose.count==joints && pose.skin_palette.count==joints && pose.previous_skin_palette.count==joints &&
        (joints==0 || (pose.local_pose.data && pose.model_pose.data && pose.previous_model_pose.data && pose.skin_palette.data && pose.previous_skin_palette.data)); };
    if (!complete_pose(previous_fixed_pose) || !complete_pose(current_fixed_pose)) {
        stats_.record_fallback(AnimationFallbackReason::InvalidEvaluationRequest);
        return false;
    }
    const uint64_t instance_key = key(instance);
    const State::DefinitionShape shape{&definition, definition.skeleton, joints};
    const auto existing = states_.find(instance_key);
    if (existing != states_.end() && (existing->second->shape.definition != shape.definition ||
        existing->second->shape.skeleton != shape.skeleton || existing->second->shape.joint_count != shape.joint_count)) {
        stats_.record_fallback(AnimationFallbackReason::InvalidEvaluationRequest);
        return false;
    }
    if (existing == states_.end() &&
        states_.size() >= budget_.limits.max_runtime_instances) {
        stats_.record_fallback(AnimationFallbackReason::RuntimeInstanceLimit);
        return false;
    }
    std::unique_ptr<State> replacement;
    State* state = nullptr;
    if (existing == states_.end()) {
        replacement = std::make_unique<State>();
        state = replacement.get();
    } else state = existing->second.get();
    const uint8_t back_slot = state->has_snapshot ? uint8_t(1u - state->front_slot) : state->front_slot;
    State::PoseBuffer& back = state->pose[back_slot];
    const float alpha=clamp01(interpolation_alpha);
    back.local.resize(joints);
    for(uint32_t joint=0;joint<joints;++joint) back.local[joint]=lerp(previous_fixed_pose.local_pose[joint],current_fixed_pose.local_pose[joint],alpha);
    if(!local_to_model(*definition.skeleton,back.local,back.model)) {
        stats_.record_fallback(AnimationFallbackReason::EvaluationFailure);
        return false;
    }
    // Presentation is a copy of a solved fixed sample, not another temporal
    // sample.  Preserve its velocity/model history exactly before layering a
    // frame target.
    back.previous_model.assign(previous_fixed_pose.previous_model_pose.data, previous_fixed_pose.previous_model_pose.data + joints);
    back.palette.resize(joints); back.previous_palette.resize(joints);
    for(uint32_t joint=0;joint<joints;++joint) { back.palette[joint]=multiply(back.model[joint],definition.inverse_bind_model[joint]); back.previous_palette[joint]=multiply(back.previous_model[joint],definition.inverse_bind_model[joint]); }
    state->shape = shape;
    state->has_snapshot = true;
    state->front_slot = back_slot;
    state->last_fixed_tick = current_fixed_pose.fixed_tick;
    state->view = {instance, current_fixed_pose.fixed_tick, frame_serial,
        {back.local.data(), joints}, {back.model.data(), joints}, {back.previous_model.data(), joints},
        {back.palette.data(), joints}, {back.previous_palette.data(), joints}};
    if (existing == states_.end()) states_.emplace(instance_key, std::move(replacement));
    ++stats_.evaluated_pose_count;
    stats_.evaluated_joint_count += joints;
    return true;
}
bool AnimationEvaluator::fixed_graph_clips(AnimatorInstanceHandle instance,
                                           std::vector<RuntimeGraphClipAdvance>& out) const {
    out.clear();
    const auto it=states_.find(key(instance));
    if(!instance.valid() || it==states_.end() || !it->second || !it->second->initialized) return false;
    out=it->second->fixed_clips;
    return true;
}
bool AnimationEvaluator::fixed_root_motion(AnimatorInstanceHandle instance,
                                           DesiredRootMotion& out) const {
    out={};
    const auto it=states_.find(key(instance));
    if(!instance.valid() || it==states_.end() || !it->second || !it->second->initialized || !it->second->fixed_root_motion.valid) return false;
    out=it->second->fixed_root_motion;
    return true;
}
bool AnimationEvaluator::solve_targets(AnimatorInstanceHandle instance,
                                       const AnimationEvaluationDefinition& definition,
                                       const std::vector<CanonicalTarget>& targets,
                                       const std::vector<AnimationTargetState>& target_states,
                                       uint64_t frame_serial) {
    const auto it=states_.find(key(instance));
    if(!instance.valid() || it==states_.end() || !it->second || !it->second->has_snapshot ||
       it->second->shape.definition!=&definition || targets.size()!=target_states.size() ||
       !validate_exclusive_target_chains(targets)) return false;
    State& state=*it->second;
    const uint8_t back_slot=uint8_t(1u-state.front_slot);
    State::PoseBuffer& back=state.pose[back_slot];
    const State::PoseBuffer& front=state.pose[state.front_slot];
    back.local=front.local; back.model=front.model;
    // All input validation happens before any live buffer is published.
    for(size_t i=0;i<targets.size();++i) {
        const AnimationTargetState& target=target_states[i];
        if(!std::isfinite(target.evaluated_weight) || target.evaluated_weight<0.0f || target.evaluated_weight>1.0f ||
           !solve_animation_target(targets[i],*definition.skeleton,target,back.local,back.model)) return false;
    }
    // Target/IK is a correction of the current sample, not a new temporal
    // sample.  Keep the graph's prior fixed model so velocity remains between
    // fixed samples and a frame-only correction cannot fabricate history.
    back.previous_model=front.previous_model;
    back.palette.resize(back.model.size()); back.previous_palette.resize(back.previous_model.size());
    for(size_t i=0;i<back.model.size();++i) { back.palette[i]=multiply(back.model[i],definition.inverse_bind_model[i]); back.previous_palette[i]=multiply(back.previous_model[i],definition.inverse_bind_model[i]); }
    state.front_slot=back_slot;
    state.view={instance,state.last_fixed_tick,frame_serial,{back.local.data(),uint32_t(back.local.size())},{back.model.data(),uint32_t(back.model.size())},{back.previous_model.data(),uint32_t(back.previous_model.size())},{back.palette.data(),uint32_t(back.palette.size())},{back.previous_palette.data(),uint32_t(back.previous_palette.size())}};
    return true;
}
bool AnimationEvaluator::capture_checkpoint(AnimatorInstanceHandle instance, AnimatorCheckpoint& out) const {
    const auto it=states_.find(key(instance));
    if(!instance.valid() || it==states_.end() || !it->second) return false;
    const State& state=*it->second;
    out.last_fixed_tick=state.last_fixed_tick;
    out.snapshot_frame_serial=state.view.frame_serial;
    out.previous_fixed_time=state.previous_fixed_time;
    out.current_fixed_time=state.current_fixed_time;
    if(!state.has_snapshot) {
        out.fixed_local_pose.clear(); out.fixed_model_pose.clear(); out.fixed_previous_model_pose.clear();
        out.fixed_skin_palette.clear(); out.fixed_previous_skin_palette.clear();
        return true;
    }
    const State::PoseBuffer& front=state.pose[state.front_slot];
    out.fixed_local_pose=front.local;
    out.fixed_model_pose=front.model;
    out.fixed_previous_model_pose=front.previous_model;
    out.fixed_skin_palette=front.palette;
    out.fixed_previous_skin_palette=front.previous_palette;
    return true;
}

bool AnimationEvaluator::validate_checkpoint(AnimatorInstanceHandle instance,
                                             const AnimationEvaluationDefinition& definition,
                                             const AnimatorCheckpoint& checkpoint) const {
    if(!instance.valid() || checkpoint.instance.slot_index!=instance.slot_index ||
       checkpoint.instance.generation!=instance.generation || !valid(definition) || !checkpoint.bounded()) return false;
    if(!std::isfinite(checkpoint.previous_fixed_time) || !std::isfinite(checkpoint.current_fixed_time)) return false;
    const size_t joints=definition.skeleton->joint_count();
    const bool empty=checkpoint.fixed_local_pose.empty() && checkpoint.fixed_model_pose.empty() &&
                     checkpoint.fixed_previous_model_pose.empty() && checkpoint.fixed_skin_palette.empty() &&
                     checkpoint.fixed_previous_skin_palette.empty();
    if(empty) return true;
    return checkpoint.fixed_local_pose.size()==joints && checkpoint.fixed_model_pose.size()==joints &&
           checkpoint.fixed_previous_model_pose.size()==joints && checkpoint.fixed_skin_palette.size()==joints &&
           checkpoint.fixed_previous_skin_palette.size()==joints;
}

bool AnimationEvaluator::restore_checkpoint(AnimatorInstanceHandle instance,
                                            const AnimationEvaluationDefinition& definition,
                                            const AnimatorCheckpoint& checkpoint) {
    if(!validate_checkpoint(instance,definition,checkpoint)) return false;
    auto replacement=std::make_unique<State>();
    replacement->shape={&definition,definition.skeleton,uint32_t(definition.skeleton->joint_count())};
    replacement->last_fixed_tick=checkpoint.last_fixed_tick;
    replacement->previous_fixed_time=checkpoint.previous_fixed_time;
    replacement->current_fixed_time=checkpoint.current_fixed_time;
    // A Play checkpoint can legitimately precede the first fixed evaluation.
    // Preserve that first-sample rule so the restored frame does not present a
    // fabricated previous pose at alpha zero.
    replacement->initialized=!checkpoint.fixed_local_pose.empty();
    if(!checkpoint.fixed_local_pose.empty()) {
        State::PoseBuffer& front=replacement->pose[0];
        front.local=checkpoint.fixed_local_pose;
        front.model=checkpoint.fixed_model_pose;
        front.previous_model=checkpoint.fixed_previous_model_pose;
        front.palette=checkpoint.fixed_skin_palette;
        front.previous_palette=checkpoint.fixed_previous_skin_palette;
        replacement->has_snapshot=true;
        replacement->front_slot=0;
        replacement->view={instance,checkpoint.last_fixed_tick,checkpoint.snapshot_frame_serial,
            {front.local.data(),uint32_t(front.local.size())},{front.model.data(),uint32_t(front.model.size())},
            {front.previous_model.data(),uint32_t(front.previous_model.size())},{front.palette.data(),uint32_t(front.palette.size())},
            {front.previous_palette.data(),uint32_t(front.previous_palette.size())}};
    }
    states_[key(instance)]=std::move(replacement);
    return true;
}
void AnimationEvaluator::forget(AnimatorInstanceHandle instance) { states_.erase(key(instance)); }

} // namespace matter::animation
