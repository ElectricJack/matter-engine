#pragma once

#include "matter/animation_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace matter {

enum class AnimationCadence : uint8_t { Fixed, Frame, Invalid = 0xff };

struct AnimationInputHandle {
    uint32_t slot_index = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t schema_index = UINT32_MAX;
    AnimationValueType value_type = static_cast<AnimationValueType>(0xff);
    AnimationCadence cadence = AnimationCadence::Invalid;
    bool valid() const {
        return slot_index != UINT32_MAX && schema_index != UINT32_MAX &&
               static_cast<uint32_t>(value_type) <= static_cast<uint32_t>(AnimationValueType::Symbol) &&
               (cadence == AnimationCadence::Fixed || cadence == AnimationCadence::Frame);
    }
};

struct AnimationTargetHandle {
    uint32_t slot_index = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t schema_index = UINT32_MAX;
    AnimationValueType value_type = static_cast<AnimationValueType>(0xff);
    AnimationCadence cadence = AnimationCadence::Invalid;
    bool valid() const {
        return slot_index != UINT32_MAX && schema_index != UINT32_MAX &&
               value_type == AnimationValueType::Transform &&
               (cadence == AnimationCadence::Fixed || cadence == AnimationCadence::Frame);
    }
};

struct AnimatorInstanceHandle {
    uint32_t slot_index = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t schema_index = UINT32_MAX;
    AnimationValueType value_type = static_cast<AnimationValueType>(0xff);
    AnimationCadence cadence = AnimationCadence::Invalid;
    bool valid() const {
        return slot_index != UINT32_MAX && schema_index == UINT32_MAX &&
               static_cast<uint32_t>(value_type) == 0xffu && cadence == AnimationCadence::Invalid;
    }
};

enum class AnimationStatus : uint8_t { Ok, BudgetExceeded, InvalidHandle, LoadFailed };

struct Animator {
    AnimatorInstanceHandle instance{};
    AnimationStatus status = AnimationStatus::Ok;
    // Allocation caps are recoverable: the owner keeps its static .part visible
    // in bind pose and may retry allocation later.
    bool bind_pose_fallback = false;
    bool valid() const { return instance.valid() && status == AnimationStatus::Ok; }
};

struct DesiredRootMotion { AnimationTransform delta{}; bool valid = false; };
struct AnimationMarkerEvent { AnimatorInstanceHandle instance{}; uint32_t marker_index = UINT32_MAX; float time = 0.0f; };
struct AnimationRuntimeStats { uint32_t active_instances = 0; uint32_t instance_capacity = 0; size_t mutable_bytes = 0; size_t mutable_budget_bytes = 0; };
struct AnimationStoreConfig { uint32_t instance_capacity = 4096; size_t mutable_budget_bytes = 64u * 1024u * 1024u; };

namespace animation {
struct AnimAsset;
struct AnimationRuntimeDefinition;
struct AnimationRuntimeBindingDescriptor;
class AnimationServiceImpl;
class AnimationSystems;
}

// A read-only, generation-qualified service snapshot used only by the
// internal runtime bridge.  It deliberately carries values, not Slot pointers:
// a service redefinition or destroy cannot leave the ECS phase with a dangling
// reference into AnimationServiceImpl.
struct AnimationRuntimeBindingLease {
    AnimatorInstanceHandle instance{};
    uint64_t asset_identity = 0;
    std::shared_ptr<const animation::AnimationRuntimeBindingDescriptor> descriptor;
    struct Value {
        AnimationValueType type = AnimationValueType::Number;
        bool boolean = false;
        double number = 0.0;
        Float3 float3{};
        Quaternion quaternion{};
        AnimationTransform transform{};
        uint32_t symbol = 0;
    };
    std::vector<Value> fixed_previous;
    std::vector<Value> fixed_current;
    std::vector<Value> frame_controls;
    std::vector<AnimationTransform> target_transforms;
    std::vector<float> target_weights;
    std::vector<uint8_t> target_enabled;
    bool valid() const { return instance.valid() && descriptor != nullptr; }
};

class AnimationService {
public:
    explicit AnimationService(AnimationStoreConfig config = {});
    ~AnimationService();
    AnimationService(AnimationService&&) noexcept;
    AnimationService& operator=(AnimationService&&) noexcept;
    AnimationService(const AnimationService&) = delete;
    AnimationService& operator=(const AnimationService&) = delete;

    const animation::AnimAsset* insert_asset(animation::AnimAsset asset);
    Animator create(const animation::AnimAsset* asset, const animation::AnimationRuntimeDefinition& definition);
    Animator replace_asset(AnimatorInstanceHandle instance, const animation::AnimAsset* asset,
                          const animation::AnimationRuntimeDefinition& definition);
    bool remove(AnimatorInstanceHandle instance);

    AnimationInputHandle input(AnimatorInstanceHandle, std::string_view name) const;
    AnimationTargetHandle target(AnimatorInstanceHandle, std::string_view name) const;
    bool set(AnimationInputHandle, bool);
    bool set(AnimationInputHandle, float);
    bool set(AnimationInputHandle, const Float3&);
    bool set(AnimationInputHandle, const Quaternion&);
    bool set(AnimationInputHandle, const AnimationTransform&);
    bool set_symbol(AnimationInputHandle, uint32_t declared_symbol);
    bool set_enabled(AnimationTargetHandle, bool);
    bool set_weight(AnimationTargetHandle, float);
    bool set_transform(AnimationTargetHandle, const AnimationTransform&);
    bool snap(AnimationTargetHandle);

    // Internal runtime bridge.  Definitions with no runtime descriptor remain
    // intentionally unbound for compatibility; malformed descriptors fail at
    // create/replace rather than becoming partially active.
    bool runtime_binding(AnimatorInstanceHandle, AnimationRuntimeBindingLease&) const;
    void attach_runtime_systems(animation::AnimationSystems* systems);

    AnimationStatus status(AnimatorInstanceHandle) const;
    AnimationRuntimeStats stats() const;
    size_t mutable_bytes() const;
    float number_value(AnimationInputHandle) const;
    bool bool_value(AnimationInputHandle) const;

private:
    std::unique_ptr<animation::AnimationServiceImpl> impl_;
};

} // namespace matter
