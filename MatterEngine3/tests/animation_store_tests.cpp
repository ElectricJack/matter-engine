#include "animation/animation_store.h"
#include "check.h"

#include <cstdio>

using namespace matter;
using namespace matter::animation;

namespace {

AnimAsset asset(uint64_t hash, uint64_t nonce) {
    AnimAsset value;
    value.resolved_hash = hash;
    value.nonce = {0, nonce};
    return value;
}

AnimationRuntimeDefinition definition() {
    AnimationRuntimeDefinition result;
    result.inputs = {
        {"speed", AnimationValueType::Number, EvaluationCadence::Fixed, AnimationValue(2.0)},
        {"aim", AnimationValueType::Float3, EvaluationCadence::Frame, AnimationValue(Float3{1,2,3})},
        {"mode", AnimationValueType::Symbol, EvaluationCadence::Fixed, AnimationValue("idle")},
    };
    result.targets = {
        {"hand", TargetDriverKind::External, EvaluationCadence::Frame, {0,1,2}, true},
        {"foot", TargetDriverKind::Controller, EvaluationCadence::Fixed, {0,3,4}, true},
    };
    result.graph_state_bytes = 16;
    result.controller_state_bytes = 8;
    result.sample_context_bytes = 4;
    result.pose_scratch_bytes = 12;
    return result;
}

void test_asset_dedup_and_handle_reuse() {
    AnimationService service;
    const AnimAsset* first = service.insert_asset(asset(7, 9));
    const AnimAsset* duplicate = service.insert_asset(asset(7, 9));
    CHECK(first != nullptr && first == duplicate, "immutable asset identity deduplicates hash and nonce");

    const Animator first_animator = service.create(first, definition());
    CHECK(first_animator.valid(), "create first animator");
    const AnimationInputHandle speed = service.input(first_animator.instance, "speed");
    CHECK(service.set(speed, 7.0f), "matching typed input write succeeds");
    CHECK(service.remove(first_animator.instance), "remove animator");
    const Animator replacement = service.create(first, definition());
    CHECK(replacement.valid() && replacement.instance.slot_index == first_animator.instance.slot_index,
          "free list reuses slot");
    CHECK(replacement.instance.generation != first_animator.instance.generation, "reuse increments generation");
    CHECK(!service.set(speed, 9.0f), "stale input never affects reused slot");
}

void test_typed_cadence_and_target_contracts() {
    AnimationService service;
    const Animator animator = service.create(service.insert_asset(asset(1, 1)), definition());
    const AnimationInputHandle speed = service.input(animator.instance, "speed");
    const AnimationInputHandle aim = service.input(animator.instance, "aim");
    const AnimationInputHandle missing = service.input(animator.instance, "missing");
    CHECK(!service.set(speed, true), "wrong input value type rejected");
    CHECK(service.number_value(speed) == 2.0f, "rejected input write leaves stored bytes unchanged");
    AnimationInputHandle wrong_cadence = aim;
    wrong_cadence.cadence = AnimationCadence::Fixed;
    CHECK(!service.set(wrong_cadence, Float3{0,0,0}), "wrong input cadence rejected");
    CHECK(!missing.valid(), "missing input is invalid");

    const AnimationTargetHandle hand = service.target(animator.instance, "hand");
    const AnimationTargetHandle foot = service.target(animator.instance, "foot");
    CHECK(service.set_enabled(hand, false), "external target enabled state writes");
    CHECK(!service.set_transform(hand, AnimationTransform{}), "disabled target rejects transform");
    CHECK(!service.set_transform(foot, AnimationTransform{}), "controller-owned target rejects transform");
    CHECK(!service.set_weight(foot, 1.0f), "controller-owned target rejects weight");
}

void test_defaults_budget_accounting_and_migration() {
    AnimationStoreConfig config;
    config.instance_capacity = 1;
    config.mutable_budget_bytes = 4096;
    AnimationService service(config);
    const AnimAsset* first_asset = service.insert_asset(asset(2, 10));
    const AnimationRuntimeDefinition first_definition = definition();
    const Animator animator = service.create(first_asset, first_definition);
    CHECK(animator.valid(), "allocate within explicit budget");
    CHECK(service.number_value(service.input(animator.instance, "speed")) == 2.0f, "input default is installed");
    CHECK(service.mutable_bytes() == first_definition.mutable_bytes(), "mutable bytes account exactly once");
    CHECK(service.create(first_asset, first_definition).status == AnimationStatus::BudgetExceeded,
          "instance cap failure records budget status");
    AnimationStoreConfig tiny_budget;
    tiny_budget.mutable_budget_bytes = first_definition.mutable_bytes() - 1;
    AnimationService too_small(tiny_budget);
    CHECK(too_small.create(too_small.insert_asset(asset(3, 1)), first_definition).status == AnimationStatus::BudgetExceeded,
          "mutable byte budget fails before allocation");

    AnimationRuntimeDefinition compatible = first_definition;
    compatible.inputs[0].default_value = AnimationValue(1.0);
    CHECK(service.set(service.input(animator.instance, "speed"), 9.0f), "write before migration");
    Animator migrated = service.replace_asset(animator.instance, service.insert_asset(asset(2, 11)), compatible);
    CHECK(migrated.valid(), "complete compatible generation swap");
    CHECK(service.number_value(service.input(migrated.instance, "speed")) == 9.0f, "matching input migrates");

    AnimationRuntimeDefinition incompatible = compatible;
    incompatible.inputs[0].type = AnimationValueType::Bool;
    incompatible.inputs[0].default_value = AnimationValue(false);
    const AnimationInputHandle old = service.input(migrated.instance, "speed");
    Animator changed = service.replace_asset(migrated.instance, service.insert_asset(asset(2, 12)), incompatible);
    CHECK(changed.valid(), "changed schema swaps atomically");
    CHECK(!service.set(old, 3.0f), "schema swap invalidates old handles");
    CHECK(!service.bool_value(service.input(changed.instance, "speed")), "changed input resets to new default");
}

} // namespace

int main() {
    test_asset_dedup_and_handle_reuse();
    test_typed_cadence_and_target_contracts();
    test_defaults_budget_accounting_and_migration();
    return check_summary();
}
