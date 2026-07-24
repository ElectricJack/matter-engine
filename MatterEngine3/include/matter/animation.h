#pragma once

#include "matter/animation_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace matter {

enum class AnimationCadence : uint8_t { Fixed, Frame, Invalid = 0xff };

struct AnimationInputHandle {
    uint32_t slot_index = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t schema_index = UINT32_MAX;
    AnimationValueType value_type = static_cast<AnimationValueType>(0xff);
    AnimationCadence cadence = AnimationCadence::Invalid;
    bool valid() const { return slot_index != UINT32_MAX; }
};

struct AnimationTargetHandle {
    uint32_t slot_index = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t schema_index = UINT32_MAX;
    AnimationValueType value_type = AnimationValueType::Transform;
    AnimationCadence cadence = AnimationCadence::Invalid;
    bool valid() const { return slot_index != UINT32_MAX; }
};

struct AnimatorInstanceHandle {
    uint32_t slot_index = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t schema_index = UINT32_MAX;
    AnimationValueType value_type = static_cast<AnimationValueType>(0xff);
    AnimationCadence cadence = AnimationCadence::Invalid;
    bool valid() const { return slot_index != UINT32_MAX; }
};

enum class AnimationStatus : uint8_t { Ok, BudgetExceeded, InvalidHandle, LoadFailed };

struct Animator {
    AnimatorInstanceHandle instance{};
    AnimationStatus status = AnimationStatus::Ok;
    bool valid() const { return instance.valid() && status == AnimationStatus::Ok; }
};

struct DesiredRootMotion { AnimationTransform delta{}; bool valid = false; };
struct AnimationMarkerEvent { AnimatorInstanceHandle instance{}; uint32_t marker_index = UINT32_MAX; float time = 0.0f; };
struct AnimationRuntimeStats { uint32_t active_instances = 0; uint32_t instance_capacity = 0; size_t mutable_bytes = 0; size_t mutable_budget_bytes = 0; };
struct AnimationStoreConfig { uint32_t instance_capacity = 4096; size_t mutable_budget_bytes = 64u * 1024u * 1024u; };

namespace animation { struct AnimAsset; struct AnimationRuntimeDefinition; class AnimationServiceImpl; }

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

    AnimationStatus status(AnimatorInstanceHandle) const;
    AnimationRuntimeStats stats() const;
    size_t mutable_bytes() const;
    float number_value(AnimationInputHandle) const;
    bool bool_value(AnimationInputHandle) const;

private:
    std::unique_ptr<animation::AnimationServiceImpl> impl_;
};

} // namespace matter
