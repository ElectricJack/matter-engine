#pragma once

#include "matter/math_types.h"

namespace matter {

struct AnimationTransform {
    Float3 translation{};
    Quaternion rotation{};
    Float3 scale{1.0f, 1.0f, 1.0f};
};

enum class AnimationValueType {
    Bool,
    Number,
    Float3,
    Quaternion,
    Transform,
    Symbol,
};

} // namespace matter
