#pragma once

// MatterEditor/src/camera_orbit.h
//
// Pure, stateless orbit math for the editor camera. Implemented in
// camera_orbit.cpp; consumed by the Camera panel's orbit/zoom buttons
// (ui.cpp), the viewport's drag/wheel orbit (main.cpp), and the headless suite
// MatterEditor/tests/test_camera_orbit.cpp.
//
// Units and frame throughout: angles in radians, distances in metres, points
// in world space, +Y up. No function here keeps state — the only state is the
// caller's CameraDesc, which apply_orbit_frame/orbit_camera_by_mouse write
// back into.
//
// Orbit-about-a-pivot camera math (issue a4203d22 part 1).
//
// The Camera panel's Orbit buttons used to inline this: decompose
// cam.position - cam.target into yaw/pitch/distance, nudge one of the three,
// recompose. That was fine while the pivot was always cam.target, but "orbit
// the SELECTION" needs the same math around a different point, and the mouse
// orbit needs it a third time. Factoring it here means one implementation and,
// just as importantly, one that is unit-testable: this TU pulls in no GLFW, no
// ImGui and no WorldSession, so a headless suite can assert the invariant that
// actually matters (an orbit preserves distance to the pivot).
//
// Conventions are inherited verbatim from the panel code this replaces, so the
// buttons feel identical:
//   yaw      atan2(dz, dx) of (position - pivot); +Y is up
//   pitch    asin(dy / distance), clamped just shy of the poles
//   position pivot + distance * (cos p cos y, sin p, cos p sin y)

#include "matter/camera.h"

namespace viewer {

// Spherical decomposition of camera.position about an arbitrary pivot.
struct OrbitFrame {
    float yaw = 0.0f;       // radians
    float pitch = 0.0f;     // radians
    float distance = 1.0f;  // metres, pivot -> camera.position
};

// Decompose `camera.position` about `pivot` into the conventions above. The
// result is RAW: pitch is not clamped to the pole limit and distance is not
// floored to the 1 m minimum — that is clamp_orbit_frame's job, below. The
// only guard applied here is against a degenerate radius (a camera sitting
// exactly on the pivot), which would otherwise make asin() produce NaN.
OrbitFrame orbit_frame_from(const matter::CameraDesc& camera,
                            const matter::Float3& pivot);

// Pitch clamped to ~+/-89 degrees so the orbit never flips or gimbal-locks at
// the pole, distance floored at 1 m. Callers clamp BEFORE applying so the
// value they also display (the Distance slider) is the one that took effect.
OrbitFrame clamp_orbit_frame(OrbitFrame frame);

// Writes camera.position back from `frame`. `look_at_pivot` additionally sets
// camera.target = pivot: that is what makes an orbit around a selected object
// keep the object in frame, and it is the one place the pivot-substituted
// orbit differs from the classic cam.target orbit (where target IS the pivot
// and the assignment is a no-op).
void apply_orbit_frame(matter::CameraDesc& camera, const matter::Float3& pivot,
                       const OrbitFrame& frame, bool look_at_pivot);

// Viewport drag/wheel orbit about `pivot`. Signs match the panel buttons:
// dragging left is Orbit Left (yaw decreases), dragging up is Orbit Up (pitch
// increases), one wheel tick forward is one Zoom In. Returns true when the
// camera actually moved, so the caller can skip the write when idle.
// Unlike the three primitives above this is a complete operation: it
// decomposes, nudges, CLAMPS and applies in one call, and it always applies
// with look_at_pivot = true, so a mouse orbit re-aims camera.target at the
// pivot every time. Zoom is multiplicative — `zoom_step` is the fraction one
// tick removes, raised to the tick count — so no number of ticks in one frame
// can drive the distance through zero or negative. Returns false and touches
// nothing when all three inputs are exactly zero.
bool orbit_camera_by_mouse(matter::CameraDesc& camera,
                           const matter::Float3& pivot, float drag_x_pixels,
                           float drag_y_pixels, float wheel_ticks,
                           float radians_per_pixel, float zoom_step);

}  // namespace viewer
