#pragma once

#include "matter/animation_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace matter::animation {

using JointIndex = uint16_t;
constexpr JointIndex kInvalidJoint = UINT16_MAX;
constexpr uint32_t kMaxJoints = 256;
constexpr uint32_t kMaxTargets = 64;
constexpr uint32_t kMaxGraphNodes = 128;
constexpr uint32_t kMaxControllers = 128;
constexpr uint32_t kMaxSkinInfluences = 4;

struct JointRange {
    JointIndex begin = kInvalidJoint;
    JointIndex end = kInvalidJoint;
    bool operator==(const JointRange& other) const { return begin == other.begin && end == other.end; }
};

struct SourceSpan {
    std::string module;
    uint32_t line = 0;
    uint32_t column = 0;
    std::string object;
    bool operator==(const SourceSpan& other) const {
        return module == other.module && line == other.line && column == other.column && object == other.object;
    }
};

struct Diagnostic {
    std::string code;
    std::string message;
    SourceSpan source;
    bool operator==(const Diagnostic& other) const { return code == other.code && message == other.message && source == other.source; }
};

struct DiagnosticLess {
    bool operator()(const Diagnostic& left, const Diagnostic& right) const;
};

struct Diagnostics {
    std::vector<Diagnostic> items;
    void add(const char* code, const SourceSpan& source, const char* message);
    void sort();
};

enum class EvaluationCadence { Fixed, Frame };
enum class TargetDriverKind { External, Controller };

struct JointDef {
    std::string name;
    std::string parent;
    AnimationTransform local{};
    float radius = 1.0f;
    SourceSpan source;
};

struct SocketDef {
    std::string name;
    std::string joint;
    AnimationTransform local{};
    SourceSpan source;
};

struct RigDefinition {
    std::vector<JointDef> joints;
    std::vector<SocketDef> sockets;
    SourceSpan source;
};

struct ClipKey { float time = 0.0f; AnimationTransform value{}; SourceSpan source; };
struct ClipTrack { std::string joint; std::vector<ClipKey> keys; SourceSpan source; };
struct ClipMarker { std::string name; float time = 0.0f; SourceSpan source; };
struct ClipDefinition {
    std::string name;
    float duration = 0.0f;
    float rate = 0.0f;
    std::vector<ClipTrack> tracks;
    std::vector<ClipMarker> markers;
    SourceSpan source;
};

struct AnimationValue {
    AnimationValueType type = AnimationValueType::Number;
    bool boolean = false;
    double number = 0.0;
    Float3 float3{};
    Quaternion quaternion{};
    AnimationTransform transform{};
    std::string symbol;
    AnimationValue() = default;
    AnimationValue(bool value) : type(AnimationValueType::Bool), boolean(value) {}
    AnimationValue(double value) : type(AnimationValueType::Number), number(value) {}
    AnimationValue(Float3 value) : type(AnimationValueType::Float3), float3(value) {}
    AnimationValue(Quaternion value) : type(AnimationValueType::Quaternion), quaternion(value) {}
    AnimationValue(AnimationTransform value) : type(AnimationValueType::Transform), transform(value) {}
    AnimationValue(const char* value) : type(AnimationValueType::Symbol), symbol(value) {}
};

struct InputSchema { std::string name; AnimationValueType type = AnimationValueType::Number; AnimationValue default_value{}; EvaluationCadence cadence = EvaluationCadence::Fixed; SourceSpan source; };
struct TargetSchema { std::string name; std::string start_joint; std::string end_joint; TargetDriverKind driver = TargetDriverKind::External; std::string controller; EvaluationCadence cadence = EvaluationCadence::Frame; SourceSpan source; };
struct ControllerDef { std::string name; SourceSpan source; EvaluationCadence cadence = EvaluationCadence::Fixed; };
struct GraphNode { std::string name; std::vector<std::string> dependencies; bool is_output = false; EvaluationCadence cadence = EvaluationCadence::Fixed; SourceSpan source; };
struct MotionDefinition { std::vector<GraphNode> nodes; SourceSpan source; };

struct SkinBindingDef { std::string name; std::vector<std::string> joints; SourceSpan source; };
struct RigidBindingDef { std::string name; std::string joint; AnimationTransform local{}; SourceSpan source; };
struct AttachmentDef { std::string name; std::string socket; AnimationTransform local{}; SourceSpan source; };

struct AnimationBuild {
    RigDefinition rig;
    std::vector<ClipDefinition> clips;
    std::vector<InputSchema> inputs;
    std::vector<TargetSchema> targets;
    std::vector<ControllerDef> controllers;
    MotionDefinition graph;
    std::vector<SkinBindingDef> skin_bindings;
    std::vector<RigidBindingDef> rigid_bindings;
    std::vector<AttachmentDef> attachments;
};

struct CanonicalJoint { std::string name; JointIndex parent = kInvalidJoint; AnimationTransform local{}; float radius = 1.0f; JointRange subtree{}; };
struct CanonicalRig { std::vector<CanonicalJoint> joints; };
struct CanonicalTarget { std::string name; std::vector<JointIndex> chain; TargetDriverKind driver = TargetDriverKind::External; EvaluationCadence cadence = EvaluationCadence::Frame; };
struct CanonicalAnimationBuild {
    CanonicalRig rig;
    std::vector<CanonicalTarget> targets;
    std::vector<uint16_t> graph_order;
    std::string authored_state;
    std::string encode() const;
};

} // namespace matter::animation
