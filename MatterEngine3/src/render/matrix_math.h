#pragma once

#include "matter/math_types.h"

namespace viewer {

matter::Mat4f mat4_identity();
matter::Mat4f mat4_translation(matter::Float3 translation);
matter::Mat4f mat4_rotation_y(float radians);
matter::Mat4f mat4_mul(const matter::Mat4f& a, const matter::Mat4f& b);
matter::Mat4f look_at_rh(matter::Float3 eye, matter::Float3 target,
                         matter::Float3 up_hint);
matter::Mat4f perspective_rh_zo(float fovy, float aspect, float near_plane,
                                float far_plane);
// Reversed-Z variant: same right-handed, zero-to-one clip convention, but
// z = -near_plane maps to NDC depth 1.0 and z = -far_plane maps to 0.0 (the
// near/far roles of perspective_rh_zo's depth terms are swapped). Only the
// two depth-row terms differ from perspective_rh_zo; everything else
// (x/y scale, w row) is identical.
matter::Mat4f perspective_rh_zo_reversed(float fovy, float aspect,
                                         float near_plane, float far_plane);
bool mat4_inverse(const matter::Mat4f& matrix, matter::Mat4f& inverse);
matter::Float4 transform(const matter::Mat4f& matrix, matter::Float4 value);
matter::Float3 transform_point(const matter::Mat4f& matrix, matter::Float3 point);
matter::Float3 transform_vector(const matter::Mat4f& matrix, matter::Float3 vector);
matter::Float3 project_ndc(const matter::Mat4f& matrix, matter::Float3 point);
matter::Float3 unproject_ndc(const matter::Mat4f& clip_to_world, matter::Float3 point);
bool extract_frustum_planes_zo(const matter::Mat4f& world_to_clip,
                               float planes[6][4]);

} // namespace viewer
