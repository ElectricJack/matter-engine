#pragma once

// MatterEngine3/src/render/matrix_math.h
//
// The small CPU matrix/vector toolkit the render subsystem uses to build and
// interrogate camera transforms. Free functions only — no state, no
// allocation, no globals — so every entry point here is safe to call from any
// thread. Callers: frame_matrices.cpp (the per-frame camera solve),
// vk_scene_renderer.cpp and vk_temporal.cpp (frustum planes), raster_cull.h,
// part_store.cpp (child-transform composition), and the matrix/partstore test
// suites.
//
// Conventions, all inherited from `matter::Mat4f` (matter/math_types.h):
//   - Storage is ROW-major with column-vector algebra: `m[0..3]` is the first
//     row, translation lives in `m[3]`, `m[7]`, `m[11]`, and a point is
//     transformed as `M * v`. GLSL's default `mat4` is column-major, so a
//     matrix handed to a shader needs a transpose on the way out.
//   - `mat4_mul(a, b)` is the product `a * b`, i.e. `b` is applied to a point
//     FIRST. Parent-then-child composition therefore reads
//     `mat4_mul(parent, child)`.
//   - View space is right-handed with the camera looking down -Z; clip depth
//     is Vulkan's zero-to-one range. `fovy` is the FULL vertical field of view
//     in RADIANS and `aspect` is width/height.
//   - Positions and distances are world metres; nothing here rescales.
//
// This is deliberately NOT libs/MathLib (`mm::Mat4`, the canonical math
// library with real operators) and NOT SpatialQueryLib's aligned SIMD
// `float3`/`float4`. It works on the plain POD interchange types so render
// code can hand the same matrices straight to engine public headers.
//
// Failure conventions: `mat4_inverse` returns false and leaves its out-param
// untouched; `extract_frustum_planes_zo` returns false but may already have
// written into `planes`. The internal vector helpers are softer — a degenerate
// `normalize` yields the zero vector, so a degenerate `look_at_rh` silently
// produces a zero basis rather than reporting anything.

#include "matter/math_types.h"

namespace viewer {

matter::Mat4f mat4_identity();
matter::Mat4f mat4_translation(matter::Float3 translation);
matter::Mat4f mat4_rotation_y(float radians);
// Row-major product `a * b`. Under the column-vector convention that means b
// is applied to a point first, so composing a transform chain from the root
// down reads mat4_mul(parent, child) (see walk_rec in part_store.cpp).
matter::Mat4f mat4_mul(const matter::Mat4f& a, const matter::Mat4f& b);
// World-to-view for a right-handed camera at `eye` looking at `target`, with
// view -Z pointing along the forward direction. `up_hint` need not be exactly
// perpendicular — it is only used to derive the right axis.
// Degenerate input fails SOFT: eye == target, or an up_hint parallel to
// forward, drives the internal normalize to zero and yields a matrix with a
// zero basis. Nothing reports it, so validate the inputs if they can be
// user-driven.
matter::Mat4f look_at_rh(matter::Float3 eye, matter::Float3 target,
                         matter::Float3 up_hint);
// Right-handed perspective onto Vulkan's zero-to-one clip depth: view -Z is
// forward, z = -near_plane maps to NDC depth 0.0 and z = -far_plane to 1.0.
// `fovy` is the full vertical FOV in radians. The w row is (0,0,-1,0), so
// clip.w is the positive view-space distance for anything in front of the
// camera. Not the shipping projection — see perspective_rh_zo_reversed below,
// which is what frame_matrices.cpp builds.
matter::Mat4f perspective_rh_zo(float fovy, float aspect, float near_plane,
                                float far_plane);
// Reversed-Z variant: same right-handed, zero-to-one clip convention, but
// z = -near_plane maps to NDC depth 1.0 and z = -far_plane maps to 0.0 (the
// near/far roles of perspective_rh_zo's depth terms are swapped). Only the
// two depth-row terms differ from perspective_rh_zo; everything else
// (x/y scale, w row) is identical.
matter::Mat4f perspective_rh_zo_reversed(float fovy, float aspect,
                                         float near_plane, float far_plane);
// General 4x4 inverse (Gauss-Jordan in double precision, partial pivoting).
// Returns false — leaving `inverse` UNTOUCHED — for a singular matrix or any
// non-finite input, pivot or result, so a false return is the caller's cue
// that the transform is unusable rather than a value to ignore.
bool mat4_inverse(const matter::Mat4f& matrix, matter::Mat4f& inverse);
matter::Float4 transform(const matter::Mat4f& matrix, matter::Float4 value);
matter::Float3 transform_point(const matter::Mat4f& matrix, matter::Float3 point);
matter::Float3 transform_vector(const matter::Mat4f& matrix, matter::Float3 vector);
// Transform a point and divide by w. There is NO guard on w: a point on or
// behind the camera plane yields inf/NaN, which the caller must handle.
matter::Float3 project_ndc(const matter::Mat4f& matrix, matter::Float3 point);
// Identical arithmetic to project_ndc — the separate name documents the
// direction of travel, and the caller is expected to pass a clip-to-world
// matrix (typically FrameMatrices::clip_to_world, i.e. the mat4_inverse of
// world_to_clip) and an NDC point.
matter::Float3 unproject_ndc(const matter::Mat4f& clip_to_world, matter::Float3 point);
// Extract the six frustum planes from a world-to-clip matrix built for the
// zero-to-one depth convention. Planes come out NORMALIZED and INWARD facing,
// so `dot(plane.xyz, p) + plane.w >= 0` means "p is inside" and the value is a
// signed distance in world metres.
//
// Slot order is left, right, bottom, top, near, far — for a STANDARD zero-to-one
// projection. Feeding a reversed-Z matrix keeps exactly the same six half-spaces
// and the same orientation, but swaps which of slots 4 and 5 is the near plane
// and which is the far; frame_matrices.cpp carries the full note. Consumers that
// test all six planes uniformly (gpu_culler.cpp, shaders_vk/cull.comp,
// aabb_culled in raster_cull.h) are unaffected; anything that wants "the near
// plane" by index is not.
//
// Returns false if any plane's normal is degenerate or non-finite, after
// possibly having written into `planes`.
bool extract_frustum_planes_zo(const matter::Mat4f& world_to_clip,
                               float planes[6][4]);

} // namespace viewer
