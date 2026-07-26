#pragma once

#include "animation/animation_targets.h"
#include "animation/animation_world_queries.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace matter::animation {

// Stable strings are serialized in ANIM metadata. They must not be RTTI names
// or addresses; an unknown id is a load failure rather than a fallback.
using NativeControllerTypeId = uint64_t;
constexpr NativeControllerTypeId kGaitControllerTypeId = 0x474149545f563100ull; // "GAIT_V1\0"

struct NativeControllerDescriptor {
    NativeControllerTypeId type = 0;
    std::vector<uint8_t> parameters;
    EvaluationCadence cadence = EvaluationCadence::Fixed;
};
struct NativeControllerLayout { size_t fixed_state_bytes = 0; size_t frame_state_bytes = 0; };
struct ControllerTargetWrite { uint16_t target_index = UINT16_MAX; AnimationTransform transform{}; float weight = 1.0f; };

// A narrow, value-only host contract. Controllers receive neither Flecs world
// nor renderer objects; the one permitted world dependency is the fixed query
// interface defined by animation_world_queries.h.
struct NativeControllerContext {
    double fixed_delta_seconds = 0.0;
    const AnimationWorldQueries* world_queries = nullptr;
    // Controller rest/predicted coordinates are rig-relative.  Runtime
    // supplies the post-authority entity world pose at the fixed boundary so
    // native controllers can issue world queries without seeing Flecs.
    Mat4f entity_world{};
    bool has_entity_world = false;
    std::vector<AnimationValue> inputs;
    std::vector<ControllerTargetWrite> writes;
};

class NativeController {
public:
    virtual ~NativeController() = default;
    virtual NativeControllerTypeId type() const noexcept = 0;
    virtual NativeControllerLayout layout() const noexcept = 0;
    virtual bool fixed_update(NativeControllerContext&) = 0;
    virtual bool checkpoint(std::vector<uint8_t>&) const = 0;
    virtual bool restore(const std::vector<uint8_t>&) = 0;
};

class NativeControllerRegistry {
public:
    using Factory = std::unique_ptr<NativeController>(*)(const uint8_t*, size_t, NativeControllerLayout&);
    bool register_factory(NativeControllerTypeId, Factory);
    std::unique_ptr<NativeController> create(const NativeControllerDescriptor&, NativeControllerLayout&) const;
    static NativeControllerRegistry with_v1_controllers();
private:
    std::map<NativeControllerTypeId, Factory> factories_;
};

// Packed little-endian v1 blob. Values are explicitly finite/limited at the
// registry boundary before state allocation.
struct GaitControllerParameters {
    uint16_t left_target = UINT16_MAX;
    uint16_t right_target = UINT16_MAX;
    Float3 left_predicted{};
    Float3 right_predicted{};
    float stride_seconds = 1.0f;
    float swing_height = 0.1f;
    float ray_distance = 2.0f;
    float step_height = 0.25f;
    float min_ground_normal_y = 0.5f;
};
std::unique_ptr<NativeController> create_gait_controller(const uint8_t*, size_t, NativeControllerLayout&);

} // namespace matter::animation
