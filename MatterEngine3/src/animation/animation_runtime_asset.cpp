#include "animation/animation_runtime_asset.h"

#include "animation/animation_binding_bake.h"
#include "animation/animation_controllers.h"
#include "animation/animation_evaluator.h"
#include "animation/ozz_adapter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace matter::animation {
namespace {

constexpr uint32_t kRuntimeSectionVersion = 1;
constexpr uint32_t kMaxSerializedString = 64u * 1024u;
constexpr uint32_t kMaxSerializedClipBytes = 64u * 1024u * 1024u;

void put_u8(std::vector<uint8_t>& bytes, uint8_t value) { bytes.push_back(value); }
void put_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(uint8_t(value));
    bytes.push_back(uint8_t(value >> 8));
}
void put_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int shift = 0; shift != 32; shift += 8) bytes.push_back(uint8_t(value >> shift));
}
void put_u64(std::vector<uint8_t>& bytes, uint64_t value) {
    for (int shift = 0; shift != 64; shift += 8) bytes.push_back(uint8_t(value >> shift));
}
void put_float(std::vector<uint8_t>& bytes, float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    put_u32(bytes, raw);
}
void put_string(std::vector<uint8_t>& bytes, const std::string& value) {
    put_u32(bytes, static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
void put_float3(std::vector<uint8_t>& bytes, Float3 value) {
    put_float(bytes, value.x);
    put_float(bytes, value.y);
    put_float(bytes, value.z);
}
void put_quaternion(std::vector<uint8_t>& bytes, Quaternion value) {
    put_float(bytes, value.x);
    put_float(bytes, value.y);
    put_float(bytes, value.z);
    put_float(bytes, value.w);
}
void put_transform(std::vector<uint8_t>& bytes, const AnimationTransform& value) {
    put_float3(bytes, value.translation);
    put_quaternion(bytes, value.rotation);
    put_float3(bytes, value.scale);
}
void put_tag(std::vector<uint8_t>& bytes, const char (&tag)[5]) {
    bytes.insert(bytes.end(), tag, tag + 4);
}

bool take_u8(const std::vector<uint8_t>& bytes, size_t& at, uint8_t& value) {
    if (at >= bytes.size()) return false;
    value = bytes[at++];
    return true;
}
bool take_u16(const std::vector<uint8_t>& bytes, size_t& at, uint16_t& value) {
    if (at > bytes.size() || bytes.size() - at < 2) return false;
    value = uint16_t(bytes[at]) | uint16_t(bytes[at + 1]) << 8;
    at += 2;
    return true;
}
bool take_u32(const std::vector<uint8_t>& bytes, size_t& at, uint32_t& value) {
    if (at > bytes.size() || bytes.size() - at < 4) return false;
    value = 0;
    for (int shift = 0; shift != 32; shift += 8) value |= uint32_t(bytes[at++]) << shift;
    return true;
}
bool take_u64(const std::vector<uint8_t>& bytes, size_t& at, uint64_t& value) {
    if (at > bytes.size() || bytes.size() - at < 8) return false;
    value = 0;
    for (int shift = 0; shift != 64; shift += 8) value |= uint64_t(bytes[at++]) << shift;
    return true;
}
bool take_float(const std::vector<uint8_t>& bytes, size_t& at, float& value) {
    uint32_t raw = 0;
    if (!take_u32(bytes, at, raw)) return false;
    std::memcpy(&value, &raw, sizeof(value));
    return std::isfinite(value);
}
bool take_string(const std::vector<uint8_t>& bytes, size_t& at, std::string& value) {
    uint32_t count = 0;
    if (!take_u32(bytes, at, count) || count > kMaxSerializedString ||
        at > bytes.size() || count > bytes.size() - at) return false;
    value.assign(reinterpret_cast<const char*>(bytes.data() + at), count);
    at += count;
    return true;
}
bool take_float3(const std::vector<uint8_t>& bytes, size_t& at, Float3& value) {
    return take_float(bytes, at, value.x) && take_float(bytes, at, value.y) &&
           take_float(bytes, at, value.z);
}
bool take_quaternion(const std::vector<uint8_t>& bytes, size_t& at, Quaternion& value) {
    return take_float(bytes, at, value.x) && take_float(bytes, at, value.y) &&
           take_float(bytes, at, value.z) && take_float(bytes, at, value.w);
}
bool take_transform(const std::vector<uint8_t>& bytes, size_t& at,
                    AnimationTransform& value) {
    if (!take_float3(bytes, at, value.translation) ||
        !take_quaternion(bytes, at, value.rotation) ||
        !take_float3(bytes, at, value.scale)) return false;
    const float q2 = value.rotation.x * value.rotation.x +
                     value.rotation.y * value.rotation.y +
                     value.rotation.z * value.rotation.z +
                     value.rotation.w * value.rotation.w;
    return q2 > 1e-12f && value.scale.x > 0.0f && value.scale.y > 0.0f &&
           value.scale.z > 0.0f;
}
bool take_tag_version(const std::vector<uint8_t>& bytes, size_t& at,
                      const char (&tag)[5]) {
    if (bytes.size() < 8 || std::memcmp(bytes.data(), tag, 4) != 0) return false;
    at = 4;
    uint32_t version = 0;
    return take_u32(bytes, at, version) && version == kRuntimeSectionVersion;
}

void add_error(Diagnostics& diagnostics, const char* code, const char* message) {
    diagnostics.add(code, {}, message);
    diagnostics.sort();
}

bool enum_value(AnimationValueType value) {
    return value >= AnimationValueType::Bool && value <= AnimationValueType::Symbol;
}
bool enum_cadence(EvaluationCadence value) {
    return value == EvaluationCadence::Fixed || value == EvaluationCadence::Frame;
}
bool enum_driver(TargetDriverKind value) {
    return value == TargetDriverKind::External || value == TargetDriverKind::Controller;
}
bool enum_node(GraphNodeKind value) {
    return value >= GraphNodeKind::Clip && value <= GraphNodeKind::Output;
}
bool finite_transform(const AnimationTransform& value) {
    const float q2 = value.rotation.x * value.rotation.x +
                     value.rotation.y * value.rotation.y +
                     value.rotation.z * value.rotation.z +
                     value.rotation.w * value.rotation.w;
    return std::isfinite(value.translation.x) && std::isfinite(value.translation.y) &&
           std::isfinite(value.translation.z) && std::isfinite(q2) && q2 > 1e-12f &&
           std::isfinite(value.scale.x) && std::isfinite(value.scale.y) &&
           std::isfinite(value.scale.z) && value.scale.x > 0.0f &&
           value.scale.y > 0.0f && value.scale.z > 0.0f;
}

bool put_value(std::vector<uint8_t>& bytes, const AnimationValue& value) {
    if (!enum_value(value.type)) return false;
    put_u8(bytes, static_cast<uint8_t>(value.type));
    switch (value.type) {
    case AnimationValueType::Bool:
        put_u8(bytes, value.boolean ? 1 : 0);
        return true;
    case AnimationValueType::Number:
        if (!std::isfinite(value.number)) return false;
        {
            uint64_t raw = 0;
            std::memcpy(&raw, &value.number, sizeof(raw));
            put_u64(bytes, raw);
        }
        return true;
    case AnimationValueType::Float3:
        put_float3(bytes, value.float3);
        return std::isfinite(value.float3.x) && std::isfinite(value.float3.y) &&
               std::isfinite(value.float3.z);
    case AnimationValueType::Quaternion:
        put_quaternion(bytes, value.quaternion);
        return std::isfinite(value.quaternion.x) && std::isfinite(value.quaternion.y) &&
               std::isfinite(value.quaternion.z) && std::isfinite(value.quaternion.w);
    case AnimationValueType::Transform:
        put_transform(bytes, value.transform);
        return finite_transform(value.transform);
    case AnimationValueType::Symbol:
        if (value.symbol.size() > kMaxSerializedString) return false;
        put_string(bytes, value.symbol);
        return true;
    }
    return false;
}

bool take_value(const std::vector<uint8_t>& bytes, size_t& at, AnimationValue& value) {
    uint8_t type = 0;
    if (!take_u8(bytes, at, type) || type > static_cast<uint8_t>(AnimationValueType::Symbol))
        return false;
    value = {};
    value.type = static_cast<AnimationValueType>(type);
    switch (value.type) {
    case AnimationValueType::Bool: {
        uint8_t boolean = 0;
        if (!take_u8(bytes, at, boolean) || boolean > 1) return false;
        value.boolean = boolean != 0;
        return true;
    }
    case AnimationValueType::Number: {
        uint64_t raw = 0;
        if (!take_u64(bytes, at, raw)) return false;
        std::memcpy(&value.number, &raw, sizeof(value.number));
        return std::isfinite(value.number);
    }
    case AnimationValueType::Float3:
        return take_float3(bytes, at, value.float3);
    case AnimationValueType::Quaternion:
        return take_quaternion(bytes, at, value.quaternion);
    case AnimationValueType::Transform:
        return take_transform(bytes, at, value.transform);
    case AnimationValueType::Symbol:
        return take_string(bytes, at, value.symbol);
    }
    return false;
}

const AnimSection* unique_section(const AnimAsset& asset, AnimSectionKind kind) {
    const AnimSection* result = nullptr;
    for (const AnimSection& section : asset.sections) {
        if (section.kind != kind) continue;
        if (result) return nullptr;
        result = &section;
    }
    return result;
}

void set_section(AnimAsset& asset, AnimSectionKind kind, std::vector<uint8_t> bytes) {
    for (AnimSection& section : asset.sections) {
        if (section.kind != kind) continue;
        section.bytes = std::move(bytes);
        return;
    }
    asset.sections.push_back({kind, std::move(bytes)});
}

int find_name(const std::vector<ClipDefinition>& values, const std::string& name) {
    for (size_t i = 0; i < values.size(); ++i)
        if (values[i].name == name) return static_cast<int>(i);
    return -1;
}
int find_name(const std::vector<InputSchema>& values, const std::string& name) {
    for (size_t i = 0; i < values.size(); ++i)
        if (values[i].name == name) return static_cast<int>(i);
    return -1;
}
int find_name(const std::vector<ControllerDef>& values, const std::string& name) {
    for (size_t i = 0; i < values.size(); ++i)
        if (values[i].name == name) return static_cast<int>(i);
    return -1;
}
int find_name(const std::vector<GraphNode>& values, const std::string& name) {
    for (size_t i = 0; i < values.size(); ++i)
        if (values[i].name == name) return static_cast<int>(i);
    return -1;
}

bool encode_rig(const CanonicalRig& rig, std::vector<uint8_t>& bytes) {
    if (rig.joints.empty() || rig.joints.size() > kMaxJoints ||
        rig.sockets.size() > kMaxJoints) return false;
    put_tag(bytes, "RIG3");
    put_u32(bytes, kRuntimeSectionVersion);
    put_u32(bytes, static_cast<uint32_t>(rig.joints.size()));
    put_u32(bytes, static_cast<uint32_t>(rig.sockets.size()));
    for (const CanonicalJoint& joint : rig.joints) {
        if (joint.name.empty() || joint.name.size() > kMaxSerializedString ||
            !finite_transform(joint.local) || !std::isfinite(joint.radius) ||
            joint.radius <= 0.0f) return false;
        put_string(bytes, joint.name);
        put_u16(bytes, joint.parent);
        put_u16(bytes, joint.subtree.begin);
        put_u16(bytes, joint.subtree.end);
        put_transform(bytes, joint.local);
        put_float(bytes, joint.radius);
    }
    for (const CanonicalSocket& socket : rig.sockets) {
        if (socket.name.empty() || socket.name.size() > kMaxSerializedString ||
            socket.joint >= rig.joints.size() || !finite_transform(socket.local)) return false;
        put_string(bytes, socket.name);
        put_u16(bytes, socket.joint);
        put_transform(bytes, socket.local);
    }
    return true;
}

bool encode_inputs_targets(const AnimationBuild& authored,
                           const CanonicalAnimationBuild& canonical,
                           std::vector<uint8_t>& bytes) {
    if (authored.inputs.size() > kMaxGraphNodes || canonical.targets.size() > kMaxTargets)
        return false;
    put_tag(bytes, "ITS3");
    put_u32(bytes, kRuntimeSectionVersion);
    put_u32(bytes, static_cast<uint32_t>(authored.inputs.size()));
    put_u32(bytes, static_cast<uint32_t>(canonical.targets.size()));
    for (const InputSchema& input : authored.inputs) {
        if (input.name.empty() || input.name.size() > kMaxSerializedString ||
            !enum_value(input.type) || !enum_cadence(input.cadence) ||
            input.default_value.type != input.type) return false;
        put_string(bytes, input.name);
        put_u8(bytes, static_cast<uint8_t>(input.type));
        put_u8(bytes, static_cast<uint8_t>(input.cadence));
        if (!put_value(bytes, input.default_value)) return false;
    }
    for (const CanonicalTarget& target : canonical.targets) {
        if (target.name.empty() || target.name.size() > kMaxSerializedString ||
            target.controller.size() > kMaxSerializedString || target.chain.size() != 3 ||
            !enum_driver(target.driver) || !enum_cadence(target.cadence)) return false;
        put_string(bytes, target.name);
        put_u8(bytes, static_cast<uint8_t>(target.driver));
        put_u8(bytes, static_cast<uint8_t>(target.cadence));
        put_string(bytes, target.controller);
        put_u8(bytes, target.enabled ? 1 : 0);
        put_u8(bytes, target.has_pole ? 1 : 0);
        put_float3(bytes, target.pole);
        put_float3(bytes, target.bend_axis);
        put_float(bytes, target.soften);
        put_float(bytes, target.twist);
        put_float(bytes, target.position_half_life);
        put_float(bytes, target.rotation_half_life);
        put_float(bytes, target.weight_half_life);
        put_u16(bytes, static_cast<uint16_t>(target.chain.size()));
        for (JointIndex joint : target.chain) put_u16(bytes, joint);
    }
    return true;
}

bool encode_graph(const AnimationBuild& authored,
                  const CanonicalAnimationBuild& canonical,
                  std::vector<uint8_t>& bytes) {
    if (authored.graph.nodes.empty() || authored.graph.nodes.size() > kMaxGraphNodes ||
        authored.controllers.size() > kMaxControllers ||
        canonical.graph_order.size() != authored.graph.nodes.size()) return false;
    put_tag(bytes, "AGC3");
    put_u32(bytes, kRuntimeSectionVersion);
    put_u32(bytes, static_cast<uint32_t>(authored.graph.nodes.size()));
    put_u32(bytes, static_cast<uint32_t>(authored.controllers.size()));
    std::vector<uint16_t> runtime_index(authored.graph.nodes.size(), UINT16_MAX);
    for (size_t i = 0; i < canonical.graph_order.size(); ++i) {
        if (canonical.graph_order[i] >= authored.graph.nodes.size()) return false;
        runtime_index[canonical.graph_order[i]] = static_cast<uint16_t>(i);
    }
    for (uint16_t authored_index : canonical.graph_order) {
        const GraphNode& node = authored.graph.nodes[authored_index];
        if (!enum_node(node.kind) || !enum_cadence(node.cadence) ||
            node.dependencies.size() > kMaxGraphNodes ||
            node.thresholds.size() > kMaxGraphNodes) return false;
        put_u8(bytes, static_cast<uint8_t>(node.kind));
        put_u8(bytes, static_cast<uint8_t>(node.cadence));
        put_u16(bytes, static_cast<uint16_t>(node.dependencies.size()));
        for (const std::string& dependency : node.dependencies) {
            const int dependency_index = find_name(authored.graph.nodes, dependency);
            if (dependency_index < 0 ||
                runtime_index[dependency_index] >= runtime_index[authored_index]) return false;
            put_u16(bytes, runtime_index[dependency_index]);
        }
        const int clip_index = node.clip.empty() ? -1 : find_name(authored.clips, node.clip);
        const int input_index = node.input.empty() ? -1 : find_name(authored.inputs, node.input);
        const int controller_index =
            node.controller.empty() ? -1 : find_name(authored.controllers, node.controller);
        if ((!node.clip.empty() && clip_index < 0) ||
            (!node.input.empty() && input_index < 0) ||
            (!node.controller.empty() && controller_index < 0)) return false;
        put_u16(bytes, clip_index < 0 ? UINT16_MAX : static_cast<uint16_t>(clip_index));
        put_u16(bytes, input_index < 0 ? UINT16_MAX : static_cast<uint16_t>(input_index));
        put_u16(bytes, controller_index < 0 ? UINT16_MAX : static_cast<uint16_t>(controller_index));
        put_u16(bytes, static_cast<uint16_t>(node.thresholds.size()));
        for (float threshold : node.thresholds) {
            if (!std::isfinite(threshold)) return false;
            put_float(bytes, threshold);
        }
    }
    for (const ControllerDef& controller : authored.controllers) {
        if (controller.name.empty() || controller.name.size() > kMaxSerializedString ||
            controller.type.empty() || controller.type.size() > kMaxSerializedString ||
            !enum_cadence(controller.cadence)) return false;
        put_string(bytes, controller.name);
        put_string(bytes, controller.type);
        put_u8(bytes, static_cast<uint8_t>(controller.cadence));
    }
    return true;
}

bool encode_clips(const AnimationBuild& authored, std::vector<uint8_t>& bytes) {
    if (authored.clips.empty() || authored.clips.size() > kMaxGraphNodes) return false;
    put_tag(bytes, "OCL3");
    put_u32(bytes, kRuntimeSectionVersion);
    put_u32(bytes, static_cast<uint32_t>(authored.clips.size()));
    for (const ClipDefinition& clip : authored.clips) {
        if (clip.name.empty() || clip.name.size() > kMaxSerializedString ||
            !std::isfinite(clip.duration) || clip.duration <= 0.0f ||
            !std::isfinite(clip.rate) || clip.ozz_blob.empty() ||
            clip.ozz_blob.size() > kMaxSerializedClipBytes ||
            clip.markers.size() > kMaxGraphNodes) return false;
        put_string(bytes, clip.name);
        put_float(bytes, clip.duration);
        put_float(bytes, clip.rate);
        put_u8(bytes, clip.loop ? 1 : 0);
        put_u8(bytes, clip.additive ? 1 : 0);
        put_u32(bytes, static_cast<uint32_t>(clip.markers.size()));
        for (size_t i = 0; i < clip.markers.size(); ++i) {
            const ClipMarker& marker = clip.markers[i];
            if (marker.name.empty() || marker.name.size() > kMaxSerializedString ||
                !std::isfinite(marker.time) || marker.time < 0.0f ||
                marker.time > clip.duration) return false;
            put_string(bytes, marker.name);
            put_float(bytes, marker.time);
            put_u32(bytes, static_cast<uint32_t>(i));
        }
        put_u32(bytes, static_cast<uint32_t>(clip.ozz_blob.size()));
        bytes.insert(bytes.end(), clip.ozz_blob.begin(), clip.ozz_blob.end());
    }
    return true;
}

struct EncodedController {
    std::string name;
    std::string type;
    EvaluationCadence cadence = EvaluationCadence::Fixed;
};
struct EncodedNode {
    RuntimeGraphNode runtime;
    uint16_t controller_index = UINT16_MAX;
};
struct DecodedClip {
    std::string name;
    float duration = 0.0f;
    float rate = 1.0f;
    bool loop = false;
    bool additive = false;
    std::vector<RuntimeClipMarker> markers;
    std::vector<uint8_t> archive;
};

bool decode_rig(const AnimSection& section, CanonicalRig& rig) {
    size_t at = 0;
    uint32_t joints = 0, sockets = 0;
    if (!take_tag_version(section.bytes, at, "RIG3") ||
        !take_u32(section.bytes, at, joints) || joints == 0 || joints > kMaxJoints ||
        !take_u32(section.bytes, at, sockets) || sockets > kMaxJoints) return false;
    rig.joints.resize(joints);
    for (uint32_t i = 0; i < joints; ++i) {
        CanonicalJoint& joint = rig.joints[i];
        if (!take_string(section.bytes, at, joint.name) || joint.name.empty() ||
            !take_u16(section.bytes, at, joint.parent) ||
            !take_u16(section.bytes, at, joint.subtree.begin) ||
            !take_u16(section.bytes, at, joint.subtree.end) ||
            !take_transform(section.bytes, at, joint.local) ||
            !take_float(section.bytes, at, joint.radius) || joint.radius <= 0.0f ||
            (i == 0 ? joint.parent != kInvalidJoint
                    : joint.parent == kInvalidJoint || joint.parent >= i) ||
            joint.subtree.begin != i || joint.subtree.end <= i ||
            joint.subtree.end > joints) return false;
    }
    rig.sockets.resize(sockets);
    for (CanonicalSocket& socket : rig.sockets) {
        if (!take_string(section.bytes, at, socket.name) || socket.name.empty() ||
            !take_u16(section.bytes, at, socket.joint) || socket.joint >= joints ||
            !take_transform(section.bytes, at, socket.local)) return false;
    }
    return at == section.bytes.size();
}

bool decode_inputs_targets(const AnimSection& section,
                           std::vector<RuntimeInputDefinition>& inputs,
                           std::vector<CanonicalTarget>& targets) {
    size_t at = 0;
    uint32_t input_count = 0, target_count = 0;
    if (!take_tag_version(section.bytes, at, "ITS3") ||
        !take_u32(section.bytes, at, input_count) || input_count > kMaxGraphNodes ||
        !take_u32(section.bytes, at, target_count) || target_count > kMaxTargets)
        return false;
    inputs.resize(input_count);
    for (RuntimeInputDefinition& input : inputs) {
        uint8_t type = 0, cadence = 0;
        if (!take_string(section.bytes, at, input.name) || input.name.empty() ||
            !take_u8(section.bytes, at, type) ||
            type > static_cast<uint8_t>(AnimationValueType::Symbol) ||
            !take_u8(section.bytes, at, cadence) ||
            cadence > static_cast<uint8_t>(EvaluationCadence::Frame) ||
            !take_value(section.bytes, at, input.default_value)) return false;
        input.type = static_cast<AnimationValueType>(type);
        input.cadence = static_cast<EvaluationCadence>(cadence);
        if (input.default_value.type != input.type) return false;
    }
    targets.resize(target_count);
    for (CanonicalTarget& target : targets) {
        uint8_t driver = 0, cadence = 0, enabled = 0, has_pole = 0;
        uint16_t chain_count = 0;
        if (!take_string(section.bytes, at, target.name) || target.name.empty() ||
            !take_u8(section.bytes, at, driver) ||
            driver > static_cast<uint8_t>(TargetDriverKind::Controller) ||
            !take_u8(section.bytes, at, cadence) ||
            cadence > static_cast<uint8_t>(EvaluationCadence::Frame) ||
            !take_string(section.bytes, at, target.controller) ||
            !take_u8(section.bytes, at, enabled) || enabled > 1 ||
            !take_u8(section.bytes, at, has_pole) || has_pole > 1 ||
            !take_float3(section.bytes, at, target.pole) ||
            !take_float3(section.bytes, at, target.bend_axis) ||
            !take_float(section.bytes, at, target.soften) ||
            !take_float(section.bytes, at, target.twist) ||
            !take_float(section.bytes, at, target.position_half_life) ||
            !take_float(section.bytes, at, target.rotation_half_life) ||
            !take_float(section.bytes, at, target.weight_half_life) ||
            !take_u16(section.bytes, at, chain_count) || chain_count != 3)
            return false;
        target.driver = static_cast<TargetDriverKind>(driver);
        target.cadence = static_cast<EvaluationCadence>(cadence);
        target.enabled = enabled != 0;
        target.has_pole = has_pole != 0;
        target.chain.resize(chain_count);
        for (JointIndex& joint : target.chain)
            if (!take_u16(section.bytes, at, joint)) return false;
        if ((target.driver == TargetDriverKind::Controller) != !target.controller.empty())
            return false;
    }
    return at == section.bytes.size();
}

bool decode_graph(const AnimSection& section, std::vector<EncodedNode>& nodes,
                  std::vector<EncodedController>& controllers) {
    size_t at = 0;
    uint32_t node_count = 0, controller_count = 0;
    if (!take_tag_version(section.bytes, at, "AGC3") ||
        !take_u32(section.bytes, at, node_count) || node_count == 0 ||
        node_count > kMaxGraphNodes ||
        !take_u32(section.bytes, at, controller_count) ||
        controller_count > kMaxControllers) return false;
    nodes.resize(node_count);
    for (uint32_t i = 0; i < node_count; ++i) {
        uint8_t kind = 0, cadence = 0;
        uint16_t dependency_count = 0, threshold_count = 0;
        EncodedNode& encoded = nodes[i];
        if (!take_u8(section.bytes, at, kind) ||
            kind > static_cast<uint8_t>(GraphNodeKind::Output) ||
            !take_u8(section.bytes, at, cadence) ||
            cadence > static_cast<uint8_t>(EvaluationCadence::Frame) ||
            !take_u16(section.bytes, at, dependency_count) ||
            dependency_count > node_count) return false;
        encoded.runtime.kind = static_cast<RuntimeGraphNodeKind>(kind);
        encoded.runtime.cadence = static_cast<EvaluationCadence>(cadence);
        encoded.runtime.dependencies.resize(dependency_count);
        for (uint16_t& dependency : encoded.runtime.dependencies)
            if (!take_u16(section.bytes, at, dependency) || dependency >= i) return false;
        if (!take_u16(section.bytes, at, encoded.runtime.clip_index) ||
            !take_u16(section.bytes, at, encoded.runtime.input_index) ||
            !take_u16(section.bytes, at, encoded.controller_index) ||
            !take_u16(section.bytes, at, threshold_count) ||
            threshold_count > node_count) return false;
        encoded.runtime.thresholds.resize(threshold_count);
        for (float& threshold : encoded.runtime.thresholds)
            if (!take_float(section.bytes, at, threshold)) return false;
    }
    controllers.resize(controller_count);
    for (EncodedController& controller : controllers) {
        uint8_t cadence = 0;
        if (!take_string(section.bytes, at, controller.name) || controller.name.empty() ||
            !take_string(section.bytes, at, controller.type) || controller.type.empty() ||
            !take_u8(section.bytes, at, cadence) ||
            cadence > static_cast<uint8_t>(EvaluationCadence::Frame)) return false;
        controller.cadence = static_cast<EvaluationCadence>(cadence);
    }
    return at == section.bytes.size();
}

bool decode_clips(const AnimSection& section, std::vector<DecodedClip>& clips) {
    size_t at = 0;
    uint32_t clip_count = 0;
    if (!take_tag_version(section.bytes, at, "OCL3") ||
        !take_u32(section.bytes, at, clip_count) || clip_count == 0 ||
        clip_count > kMaxGraphNodes) return false;
    clips.resize(clip_count);
    for (DecodedClip& clip : clips) {
        uint8_t loop = 0, additive = 0;
        uint32_t marker_count = 0, archive_size = 0;
        if (!take_string(section.bytes, at, clip.name) || clip.name.empty() ||
            !take_float(section.bytes, at, clip.duration) || clip.duration <= 0.0f ||
            !take_float(section.bytes, at, clip.rate) ||
            !take_u8(section.bytes, at, loop) || loop > 1 ||
            !take_u8(section.bytes, at, additive) || additive > 1 ||
            !take_u32(section.bytes, at, marker_count) || marker_count > kMaxGraphNodes)
            return false;
        clip.loop = loop != 0;
        clip.additive = additive != 0;
        clip.markers.resize(marker_count);
        for (RuntimeClipMarker& marker : clip.markers) {
            std::string marker_name;
            if (!take_string(section.bytes, at, marker_name) || marker_name.empty() ||
                !take_float(section.bytes, at, marker.time) || marker.time < 0.0f ||
                marker.time > clip.duration ||
                !take_u32(section.bytes, at, marker.marker_index)) return false;
        }
        if (!take_u32(section.bytes, at, archive_size) || archive_size == 0 ||
            archive_size > kMaxSerializedClipBytes || at > section.bytes.size() ||
            archive_size > section.bytes.size() - at) return false;
        clip.archive.assign(section.bytes.begin() + at,
                            section.bytes.begin() + at + archive_size);
        at += archive_size;
    }
    return at == section.bytes.size();
}

bool validate_rig_against_skeleton(const CanonicalRig& rig,
                                   const OzzSkeleton& skeleton) {
    if (rig.joints.size() != skeleton.joint_count()) return false;
    constexpr float kRestEpsilon = 1e-5f;
    const auto near = [](float left, float right) {
        return std::fabs(left - right) <= kRestEpsilon;
    };
    const auto near_float3 = [&](const Float3& left, const Float3& right) {
        return near(left.x, right.x) && near(left.y, right.y) &&
               near(left.z, right.z);
    };
    const auto near_quaternion = [&](const Quaternion& left,
                                     const Quaternion& right) {
        const bool same = near(left.x, right.x) && near(left.y, right.y) &&
                          near(left.z, right.z) && near(left.w, right.w);
        const bool negated = near(left.x, -right.x) && near(left.y, -right.y) &&
                             near(left.z, -right.z) && near(left.w, -right.w);
        return same || negated;
    };
    for (JointIndex i = 0; i < rig.joints.size(); ++i) {
        AnimationTransform rest;
        if (rig.joints[i].parent != skeleton.parent(i) ||
            !(rig.joints[i].subtree == skeleton.subtree(i)) ||
            !skeleton.rest_local(i, rest) ||
            !near_float3(rig.joints[i].local.translation, rest.translation) ||
            !near_quaternion(rig.joints[i].local.rotation, rest.rotation) ||
            !near_float3(rig.joints[i].local.scale, rest.scale)) return false;
    }
    return true;
}

struct OwnedEvaluation {
    OzzSkeleton skeleton;
    std::vector<OzzAnimation> animations;
    AnimationEvaluationDefinition value;
};

bool compile_controller(const EncodedController& authored, uint16_t controller_index,
                        const CanonicalRig& rig,
                        const std::vector<RuntimeInputDefinition>& inputs,
                        const std::vector<CanonicalTarget>& targets,
                        const std::vector<Mat4f>& rest_model,
                        AnimationRuntimeBindingDescriptor::Controller& out,
                        size_t& state_bytes) {
    if (authored.type != "proceduralGait" ||
        authored.cadence != EvaluationCadence::Fixed) return false;
    for (uint16_t i = 0; i < targets.size(); ++i)
        if (targets[i].driver == TargetDriverKind::Controller &&
            targets[i].controller == authored.name) out.target_indices.push_back(i);
    if (out.target_indices.size() != 2) return false;
    GaitControllerParameters parameters{};
    parameters.left_target = out.target_indices[0];
    parameters.right_target = out.target_indices[1];
    const auto predicted = [&](uint16_t target) {
        const JointIndex end = targets[target].chain.back();
        return Float3{rest_model[end].m[3], rest_model[end].m[7], rest_model[end].m[11]};
    };
    parameters.left_predicted = predicted(parameters.left_target);
    parameters.right_predicted = predicted(parameters.right_target);
    out.descriptor.type = kGaitControllerTypeId;
    out.descriptor.cadence = authored.cadence;
    out.descriptor.parameters.resize(sizeof(parameters));
    std::memcpy(out.descriptor.parameters.data(), &parameters, sizeof(parameters));
    for (uint16_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i].name != "speed") continue;
        if (inputs[i].type != AnimationValueType::Number ||
            inputs[i].cadence != EvaluationCadence::Fixed) return false;
        out.inputs.push_back({i, inputs[i].type, inputs[i].cadence});
    }
    NativeControllerLayout layout{};
    NativeControllerRegistry registry = NativeControllerRegistry::with_v1_controllers();
    if (!registry.create(out.descriptor, layout) ||
        layout.fixed_state_bytes > std::numeric_limits<size_t>::max() - state_bytes)
        return false;
    state_bytes += layout.fixed_state_bytes;
    out.priority = controller_index;
    return true;
}

} // namespace

bool encode_animation_runtime_sections(const AnimationBuild& authored,
                                       const CanonicalAnimationBuild& canonical,
                                       AnimAsset& asset,
                                       Diagnostics& diagnostics) {
    diagnostics.items.clear();
    std::vector<uint8_t> rig, inputs_targets, graph, clips;
    if (!encode_rig(canonical.rig, rig) ||
        !encode_inputs_targets(authored, canonical, inputs_targets) ||
        !encode_graph(authored, canonical, graph) ||
        !encode_clips(authored, clips) || authored.ozz_skeleton_blob.empty()) {
        add_error(diagnostics, "runtime-asset-encode",
                  "canonical animation runtime sections could not be encoded");
        return false;
    }
    set_section(asset, AnimSectionKind::RigSchema, std::move(rig));
    set_section(asset, AnimSectionKind::InputTargetSchemas, std::move(inputs_targets));
    set_section(asset, AnimSectionKind::GraphControllerBytecode, std::move(graph));
    set_section(asset, AnimSectionKind::OzzSkeleton, authored.ozz_skeleton_blob);
    set_section(asset, AnimSectionKind::OzzClips, std::move(clips));
    return true;
}

bool decode_animation_runtime_asset(const AnimAsset& asset,
                                    DecodedAnimationRuntimeAsset& out,
                                    Diagnostics& diagnostics) {
    diagnostics.items.clear();
    if (asset.target_abi_tag != kAnimationTargetAbiTag ||
        asset.ozz_tag_hash != kAnimationOzzTagHash) {
        add_error(diagnostics, "runtime-asset-compatibility",
                  "animation runtime ABI or Ozz compatibility tag mismatch");
        return false;
    }
    const AnimSection* rig_section = unique_section(asset, AnimSectionKind::RigSchema);
    const AnimSection* input_section =
        unique_section(asset, AnimSectionKind::InputTargetSchemas);
    const AnimSection* graph_section =
        unique_section(asset, AnimSectionKind::GraphControllerBytecode);
    const AnimSection* skeleton_section =
        unique_section(asset, AnimSectionKind::OzzSkeleton);
    const AnimSection* clips_section = unique_section(asset, AnimSectionKind::OzzClips);
    if (!rig_section || !input_section || !graph_section || !skeleton_section ||
        !clips_section) {
        add_error(diagnostics, "runtime-asset-section",
                  "required animation runtime section is missing or duplicated");
        return false;
    }

    DecodedAnimationRuntimeAsset candidate;
    std::vector<CanonicalTarget> targets;
    std::vector<EncodedNode> encoded_nodes;
    std::vector<EncodedController> controllers;
    std::vector<DecodedClip> clips;
    if (!decode_rig(*rig_section, candidate.rig) ||
        !decode_inputs_targets(*input_section, candidate.definition.inputs, targets) ||
        !decode_graph(*graph_section, encoded_nodes, controllers) ||
        !decode_clips(*clips_section, clips)) {
        add_error(diagnostics, "runtime-asset-schema",
                  "animation runtime section is corrupt or has an unsupported schema");
        return false;
    }
    for (const CanonicalTarget& target : targets)
        for (JointIndex joint : target.chain)
            if (joint >= candidate.rig.joints.size()) {
                add_error(diagnostics, "runtime-asset-target",
                          "animation target references an invalid joint");
                return false;
            }

    BindingBake binding;
    if (!get_anim_binding_bake(asset, binding) ||
        binding.inverse_bind_matrices.size() != candidate.rig.joints.size()) {
        add_error(diagnostics, "runtime-asset-binding",
                  "animation runtime inverse-bind payload is missing or incompatible");
        return false;
    }

    auto owned = std::make_shared<OwnedEvaluation>();
    if (!deserialize_skeleton(skeleton_section->bytes.data(),
                              skeleton_section->bytes.size(), owned->skeleton,
                              diagnostics) ||
        !validate_rig_against_skeleton(candidate.rig, owned->skeleton)) {
        if (diagnostics.items.empty())
            add_error(diagnostics, "runtime-asset-skeleton",
                      "canonical rig and Ozz skeleton disagree");
        return false;
    }
    owned->animations.reserve(clips.size());
    for (const DecodedClip& clip : clips) {
        OzzAnimation animation;
        if (!deserialize_animation(clip.archive.data(), clip.archive.size(),
                                   animation, diagnostics)) return false;
        if (animation.name() != clip.name ||
            animation.track_count() != owned->skeleton.joint_count() ||
            std::fabs(animation.duration() - clip.duration) > 1e-5f) {
            add_error(diagnostics, "runtime-asset-clip",
                      "Ozz clip identity, topology, or duration disagrees with its runtime schema");
            return false;
        }
        owned->animations.push_back(std::move(animation));
    }

    owned->value.skeleton = &owned->skeleton;
    owned->value.inverse_bind_model = binding.inverse_bind_matrices;
    for (const RuntimeInputDefinition& input : candidate.definition.inputs)
        owned->value.inputs.push_back({input.type, input.cadence});
    for (size_t i = 0; i < clips.size(); ++i) {
        const DecodedClip& clip = clips[i];
        owned->value.clips.push_back(
            {&owned->animations[i], clip.duration, clip.loop, clip.additive,
             clip.rate, clip.markers});
    }
    std::vector<uint16_t> controller_references(controllers.size(), 0);
    for (EncodedNode& encoded : encoded_nodes) {
        if ((encoded.runtime.clip_index != UINT16_MAX &&
             encoded.runtime.clip_index >= owned->value.clips.size()) ||
            (encoded.runtime.input_index != UINT16_MAX &&
             encoded.runtime.input_index >= owned->value.inputs.size()) ||
            (encoded.controller_index != UINT16_MAX &&
             encoded.controller_index >= controllers.size())) {
            add_error(diagnostics, "runtime-asset-graph",
                      "compiled animation graph references an invalid declaration");
            return false;
        }
        const bool controller_node =
            encoded.runtime.kind == RuntimeGraphNodeKind::NativeController;
        if (controller_node != (encoded.controller_index != UINT16_MAX)) {
            add_error(diagnostics, "runtime-asset-graph",
                      "compiled animation graph has an invalid controller reference");
            return false;
        }
        if (controller_node) {
            if (++controller_references[encoded.controller_index] != 1) {
                add_error(diagnostics, "runtime-asset-graph",
                          "compiled animation controller is referenced more than once");
                return false;
            }
            encoded.runtime.controller_index = encoded.controller_index;
        }
        owned->value.nodes.push_back(encoded.runtime);
    }
    for (uint16_t references : controller_references)
        if (references != 1) {
            add_error(diagnostics, "runtime-asset-graph",
                      "compiled animation controller has no graph reference");
            return false;
        }
    if (!valid_animation_evaluation_definition(owned->value) ||
        !validate_exclusive_target_chains(targets)) {
        add_error(diagnostics, "runtime-asset-definition",
                  "decoded animation runtime definition is invalid");
        return false;
    }

    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation =
        std::shared_ptr<const AnimationEvaluationDefinition>(owned, &owned->value);
    descriptor->targets = targets;
    descriptor->fixed_work.clip.duration = owned->value.clips.front().duration;
    descriptor->fixed_work.clip.loop = owned->value.clips.front().loop;
    descriptor->fixed_work.clip.rate = owned->value.clips.front().rate;
    descriptor->fixed_work.clip.markers = owned->value.clips.front().markers;
    for (uint16_t i = 0; i < targets.size(); ++i) {
        const CanonicalTarget& target = targets[i];
        candidate.definition.targets.push_back(
            {target.name, target.driver, target.cadence, target.chain, target.enabled});
        if (descriptor->target_index == UINT16_MAX &&
            target.driver == TargetDriverKind::External)
            descriptor->target_index = i;
    }
    std::vector<AnimationTransform> rest(candidate.rig.joints.size());
    for (JointIndex i = 0; i < rest.size(); ++i)
        if (!owned->skeleton.rest_local(i, rest[i])) {
            add_error(diagnostics, "runtime-asset-rest-pose",
                      "Ozz skeleton rest pose could not be read");
            return false;
        }
    std::vector<Mat4f> rest_model;
    if (!local_to_model(owned->skeleton, rest, rest_model)) {
        add_error(diagnostics, "runtime-asset-rest-pose",
                  "Ozz skeleton rest pose could not be modeled");
        return false;
    }
    size_t controller_state_bytes = 0;
    for (uint16_t i = 0; i < controllers.size(); ++i) {
        AnimationRuntimeBindingDescriptor::Controller controller;
        if (!compile_controller(controllers[i], i, candidate.rig,
                                candidate.definition.inputs, targets, rest_model,
                                controller, controller_state_bytes)) {
            add_error(diagnostics, "runtime-asset-controller",
                      "native controller declaration is unsupported or incomplete");
            return false;
        }
        descriptor->controllers.push_back(std::move(controller));
    }
    candidate.definition.graph_state_bytes =
        owned->value.nodes.size() * sizeof(float);
    candidate.definition.controller_state_bytes = controller_state_bytes;
    candidate.definition.sample_context_bytes =
        owned->value.clips.size() * sizeof(uint64_t);
    candidate.definition.pose_scratch_bytes =
        candidate.rig.joints.size() * sizeof(AnimationTransform);
    candidate.definition.binding = std::move(descriptor);
    out = std::move(candidate);
    return true;
}

} // namespace matter::animation
