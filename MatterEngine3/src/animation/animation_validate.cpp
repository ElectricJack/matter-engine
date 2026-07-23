#include "animation/animation_validate.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>

namespace matter::animation {
namespace {

bool finite(float value) { return std::isfinite(value); }
bool finite3(const Float3& value) { return finite(value.x) && finite(value.y) && finite(value.z); }
Float3 rotate_inverse(const Quaternion& q, const Float3& v) {
    const Quaternion p{-q.x,-q.y,-q.z,q.w};
    const Quaternion t{q.w*v.x + q.y*v.z - q.z*v.y, q.w*v.y + q.z*v.x - q.x*v.z, q.w*v.z + q.x*v.y - q.y*v.x, -q.x*v.x-q.y*v.y-q.z*v.z};
    return {t.x*p.w + t.w*p.x + t.y*p.z - t.z*p.y, t.y*p.w + t.w*p.y + t.z*p.x - t.x*p.z, t.z*p.w + t.w*p.z + t.x*p.y - t.y*p.x};
}
Quaternion qmul_local(const Quaternion&a,const Quaternion&b){return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
Float3 qrotate_local(const Quaternion&q,const Float3&v){Quaternion p{v.x,v.y,v.z,0}; Quaternion qi{-q.x,-q.y,-q.z,q.w}; Quaternion t=qmul_local(q,p); Quaternion r=qmul_local(t,qi); return {r.x,r.y,r.z};}
Quaternion bind_model_rotation(const AnimationBuild& build,const std::vector<int>& parents,int index){if(index<0)return {0,0,0,1};const auto&q=build.rig.joints[(size_t)index].local.rotation;return parents[index]<0?q:qmul_local(bind_model_rotation(build,parents,parents[index]),q);}
Float3 bind_model_position(const AnimationBuild& build,const std::vector<int>& parents,int index){if(index<0)return {};const auto& j=build.rig.joints[(size_t)index];if(parents[index]<0)return j.local.translation;const int p=parents[index];const Float3 pt=bind_model_position(build,parents,p);const Float3 d=qrotate_local(bind_model_rotation(build,parents,p),j.local.translation);return {pt.x+d.x,pt.y+d.y,pt.z+d.z};}
bool finiteq(const Quaternion& value) { return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w); }
bool valid_cadence(EvaluationCadence cadence) { return cadence == EvaluationCadence::Fixed || cadence == EvaluationCadence::Frame; }
bool valid_input_type(AnimationValueType type) {
    return type == AnimationValueType::Bool || type == AnimationValueType::Number || type == AnimationValueType::Float3 ||
           type == AnimationValueType::Quaternion || type == AnimationValueType::Transform || type == AnimationValueType::Symbol;
}

bool valid_transform(const AnimationTransform& transform) {
    return finite3(transform.translation) && finiteq(transform.rotation) && finite3(transform.scale);
}

float quaternion_length_squared(const Quaternion& value);

bool valid_animation_value(const AnimationValue& value) {
    switch (value.type) {
    case AnimationValueType::Bool: return true;
    case AnimationValueType::Number: return std::isfinite(value.number);
    case AnimationValueType::Float3: return finite3(value.float3);
    case AnimationValueType::Quaternion: return finiteq(value.quaternion) && quaternion_length_squared(value.quaternion) > 1e-12f;
    case AnimationValueType::Transform: return valid_transform(value.transform) && quaternion_length_squared(value.transform.rotation) > 1e-12f;
    case AnimationValueType::Symbol: return !value.symbol.empty();
    }
    return false;
}

float quaternion_length_squared(const Quaternion& value) {
    return value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
}

template <typename Def>
void duplicate_names(const std::vector<Def>& values, const char* code, Diagnostics& diagnostics) {
    for (size_t i = 0; i < values.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (values[i].name == values[j].name) diagnostics.add(code, values[i].source, "duplicate authored name");
}

int find_joint(const AnimationBuild& build, const std::string& name) {
    for (size_t i = 0; i < build.rig.joints.size(); ++i)
        if (build.rig.joints[i].name == name) return static_cast<int>(i);
    return -1;
}

int find_controller(const AnimationBuild& build, const std::string& name) {
    for (size_t i = 0; i < build.controllers.size(); ++i)
        if (build.controllers[i].name == name) return static_cast<int>(i);
    return -1;
}

int find_graph_node(const MotionDefinition& graph, const std::string& name) {
    for (size_t i = 0; i < graph.nodes.size(); ++i)
        if (graph.nodes[i].name == name) return static_cast<int>(i);
    return -1;
}

void validate_transform(const AnimationTransform& transform, const SourceSpan& source, Diagnostics& diagnostics) {
    if (!valid_transform(transform)) diagnostics.add("non-finite-transform", source, "transform contains a non-finite value");
    if (finiteq(transform.rotation) && quaternion_length_squared(transform.rotation) <= 1e-12f)
        diagnostics.add("non-normalizable-rotation", source, "rotation cannot be normalized");
}

void append_string(std::ostringstream& output, const std::string& value) {
    output << value.size() << ':' << value << '|';
}

void append_transform(std::ostringstream& output, const AnimationTransform& value) {
    output << value.translation.x << ',' << value.translation.y << ',' << value.translation.z << ','
           << value.rotation.x << ',' << value.rotation.y << ',' << value.rotation.z << ',' << value.rotation.w << ','
           << value.scale.x << ',' << value.scale.y << ',' << value.scale.z << '|';
}

std::string encode_authored_state(const AnimationBuild& build) {
    std::ostringstream output;
    output << std::setprecision(9);
    for (const SocketDef& socket : build.rig.sockets) { append_string(output, socket.name); append_string(output, socket.joint); append_transform(output, socket.local); }
    for (const ClipDefinition& clip : build.clips) {
        append_string(output, clip.name); output << clip.duration << ',' << clip.rate << ',' << clip.loop << ',' << clip.additive << ',' << clip.ozz_blob.size() << '|';
        for (const ClipTrack& track : clip.tracks) { append_string(output, track.joint); for (const ClipKey& key : track.keys) { output << key.time << '|'; append_transform(output, key.value); } }
        for (const ClipMarker& marker : clip.markers) { append_string(output, marker.name); output << marker.time << '|'; }
    }
    for (const InputSchema& input : build.inputs) {
        append_string(output, input.name); output << static_cast<int>(input.type) << ',' << static_cast<int>(input.cadence) << ',' << static_cast<int>(input.default_value.type) << '|';
        const AnimationValue& value = input.default_value;
        switch (value.type) {
        case AnimationValueType::Bool: output << value.boolean; break;
        case AnimationValueType::Number: output << value.number; break;
        case AnimationValueType::Float3: output << value.float3.x << ',' << value.float3.y << ',' << value.float3.z; break;
        case AnimationValueType::Quaternion: output << value.quaternion.x << ',' << value.quaternion.y << ',' << value.quaternion.z << ',' << value.quaternion.w; break;
        case AnimationValueType::Transform: append_transform(output, value.transform); break;
        case AnimationValueType::Symbol: append_string(output, value.symbol); break;
        }
        output << '|';
    }
    for (const TargetSchema& target : build.targets) { append_string(output, target.name); append_string(output, target.start_joint); append_string(output, target.end_joint); append_string(output, target.controller); output << static_cast<int>(target.driver) << ',' << static_cast<int>(target.cadence) << ',' << target.has_pole << ',' << target.pole.x << ',' << target.pole.y << ',' << target.pole.z << ',' << target.soften << ',' << target.twist << ',' << target.position_half_life << ',' << target.rotation_half_life << ',' << target.weight_half_life << ',' << target.enabled << ',' << target.require_explicit_pole << '|'; }
    for (const ControllerDef& controller : build.controllers) { append_string(output, controller.name); append_string(output, controller.type); output << static_cast<int>(controller.cadence) << '|'; }
    for (const GraphNode& node : build.graph.nodes) { append_string(output, node.name); append_string(output, node.clip); append_string(output, node.input); append_string(output, node.controller); output << node.is_output << ',' << static_cast<int>(node.kind) << ',' << static_cast<int>(node.cadence) << '|'; for (float threshold : node.thresholds) output << threshold << ','; output << '|'; for (const std::string& dependency : node.dependencies) append_string(output, dependency); }
    for (const SkinBindingDef& binding : build.skin_bindings) { append_string(output, binding.name); for (const std::string& joint : binding.joints) append_string(output, joint); }
    for (const RigidBindingDef& binding : build.rigid_bindings) { append_string(output, binding.name); append_string(output, binding.joint); append_transform(output, binding.local); }
    for (const AttachmentDef& attachment : build.attachments) { append_string(output, attachment.name); append_string(output, attachment.socket); append_transform(output, attachment.local); }
    return output.str();
}

bool validate_impl(const AnimationBuild& build, Diagnostics& diagnostics, CanonicalAnimationBuild* canonical) {
    diagnostics.items.clear();
    if (build.rig.joints.size() > kMaxJoints) diagnostics.add("joint-limit", build.rig.source, "joint count exceeds v1 limit");
    if (build.targets.size() > kMaxTargets) diagnostics.add("target-limit", build.rig.source, "target count exceeds v1 limit");
    if (build.graph.nodes.size() > kMaxGraphNodes) diagnostics.add("graph-node-limit", build.graph.source, "graph node count exceeds v1 limit");
    if (build.controllers.size() > kMaxControllers || build.controllers.size() + build.graph.nodes.size() > kMaxGraphNodes)
        diagnostics.add("controller-limit", build.graph.source, "controller and graph node count exceeds v1 limit");

    duplicate_names(build.rig.joints, "duplicate-joint", diagnostics);
    duplicate_names(build.rig.sockets, "duplicate-socket", diagnostics);
    duplicate_names(build.clips, "duplicate-clip", diagnostics);
    duplicate_names(build.inputs, "duplicate-input", diagnostics);
    duplicate_names(build.targets, "duplicate-target", diagnostics);
    duplicate_names(build.controllers, "duplicate-controller", diagnostics);
    duplicate_names(build.graph.nodes, "duplicate-graph-node", diagnostics);

    std::vector<int> parents(build.rig.joints.size(), -1);
    uint32_t roots = 0;
    for (size_t i = 0; i < build.rig.joints.size(); ++i) {
        const JointDef& joint = build.rig.joints[i];
        validate_transform(joint.local, joint.source, diagnostics);
        if (!finite(joint.radius) || joint.radius <= 0.0f) diagnostics.add("invalid-joint-radius", joint.source, "joint radius must be finite and positive");
        if (joint.parent.empty()) { ++roots; continue; }
        parents[i] = find_joint(build, joint.parent);
        if (parents[i] < 0) diagnostics.add("missing-parent", joint.source, "joint parent is not declared");
        else if (parents[i] >= static_cast<int>(i)) diagnostics.add("parent-forward-reference", joint.source, "joint parent must be declared first");
    }
    if (roots != 1) diagnostics.add("multiple-roots", build.rig.source, "rig must declare exactly one root");
    for (size_t i = 0; i < parents.size(); ++i) {
        int slow = static_cast<int>(i), fast = static_cast<int>(i);
        do {
            slow = slow >= 0 ? parents[slow] : -1;
            fast = fast >= 0 ? parents[fast] : -1;
            fast = fast >= 0 ? parents[fast] : -1;
        } while (slow >= 0 && fast >= 0 && slow != fast);
        if (slow >= 0 && slow == fast) { diagnostics.add("joint-cycle", build.rig.joints[i].source, "joint ancestry contains a cycle"); break; }
    }
    for (const SocketDef& socket : build.rig.sockets) {
        validate_transform(socket.local, socket.source, diagnostics);
        if (find_joint(build, socket.joint) < 0) diagnostics.add("missing-socket-joint", socket.source, "socket joint is not declared");
    }

    for (const ClipDefinition& clip : build.clips) {
        if (!finite(clip.duration) || clip.duration <= 0.0f) diagnostics.add("invalid-clip-duration", clip.source, "clip duration must be finite and positive");
        if (!finite(clip.rate) || clip.rate <= 0.0f) diagnostics.add("invalid-clip-rate", clip.source, "clip rate must be finite and positive");
        for (const ClipTrack& track : clip.tracks) {
            if (find_joint(build, track.joint) < 0) diagnostics.add("missing-track-joint", track.source, "clip track joint is not declared");
            float prior = -INFINITY;
            for (const ClipKey& key : track.keys) {
                if (!finite(key.time) || key.time < 0.0f || (finite(clip.duration) && key.time > clip.duration)) diagnostics.add("key-out-of-range", key.source, "clip key time is outside duration");
                if (key.time < prior) diagnostics.add("nonmonotonic-key", key.source, "clip keys must be monotonic");
                prior = key.time;
                validate_transform(key.value, key.source, diagnostics);
            }
        }
        for (const ClipMarker& marker : clip.markers)
            if (!finite(marker.time) || marker.time < 0.0f || marker.time >= 1.0f) diagnostics.add("marker-out-of-range", marker.source, "marker time must be normalized in [0,1)");
    }

    for (const InputSchema& input : build.inputs) {
        if (!valid_input_type(input.type)) diagnostics.add("unsupported-input-type", input.source, "input uses an unsupported value type");
        else if (input.type != input.default_value.type) diagnostics.add("input-default-type", input.source, "input default does not match declared type");
        else if (!valid_animation_value(input.default_value)) diagnostics.add("non-finite-input-default", input.source, "input default is invalid for its type");
        if (!valid_cadence(input.cadence)) diagnostics.add("invalid-cadence", input.source, "input cadence is unsupported");
    }
    for (const ControllerDef& controller : build.controllers) {
        if (controller.type.empty()) diagnostics.add("invalid-controller-type", controller.source, "controller type must identify a native controller");
        if (!valid_cadence(controller.cadence)) diagnostics.add("invalid-cadence", controller.source, "controller cadence is unsupported");
    }
    for (const SkinBindingDef& binding : build.skin_bindings) {
        if (binding.joints.size() > kMaxSkinInfluences) diagnostics.add("skin-influence-limit", binding.source, "skin binding exceeds four influences");
        for (const std::string& joint : binding.joints)
            if (find_joint(build, joint) < 0) diagnostics.add("missing-skin-joint", binding.source, "skin binding joint is not declared");
    }
    for (const RigidBindingDef& binding : build.rigid_bindings) {
        if (find_joint(build, binding.joint) < 0) diagnostics.add("missing-rigid-joint", binding.source, "rigid binding joint is not declared");
        validate_transform(binding.local, binding.source, diagnostics);
    }
    for (const AttachmentDef& attachment : build.attachments) {
        bool found = false;
        for (const SocketDef& socket : build.rig.sockets) if (socket.name == attachment.socket) found = true;
        if (!found) diagnostics.add("missing-attachment-socket", attachment.source, "attachment socket is not declared");
        validate_transform(attachment.local, attachment.source, diagnostics);
    }

    std::vector<std::vector<JointIndex>> chains;
    for (const TargetSchema& target : build.targets) {
        if (!valid_cadence(target.cadence)) diagnostics.add("invalid-cadence", target.source, "target cadence is unsupported");
        if (target.driver != TargetDriverKind::External && target.driver != TargetDriverKind::Controller)
            diagnostics.add("invalid-target-driver", target.source, "target must have exactly one supported driver");
        const int controller = find_controller(build, target.controller);
        if (target.driver == TargetDriverKind::Controller && controller < 0)
            diagnostics.add("bad-controller-reference", target.source, "controller target references an unknown controller");
        else if (target.driver == TargetDriverKind::Controller && build.controllers[controller].cadence == EvaluationCadence::Frame && target.cadence == EvaluationCadence::Fixed)
            diagnostics.add("frame-controller-to-fixed-target", target.source, "fixed target cannot depend on a frame controller");
        if (target.driver == TargetDriverKind::External && !target.controller.empty())
            diagnostics.add("multiple-target-drivers", target.source, "external target cannot also name a controller");
        if (!finite(target.soften) || target.soften < 0.0f || target.soften > 1.0f) diagnostics.add("invalid-target-soften", target.source, "target soften must be finite in [0,1]");
        if (!finite(target.twist)) diagnostics.add("invalid-target-twist", target.source, "target twist must be finite");
        if (!finite(target.position_half_life) || target.position_half_life < 0.0f ||
            !finite(target.rotation_half_life) || target.rotation_half_life < 0.0f ||
            !finite(target.weight_half_life) || target.weight_half_life < 0.0f)
            diagnostics.add("invalid-target-half-life", target.source, "target half-lives must be finite and non-negative");
        if (target.has_pole && !finite3(target.pole)) diagnostics.add("invalid-target-pole", target.source, "target pole must be finite");
        const int start = find_joint(build, target.start_joint), end = find_joint(build, target.end_joint);
        if (start < 0) diagnostics.add("missing-target-start-joint", target.source, "target start joint is not declared");
        if (end < 0) diagnostics.add("missing-target-end-joint", target.source, "target end joint is not declared");
        if (start < 0 || end < 0) continue;
        std::vector<JointIndex> chain;
        for (int current = end; current >= 0; current = parents[current]) {
            chain.push_back(static_cast<JointIndex>(current));
            if (current == start) break;
        }
        if (chain.empty() || chain.back() != static_cast<JointIndex>(start)) { diagnostics.add("target-not-descendant", target.source, "target end must descend from start"); continue; }
        std::reverse(chain.begin(), chain.end());
        if (chain.size() != 3) diagnostics.add("target-chain-length", target.source, "v1 target chain must contain exactly three joints");
        for (size_t i = 1; i < chain.size(); ++i) {
            const Float3& segment = build.rig.joints[chain[i]].local.translation;
            if (finite3(segment) && segment.x * segment.x + segment.y * segment.y + segment.z * segment.z <= 1e-12f)
                diagnostics.add("degenerate-target-segment", build.rig.joints[chain[i]].source, "target segment must be non-degenerate");
        }
        if (chain.size() == 3 && !target.has_pole) {
            const int startAuth=(int)chain[0], midAuth=(int)chain[1], endAuth=(int)chain[2];
            const Float3 startPos=bind_model_position(build,parents,startAuth), midPos=bind_model_position(build,parents,midAuth), endPos=bind_model_position(build,parents,endAuth);
            const Float3 a=rotate_inverse(build.rig.joints[startAuth].local.rotation,{midPos.x-startPos.x,midPos.y-startPos.y,midPos.z-startPos.z});
            const Float3 b=rotate_inverse(build.rig.joints[startAuth].local.rotation,{endPos.x-startPos.x,endPos.y-startPos.y,endPos.z-startPos.z});
            const Float3 cross{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
            if (cross.x*cross.x + cross.y*cross.y + cross.z*cross.z <= 1e-12f)
                diagnostics.add("collinear-target-pole-required", target.source, "collinear target chain requires an explicit pole");
        }
        for (const std::vector<JointIndex>& other : chains) {
            bool overlap = false;
            for (JointIndex left : chain) for (JointIndex right : other) if (left == right) overlap = true;
            if (overlap) { diagnostics.add("overlapping-target-chain", target.source, "writable target chains overlap"); diagnostics.add("multiple-target-drivers", target.source, "joint has more than one target driver"); break; }
        }
        chains.push_back(chain);
    }

    uint32_t outputs = 0;
    std::vector<std::vector<uint16_t>> outgoing(build.graph.nodes.size());
    std::vector<uint16_t> indegree(build.graph.nodes.size(), 0);
    for (size_t i = 0; i < build.graph.nodes.size(); ++i) {
        const GraphNode& node = build.graph.nodes[i];
        const bool synthetic = node.name.rfind("__", 0) == 0;
        const int kind = static_cast<int>(node.kind);
        if (kind < static_cast<int>(GraphNodeKind::Clip) || kind > static_cast<int>(GraphNodeKind::Output)) {
            diagnostics.add("invalid-graph-kind", node.source, "graph node kind is unsupported");
        } else if (!synthetic) {
            switch (node.kind) {
            case GraphNodeKind::Clip: {
                bool found = !node.clip.empty();
                for (const auto& clip : build.clips) if (clip.name == node.clip) found = true;
                if (node.clip.empty() || !found) diagnostics.add("missing-clip-reference", node.source, "clip graph node requires a declared clip");
                if (!node.dependencies.empty() || !node.input.empty() || !node.thresholds.empty() || !node.controller.empty()) diagnostics.add("clip-fields", node.source, "clip node has unsupported fields");
                break;
            }
            case GraphNodeKind::Blend1D: {
                int input = -1;
                for (size_t j = 0; j < build.inputs.size(); ++j) if (build.inputs[j].name == node.input) input = static_cast<int>(j);
                if (input < 0) diagnostics.add("missing-blend-input", node.source, "blend1D requires a declared input");
                else {
                    if (build.inputs[input].type != AnimationValueType::Number) diagnostics.add("blend-input-type", node.source, "blend1D input must be numeric");
                    if (build.inputs[input].cadence == EvaluationCadence::Frame && node.cadence == EvaluationCadence::Fixed)
                        diagnostics.add("frame-to-fixed-dependency", node.source, "fixed graph node cannot depend on frame data");
                }
                if (node.dependencies.size() < 2 || node.thresholds.size() != node.dependencies.size()) diagnostics.add("blend1d-arity", node.source, "blend1D thresholds must match at least two inputs");
                for (size_t j = 0; j < node.thresholds.size(); ++j) if (!finite(node.thresholds[j]) || (j && node.thresholds[j] <= node.thresholds[j - 1])) diagnostics.add("blend1d-thresholds", node.source, "blend1D thresholds must be finite and strictly ordered");
                if (!node.clip.empty() || !node.controller.empty()) diagnostics.add("blend1d-fields", node.source, "blend1D node has unsupported fields");
                break;
            }
            case GraphNodeKind::Additive:
                if (node.dependencies.size() != 2) diagnostics.add("additive-arity", node.source, "additive node requires exactly two inputs");
                if (!node.input.empty() || !node.clip.empty() || !node.thresholds.empty() || !node.controller.empty()) diagnostics.add("additive-fields", node.source, "additive node has unsupported fields");
                break;
            case GraphNodeKind::NativeController:
                if (node.controller.empty() || find_controller(build, node.controller) < 0) diagnostics.add("missing-controller-reference", node.source, "native controller node requires a declared controller");
                else if (build.controllers[find_controller(build, node.controller)].cadence == EvaluationCadence::Frame && node.cadence == EvaluationCadence::Fixed)
                    diagnostics.add("frame-to-fixed-dependency", node.source, "fixed graph node cannot depend on frame data");
                if (node.dependencies.size() > 1) diagnostics.add("native-controller-arity", node.source, "native controller node accepts at most one input");
                if (!node.input.empty() || !node.clip.empty() || !node.thresholds.empty()) diagnostics.add("native-controller-fields", node.source, "native controller node has unsupported fields");
                break;
            case GraphNodeKind::Output:
                if (!node.is_output) diagnostics.add("graph-output-flag", node.source, "output node must be marked output");
                if (node.dependencies.size() != 1) diagnostics.add("output-arity", node.source, "output node requires exactly one dependency");
                if (!node.input.empty() || !node.clip.empty() || !node.thresholds.empty() || !node.controller.empty()) diagnostics.add("output-fields", node.source, "output node has unsupported fields");
                break;
            }
            if (node.kind != GraphNodeKind::Output && node.is_output) diagnostics.add("graph-output-kind", node.source, "only output nodes may be marked output");
            if (node.is_output) ++outputs;
        } else if (node.kind == GraphNodeKind::Output && node.is_output) {
            ++outputs;
        }
        if (!valid_cadence(node.cadence)) diagnostics.add("invalid-cadence", node.source, "graph cadence is unsupported");
        for (const std::string& dependency : node.dependencies) {
            const int dep = find_graph_node(build.graph, dependency);
            if (dep < 0) { diagnostics.add("missing-graph-reference", node.source, "graph dependency is not declared"); continue; }
            outgoing[dep].push_back(static_cast<uint16_t>(i)); ++indegree[i];
            if (build.graph.nodes[dep].cadence == EvaluationCadence::Frame && node.cadence == EvaluationCadence::Fixed)
                diagnostics.add("frame-to-fixed-dependency", node.source, "fixed graph node cannot depend on frame data");
        }
    }
    if (outputs == 0) diagnostics.add("missing-graph-output", build.graph.source, "graph must declare one output");
    if (outputs > 1) diagnostics.add("multiple-graph-output", build.graph.source, "graph must declare exactly one output");
    std::vector<uint16_t> order;
    std::vector<uint16_t> pending = indegree;
    for (size_t pass = 0; pass < build.graph.nodes.size(); ++pass) {
        int next = -1;
        for (size_t i = 0; i < pending.size(); ++i) if (pending[i] == 0) { next = static_cast<int>(i); break; }
        if (next < 0) break;
        pending[next] = UINT16_MAX; order.push_back(static_cast<uint16_t>(next));
        for (uint16_t child : outgoing[next]) --pending[child];
    }
    if (order.size() != build.graph.nodes.size()) diagnostics.add("graph-cycle", build.graph.source, "graph contains a cycle");
    if (!build.graph.nodes.empty()) {
        std::vector<bool> used(build.graph.nodes.size(), false); std::vector<uint16_t> todo;
        for(size_t i=0;i<build.graph.nodes.size();++i) if(build.graph.nodes[i].is_output) todo.push_back((uint16_t)i);
        while(!todo.empty()){const uint16_t x=todo.back();todo.pop_back();if(used[x])continue;used[x]=true;for(const auto& dep:build.graph.nodes[x].dependencies){int d=find_graph_node(build.graph,dep);if(d>=0)todo.push_back((uint16_t)d);}}
        for(size_t i=0;i<used.size();++i) if(!used[i]) diagnostics.add("unused-graph-node", build.graph.nodes[i].source, "graph node is not reachable from output");
    }

    diagnostics.sort();
    if (!diagnostics.items.empty()) return false;
    if (!canonical) return true;
    canonical->rig.joints.clear(); canonical->rig.sockets.clear(); canonical->targets.clear(); canonical->graph_order = order; canonical->authored_state = encode_authored_state(build);
    std::vector<std::vector<size_t>> children(build.rig.joints.size());
    size_t root = 0;
    for (size_t i = 0; i < parents.size(); ++i) { if (parents[i] < 0) root = i; else children[parents[i]].push_back(i); }
    std::function<void(size_t, JointIndex)> visit = [&](size_t authored, JointIndex parent) {
        const JointIndex index = static_cast<JointIndex>(canonical->rig.joints.size());
        canonical->rig.joints.push_back({build.rig.joints[authored].name, parent, build.rig.joints[authored].local, build.rig.joints[authored].radius, {index, index}, build.rig.joints[authored].source});
        for (size_t child : children[authored]) visit(child, index);
        canonical->rig.joints[index].subtree.end = static_cast<JointIndex>(canonical->rig.joints.size());
    };
    visit(root, kInvalidJoint);
    for (const SocketDef& socket : build.rig.sockets) {
        JointIndex joint = kInvalidJoint;
        for (size_t i = 0; i < canonical->rig.joints.size(); ++i)
            if (canonical->rig.joints[i].name == socket.joint) { joint = static_cast<JointIndex>(i); break; }
        canonical->rig.sockets.push_back({socket.name, joint, socket.local, socket.source});
    }
    for (size_t i = 0; i < build.targets.size(); ++i) {
        CanonicalTarget target; target.name = build.targets[i].name; target.driver = build.targets[i].driver; target.cadence = build.targets[i].cadence; target.controller=build.targets[i].controller; target.pole=build.targets[i].pole; target.has_pole=build.targets[i].has_pole; target.soften=build.targets[i].soften; target.twist=build.targets[i].twist; target.position_half_life=build.targets[i].position_half_life; target.rotation_half_life=build.targets[i].rotation_half_life; target.weight_half_life=build.targets[i].weight_half_life; target.enabled=build.targets[i].enabled;
        for (const std::string& name : {build.targets[i].start_joint, build.rig.joints[find_joint(build, build.targets[i].end_joint)].parent, build.targets[i].end_joint}) {
            for (size_t joint = 0; joint < canonical->rig.joints.size(); ++joint) if (canonical->rig.joints[joint].name == name) target.chain.push_back(static_cast<JointIndex>(joint));
        }
        if (target.chain.size() == 3) {
            const int endAuth=find_joint(build, build.targets[i].end_joint); const int midAuth=endAuth>=0?parents[endAuth]:-1;
            const Float3 startPos=bind_model_position(build,parents,find_joint(build,build.targets[i].start_joint));
            const Float3 midPos=bind_model_position(build,parents,midAuth), endPos=bind_model_position(build,parents,endAuth);
            const int startIndex=find_joint(build,build.targets[i].start_joint); const Quaternion startRot=startIndex>=0?bind_model_rotation(build,parents,startIndex):Quaternion{0,0,0,1};
            const Float3 a=startIndex>=0?rotate_inverse(startRot,{midPos.x-startPos.x,midPos.y-startPos.y,midPos.z-startPos.z}):Float3{};
            const Float3 b=startIndex>=0?rotate_inverse(startRot,{endPos.x-startPos.x,endPos.y-startPos.y,endPos.z-startPos.z}):Float3{};
            Float3 axis{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
            const float len=std::sqrt(axis.x*axis.x+axis.y*axis.y+axis.z*axis.z);
            if (len > 1e-6f) { axis.x/=len; axis.y/=len; axis.z/=len; target.bend_axis=axis; }
            if (target.has_pole) { const float pn=std::sqrt(target.pole.x*target.pole.x+target.pole.y*target.pole.y+target.pole.z*target.pole.z); if(pn>1e-6f){target.pole.x/=pn;target.pole.y/=pn;target.pole.z/=pn; int rootAuth=-1; for(size_t ri=0;ri<parents.size();++ri)if(parents[ri]<0){rootAuth=(int)ri;break;} if(rootAuth>=0) target.pole=rotate_inverse(build.rig.joints[rootAuth].local.rotation,target.pole);} }
        }
        canonical->targets.push_back(target);
    }
    return true;
}

} // namespace

bool validate_animation_build(const AnimationBuild& build, Diagnostics& diagnostics) { return validate_impl(build, diagnostics, nullptr); }
bool validate_and_canonicalize_animation_build(const AnimationBuild& build, CanonicalAnimationBuild& canonical, Diagnostics& diagnostics) { return validate_impl(build, diagnostics, &canonical); }

} // namespace matter::animation
