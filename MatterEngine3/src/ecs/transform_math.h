// MatterEngine3/src/ecs/transform_math.h
//
// The one place a `matter::ecs::LocalTransform` becomes a matrix. Header-only
// and dependency-light on purpose: transform_system.cpp includes it inside the
// propagation hot loop, and the ECS tests include it to check the composition
// directly.
//
// Not a general math library — for everyday vector/matrix work use
// libs/MathLib (`mm::`). This exists because `Mat4f` (matter/math_types.h) is
// the POD interchange type `ecs::WorldTransform` stores.

#pragma once

#include "matter/ecs.h"

#include <cmath>

namespace matter::ecs {

// Composes the transform as M = T * R * S, ROW-MAJOR: `m[3]`, `m[7]` and
// `m[11]` are the X/Y/Z translation in metres, and each column of the upper 3x3
// is pre-scaled by the matching `scale` component (so scale is applied in the
// entity's own frame, before rotation). `m[12..14]` stay 0 and `m[15]` is 1.
//
// The rotation is normalized here rather than being assumed unit, and the
// intermediate math is done in double to keep the normalization honest for
// long-lived accumulated quaternions. A non-finite or zero-length rotation
// falls back to IDENTITY rather than failing — the caller gets an unrotated but
// correctly translated and scaled matrix, never a NaN one.
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
