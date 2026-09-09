#pragma once
// MatterEngine3/src/animation/animation_ir.h
//
// The AUTHORED intermediate representation of an animation asset -- what the
// JavaScript animation DSL produces and what the baker consumes. It is not the
// runtime form: `animation_evaluator.h` holds that, and
// `animation_runtime_asset.cpp` is the encoder/decoder between the two.
//
// Two layers live here
// --------------------
// 1. `AnimationBuild` and its parts (`RigDefinition`, `ClipDefinition`,
//    `InputSchema`, `TargetSchema`, `ControllerDef`, `MotionDefinition`, the
//    binding/attachment declarations). Everything in this layer refers to
//    other declarations BY NAME and carries a `SourceSpan` so a validation
//    failure can point at the authoring site.
// 2. `CanonicalAnimationBuild` and its `Canonical*` parts. Names have been
//    resolved to indices, joints flattened into a parent-before-child order
//    with precomputed subtree ranges, and the whole thing is serializable to
//    a stable string by `encode()` for determinism hashing.
//
// Conventions
// -----------
// - `JointIndex` is a 16-bit index into `CanonicalRig::joints`;
//   `kInvalidJoint` (UINT16_MAX) is the "no joint" sentinel and is also the
//   root's parent.
// - The `kMax*` limits are re-exports of the hard ceilings in
//   `animation_budget.h`, so there is one place a limit is declared.
// - `AnimationTransform` is translation + rotation + scale in the parent
//   joint's space; distances are world metres at unit rig scale.
// - `SourceSpan::line`/`column` are 1-based positions in the named module;
//   `object` names the authored object the span belongs to.
//
// Nothing here holds engine, GPU, or Ozz resources -- these are plain value
// types, freely copyable, and carry no threading rules of their own.

#include "animation/animation_budget.h"

#include "matter/animation_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace matter::animation {

// Authoring-side limits. Every one that has a runtime consequence is an alias
// of the corresponding hard ceiling in `AnimationBudgetConfig`, so a bake can
// never accept something the runtime would later refuse. `kMaxTargets` and
// `kMaxSkinInfluences` are authoring-only and have no budget counterpart:
// `kMaxSkinInfluences` is the per-vertex weight count the skinning path is
// built around.
using JointIndex = uint16_t;
constexpr JointIndex kInvalidJoint = UINT16_MAX;
constexpr uint32_t kMaxJoints = AnimationBudgetConfig::kHardMaxJointsPerAsset;
constexpr uint32_t kMaxTargets = 64;
constexpr uint32_t kMaxGraphNodes = AnimationBudgetConfig::kHardMaxGraphNodes;
constexpr uint32_t kMaxControllers = AnimationBudgetConfig::kHardMaxControllerNodes;
constexpr uint32_t kMaxSkinInfluences = 4;

// Half-open `[begin, end)` range of joint indices. Used for a joint's subtree:
// canonical joints are ordered parent-before-child and contiguously, so a
// subtree is exactly a range and `begin` is always the joint's own index.
struct JointRange {
    JointIndex begin = kInvalidJoint;
    JointIndex end = kInvalidJoint;
    bool operator==(const JointRange& other) const { return begin == other.begin && end == other.end; }
};

// Where an authored declaration came from, for diagnostics. Carried through
// the whole IR so a validation failure can name the file, position, and
// object rather than an index. A default-constructed span (empty module, zero
// line) means "no authoring site", e.g. for errors raised while decoding a
// baked asset.
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

// Accumulated bake/load errors. `add` appends; `sort` orders by
// (module, line, column, object, code, message) via `DiagnosticLess` so that
// two runs over the same input emit byte-identical diagnostics regardless of
// the order the checks happened to fire in. Sort before comparing or hashing.
struct Diagnostics {
    std::vector<Diagnostic> items;
    void add(const char* code, const SourceSpan& source, const char* message);
    void sort();
};

// Which clock a declaration is driven by. `Fixed` = the simulation tick
// (interpolated between samples); `Frame` = the render frame (sampled once,
// never interpolated). The graph validator forbids a Fixed node depending on a
// Frame one.
enum class EvaluationCadence { Fixed, Frame };
// Who supplies a target's desired transform: the host application/script
// (`External`) or a native controller node (`Controller`), in which case
// `TargetSchema::controller` must name it.
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
// One authored animation clip.
//
// `duration` is seconds. `rate` is the authored bake SAMPLE rate (sampling
// density in samples/second, consumed entirely at bake time) -- it is NOT a
// playback multiplier, and conflating the two once made a `sampleRate: 16`
// clip play 16x too fast; see the comment at the `put_float(bytes, 1.0f)` slot
// in `animation_runtime_asset.cpp`.
//
// `additive` marks the clip as bind-relative deltas rather than a normal pose,
// which the graph validator tracks separately end to end. `ozz_blob` is the
// serialized Ozz animation archive produced from `tracks`; once it is present
// it, not `tracks`, is what ships.
struct ClipDefinition {
    std::string name;
    float duration = 0.0f;
    float rate = 0.0f;
    std::vector<ClipTrack> tracks;
    std::vector<ClipMarker> markers;
    SourceSpan source;
    bool loop = false;
    bool additive = false;
    std::vector<uint8_t> ozz_blob;
};

// A dynamically typed animation input/parameter value. `type` selects which
// member is live -- the others keep their default and are not cleared, so
// never read a member without checking `type` first. The implicit constructors
// exist so authored defaults can be written as plain literals; note that
// `const char*` constructs a Symbol, not a string value.
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
struct TargetSchema { std::string name; std::string start_joint; std::string end_joint; TargetDriverKind driver = TargetDriverKind::External; std::string controller; EvaluationCadence cadence = EvaluationCadence::Frame; SourceSpan source; Float3 pole{0.0f, 0.0f, 0.0f}; bool has_pole = false; float soften = 1.0f; float twist = 0.0f; float position_half_life = 0.0f; float rotation_half_life = 0.0f; float weight_half_life = 0.0f; bool enabled = true; bool require_explicit_pole = false; };
struct ControllerDef { std::string name; SourceSpan source; EvaluationCadence cadence = EvaluationCadence::Fixed; std::string type; };
enum class GraphNodeKind { Clip, Blend1D, Additive, NativeController, Output };
struct GraphNode { std::string name; std::vector<std::string> dependencies; bool is_output = false; EvaluationCadence cadence = EvaluationCadence::Fixed; SourceSpan source; GraphNodeKind kind = GraphNodeKind::Output; std::string clip; std::string input; std::vector<float> thresholds; std::string controller; };
struct MotionDefinition { std::vector<GraphNode> nodes; SourceSpan source; };

// Authoring declarations are retained in the bake IR.  They describe the
// geometry-to-rig relationship; A8 owns publication of the resulting binding
// payload, and Phase C owns runtime deformation.
struct BindingGeometryRange {
    // Half-open authored ranges. `op_*` refers to the procedural field stream;
    // `triangle_*` refers to direct triangles before shared indexing. A8 maps
    // both through the one indexed-geometry builder.
    uint32_t op_begin = 0;
    uint32_t op_end = 0;
    uint32_t triangle_begin = 0;
    uint32_t triangle_end = 0;
};
struct SkinBindingDef {
    std::string name;
    std::vector<std::string> joints; // selected parent-child segments by child joint
    // Radius controls the generated field envelope; falloff controls the
    // post-bake weight field.  voxel_size is retained so implicit geometry is
    // reproducible from source alone.
    float radius_scale = 1.0f;
    float falloff = 1.0f;
    float voxel_size = 0.1f;
    bool generated = false;
    std::vector<BindingGeometryRange> geometry;
    SourceSpan source;
};
struct RigidBindingDef {
    std::string name;
    std::string joint; // one record per selected parent-child segment
    AnimationTransform local{};
    bool decorative = false;
    std::vector<BindingGeometryRange> geometry;
    SourceSpan source;
};
struct AttachmentDef {
    std::string name;
    // Exactly one target is populated.  Sockets retain their symbolic name;
    // direct joints retain the declared joint name for canonical resolution.
    std::string socket;
    std::string joint;
    uint64_t child_hash = 0; // resolved during the parent bake; never a module name at runtime
    // Bake-time ANLK status of child_hash. v1 rejects nested committed animation;
    // this is retained in authored IR so final validation cannot lose that fact.
    bool child_has_committed_animation = false;
    AnimationTransform local{};
    SourceSpan source;
};

// The complete authored asset, as parsed. Everything cross-references by
// name; nothing is index-resolved yet and nothing is validated yet. This is
// the input to canonicalization, to validation (`animation_validate.h`), and
// to `encode_animation_runtime_sections`.
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
    std::vector<uint8_t> ozz_skeleton_blob;
};

struct CanonicalJoint { std::string name; JointIndex parent = kInvalidJoint; AnimationTransform local{}; float radius = 1.0f; JointRange subtree{}; SourceSpan source; };
struct CanonicalSocket { std::string name; JointIndex joint = kInvalidJoint; AnimationTransform local{}; SourceSpan source; };
struct CanonicalRig { std::vector<CanonicalJoint> joints; std::vector<CanonicalSocket> sockets; };
// A name-resolved IK target. `chain` is exactly three joint indices --
// root, mid, end -- ordered from the top of the chain down. `pole` and
// `bend_axis` are rig-space directions disambiguating the solve plane;
// `soften` is a normalized 0-1 reach softening factor, `twist` is radians
// about the chain axis, and the three `*_half_life` values are smoothing
// half-lives in SECONDS (0 = no smoothing, apply instantly).
struct CanonicalTarget { std::string name; std::vector<JointIndex> chain; TargetDriverKind driver = TargetDriverKind::External; EvaluationCadence cadence = EvaluationCadence::Frame; std::string controller; Float3 pole{0.0f, 0.0f, 0.0f}; bool has_pole = false; Float3 bend_axis{0.0f, 0.0f, 1.0f}; float soften = 1.0f; float twist = 0.0f; float position_half_life = 0.0f; float rotation_half_life = 0.0f; float weight_half_life = 0.0f; bool enabled = true; };
// The canonical, index-resolved projection of an `AnimationBuild`: the parts
// whose exact content must be reproducible bake to bake. `graph_order` is the
// topological node order (authored indices, dependencies first) that the
// runtime graph is emitted in. `encode()` serializes it to the string the
// determinism hash is taken over, so any field added here changes that hash.
struct CanonicalAnimationBuild {
    CanonicalRig rig;
    std::vector<CanonicalTarget> targets;
    std::vector<uint16_t> graph_order;
    std::string authored_state;
    std::string encode() const;
};

} // namespace matter::animation
