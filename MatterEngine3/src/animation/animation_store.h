#pragma once

// MatterEngine3/src/animation/animation_store.h
//
// The immutable runtime schema an animator is created against. A decoded ANIM
// asset (`animation/animation_runtime_asset.h`) yields one
// `AnimationRuntimeDefinition`; `AnimationService::create` /
// `replace_asset` (public API in `matter/animation.h`) take it by const
// reference and intern an owned copy keyed by the `AnimAsset*`.
//
// Layering: this header is the schema half of the animation service. The
// service implementation, its slot table and all mutable per-instance state
// live in `animation_store.cpp`; the ECS-phase runtime that evaluates those
// instances lives in `animation_systems.h`.
//
// Conventions:
// - `inputs` and `targets` are ordered, and their order IS the handle space:
//   `AnimationInputHandle::schema_index` / `AnimationTargetHandle::schema_index`
//   index straight into these vectors, and the shared descriptor's parallel
//   `evaluation->inputs` / `binding->targets` must agree element-for-element.
// - Every `*_bytes` field is a byte count of per-instance CPU scratch that the
//   service must reserve up front; see `mutable_bytes()` below.
// - Definitions are immutable once admitted. A live animator that needs a
//   different graph goes through `replace_asset`, which admits a second
//   definition rather than mutating this one.

#include "animation/anim_asset.h"
#include "animation/animation_ir.h"
#include "animation/animation_systems.h"
#include "matter/animation.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace matter::animation {

// One authored control input. `name` is what `AnimationService::input()`
// looks up; `type` is enforced on every write, so a handle whose value type
// does not match this declaration is rejected rather than coerced.
// `cadence` decides which of the service's staged control buffers a write
// lands in: Fixed writes become observable at the fixed-step boundary,
// Frame writes at the frame boundary.
struct RuntimeInputDefinition {
    std::string name;
    AnimationValueType type = AnimationValueType::Number;
    EvaluationCadence cadence = EvaluationCadence::Fixed;
    AnimationValue default_value{};
};

// One authored IK/aim target. `driver` decides who may write it: External
// targets are writable through the public `AnimationService::set_transform` /
// `set_weight` / `set_enabled` API, Controller targets are owned exclusively
// by a native controller and reject public writes. `joint_chain` is the
// solved chain in joint indices into the rig's skeleton; the v1 two-bone
// solver requires exactly three joints and chains may not overlap between
// targets (`validate_exclusive_target_chains` in `animation_targets.h`).
// `enabled` is the initial state only - it is the seed for the runtime's
// evaluated weight, not a lasting constraint.
struct RuntimeTargetDefinition {
    std::string name;
    TargetDriverKind driver = TargetDriverKind::External;
    EvaluationCadence cadence = EvaluationCadence::Frame;
    std::vector<JointIndex> joint_chain;
    bool enabled = true;
};

// Immutable runtime schema selected by a fully loaded ANIM bundle. B2 consumes
// the state-size fields while B1 only reserves and accounts for them.
//
// Lifetime: the service copies an admitted definition into its own
// `unique_ptr` and hands out only raw pointers to that copy, so the caller's
// instance may go out of scope immediately after `create`. Definitions are
// interned per `AnimAsset*`, meaning several animators sharing an asset share
// one definition object; `replace_asset` may admit a second definition for the
// same asset, which is then owned separately until the service is destroyed.
//
// A definition is admitted only if it validates against the active
// `AnimationBudgetConfig`: joint/graph-node/controller-node counts, target
// count and chain exclusivity, controller input cadences, and agreement
// between `inputs`/`targets` here and the same lists inside `binding`.
struct AnimationRuntimeDefinition {
    std::vector<RuntimeInputDefinition> inputs;
    std::vector<RuntimeTargetDefinition> targets;
    // Per-instance CPU scratch sizes in bytes, all reserved at create time
    // and materialized as byte vectors on the service's slot. Zero is a
    // legitimate value (nothing of that kind is needed for this schema);
    // it is not a "not computed yet" sentinel.
    size_t graph_state_bytes = 0;
    size_t controller_state_bytes = 0;
    size_t sample_context_bytes = 0;
    size_t pose_scratch_bytes = 0;
    // Null intentionally means legacy/unbound: it can be created but never
    // participates in runtime evaluation.  A non-null descriptor must be
    // complete enough to build both evaluator and fixed simulation work.
    std::shared_ptr<const AnimationRuntimeBindingDescriptor> binding;
    // Total per-instance mutable bytes this schema will cost the service,
    // charged against `AnimationStoreConfig::mutable_budget_bytes` at create
    // and replace time. `SIZE_MAX` is the overflow/invalid sentinel: any
    // arithmetic overflow, or a controller the native registry refuses to
    // instantiate, saturates the result, and a definition that returns
    // `SIZE_MAX` is rejected outright rather than admitted at a huge cost.
    //
    // Not a cheap accessor. It walks the whole binding - every clip's Ozz
    // sample-context size, every graph node, every controller - and it
    // actually constructs each declared native controller through
    // `NativeControllerRegistry` to learn its state size, so each call
    // allocates. Nothing is cached; the service calls it several times per
    // create/replace.
    size_t mutable_bytes() const;
};

} // namespace matter::animation
