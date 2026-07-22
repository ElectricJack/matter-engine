#include "matrix_math.h"

#include <algorithm>
#include <cmath>

namespace viewer {
namespace {

constexpr float kLengthEpsilonSquared = 1e-12f;

matter::Float3 subtract(matter::Float3 a, matter::Float3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

float dot(matter::Float3 a, matter::Float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

matter::Float3 cross(matter::Float3 a, matter::Float3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

matter::Float3 normalize(matter::Float3 value) {
    const float length_squared = dot(value, value);
    if (!(length_squared > kLengthEpsilonSquared) ||
        !std::isfinite(length_squared)) {
        return {};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    return {value.x * inverse_length, value.y * inverse_length,
            value.z * inverse_length};
}

} // namespace

matter::Mat4f mat4_identity() {
    matter::Mat4f identity{};
    identity.m[0] = 1.0f;
    identity.m[5] = 1.0f;
    identity.m[10] = 1.0f;
    identity.m[15] = 1.0f;
    return identity;
}

matter::Mat4f mat4_translation(matter::Float3 translation) {
    matter::Mat4f matrix = mat4_identity();
    matrix.m[3] = translation.x;
    matrix.m[7] = translation.y;
    matrix.m[11] = translation.z;
    return matrix;
}

matter::Mat4f mat4_rotation_y(float radians) {
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    return matter::Mat4f{{cosine, 0.0f, sine, 0.0f,
                          0.0f, 1.0f, 0.0f, 0.0f,
                          -sine, 0.0f, cosine, 0.0f,
                          0.0f, 0.0f, 0.0f, 1.0f}};
}

matter::Mat4f mat4_mul(const matter::Mat4f& a, const matter::Mat4f& b) {
    matter::Mat4f result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            for (int inner = 0; inner < 4; ++inner) {
                result.m[row * 4 + column] +=
                    a.m[row * 4 + inner] * b.m[inner * 4 + column];
            }
        }
    }
    return result;
}

matter::Mat4f look_at_rh(matter::Float3 eye, matter::Float3 target,
                         matter::Float3 up_hint) {
    const matter::Float3 forward = normalize(subtract(target, eye));
    const matter::Float3 right = normalize(cross(forward, up_hint));
    const matter::Float3 up = cross(right, forward);
    return matter::Mat4f{{right.x, right.y, right.z, -dot(right, eye),
                          up.x, up.y, up.z, -dot(up, eye),
                          -forward.x, -forward.y, -forward.z, dot(forward, eye),
                          0.0f, 0.0f, 0.0f, 1.0f}};
}

matter::Mat4f perspective_rh_zo(float fovy, float aspect, float near_plane,
                                float far_plane) {
    const float y_scale = 1.0f / std::tan(fovy * 0.5f);
    matter::Mat4f projection{};
    projection.m[0] = y_scale / aspect;
    projection.m[5] = y_scale;
    projection.m[10] = far_plane / (near_plane - far_plane);
    projection.m[11] = far_plane * near_plane / (near_plane - far_plane);
    projection.m[14] = -1.0f;
    return projection;
}

// Reversed-Z (near->1, far->0). Derivation from the row-major/column-vector
// layout above: row 3 (m[14] = -1, m[15] = 0) is unchanged, so clip.w = -z
// still equals view-space distance d for any point in front of the camera.
// NDC depth = (m[10]*z + m[11]) / clip.w = m[11]/d - m[10]. Solving
// m[11]/near - m[10] = 1 and m[11]/far - m[10] = 0 simultaneously gives
// m[10] = near/(far-near) and m[11] = far*near/(far-near) — the near/far
// roles are swapped relative to perspective_rh_zo's depth terms, and only
// those two terms change.
matter::Mat4f perspective_rh_zo_reversed(float fovy, float aspect,
                                         float near_plane, float far_plane) {
    const float y_scale = 1.0f / std::tan(fovy * 0.5f);
    matter::Mat4f projection{};
    projection.m[0] = y_scale / aspect;
    projection.m[5] = y_scale;
    projection.m[10] = near_plane / (far_plane - near_plane);
    projection.m[11] = far_plane * near_plane / (far_plane - near_plane);
    projection.m[14] = -1.0f;
    return projection;
}

bool mat4_inverse(const matter::Mat4f& matrix, matter::Mat4f& inverse) {
    double augmented[4][8]{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const float value = matrix.m[row * 4 + column];
            if (!std::isfinite(value)) {
                return false;
            }
            augmented[row][column] = value;
        }
        augmented[row][row + 4] = 1.0;
    }

    for (int column = 0; column < 4; ++column) {
        int pivot_row = column;
        for (int row = column + 1; row < 4; ++row) {
            if (std::fabs(augmented[row][column]) >
                std::fabs(augmented[pivot_row][column])) {
                pivot_row = row;
            }
        }
        const double pivot = augmented[pivot_row][column];
        if (!std::isfinite(pivot) || pivot == 0.0) {
            return false;
        }
        if (pivot_row != column) {
            for (int element = 0; element < 8; ++element) {
                std::swap(augmented[column][element],
                          augmented[pivot_row][element]);
            }
        }

        const double divisor = augmented[column][column];
        for (int element = 0; element < 8; ++element) {
            augmented[column][element] /= divisor;
        }
        for (int row = 0; row < 4; ++row) {
            if (row == column) {
                continue;
            }
            const double factor = augmented[row][column];
            for (int element = 0; element < 8; ++element) {
                augmented[row][element] -= factor * augmented[column][element];
            }
        }
    }

    matter::Mat4f candidate{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const double value = augmented[row][column + 4];
            if (!std::isfinite(value)) {
                return false;
            }
            const float candidate_value = static_cast<float>(value);
            if (!std::isfinite(candidate_value)) {
                return false;
            }
            candidate.m[row * 4 + column] = candidate_value;
        }
    }
    inverse = candidate;
    return true;
}

matter::Float4 transform(const matter::Mat4f& matrix, matter::Float4 value) {
    return {
        matrix.m[0] * value.x + matrix.m[1] * value.y +
            matrix.m[2] * value.z + matrix.m[3] * value.w,
        matrix.m[4] * value.x + matrix.m[5] * value.y +
            matrix.m[6] * value.z + matrix.m[7] * value.w,
        matrix.m[8] * value.x + matrix.m[9] * value.y +
            matrix.m[10] * value.z + matrix.m[11] * value.w,
        matrix.m[12] * value.x + matrix.m[13] * value.y +
            matrix.m[14] * value.z + matrix.m[15] * value.w,
    };
}

matter::Float3 transform_point(const matter::Mat4f& matrix,
                               matter::Float3 point) {
    const matter::Float4 result =
        transform(matrix, {point.x, point.y, point.z, 1.0f});
    return {result.x, result.y, result.z};
}

matter::Float3 transform_vector(const matter::Mat4f& matrix,
                                matter::Float3 vector) {
    const matter::Float4 result =
        transform(matrix, {vector.x, vector.y, vector.z, 0.0f});
    return {result.x, result.y, result.z};
}

matter::Float3 project_ndc(const matter::Mat4f& matrix, matter::Float3 point) {
    const matter::Float4 clip =
        transform(matrix, {point.x, point.y, point.z, 1.0f});
    const float inverse_w = 1.0f / clip.w;
    return {clip.x * inverse_w, clip.y * inverse_w, clip.z * inverse_w};
}

matter::Float3 unproject_ndc(const matter::Mat4f& clip_to_world,
                             matter::Float3 point) {
    return project_ndc(clip_to_world, point);
}

bool extract_frustum_planes_zo(const matter::Mat4f& world_to_clip,
                               float planes[6][4]) {
    for (int column = 0; column < 4; ++column) {
        const float row0 = world_to_clip.m[column];
        const float row1 = world_to_clip.m[4 + column];
        const float row2 = world_to_clip.m[8 + column];
        const float row3 = world_to_clip.m[12 + column];
        planes[0][column] = row3 + row0;
        planes[1][column] = row3 - row0;
        planes[2][column] = row3 + row1;
        planes[3][column] = row3 - row1;
        planes[4][column] = row2;
        planes[5][column] = row3 - row2;
    }

    for (int plane_index = 0; plane_index < 6; ++plane_index) {
        const float length_squared =
            planes[plane_index][0] * planes[plane_index][0] +
            planes[plane_index][1] * planes[plane_index][1] +
            planes[plane_index][2] * planes[plane_index][2];
        if (!(length_squared > 0.0f) || !std::isfinite(length_squared)) {
            return false;
        }
        const float inverse_length = 1.0f / std::sqrt(length_squared);
        for (int column = 0; column < 4; ++column) {
            planes[plane_index][column] *= inverse_length;
        }
    }
    return true;
}

} // namespace viewer
