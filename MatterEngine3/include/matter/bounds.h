#pragma once

#include "math_types.h"

namespace matter {

struct Aabb {
    Float3 minimum{};
    Float3 maximum{};
};

} // namespace matter
