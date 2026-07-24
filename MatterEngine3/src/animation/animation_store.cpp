#include "animation/animation_store.h"

#include "animation/animation_asset_store.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
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
constexpr uint32_t kHardInstanceCapacity = 4096;
constexpr size_t kHardMutableBudgetBytes = 64u * 1024u * 1024u;

AnimationStoreConfig bounded_config(AnimationStoreConfig requested) {
    requested.instance_capacity = std::min(requested.instance_capacity, kHardInstanceCapacity);
    requested.mutable_budget_bytes = std::min(requested.mutable_budget_bytes, kHardMutableBudgetBytes);
    return requested;
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
    std::vector<StoredValue> frame_controls;
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

bool valid_binding(const std::shared_ptr<const AnimationRuntimeBindingDescriptor>& binding) {
    if (!binding) return true; // Legacy definitions are deliberately unbound.
    const AnimationEvaluationDefinition* evaluation = binding->evaluation.get();
    const AnimationFixedWork& work = binding->fixed_work;
    if (!evaluation || !valid_animation_evaluation_definition(*evaluation) ||
        !std::isfinite(work.clip.duration) || work.clip.duration <= 0.0f ||
        !std::isfinite(work.clip.time) || !std::isfinite(work.clip.rate)) return false;
    for (const RuntimeClipMarker& marker : work.clip.markers)
        if (!std::isfinite(marker.time) || marker.time < 0.0f || marker.time > work.clip.duration) return false;
    for (const AnimationWorldQueryRequest& query : work.queries)
        if (!std::isfinite(query.max_distance) || query.max_distance < 0.0f) return false;
    return true;
}

bool valid_definition(const AnimationRuntimeDefinition& definition) {
    for (const RuntimeInputDefinition& input : definition.inputs) {
        if (input.default_value.type != input.type) return false;
    }
    if (!valid_binding(definition.binding)) return false;
    if (definition.binding) {
        const auto& evaluation = *definition.binding->evaluation;
        if (evaluation.inputs.size() != definition.inputs.size()) return false;
        for (size_t i = 0; i < definition.inputs.size(); ++i)
            if (evaluation.inputs[i].type != definition.inputs[i].type || evaluation.inputs[i].cadence != definition.inputs[i].cadence) return false;
        if (definition.binding->target_index != UINT16_MAX && definition.binding->target_index >= definition.targets.size()) return false;
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

Slot make_slot(const AnimAsset* asset, const AnimationRuntimeDefinition* definition, uint32_t generation) {
    Slot slot;
    slot.alive = true;
    slot.generation = generation;
    slot.asset = asset;
    slot.definition = definition;
    slot.fixed_previous.resize(definition->inputs.size());
    slot.fixed_current.resize(definition->inputs.size());
    slot.frame_controls.resize(definition->inputs.size());
    for (size_t i = 0; i < definition->inputs.size(); ++i) {
        const StoredValue value = store_value(definition->inputs[i].default_value, definition->inputs[i].type);
        slot.fixed_previous[i] = value;
        slot.fixed_current[i] = value;
        slot.frame_controls[i] = value;
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
    explicit AnimationServiceImpl(AnimationStoreConfig config) : config_(bounded_config(config)) {
        slots_.reserve(config_.instance_capacity);
        free_indices_.reserve(config_.instance_capacity);
    }
    ~AnimationServiceImpl() { attach_runtime_systems(nullptr, nullptr); }

    const AnimAsset* insert_asset(AnimAsset asset) { return assets_.insert(std::move(asset)); }

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
        refresh_runtime_binding(result.instance, old);
        return result;
    }

    Animator replace(AnimatorInstanceHandle handle, const AnimAsset* asset, const AnimationRuntimeDefinition& definition) {
        Slot* old = slot(handle);
        if (!old) return {{}, AnimationStatus::InvalidHandle};
        if (!owns(asset)) return {{}, AnimationStatus::LoadFailed};
        const AnimationRuntimeDefinition* schema = schema_for(asset, definition);
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
                    replacement.frame_controls[next] = old->frame_controls[prior];
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
        *old = std::move(replacement);
        mutable_bytes_ = mutable_bytes_ - old_bytes + new_bytes;
        if (systems_) systems_->detach_service_binding(stale);
        const Animator result{instance_handle(index, *old), AnimationStatus::Ok};
        refresh_runtime_binding(result.instance, *old);
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
        if (schema.cadence == EvaluationCadence::Fixed) owner->fixed_current[handle.schema_index] = value;
        else owner->frame_controls[handle.schema_index] = value;
        refresh_runtime_binding(instance_handle(handle.slot_index, *owner), *owner);
        return true;
    }

    bool set_enabled(AnimationTargetHandle handle, bool enabled) {
        TargetState* state = writable_target(handle);
        if (!state) return false;
        state->enabled = enabled;
        Slot* owner = slot(handle.slot_index, handle.generation);
        refresh_runtime_binding(instance_handle(handle.slot_index, *owner), *owner);
        return true;
    }
    bool set_weight(AnimationTargetHandle handle, float weight) {
        TargetState* state = writable_target(handle);
        if (!state || !state->enabled) return false;
        state->weight = weight;
        Slot* owner = slot(handle.slot_index, handle.generation);
        refresh_runtime_binding(instance_handle(handle.slot_index, *owner), *owner);
        return true;
    }
    bool set_transform(AnimationTargetHandle handle, const AnimationTransform& transform) {
        TargetState* state = writable_target(handle);
        if (!state || !state->enabled) return false;
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
    AnimationRuntimeStats stats() const { return {active_count_, config_.instance_capacity, mutable_bytes_, config_.mutable_budget_bytes}; }
    size_t mutable_bytes() const { return mutable_bytes_; }
    float number_value(AnimationInputHandle handle) const { const StoredValue* v = read_input(handle, AnimationValueType::Number); return v ? v->number : 0.0f; }
    bool bool_value(AnimationInputHandle handle) const { const StoredValue* v = read_input(handle, AnimationValueType::Bool); return v && v->boolean; }

    bool runtime_binding(AnimatorInstanceHandle handle, AnimationRuntimeBindingLease& out) const {
        out = {};
        const Slot* value = slot(handle);
        if (!value || !value->definition->binding) return false;
        out.instance = handle;
        out.asset_identity = value->asset->resolved_hash;
        out.descriptor = value->definition->binding;
        out.fixed_previous.reserve(value->fixed_previous.size());
        out.fixed_current.reserve(value->fixed_current.size());
        out.frame_controls.reserve(value->frame_controls.size());
        for (const StoredValue& control : value->fixed_previous) out.fixed_previous.push_back(animation_value(control));
        for (const StoredValue& control : value->fixed_current) out.fixed_current.push_back(animation_value(control));
        for (const StoredValue& control : value->frame_controls) out.frame_controls.push_back(animation_value(control));
        out.target_transforms.reserve(value->targets.size()); out.target_weights.reserve(value->targets.size()); out.target_enabled.reserve(value->targets.size());
        for (const TargetState& target : value->targets) { out.target_transforms.push_back(target.transform); out.target_weights.push_back(target.weight); out.target_enabled.push_back(target.enabled ? 1u : 0u); }
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
        systems_->attach_service(owner);
        for (uint32_t index = 0; index < slots_.size(); ++index) if (slots_[index].alive) refresh_runtime_binding(instance_handle(index, slots_[index]), slots_[index]);
    }

private:
    void refresh_runtime_binding(AnimatorInstanceHandle handle, const Slot& value) {
        if (!systems_ || !value.definition->binding) return;
        AnimationRuntimeBindingLease lease;
        if (!runtime_binding(handle, lease) || !systems_->refresh_service_binding(lease)) systems_->detach_service_binding(handle);
    }
    const AnimationRuntimeDefinition* schema_for(const AnimAsset* asset, const AnimationRuntimeDefinition& definition) {
        const auto existing = schemas_.find(asset);
        if (existing != schemas_.end()) return same_definition(*existing->second, definition) ? existing->second.get() : nullptr;
        if (!valid_definition(definition)) return nullptr;
        auto owned = std::make_unique<AnimationRuntimeDefinition>(definition);
        const AnimationRuntimeDefinition* result = owned.get();
        schemas_.emplace(asset, std::move(owned));
        return result;
    }
    bool owns(const AnimAsset* asset) const {
        return asset && assets_.find(asset->resolved_hash, asset->nonce) == asset;
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
    const StoredValue* read_input(AnimationInputHandle handle, AnimationValueType type) const {
        if (!handle.valid()) return nullptr;
        const Slot* owner = slot(handle.slot_index, handle.generation);
        if (!owner || handle.schema_index >= owner->definition->inputs.size() || handle.value_type != type) return nullptr;
        const RuntimeInputDefinition& schema = owner->definition->inputs[handle.schema_index];
        if (schema.type != type || handle.cadence != public_cadence(schema.cadence)) return nullptr;
        return schema.cadence == EvaluationCadence::Fixed ? &owner->fixed_current[handle.schema_index] : &owner->frame_controls[handle.schema_index];
    }
    AnimationStoreConfig config_;
    AnimationAssetStore assets_;
    std::map<const AnimAsset*, std::unique_ptr<const AnimationRuntimeDefinition>> schemas_;
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
Animator AnimationService::create(const animation::AnimAsset* asset, const animation::AnimationRuntimeDefinition& definition) { return impl_->create(asset, definition); }
Animator AnimationService::replace_asset(AnimatorInstanceHandle instance, const animation::AnimAsset* asset, const animation::AnimationRuntimeDefinition& definition) { return impl_->replace(instance, asset, definition); }
bool AnimationService::remove(AnimatorInstanceHandle instance) { return impl_->remove(instance); }
bool AnimationService::runtime_binding(AnimatorInstanceHandle instance, AnimationRuntimeBindingLease& out) const { return impl_->runtime_binding(instance, out); }
void AnimationService::attach_runtime_systems(animation::AnimationSystems* systems) { impl_->attach_runtime_systems(systems, systems ? this : nullptr); }
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
