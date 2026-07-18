#pragma once

#include "matter/ecs.h"

#include <cmath>

namespace matter::ecs {

inline Mat4f trs_matrix(const LocalTransform& transform) {
    double x = transform.rotation.x;
    double y = transform.rotation.y;
    double z = transform.rotation.z;
    double w = transform.rotation.w;
    const double length_squared = x * x + y * y + z * z + w * w;
    if (std::isfinite(x) && std::isfinite(y) &&
        std::isfinite(z) && std::isfinite(w) &&
        std::isfinite(length_squared) && length_squared > 0.0) {
        const double inverse_length = 1.0 / std::sqrt(length_squared);
        x *= inverse_length;
        y *= inverse_length;
        z *= inverse_length;
        w *= inverse_length;
    } else {
        x = 0.0;
        y = 0.0;
        z = 0.0;
        w = 1.0;
    }

    const double xx = x * x;
    const double yy = y * y;
    const double zz = z * z;
    const double xy = x * y;
    const double xz = x * z;
    const double yz = y * z;
    const double xw = x * w;
    const double yw = y * w;
    const double zw = z * w;

    Mat4f result{};
    result.m[0] = static_cast<float>((1.0 - 2.0 * (yy + zz)) * transform.scale.x);
    result.m[1] = static_cast<float>((2.0 * (xy - zw)) * transform.scale.y);
    result.m[2] = static_cast<float>((2.0 * (xz + yw)) * transform.scale.z);
    result.m[3] = transform.translation.x;
    result.m[4] = static_cast<float>((2.0 * (xy + zw)) * transform.scale.x);
    result.m[5] = static_cast<float>((1.0 - 2.0 * (xx + zz)) * transform.scale.y);
    result.m[6] = static_cast<float>((2.0 * (yz - xw)) * transform.scale.z);
    result.m[7] = transform.translation.y;
    result.m[8] = static_cast<float>((2.0 * (xz - yw)) * transform.scale.x);
    result.m[9] = static_cast<float>((2.0 * (yz + xw)) * transform.scale.y);
    result.m[10] = static_cast<float>((1.0 - 2.0 * (xx + yy)) * transform.scale.z);
    result.m[11] = transform.translation.z;
    result.m[15] = 1.0f;
    return result;
}

} // namespace matter::ecs
