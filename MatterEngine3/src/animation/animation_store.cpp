#include "animation/animation_store.h"

#include "animation/animation_asset_store.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace matter::animation {
struct StoredValue {
    AnimationValueType type = AnimationValueType::Number;
    bool boolean = false;
    float number = 0.0f;
    Float3 float3{};
    Quaternion quaternion{};
    AnimationTransform transform{};
    uint32_t symbol = 0;
};

namespace {

constexpr uint32_t kInvalid = UINT32_MAX;

AnimationStoreConfig bounded_config(AnimationStoreConfig requested) {
    const AnimationBudgetConfig central{};
    const auto bounded = [](auto requested_value, auto default_value, auto hard_value) {
        return requested_value == 0 ? default_value : std::min(requested_value, hard_value);
    };
    requested.instance_capacity = bounded(requested.instance_capacity, central.max_runtime_instances,
                                          AnimationBudgetConfig::kHardMaxRuntimeInstances);
    requested.mutable_budget_bytes = bounded(requested.mutable_budget_bytes, central.max_mutable_bytes,
                                             AnimationBudgetConfig::kHardMaxMutableBytes);
    requested.asset_capacity = bounded(requested.asset_capacity, central.max_assets,
                                       AnimationBudgetConfig::kHardMaxAssets);
    requested.max_joints_per_asset = bounded(requested.max_joints_per_asset, central.max_joints_per_asset,
                                             AnimationBudgetConfig::kHardMaxJointsPerAsset);
    requested.max_graph_nodes = bounded(requested.max_graph_nodes, central.max_graph_nodes,
                                        AnimationBudgetConfig::kHardMaxGraphNodes);
    requested.max_controller_nodes = bounded(requested.max_controller_nodes, central.max_controller_nodes,
                                             AnimationBudgetConfig::kHardMaxControllerNodes);
    return requested;
}

AnimationBudgetConfig runtime_budget(const AnimationStoreConfig& config) {
    AnimationBudgetConfig result{};
    result.max_assets = config.asset_capacity;
    result.max_runtime_instances = config.instance_capacity;
    result.max_joints_per_asset = config.max_joints_per_asset;
    result.max_graph_nodes = config.max_graph_nodes;
    result.max_controller_nodes = config.max_controller_nodes;
    result.max_mutable_bytes = config.mutable_budget_bytes;
    return result;
}

AnimationCadence public_cadence(EvaluationCadence cadence) {
    return cadence == EvaluationCadence::Fixed ? AnimationCadence::Fixed : AnimationCadence::Frame;
}

uint32_t symbol_id(const std::string& value) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : value) { hash ^= c; hash *= 16777619u; }
    return hash;
}

StoredValue store_value(const AnimationValue& value, AnimationValueType type) {
    StoredValue result;
    result.type = type;
    switch (type) {
        case AnimationValueType::Bool: result.boolean = value.boolean; break;
        case AnimationValueType::Number: result.number = static_cast<float>(value.number); break;
        case AnimationValueType::Float3: result.float3 = value.float3; break;
        case AnimationValueType::Quaternion: result.quaternion = value.quaternion; break;
        case AnimationValueType::Transform: result.transform = value.transform; break;
        case AnimationValueType::Symbol: result.symbol = symbol_id(value.symbol); break;
    }
    return result;
}

struct TargetState {
    bool enabled = true;
    float weight = 1.0f;
    AnimationTransform transform{};
    bool snap_requested = false;
};

struct Slot {
    bool alive = false;
    uint32_t generation = 0;
    const AnimAsset* asset = nullptr;
    const AnimationRuntimeDefinition* definition = nullptr;
    std::vector<StoredValue> fixed_previous;
    std::vector<StoredValue> fixed_current;
    std::vector<StoredValue> fixed_pending;
    std::vector<StoredValue> frame_controls;
    std::vector<StoredValue> frame_pending;
    std::vector<TargetState> targets;
    std::vector<uint8_t> graph_state;
    std::vector<uint8_t> controller_state;
    std::vector<uint8_t> sample_context;
    std::vector<uint8_t> pose_scratch;
};

bool same_target(const RuntimeTargetDefinition& left, const RuntimeTargetDefinition& right) {
    return left.name == right.name && left.driver == right.driver && left.cadence == right.cadence &&
           left.joint_chain == right.joint_chain;
}

bool same_input(const RuntimeInputDefinition& left, const RuntimeInputDefinition& right) {
    return left.name == right.name && left.type == right.type && left.cadence == right.cadence;
}

bool same_float3(const Float3& left, const Float3& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool same_quaternion(const Quaternion& left, const Quaternion& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z && left.w == right.w;
}

bool same_transform(const AnimationTransform& left, const AnimationTransform& right) {
    return same_float3(left.translation, right.translation) && same_quaternion(left.rotation, right.rotation) &&
           same_float3(left.scale, right.scale);
}

bool finite_transform(const AnimationTransform& value) {
    return std::isfinite(value.translation.x) && std::isfinite(value.translation.y) && std::isfinite(value.translation.z) &&
           std::isfinite(value.rotation.x) && std::isfinite(value.rotation.y) && std::isfinite(value.rotation.z) && std::isfinite(value.rotation.w) &&
           std::isfinite(value.scale.x) && std::isfinite(value.scale.y) && std::isfinite(value.scale.z);
}

bool same_value(const AnimationValue& left, const AnimationValue& right) {
    if (left.type != right.type) return false;
    switch (left.type) {
        case AnimationValueType::Bool: return left.boolean == right.boolean;
        case AnimationValueType::Number: return left.number == right.number;
        case AnimationValueType::Float3: return same_float3(left.float3, right.float3);
        case AnimationValueType::Quaternion: return same_quaternion(left.quaternion, right.quaternion);
        case AnimationValueType::Transform: return same_transform(left.transform, right.transform);
        case AnimationValueType::Symbol: return left.symbol == right.symbol;
    }
    return false;
}

bool same_runtime_target(const RuntimeTargetDefinition& left, const RuntimeTargetDefinition& right) {
    return same_target(left, right) && left.enabled == right.enabled;
}

bool same_definition(const AnimationRuntimeDefinition& left, const AnimationRuntimeDefinition& right) {
    if (left.graph_state_bytes != right.graph_state_bytes ||
        left.controller_state_bytes != right.controller_state_bytes ||
        left.sample_context_bytes != right.sample_context_bytes ||
        left.pose_scratch_bytes != right.pose_scratch_bytes || left.binding.get() != right.binding.get() ||
        left.inputs.size() != right.inputs.size() || left.targets.size() != right.targets.size()) return false;
    for (size_t i = 0; i < left.inputs.size(); ++i) {
        if (!same_input(left.inputs[i], right.inputs[i]) ||
            !same_value(left.inputs[i].default_value, right.inputs[i].default_value)) return false;
    }
    for (size_t i = 0; i < left.targets.size(); ++i) {
        if (!same_runtime_target(left.targets[i], right.targets[i])) return false;
    }
    return true;
}

bool valid_binding(const std::shared_ptr<const AnimationRuntimeBindingDescriptor>& binding,
                   const AnimationBudgetConfig& budget) {
    if (!binding) return true; // Legacy definitions are deliberately unbound.
    const AnimationEvaluationDefinition* evaluation = binding->evaluation.get();
    const AnimationFixedWork& work = binding->fixed_work;
    if (!evaluation || !valid_animation_evaluation_definition(*evaluation) ||
        evaluation->skeleton->joint_count() > budget.max_joints_per_asset ||
        evaluation->nodes.size() > budget.max_graph_nodes ||
        !std::isfinite(work.clip.duration) || work.clip.duration <= 0.0f ||
        !std::isfinite(work.clip.time) || !std::isfinite(work.clip.rate)) return false;
    for (const RuntimeClipMarker& marker : work.clip.markers)
        if (!std::isfinite(marker.time) || marker.time < 0.0f || marker.time > work.clip.duration) return false;
    for (const AnimationWorldQueryRequest& query : work.queries)
        if (!std::isfinite(query.max_distance) || query.max_distance < 0.0f) return false;
    if (binding->targets.size()>kMaxTargets ||
        !validate_exclusive_target_chains(binding->targets)) return false;
    for (const CanonicalTarget& target : binding->targets)
        for (JointIndex joint : target.chain)
            if (joint >= evaluation->skeleton->joint_count()) return false;
    std::set<uint16_t> controller_targets;
    NativeControllerRegistry registry=NativeControllerRegistry::with_v1_controllers();
    if (binding->controllers.size() > budget.max_controller_nodes) return false;
    for(const auto& controller:binding->controllers) {
        if(controller.descriptor.cadence!=EvaluationCadence::Fixed ||
           !std::isfinite(static_cast<double>(controller.priority))) return false;
        NativeControllerLayout layout{}; if(!registry.create(controller.descriptor,layout) || layout.frame_state_bytes!=0 || layout.fixed_state_bytes>64u*1024u) return false;
        // Controller inputs are an ordered, fixed-only capability list.  Do
        // not permit an undeclared frame value to leak into deterministic
        // fixed simulation.
        for (const auto& input : controller.inputs) {
            if (input.cadence != EvaluationCadence::Fixed || input.input_index >= evaluation->inputs.size() ||
                evaluation->inputs[input.input_index].cadence != EvaluationCadence::Fixed ||
                evaluation->inputs[input.input_index].type != input.type) return false;
        }
        for(uint16_t target:controller.target_indices) if(target>=binding->targets.size() || binding->targets[target].driver!=TargetDriverKind::Controller || !controller_targets.insert(target).second) return false;
    }
    return true;
}

bool valid_definition(const AnimationRuntimeDefinition& definition,
                      const AnimationBudgetConfig& budget) {
    for (const RuntimeInputDefinition& input : definition.inputs) {
        if (input.default_value.type != input.type) return false;
    }
    if (!valid_binding(definition.binding, budget)) return false;
    if (definition.binding) {
        const auto& evaluation = *definition.binding->evaluation;
        if (evaluation.inputs.size() != definition.inputs.size()) return false;
        for (size_t i = 0; i < definition.inputs.size(); ++i)
            if (evaluation.inputs[i].type != definition.inputs[i].type || evaluation.inputs[i].cadence != definition.inputs[i].cadence) return false;
        if (definition.binding->target_index != UINT16_MAX && definition.binding->target_index >= definition.targets.size()) return false;
        if (definition.binding->targets.size() != definition.targets.size()) return false;
        for (size_t i = 0; i < definition.targets.size(); ++i) {
            const RuntimeTargetDefinition& runtime = definition.targets[i];
            const CanonicalTarget& canonical = definition.binding->targets[i];
            if (runtime.name != canonical.name ||
                runtime.driver != canonical.driver ||
                runtime.cadence != canonical.cadence ||
                runtime.joint_chain != canonical.chain ||
                runtime.enabled != canonical.enabled) return false;
        }
    }
    return definition.mutable_bytes() != std::numeric_limits<size_t>::max();
}

AnimationRuntimeBindingLease::Value animation_value(const StoredValue& value) {
    AnimationRuntimeBindingLease::Value result{};
    result.type = value.type; result.boolean = value.boolean; result.number = value.number;
    result.float3 = value.float3; result.quaternion = value.quaternion;
    result.transform = value.transform; result.symbol = value.symbol;
    return result;
}

AnimationValue checkpoint_value(const StoredValue& value) {
    switch (value.type) {
        case AnimationValueType::Bool: return AnimationValue(value.boolean);
        case AnimationValueType::Number: return AnimationValue(static_cast<double>(value.number));
        case AnimationValueType::Float3: return AnimationValue(value.float3);
        case AnimationValueType::Quaternion: return AnimationValue(value.quaternion);
        case AnimationValueType::Transform: return AnimationValue(value.transform);
        case AnimationValueType::Symbol: { AnimationValue result{}; result.type=AnimationValueType::Symbol; result.symbol=std::to_string(value.symbol); return result; }
    }
    return {};
}

bool checkpoint_stored_value(const AnimationValue& value, AnimationValueType expected, StoredValue& out) {
    if(value.type!=expected) return false;
    out={}; out.type=expected;
    switch(expected) {
        case AnimationValueType::Bool: out.boolean=value.boolean; return true;
        case AnimationValueType::Number: if(!std::isfinite(value.number)) return false; out.number=static_cast<float>(value.number); return std::isfinite(out.number);
        case AnimationValueType::Float3: out.float3=value.float3; return std::isfinite(out.float3.x)&&std::isfinite(out.float3.y)&&std::isfinite(out.float3.z);
        case AnimationValueType::Quaternion: out.quaternion=value.quaternion; return std::isfinite(out.quaternion.x)&&std::isfinite(out.quaternion.y)&&std::isfinite(out.quaternion.z)&&std::isfinite(out.quaternion.w);
        case AnimationValueType::Transform: out.transform=value.transform; return finite_transform(out.transform);
        case AnimationValueType::Symbol: {
            uint64_t parsed=0; if(value.symbol.empty()) return false;
            for(char c:value.symbol) { if(c<'0'||c>'9'||parsed>(UINT32_MAX-uint64_t(c-'0'))/10u) return false; parsed=parsed*10u+uint64_t(c-'0'); }
            out.symbol=static_cast<uint32_t>(parsed); return true;
        }
    }
    return false;
}

Slot make_slot(const AnimAsset* asset, const AnimationRuntimeDefinition* definition, uint32_t generation) {
    Slot slot;
    slot.alive = true;
    slot.generation = generation;
    slot.asset = asset;
    slot.definition = definition;
    slot.fixed_previous.resize(definition->inputs.size());
    slot.fixed_current.resize(definition->inputs.size());
    slot.fixed_pending.resize(definition->inputs.size());
    slot.frame_controls.resize(definition->inputs.size());
    slot.frame_pending.resize(definition->inputs.size());
    for (size_t i = 0; i < definition->inputs.size(); ++i) {
        const StoredValue value = store_value(definition->inputs[i].default_value, definition->inputs[i].type);
        slot.fixed_previous[i] = value;
        slot.fixed_current[i] = value;
        slot.fixed_pending[i] = value;
        slot.frame_controls[i] = value;
        slot.frame_pending[i] = value;
    }
    slot.targets.resize(definition->targets.size());
    for (size_t i = 0; i < definition->targets.size(); ++i) slot.targets[i].enabled = definition->targets[i].enabled;
    slot.graph_state.resize(definition->graph_state_bytes);
    slot.controller_state.resize(definition->controller_state_bytes);
    slot.sample_context.resize(definition->sample_context_bytes);
    slot.pose_scratch.resize(definition->pose_scratch_bytes);
    return slot;
}

} // namespace

size_t AnimationRuntimeDefinition::mutable_bytes() const {
    constexpr size_t kMax = std::numeric_limits<size_t>::max();
    size_t total = 0;
    const auto add = [&total](size_t value) {
        if (value > std::numeric_limits<size_t>::max() - total) {
            total = std::numeric_limits<size_t>::max();
            return false;
        }
        total += value;
        return true;
    };
    if (inputs.size() > kMax / (3u * sizeof(StoredValue)) ||
        targets.size() > kMax / sizeof(TargetState)) return kMax;
    if (!add(inputs.size() * 3u * sizeof(StoredValue)) || !add(targets.size() * sizeof(TargetState)) ||
        !add(graph_state_bytes) || !add(controller_state_bytes) || !add(sample_context_bytes) ||
        !add(pose_scratch_bytes)) return kMax;
    return total;
}

class AnimationServiceImpl {
public:
    explicit AnimationServiceImpl(AnimationStoreConfig config) : config_(bounded_config(config)), budget_(runtime_budget(config_)) {
        slots_.reserve(config_.instance_capacity);
        free_indices_.reserve(config_.instance_capacity);
    }
    ~AnimationServiceImpl() { attach_runtime_systems(nullptr, nullptr); }

    const AnimAsset* insert_asset(AnimAsset asset) {
        if (assets_.size() >= budget_.max_assets && !assets_.find(asset.resolved_hash, asset.nonce)) return nullptr;
        return assets_.insert(std::move(asset));
    }

    bool release_asset(const AnimAsset* asset) {
        if (!owns(asset)) return false;
        for (const Slot& slot : slots_)
            if (slot.alive && slot.asset == asset) return false;
        schemas_.erase(asset);
        return assets_.erase(asset);
    }

    Animator create(const AnimAsset* asset, const AnimationRuntimeDefinition& definition) {
        if (!owns(asset)) return {{}, AnimationStatus::LoadFailed};
        const AnimationRuntimeDefinition* schema = schema_for(asset, definition);
        if (!schema) return {{}, AnimationStatus::LoadFailed};
        if (!can_fit(schema->mutable_bytes()) || active_count_ == config_.instance_capacity) {
            return {{}, AnimationStatus::BudgetExceeded, true};
        }
        uint32_t index;
        if (free_indices_.empty()) {
            index = static_cast<uint32_t>(slots_.size());
            slots_.push_back({});
        } else {
            index = free_indices_.back();
            free_indices_.pop_back();
        }
        Slot& old = slots_[index];
        const uint32_t generation = old.generation + 1u;
        old = make_slot(asset, schema, generation);
        mutable_bytes_ += schema->mutable_bytes();
        ++active_count_;
        const Animator result{instance_handle(index, old), AnimationStatus::Ok};
        if (!refresh_runtime_binding(result.instance, old)) {
            old.alive = false;
            old.asset = nullptr;
            mutable_bytes_ -= schema->mutable_bytes();
            --active_count_;
            free_indices_.push_back(index);
            return {{}, AnimationStatus::LoadFailed};
        }
        return result;
    }

    Animator replace(AnimatorInstanceHandle handle, const AnimAsset* asset, const AnimationRuntimeDefinition& definition) {
        Slot* old = slot(handle);
        if (!old) return {{}, AnimationStatus::InvalidHandle};
        if (!owns(asset)) return {{}, AnimationStatus::LoadFailed};
        // A live instance may intentionally move to a new immutable graph for
        // the same asset identity.  Keep the old schema alive for siblings,
        // then publish a generation-qualified replacement transactionally.
        const AnimationRuntimeDefinition* schema = schema_for(asset, definition, true);
        if (!schema) return {{}, AnimationStatus::LoadFailed};
        const size_t old_bytes = old->definition->mutable_bytes();
        const size_t new_bytes = schema->mutable_bytes();
        if (mutable_bytes_ < old_bytes || new_bytes > config_.mutable_budget_bytes - (mutable_bytes_ - old_bytes)) {
            return {{}, AnimationStatus::BudgetExceeded, true};
        }
        Slot replacement = make_slot(asset, schema, old->generation + 1u);
        for (size_t next = 0; next < schema->inputs.size(); ++next) {
            for (size_t prior = 0; prior < old->definition->inputs.size(); ++prior) {
                if (same_input(schema->inputs[next], old->definition->inputs[prior])) {
                    replacement.fixed_previous[next] = old->fixed_previous[prior];
                    replacement.fixed_current[next] = old->fixed_current[prior];
                    replacement.fixed_pending[next] = old->fixed_pending[prior];
                    replacement.frame_controls[next] = old->frame_controls[prior];
                    replacement.frame_pending[next] = old->frame_pending[prior];
                    break;
                }
            }
        }
        for (size_t next = 0; next < schema->targets.size(); ++next) {
            for (size_t prior = 0; prior < old->definition->targets.size(); ++prior) {
                if (same_target(schema->targets[next], old->definition->targets[prior])) {
                    replacement.targets[next] = old->targets[prior];
                    break;
                }
            }
        }
        const uint32_t index = handle.slot_index;
        const AnimatorInstanceHandle stale = instance_handle(index, *old);
        Slot prior = std::move(*old);
        *old = std::move(replacement);
        mutable_bytes_ = mutable_bytes_ - old_bytes + new_bytes;
        if (systems_) systems_->detach_service_binding(stale);
        const Animator result{instance_handle(index, *old), AnimationStatus::Ok};
        if (!refresh_runtime_binding(result.instance, *old)) {
            if (systems_) systems_->detach_service_binding(result.instance);
            *old = std::move(prior);
            mutable_bytes_ = mutable_bytes_ - new_bytes + old_bytes;
            (void)refresh_runtime_binding(stale, *old);
            return {{}, AnimationStatus::LoadFailed};
        }
        return result;
    }

    bool remove(AnimatorInstanceHandle handle) {
        Slot* value = slot(handle);
        if (!value) return false;
        if (systems_) systems_->detach_service_binding(handle);
        mutable_bytes_ -= value->definition->mutable_bytes();
        value->alive = false;
        value->asset = nullptr;
        ++value->generation;
        --active_count_;
        free_indices_.push_back(handle.slot_index);
        return true;
    }

    AnimationInputHandle input(AnimatorInstanceHandle handle, std::string_view name) const {
        const Slot* value = slot(handle);
        if (!value) return {};
        for (uint32_t i = 0; i < value->definition->inputs.size(); ++i) {
            const RuntimeInputDefinition& input = value->definition->inputs[i];
            if (input.name == name) return {handle.slot_index, value->generation, i, input.type, public_cadence(input.cadence)};
        }
        return {};
    }

    AnimationTargetHandle target(AnimatorInstanceHandle handle, std::string_view name) const {
        const Slot* value = slot(handle);
        if (!value) return {};
        for (uint32_t i = 0; i < value->definition->targets.size(); ++i) {
            const RuntimeTargetDefinition& target = value->definition->targets[i];
            if (target.name == name) return {handle.slot_index, value->generation, i, AnimationValueType::Transform, public_cadence(target.cadence)};
        }
        return {};
    }

    bool set(AnimationInputHandle handle, const StoredValue& value, AnimationValueType type) {
        if (!handle.valid()) return false;
        Slot* owner = slot(handle.slot_index, handle.generation);
        if (!owner || handle.schema_index >= owner->definition->inputs.size()) return false;
        const RuntimeInputDefinition& schema = owner->definition->inputs[handle.schema_index];
        if (handle.value_type != type || schema.type != type || handle.cadence != public_cadence(schema.cadence)) return false;
        if (schema.cadence == EvaluationCadence::Fixed) owner->fixed_pending[handle.schema_index] = value;
        else owner->frame_pending[handle.schema_index] = value;
        return true;
    }

    bool set_enabled(AnimationTargetHandle handle, bool enabled) {
        TargetState* state = writable_target(handle);
        if (!state) return false;
        state->enabled = enabled;
        // Disable is a fade request, not a write lock.  Re-enabling starts
        // from a full desired influence while retaining any transform/weight
        // supplied during the fade-out interval.
        if (enabled) state->weight = 1.0f;
        Slot* owner = slot(handle.slot_index, handle.generation);
        refresh_runtime_binding(instance_handle(handle.slot_index, *owner), *owner);
        return true;
    }
    bool set_weight(AnimationTargetHandle handle, float weight) {
        TargetState* state = writable_target(handle);
        if (!state || !std::isfinite(weight) || weight < 0.0f || weight > 1.0f) return false;
        state->weight = weight;
        Slot* owner = slot(handle.slot_index, handle.generation);
        refresh_runtime_binding(instance_handle(handle.slot_index, *owner), *owner);
        return true;
    }
    bool set_transform(AnimationTargetHandle handle, const AnimationTransform& transform) {
        TargetState* state = writable_target(handle);
        if (!state || !finite_transform(transform)) return false;
        state->transform = transform;
        Slot* owner = slot(handle.slot_index, handle.generation);
        refresh_runtime_binding(instance_handle(handle.slot_index, *owner), *owner);
        return true;
    }
    bool snap(AnimationTargetHandle handle) {
        TargetState* state = writable_target(handle);
        if (!state) return false;
        state->snap_requested = true;
        Slot* owner = slot(handle.slot_index, handle.generation);
        refresh_runtime_binding(instance_handle(handle.slot_index, *owner), *owner);
        return true;
    }

    AnimationStatus status(AnimatorInstanceHandle handle) const { return slot(handle) ? AnimationStatus::Ok : AnimationStatus::InvalidHandle; }
    AnimationRuntimeStats stats() const {
        return {active_count_, static_cast<uint32_t>(assets_.size()),
                config_.instance_capacity, mutable_bytes_, config_.mutable_budget_bytes,
                config_.asset_capacity, config_.max_joints_per_asset,
                config_.max_graph_nodes, config_.max_controller_nodes};
    }
    size_t mutable_bytes() const { return mutable_bytes_; }
    float number_value(AnimationInputHandle handle) const { const StoredValue* v = read_pending_input(handle, AnimationValueType::Number); return v ? v->number : 0.0f; }
    bool bool_value(AnimationInputHandle handle) const { const StoredValue* v = read_pending_input(handle, AnimationValueType::Bool); return v && v->boolean; }

    bool sample_fixed_controls() { return sample_controls(EvaluationCadence::Fixed); }
    bool sample_frame_controls() { return sample_controls(EvaluationCadence::Frame); }

    bool capture_runtime_checkpoints(std::vector<AnimatorCheckpoint>& out) const {
        std::vector<AnimatorCheckpoint> captured;
        size_t bytes=0;
        for(uint32_t index=0; index<slots_.size(); ++index) {
            const Slot& value=slots_[index];
            if(!value.alive || !value.definition->binding) continue;
            AnimatorCheckpoint checkpoint{};
            checkpoint.instance=instance_handle(index,value);
            checkpoint.asset_identity=value.asset->resolved_hash;
            checkpoint.asset_nonce_high=value.asset->nonce.high;
            checkpoint.asset_nonce_low=value.asset->nonce.low;
            checkpoint.descriptor_identity=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value.definition->binding.get()));
            checkpoint.fixed_inputs.reserve(value.fixed_current.size()); checkpoint.fixed_previous_inputs.reserve(value.fixed_previous.size());
            checkpoint.frame_inputs.reserve(value.frame_controls.size());
            for(const StoredValue& input:value.fixed_current) checkpoint.fixed_inputs.push_back(checkpoint_value(input));
            for(const StoredValue& input:value.fixed_previous) checkpoint.fixed_previous_inputs.push_back(checkpoint_value(input));
            for(const StoredValue& input:value.frame_controls) checkpoint.frame_inputs.push_back(checkpoint_value(input));
            checkpoint.target_desired.reserve(value.targets.size()); checkpoint.target_weights.reserve(value.targets.size());
            checkpoint.target_enabled_states.reserve(value.targets.size()); checkpoint.target_snap_requested_states.reserve(value.targets.size());
            for(const TargetState& target:value.targets) { checkpoint.target_desired.push_back(target.transform); checkpoint.target_weights.push_back(target.weight); checkpoint.target_enabled_states.push_back(target.enabled?1u:0u); checkpoint.target_snap_requested_states.push_back(target.snap_requested?1u:0u); }
            checkpoint.graph_state=value.graph_state; checkpoint.controller_state=value.controller_state;
            checkpoint.sample_context_state=value.sample_context; checkpoint.pose_scratch_state=value.pose_scratch;
            if(systems_ && !systems_->capture_service_checkpoint(checkpoint)) return false;
            if(!checkpoint.bounded() || checkpoint.serialized_size()>64u*1024u-bytes) return false;
            bytes+=checkpoint.serialized_size(); captured.push_back(std::move(checkpoint));
        }
        out=std::move(captured);
        return true;
    }

    bool validate_runtime_checkpoints(const std::vector<AnimatorCheckpoint>& checkpoints) const {
        size_t expected=0, bytes=0; std::set<uint64_t> seen;
        for(const Slot& value:slots_) if(value.alive && value.definition->binding) ++expected;
        if(checkpoints.size()!=expected) return false;
        for(const AnimatorCheckpoint& checkpoint:checkpoints) {
            if(!checkpoint.instance.valid() || !checkpoint.bounded() || checkpoint.serialized_size()>64u*1024u-bytes) return false;
            bytes+=checkpoint.serialized_size();
            const uint64_t slot_key=(uint64_t(checkpoint.instance.slot_index)<<32u)|checkpoint.instance.generation;
            if(!seen.insert(slot_key).second) return false;
            const Slot* value=slot(checkpoint.instance);
            if(!value || !value->definition->binding || value->asset->resolved_hash!=checkpoint.asset_identity ||
               value->asset->nonce.high!=checkpoint.asset_nonce_high || value->asset->nonce.low!=checkpoint.asset_nonce_low ||
               static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value->definition->binding.get()))!=checkpoint.descriptor_identity) return false;
            const size_t inputs=value->definition->inputs.size(), targets=value->definition->targets.size();
            if(checkpoint.fixed_inputs.size()!=inputs || checkpoint.fixed_previous_inputs.size()!=inputs || checkpoint.frame_inputs.size()!=inputs ||
               checkpoint.target_desired.size()!=targets || checkpoint.target_weights.size()!=targets ||
               checkpoint.target_enabled_states.size()!=targets || checkpoint.target_snap_requested_states.size()!=targets ||
               checkpoint.graph_state.size()!=value->graph_state.size() || checkpoint.controller_state.size()!=value->controller_state.size() ||
               checkpoint.sample_context_state.size()!=value->sample_context.size() || checkpoint.pose_scratch_state.size()!=value->pose_scratch.size()) return false;
            StoredValue decoded{};
            for(size_t i=0;i<inputs;++i) if(!checkpoint_stored_value(checkpoint.fixed_inputs[i],value->definition->inputs[i].type,decoded) ||
                !checkpoint_stored_value(checkpoint.fixed_previous_inputs[i],value->definition->inputs[i].type,decoded) ||
                !checkpoint_stored_value(checkpoint.frame_inputs[i],value->definition->inputs[i].type,decoded)) return false;
            for(size_t target=0;target<targets;++target)
                if(!std::isfinite(checkpoint.target_weights[target]) || !finite_transform(checkpoint.target_desired[target])) return false;
            if(systems_ && !systems_->validate_service_checkpoint(checkpoint)) return false;
        }
        return true;
    }

    bool restore_runtime_checkpoints(const std::vector<AnimatorCheckpoint>& checkpoints) {
        if(!validate_runtime_checkpoints(checkpoints)) return false;
        struct RestoredSlot { Slot* slot=nullptr; std::vector<StoredValue> fixed_current, fixed_previous, frame; };
        std::vector<RestoredSlot> restored; restored.reserve(checkpoints.size());
        for(const AnimatorCheckpoint& checkpoint:checkpoints) {
            Slot* value=slot(checkpoint.instance); RestoredSlot next{}; next.slot=value;
            next.fixed_current.resize(value->definition->inputs.size()); next.fixed_previous.resize(value->definition->inputs.size()); next.frame.resize(value->definition->inputs.size());
            for(size_t i=0;i<next.fixed_current.size();++i) {
                const AnimationValueType type=value->definition->inputs[i].type;
                if(!checkpoint_stored_value(checkpoint.fixed_inputs[i],type,next.fixed_current[i]) || !checkpoint_stored_value(checkpoint.fixed_previous_inputs[i],type,next.fixed_previous[i]) || !checkpoint_stored_value(checkpoint.frame_inputs[i],type,next.frame[i])) return false;
            }
            restored.push_back(std::move(next));
        }
        // Every failure-prone check and allocation is complete.  Commit both
        // service-owned state and the attached evaluator only after that point.
        for(size_t index=0; index<checkpoints.size(); ++index) {
            Slot& value=*restored[index].slot; const AnimatorCheckpoint& checkpoint=checkpoints[index];
            value.fixed_current=std::move(restored[index].fixed_current); value.fixed_previous=std::move(restored[index].fixed_previous); value.frame_controls=std::move(restored[index].frame);
            for(size_t target=0;target<value.targets.size();++target) { value.targets[target].transform=checkpoint.target_desired[target]; value.targets[target].weight=checkpoint.target_weights[target]; value.targets[target].enabled=checkpoint.target_enabled_states[target]!=0; value.targets[target].snap_requested=checkpoint.target_snap_requested_states[target]!=0; }
            value.graph_state=checkpoint.graph_state; value.controller_state=checkpoint.controller_state; value.sample_context=checkpoint.sample_context_state; value.pose_scratch=checkpoint.pose_scratch_state;
        }
        if(systems_) {
            // Rebuild the value-owned leases from the restored service slots
            // before restoring evaluator/fixed transient state.  Otherwise a
            // later frame would silently sample the pre-restore controls.
            for(const AnimatorCheckpoint& checkpoint:checkpoints) {
                AnimationRuntimeBindingLease lease;
                if(!runtime_binding(checkpoint.instance,lease) || !systems_->refresh_service_binding(lease)) return false;
            }
            for(const AnimatorCheckpoint& checkpoint:checkpoints)
                if(!systems_->restore_service_checkpoint(checkpoint)) return false;
        }
        return true;
    }

    bool runtime_binding(AnimatorInstanceHandle handle, AnimationRuntimeBindingLease& out) const {
        out = {};
        const Slot* value = slot(handle);
        if (!value || !value->definition->binding) return false;
        out.instance = handle;
        out.asset_identity = value->asset->resolved_hash;
        out.descriptor = value->definition->binding;
        out.descriptor_identity = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value->definition->binding.get()));
        out.fixed_previous.reserve(value->fixed_previous.size());
        out.fixed_current.reserve(value->fixed_current.size());
        out.frame_controls.reserve(value->frame_controls.size());
        for (const StoredValue& control : value->fixed_previous) out.fixed_previous.push_back(animation_value(control));
        for (const StoredValue& control : value->fixed_current) out.fixed_current.push_back(animation_value(control));
        for (const StoredValue& control : value->frame_controls) out.frame_controls.push_back(animation_value(control));
        out.target_transforms.reserve(value->targets.size()); out.target_weights.reserve(value->targets.size()); out.target_enabled.reserve(value->targets.size()); out.target_snap_requested.reserve(value->targets.size());
        for (const TargetState& target : value->targets) { out.target_transforms.push_back(target.transform); out.target_weights.push_back(target.weight); out.target_enabled.push_back(target.enabled ? 1u : 0u); out.target_snap_requested.push_back(target.snap_requested ? 1u : 0u); }
        return out.valid();
    }

    void attach_runtime_systems(AnimationSystems* systems, AnimationService* owner) {
        if (systems_ == systems) return;
        if (systems_) {
            for (uint32_t index = 0; index < slots_.size(); ++index)
                if (slots_[index].alive) systems_->detach_service_binding(instance_handle(index, slots_[index]));
            systems_->attach_service(nullptr);
        }
        systems_ = systems;
        if (!systems_) return;
        systems_->set_budget_config(budget_);
        systems_->attach_service(owner);
        for (uint32_t index = 0; index < slots_.size(); ++index) if (slots_[index].alive) refresh_runtime_binding(instance_handle(index, slots_[index]), slots_[index]);
    }

private:
    bool refresh_runtime_binding(AnimatorInstanceHandle handle, const Slot& value) {
        if (!systems_ || !value.definition->binding) return true;
        AnimationRuntimeBindingLease lease;
        if (!runtime_binding(handle, lease) || !systems_->refresh_service_binding(lease)) {
            systems_->detach_service_binding(handle);
            return false;
        }
        return true;
    }
    bool sample_controls(EvaluationCadence cadence) {
        for (uint32_t index = 0; index < slots_.size(); ++index) {
            Slot& value = slots_[index];
            if (!value.alive) continue;
            const auto previous = value.fixed_previous;
            const auto current = value.fixed_current;
            const auto frame = value.frame_controls;
            if (cadence == EvaluationCadence::Fixed) {
                value.fixed_previous = value.fixed_current;
                value.fixed_current = value.fixed_pending;
            } else value.frame_controls = value.frame_pending;
            const AnimatorInstanceHandle handle = instance_handle(index, value);
            if (!refresh_runtime_binding(handle, value)) {
                value.fixed_previous = previous;
                value.fixed_current = current;
                value.frame_controls = frame;
                (void)refresh_runtime_binding(handle, value);
                return false;
            }
        }
        return true;
    }
    const AnimationRuntimeDefinition* schema_for(const AnimAsset* asset, const AnimationRuntimeDefinition& definition,
                                                  bool allow_replacement = false) {
        const auto existing = schemas_.find(asset);
        if (existing != schemas_.end()) {
            if (same_definition(*existing->second, definition)) return existing->second.get();
            if (!allow_replacement || !valid_definition(definition, budget_)) return nullptr;
            auto owned = std::make_unique<AnimationRuntimeDefinition>(definition);
            const AnimationRuntimeDefinition* result = owned.get();
            replacement_schemas_.push_back(std::move(owned));
            return result;
        }
        if (!valid_definition(definition, budget_)) return nullptr;
        auto owned = std::make_unique<AnimationRuntimeDefinition>(definition);
        const AnimationRuntimeDefinition* result = owned.get();
        schemas_.emplace(asset, std::move(owned));
        return result;
    }
    bool owns(const AnimAsset* asset) const {
        return assets_.contains(asset);
    }
    bool can_fit(size_t bytes) const {
        return mutable_bytes_ <= config_.mutable_budget_bytes && bytes <= config_.mutable_budget_bytes - mutable_bytes_;
    }
    AnimatorInstanceHandle instance_handle(uint32_t index, const Slot& value) const { return {index, value.generation, kInvalid, static_cast<AnimationValueType>(0xff), AnimationCadence::Invalid}; }
    Slot* slot(AnimatorInstanceHandle handle) { return handle.valid() ? slot(handle.slot_index, handle.generation) : nullptr; }
    const Slot* slot(AnimatorInstanceHandle handle) const { return handle.valid() ? slot(handle.slot_index, handle.generation) : nullptr; }
    Slot* slot(uint32_t index, uint32_t generation) {
        if (index >= slots_.size()) return nullptr;
        Slot& value = slots_[index];
        return value.alive && value.generation == generation ? &value : nullptr;
    }
    const Slot* slot(uint32_t index, uint32_t generation) const {
        if (index >= slots_.size()) return nullptr;
        const Slot& value = slots_[index];
        return value.alive && value.generation == generation ? &value : nullptr;
    }
    TargetState* writable_target(AnimationTargetHandle handle) {
        if (!handle.valid()) return nullptr;
        Slot* owner = slot(handle.slot_index, handle.generation);
        if (!owner || handle.schema_index >= owner->definition->targets.size() || handle.value_type != AnimationValueType::Transform) return nullptr;
        const RuntimeTargetDefinition& schema = owner->definition->targets[handle.schema_index];
        if (schema.driver != TargetDriverKind::External || handle.cadence != public_cadence(schema.cadence)) return nullptr;
        return &owner->targets[handle.schema_index];
    }
    const StoredValue* read_pending_input(AnimationInputHandle handle, AnimationValueType type) const {
        if (!handle.valid()) return nullptr;
        const Slot* owner = slot(handle.slot_index, handle.generation);
        if (!owner || handle.schema_index >= owner->definition->inputs.size() || handle.value_type != type) return nullptr;
        const RuntimeInputDefinition& schema = owner->definition->inputs[handle.schema_index];
        if (schema.type != type || handle.cadence != public_cadence(schema.cadence)) return nullptr;
        return schema.cadence == EvaluationCadence::Fixed ? &owner->fixed_pending[handle.schema_index] : &owner->frame_pending[handle.schema_index];
    }
    AnimationStoreConfig config_;
    AnimationBudgetConfig budget_;
    AnimationAssetStore assets_;
    std::map<const AnimAsset*, std::unique_ptr<const AnimationRuntimeDefinition>> schemas_;
    // Asset-keyed schema interning cannot discard an active sibling's
    // descriptor. Replacements therefore own their immutable descriptors here
    // until the service itself is destroyed.
    std::vector<std::unique_ptr<const AnimationRuntimeDefinition>> replacement_schemas_;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_indices_;
    size_t mutable_bytes_ = 0;
    uint32_t active_count_ = 0;
    AnimationSystems* systems_ = nullptr;
};

} // namespace matter::animation

namespace matter {

AnimationService::AnimationService(AnimationStoreConfig config) : impl_(std::make_unique<animation::AnimationServiceImpl>(config)) {}
AnimationService::~AnimationService() = default;
AnimationService::AnimationService(AnimationService&&) noexcept = default;
AnimationService& AnimationService::operator=(AnimationService&&) noexcept = default;
const animation::AnimAsset* AnimationService::insert_asset(animation::AnimAsset asset) { return impl_->insert_asset(std::move(asset)); }
bool AnimationService::release_asset(const animation::AnimAsset* asset) { return impl_->release_asset(asset); }
Animator AnimationService::create(const animation::AnimAsset* asset, const animation::AnimationRuntimeDefinition& definition) { return impl_->create(asset, definition); }
Animator AnimationService::replace_asset(AnimatorInstanceHandle instance, const animation::AnimAsset* asset, const animation::AnimationRuntimeDefinition& definition) { return impl_->replace(instance, asset, definition); }
bool AnimationService::remove(AnimatorInstanceHandle instance) { return impl_->remove(instance); }
bool AnimationService::runtime_binding(AnimatorInstanceHandle instance, AnimationRuntimeBindingLease& out) const { return impl_->runtime_binding(instance, out); }
bool AnimationService::sample_fixed_controls() { return impl_->sample_fixed_controls(); }
bool AnimationService::sample_frame_controls() { return impl_->sample_frame_controls(); }
void AnimationService::attach_runtime_systems(animation::AnimationSystems* systems) { impl_->attach_runtime_systems(systems, systems ? this : nullptr); }
bool AnimationService::capture_runtime_checkpoints(std::vector<animation::AnimatorCheckpoint>& out) const { return impl_->capture_runtime_checkpoints(out); }
bool AnimationService::validate_runtime_checkpoints(const std::vector<animation::AnimatorCheckpoint>& checkpoints) const { return impl_->validate_runtime_checkpoints(checkpoints); }
bool AnimationService::restore_runtime_checkpoints(const std::vector<animation::AnimatorCheckpoint>& checkpoints) { return impl_->restore_runtime_checkpoints(checkpoints); }
AnimationInputHandle AnimationService::input(AnimatorInstanceHandle instance, std::string_view name) const { return impl_->input(instance, name); }
AnimationTargetHandle AnimationService::target(AnimatorInstanceHandle instance, std::string_view name) const { return impl_->target(instance, name); }
bool AnimationService::set(AnimationInputHandle h, bool v) { animation::StoredValue value; value.type = AnimationValueType::Bool; value.boolean = v; return impl_->set(h, value, value.type); }
bool AnimationService::set(AnimationInputHandle h, float v) { animation::StoredValue value; value.type = AnimationValueType::Number; value.number = v; return impl_->set(h, value, value.type); }
bool AnimationService::set(AnimationInputHandle h, const Float3& v) { animation::StoredValue value; value.type = AnimationValueType::Float3; value.float3 = v; return impl_->set(h, value, value.type); }
bool AnimationService::set(AnimationInputHandle h, const Quaternion& v) { animation::StoredValue value; value.type = AnimationValueType::Quaternion; value.quaternion = v; return impl_->set(h, value, value.type); }
bool AnimationService::set(AnimationInputHandle h, const AnimationTransform& v) { animation::StoredValue value; value.type = AnimationValueType::Transform; value.transform = v; return impl_->set(h, value, value.type); }
bool AnimationService::set_symbol(AnimationInputHandle h, uint32_t v) { animation::StoredValue value; value.type = AnimationValueType::Symbol; value.symbol = v; return impl_->set(h, value, value.type); }
bool AnimationService::set_enabled(AnimationTargetHandle h, bool v) { return impl_->set_enabled(h, v); }
bool AnimationService::set_weight(AnimationTargetHandle h, float v) { return impl_->set_weight(h, v); }
bool AnimationService::set_transform(AnimationTargetHandle h, const AnimationTransform& v) { return impl_->set_transform(h, v); }
bool AnimationService::snap(AnimationTargetHandle h) { return impl_->snap(h); }
AnimationStatus AnimationService::status(AnimatorInstanceHandle h) const { return impl_->status(h); }
AnimationRuntimeStats AnimationService::stats() const { return impl_->stats(); }
size_t AnimationService::mutable_bytes() const { return impl_->mutable_bytes(); }
float AnimationService::number_value(AnimationInputHandle h) const { return impl_->number_value(h); }
bool AnimationService::bool_value(AnimationInputHandle h) const { return impl_->bool_value(h); }

} // namespace matter
