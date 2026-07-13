#pragma once

#include "matter/math_types.h"

namespace matter {

struct CameraDesc {
    Float3 position{20.0f, 16.0f, 34.0f};
    Float3 target{0.0f, 9.0f, 0.0f};
    Float3 up{0.0f, 1.0f, 0.0f};
    float vertical_fov_radians = 0.78539816339f;
    float near_plane = 1.0f;
    float far_plane = 5000.0f;
};

} // namespace matter
