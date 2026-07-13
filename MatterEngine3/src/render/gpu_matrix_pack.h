#pragma once

#include "matter/math_types.h"

namespace viewer {

struct GpuMat4 {
    float elements[16]{};
};

inline GpuMat4 pack_glsl_mat4(const matter::Mat4f& matrix) {
    GpuMat4 packed{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            packed.elements[column * 4 + row] = matrix.m[row * 4 + column];
        }
    }
    return packed;
}

} // namespace viewer
