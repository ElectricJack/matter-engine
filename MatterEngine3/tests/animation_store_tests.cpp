#include "animation/animation_store.h"
#include "check.h"

#include <cstring>
#include <cstdio>
#include <limits>

using namespace matter;
using namespace matter::animation;

namespace {

AnimAsset asset(uint64_t hash, uint64_t nonce) {
    AnimAsset value;
    value.resolved_hash = hash;
    value.nonce = {0, nonce};
    return value;
}

AnimAsset asset_with_section(uint64_t hash, uint64_t nonce, uint8_t byte) {
    AnimAsset value = asset(hash, nonce);
    value.sections.push_back({AnimSectionKind::RigSchema, {byte}});
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
        {"hand", TargetDriverKind::External, EvaluationCadence::Frame, {1,2,3}, true},
        {"foot", TargetDriverKind::Controller, EvaluationCadence::Fixed, {4,5,6}, true},
        {"footAux", TargetDriverKind::Controller, EvaluationCadence::Fixed, {7,8,9}, true},
    };
    result.graph_state_bytes = 16;
    result.controller_state_bytes = 8;
    result.sample_context_bytes = 4;
    result.pose_scratch_bytes = 12;
    return result;
}

struct RuntimeBindingFixture {
    OzzSkeleton skeleton;
    OzzAnimation clip;
    std::shared_ptr<AnimationEvaluationDefinition> evaluation =
        std::make_shared<AnimationEvaluationDefinition>();
    std::shared_ptr<AnimationRuntimeBindingDescriptor> descriptor =
        std::make_shared<AnimationRuntimeBindingDescriptor>();

    RuntimeBindingFixture() {
        RigDefinition rig;
        const auto add_chain = [&](const char* root, const char* mid,
                                   const char* end) {
            rig.joints.push_back(
                {root, "root", AnimationTransform{}, 1.0f,
                 {"test", 1, 1, root}});
            rig.joints.push_back(
                {mid, root, AnimationTransform{}, 1.0f,
                 {"test", 1, 1, mid}});
            rig.joints.push_back(
                {end, mid, AnimationTransform{}, 1.0f,
                 {"test", 1, 1, end}});
        };
        rig.joints.push_back(
            {"root", "", AnimationTransform{}, 1.0f,
             {"test", 1, 1, "root"}});
        add_chain("handRoot", "handMid", "handEnd");
        add_chain("footRoot", "footMid", "footEnd");
        add_chain("footAuxRoot", "footAuxMid", "footAuxEnd");
        ClipDefinition source;
        source.name = "idle";
        source.duration = 1.0f;
        source.rate = 30.0f;
        source.loop = true;
        source.source = {"test", 1, 1, "clip"};
        source.tracks.push_back(
            {"root",
             {{0.0f, AnimationTransform{}, {"test", 1, 1, "a"}},
              {1.0f, AnimationTransform{}, {"test", 1, 1, "b"}}},
             {"test", 1, 1, "track"}});
        Diagnostics diagnostics;
        CHECK(build_skeleton(rig, skeleton, diagnostics) &&
                  build_clip(rig, source, clip, diagnostics),
              "build store runtime-binding fixture");

        Mat4f identity{};
        identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1.0f;
        evaluation->skeleton = &skeleton;
        evaluation->clips = {{&clip, 1.0f, true, false}};
        evaluation->inputs = {
            {AnimationValueType::Number, EvaluationCadence::Fixed},
            {AnimationValueType::Float3, EvaluationCadence::Frame},
            {AnimationValueType::Symbol, EvaluationCadence::Fixed},
        };
        evaluation->nodes = {
            {RuntimeGraphNodeKind::Clip, {}, 0},
            {RuntimeGraphNodeKind::NativeController, {0}, UINT16_MAX,
             UINT16_MAX, {}, 1.0f, EvaluationCadence::Fixed, 0},
            {RuntimeGraphNodeKind::Output, {1}},
        };
        evaluation->inverse_bind_model.assign(rig.joints.size(), identity);
        descriptor->evaluation = evaluation;
        descriptor->fixed_work.clip.duration = 1.0f;
        descriptor->fixed_work.clip.loop = true;
        descriptor->fixed_work.clip.rate = 1.0f;
        const auto target = [](const char* name, std::vector<JointIndex> chain,
                               TargetDriverKind driver,
                               EvaluationCadence cadence,
                               const char* controller = "") {
            CanonicalTarget value;
            value.name = name;
            value.chain = std::move(chain);
            value.driver = driver;
            value.cadence = cadence;
            value.controller = controller;
            return value;
        };
        descriptor->targets = {
            target("hand", {1, 2, 3}, TargetDriverKind::External,
                   EvaluationCadence::Frame),
            target("foot", {4, 5, 6}, TargetDriverKind::Controller,
                   EvaluationCadence::Fixed, "gait"),
            target("footAux", {7, 8, 9}, TargetDriverKind::Controller,
                   EvaluationCadence::Fixed, "gait"),
        };
        GaitControllerParameters gait;
        gait.left_target = 1;
        gait.right_target = 2;
        AnimationRuntimeBindingDescriptor::Controller controller;
        controller.descriptor.type = kGaitControllerTypeId;
        controller.descriptor.cadence = EvaluationCadence::Fixed;
        controller.descriptor.parameters.resize(sizeof(gait));
        std::memcpy(controller.descriptor.parameters.data(), &gait, sizeof(gait));
        controller.target_indices = {1, 2};
        controller.inputs = {
            {0, AnimationValueType::Number, EvaluationCadence::Fixed},
        };
        descriptor->controllers.push_back(std::move(controller));
    }
};

AnimationRuntimeDefinition bound_definition() {
    static RuntimeBindingFixture fixture;
    AnimationRuntimeDefinition result = definition();
    result.binding = fixture.descriptor;
    return result;
}

void test_bound_definition_requires_exact_target_schema() {
    AnimationRuntimeDefinition malformed = bound_definition();
    malformed.targets.resize(1);
    auto descriptor =
        std::make_shared<AnimationRuntimeBindingDescriptor>(*malformed.binding);
    descriptor->targets.clear();
    descriptor->controllers.clear();
    auto evaluation =
        std::make_shared<AnimationEvaluationDefinition>(*descriptor->evaluation);
    evaluation->nodes = {
        {RuntimeGraphNodeKind::Clip, {}, 0},
        {RuntimeGraphNodeKind::Output, {0}},
    };
    descriptor->evaluation = evaluation;
    malformed.binding = descriptor;
    AnimationService service;
    CHECK(!service.create(service.insert_asset(asset(91, 1)), malformed).valid(),
          "bound definition cannot omit its runtime target schema");
}

void test_asset_dedup_and_handle_reuse() {
    AnimationService service;
    const AnimAsset* first = service.insert_asset(asset(7, 9));
    const AnimAsset* duplicate = service.insert_asset(asset(7, 9));
    CHECK(first != nullptr && first == duplicate, "immutable asset identity deduplicates hash and nonce");

    const Animator first_animator = service.create(first, definition());
    CHECK(first_animator.valid(), "create first animator");
    const AnimationInputHandle speed = service.input(first_animator.instance, "speed");
    const AnimationTargetHandle hand = service.target(first_animator.instance, "hand");
    CHECK(service.set(speed, 7.0f), "matching typed input write succeeds");
    CHECK(service.remove(first_animator.instance), "remove animator");
    const Animator replacement = service.create(first, definition());
    CHECK(replacement.valid() && replacement.instance.slot_index == first_animator.instance.slot_index,
          "free list reuses slot");
    CHECK(replacement.instance.generation != first_animator.instance.generation, "reuse increments generation");
    CHECK(!service.set(speed, 9.0f), "stale input never affects reused slot");
    CHECK(!service.set_enabled(hand, false), "stale target never affects reused slot");
}

void test_asset_release_requires_zero_live_instances() {
    AnimationService service;
    const AnimAsset* value = service.insert_asset(asset(71, 3));
    const Animator animator = service.create(value, definition());
    CHECK(value && animator.valid() && service.stats().active_assets == 1,
          "inserted immutable asset is reflected in runtime accounting");
    CHECK(!service.release_asset(value) && service.stats().active_assets == 1,
          "asset release is rejected while a live animator references it");
    CHECK(service.remove(animator.instance) && service.release_asset(value) &&
              service.stats().active_assets == 0,
          "last-instance removal permits immutable asset and schema release");
    CHECK(!service.release_asset(value),
          "released pointer cannot erase unrelated or replacement ownership");
}

void test_asset_identity_conflict_fails_without_mutating_store_or_runtime() {
    AnimationService service;
    const AnimAsset* first = service.insert_asset(asset_with_section(70, 1, 7));
    const AnimAsset* equal = service.insert_asset(asset_with_section(70, 1, 7));
    CHECK(first != nullptr && equal == first, "equal immutable asset payload reuses its identity");
    CHECK(service.create(first, definition()).valid(), "original immutable asset remains usable");
    const size_t bytes_before_conflict = service.mutable_bytes();

    const AnimAsset* conflicting = service.insert_asset(asset_with_section(70, 1, 8));
    CHECK(conflicting == nullptr, "conflicting immutable payload for an existing identity is rejected");
    CHECK(first->sections.size() == 1 && first->sections[0].bytes == std::vector<uint8_t>{7},
          "rejected payload cannot overwrite the original immutable asset");
    CHECK(service.create(conflicting, definition()).status == AnimationStatus::LoadFailed,
          "rejected immutable asset propagates a load failure through AnimationService");
    CHECK(service.mutable_bytes() == bytes_before_conflict,
          "rejected immutable asset cannot change mutable runtime accounting");
}

void test_typed_cadence_and_target_contracts() {
    AnimationService service;
    const Animator animator = service.create(service.insert_asset(asset(1, 1)), bound_definition());
    const AnimationInputHandle speed = service.input(animator.instance, "speed");
    const AnimationInputHandle aim = service.input(animator.instance, "aim");
    const AnimationInputHandle missing = service.input(animator.instance, "missing");
    CHECK(!service.set(speed, true), "wrong input value type rejected");
    CHECK(service.number_value(speed) == 2.0f, "rejected input write leaves stored bytes unchanged");
    AnimationInputHandle wrong_cadence = aim;
    wrong_cadence.cadence = AnimationCadence::Fixed;
    CHECK(!service.set(wrong_cadence, Float3{0,0,0}), "wrong input cadence rejected");
    AnimationInputHandle wrong_field = speed;
    wrong_field.schema_index = aim.schema_index;
    CHECK(!service.set(wrong_field, 3.0f), "wrong input schema index rejected");
    CHECK(service.number_value(speed) == 2.0f, "wrong field write leaves original value unchanged");
    AnimationInputHandle wrong_value_type = speed;
    wrong_value_type.value_type = AnimationValueType::Float3;
    CHECK(!service.set(wrong_value_type, Float3{}), "forged input value type rejected");
    AnimatorInstanceHandle malformed_instance = animator.instance;
    malformed_instance.schema_index = 0;
    CHECK(!service.input(malformed_instance, "speed").valid(), "malformed instance metadata is rejected");
    CHECK(!missing.valid(), "missing input is invalid");

    const AnimationTargetHandle hand = service.target(animator.instance, "hand");
    const AnimationTargetHandle foot = service.target(animator.instance, "foot");
    AnimationTargetHandle wrong_target_cadence = hand;
    wrong_target_cadence.cadence = AnimationCadence::Fixed;
    CHECK(!service.set_enabled(wrong_target_cadence, false), "wrong target cadence rejected");
    AnimationTargetHandle wrong_target_type = hand;
    wrong_target_type.value_type = AnimationValueType::Number;
    CHECK(!service.set_enabled(wrong_target_type, false), "wrong target value type rejected");
    CHECK(service.set_enabled(hand, false), "external target enabled state writes");
    AnimationTransform updated{}; updated.translation = {3, 4, 5};
    CHECK(service.set_transform(hand, updated), "disabled target continues to accept the latest desired transform during fade-out");
    CHECK(service.set_weight(hand, 0.25f), "disabled target continues to accept desired weight during fade-out");
    CHECK(service.set_enabled(hand, true), "re-enable target after desired updates");
    AnimationRuntimeBindingLease lease{};
    CHECK(service.runtime_binding(animator.instance, lease) && lease.target_enabled[hand.schema_index] == 1 &&
              lease.target_weights[hand.schema_index] == 1.0f &&
              lease.target_transforms[hand.schema_index].translation.x == 3.0f,
          "reenable restores full desired weight while retaining the most recent desired transform");
    CHECK(!service.set_weight(hand, std::numeric_limits<float>::quiet_NaN()), "non-finite target weight rejected without a refresh");
    CHECK(!service.set_transform(foot, AnimationTransform{}), "controller-owned target rejects transform");
    CHECK(!service.set_weight(foot, 1.0f), "controller-owned target rejects weight");
}

void test_api_control_writes_wait_for_their_declared_sampling_boundary() {
    AnimationService service;
    const Animator animator = service.create(service.insert_asset(asset(76, 1)), bound_definition());
    const AnimationInputHandle speed = service.input(animator.instance, "speed");
    const AnimationInputHandle aim = service.input(animator.instance, "aim");
    CHECK(animator.valid() && service.set(speed, 9.0f) && service.set(aim, Float3{9, 8, 7}),
          "queue fixed and frame API writes through the public service");
    AnimationRuntimeBindingLease lease{};
    CHECK(service.runtime_binding(animator.instance, lease) &&
              lease.fixed_current[speed.schema_index].number == 2.0 &&
              lease.frame_controls[aim.schema_index].float3.x == 1.0f,
          "API writes remain pending until their respective fixed or frame sampling boundary");
}

void test_hard_caps_and_bind_pose_degradation() {
    AnimationStoreConfig requested;
    requested.instance_capacity = std::numeric_limits<uint32_t>::max();
    requested.mutable_budget_bytes = std::numeric_limits<size_t>::max();
    AnimationService service(requested);
    const AnimationRuntimeStats effective = service.stats();
    CHECK(effective.instance_capacity == 4096, "instance capacity cannot override hard cap");
    CHECK(effective.mutable_budget_bytes == 64u * 1024u * 1024u,
          "mutable-state budget cannot override hard cap");

    AnimationRuntimeDefinition empty;
    const AnimAsset* value = service.insert_asset(asset(41, 1));
    Animator last;
    for (uint32_t i = 0; i < 4096; ++i) last = service.create(value, empty);
    CHECK(last.valid(), "hard-cap service allocates all permitted instances");
    const Animator capped = service.create(value, empty);
    CHECK(capped.status == AnimationStatus::BudgetExceeded && capped.bind_pose_fallback,
          "instance cap reports recoverable bind-pose fallback");

    AnimationStoreConfig memory_requested;
    memory_requested.mutable_budget_bytes = std::numeric_limits<size_t>::max();
    AnimationService memory_service(memory_requested);
    AnimationRuntimeDefinition oversized;
    oversized.graph_state_bytes = 64u * 1024u * 1024u + 1u;
    const Animator over_budget = memory_service.create(memory_service.insert_asset(asset(42, 1)), oversized);
    CHECK(over_budget.status == AnimationStatus::BudgetExceeded && over_budget.bind_pose_fallback,
          "mutable hard cap reports recoverable bind-pose fallback");
}

void test_conflicting_deduplicated_schema_is_rejected() {
    AnimationService service;
    const AnimAsset* first = service.insert_asset(asset(50, 1));
    const AnimAsset* duplicate = service.insert_asset(asset(50, 1));
    const AnimationRuntimeDefinition baseline = definition();
    CHECK(service.create(first, baseline).valid(), "first definition fixes deduplicated asset schema");

    AnimationRuntimeDefinition conflicting = baseline;
    conflicting.graph_state_bytes += 64;
    const Animator rejected = service.create(duplicate, conflicting);
    CHECK(rejected.status == AnimationStatus::LoadFailed,
          "conflicting schema for deduplicated asset is rejected before allocation");
    CHECK(service.mutable_bytes() == baseline.mutable_bytes(),
          "rejected conflicting schema cannot corrupt mutable accounting");
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

void test_shared_mutable_reservation_releases_and_rolls_back() {
    const AnimationRuntimeDefinition baseline = bound_definition();
    AnimationStoreConfig config;
    config.mutable_budget_bytes = baseline.mutable_bytes();
    AnimationService service(config);
    const AnimAsset* first = service.insert_asset(asset(93, 1));
    const Animator original = service.create(first, baseline);
    CHECK(original.valid() && service.stats().mutable_bytes == baseline.mutable_bytes(),
          "the full shared runtime reservation admits exactly once");

    AnimationRuntimeDefinition larger = baseline;
    ++larger.pose_scratch_bytes;
    const Animator rejected = service.replace_asset(original.instance, first, larger);
    const AnimationRuntimeStats after_reject = service.stats();
    CHECK(rejected.status == AnimationStatus::BudgetExceeded &&
              after_reject.mutable_bytes == baseline.mutable_bytes() &&
              service.status(original.instance) == AnimationStatus::Ok,
          "a failed reload leaves the prior shared reservation and instance live");

    CHECK(service.remove(original.instance) && service.stats().mutable_bytes == 0,
          "destroying an animator returns every reserved runtime-state byte");
    const Animator replacement = service.create(first, baseline);
    CHECK(replacement.valid() && service.stats().mutable_bytes == baseline.mutable_bytes(),
          "released shared capacity is reusable by a later animator");
}

void test_public_runtime_stats_expose_aggregate_counters_and_reasons() {
    AnimationStoreConfig config;
    config.instance_capacity = 1;
    AnimationService service(config);
    const AnimAsset* value = service.insert_asset(asset(94, 1));
    const AnimationRuntimeDefinition runtime = definition();
    const Animator admitted = service.create(value, runtime);
    const Animator rejected = service.create(value, runtime);
    const AnimationRuntimeStats stats = service.stats();
    CHECK(admitted.valid() && rejected.status == AnimationStatus::BudgetExceeded &&
              stats.evaluated_pose_count == 0 && stats.world_query_count == 0 &&
              stats.submitted_skin_work_items == 0 && stats.fallback_count == 1 &&
              stats.fallback_counts[static_cast<size_t>(AnimationRuntimeFallbackReason::RuntimeInstanceLimit)] == 1,
          "public runtime stats expose aggregate counters and exact fallback reasons without runtime internals");
}

} // namespace

int main() {
    test_bound_definition_requires_exact_target_schema();
    test_asset_dedup_and_handle_reuse();
    test_asset_release_requires_zero_live_instances();
    test_asset_identity_conflict_fails_without_mutating_store_or_runtime();
    test_typed_cadence_and_target_contracts();
    test_api_control_writes_wait_for_their_declared_sampling_boundary();
    test_defaults_budget_accounting_and_migration();
    test_shared_mutable_reservation_releases_and_rolls_back();
    test_public_runtime_stats_expose_aggregate_counters_and_reasons();
    test_hard_caps_and_bind_pose_degradation();
    test_conflicting_deduplicated_schema_is_rejected();
    return check_summary();
}
