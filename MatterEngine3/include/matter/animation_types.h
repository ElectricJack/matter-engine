#pragma once

// MatterEngine3/include/matter/animation_types.h
//
// The leaf value types of the animation surface, kept in their own header so
// that `matter/animation.h`, `matter/animation_debug.h` and the private
// runtime headers all agree on them without any of them including each other.
// Depends on nothing but `matter/math_types.h` (Float3/Quaternion/Mat4f).

#include "matter/math_types.h"

namespace matter {

// A decomposed TRS transform — the interchange form for poses, sockets, IK
// targets and root motion throughout the animation subsystem. Kept
// decomposed rather than as a Mat4f so rotations can be blended as
// quaternions.
//
// The default-constructed value is IDENTITY, not zero: `scale` defaults to
// (1,1,1) and `Quaternion` defaults to w = 1 (matter/math_types.h). A
// zero-initialized `AnimationTransform{}` is therefore safe to use directly.
struct AnimationTransform {
    Float3 translation{};
    Quaternion rotation{};
    Float3 scale{1.0f, 1.0f, 1.0f};
};

// The type tag for an authored animation control value. It selects which
// `AnimationService::set()` overload is legal for a given input handle, and
// which member of `AnimationRuntimeBindingLease::Value` carries the payload.
//
// The numeric ORDER is load-bearing for the on-disk runtime asset: the tag is
// serialized as a raw u8 (animation_runtime_asset.cpp), so renumbering or
// reordering existing entries invalidates every baked asset. APPENDING is
// safe. Targets are always `Transform` (`AnimationTargetHandle::valid()`
// requires it).
enum class AnimationValueType {
    Bool,
    Number,
    Float3,
    Quaternion,
    Transform,
    Symbol,
};

// Is `type` one of the enumerators above (as opposed to the 0xff sentinel the
// handle structs default to, or a garbage byte off disk)?
//
// The switch is deliberately exhaustive with NO `default:`, so appending an
// enumerator above raises -Wswitch here rather than silently rejecting the new
// type. That is the whole point of this function: the range check it replaced
// was `<= AnimationValueType::Symbol`, which quietly treated anything added
// after Symbol as invalid.
constexpr bool is_animation_value_type(AnimationValueType type) noexcept {
    switch (type) {
    case AnimationValueType::Bool:
    case AnimationValueType::Number:
    case AnimationValueType::Float3:
    case AnimationValueType::Quaternion:
    case AnimationValueType::Transform:
    case AnimationValueType::Symbol:
        return true;
    }
    return false;
}

} // namespace matter
