// MatterEditor/src/camera_orbit.cpp
//
// Implementation of the orbit-about-a-pivot camera math. camera_orbit.h carries
// the design rationale and the yaw/pitch/distance conventions; read it first.
//
// Deliberately dependency-light: this TU includes only the camera types and
// <cmath> -- no GLFW, no ImGui, no WorldSession -- which is what makes the
// invariant that matters (an orbit preserves distance to the pivot) assertable
// without a window.
//
// The two constants below are the pole and near-pivot guards, lifted verbatim
// from the Camera panel code this file replaced so the buttons feel identical.
// Everything else is a straight spherical decompose / nudge / recompose:
// orbit_frame_from -> mutate the frame -> clamp_orbit_frame -> apply_orbit_frame.
// Clamping between the mutation and the apply is the intended order, so that a
// value the panel also displays is the value that took effect.
//
// Angles are radians and distances world units. No state is kept between calls;
// the camera itself is the storage.

#include "camera_orbit.h"

#include <cmath>

namespace viewer {
namespace {

// ~89 degrees. Lifted unchanged from draw_camera_panel, where it was the bare
// 1.5533f literal guarding the pole.
constexpr float kPitchLimit = 1.5533f;
// Minimum orbit radius, also unchanged from the panel. Zooming in past this
// would put the camera inside the pivot and make yaw meaningless.
constexpr float kMinDistance = 1.0f;

}  // namespace

// Decomposes camera.position about `pivot` into yaw/pitch/distance. Never
// returns a NaN or a zero distance: a coincident camera and pivot yield an
// arbitrary tiny radius, and the asin argument is clamped -- see the two local
// comments for why each guard is needed.
OrbitFrame orbit_frame_from(const matter::CameraDesc& camera,
                            const matter::Float3& pivot) {
    const float dx = camera.position.x - pivot.x;
    const float dy = camera.position.y - pivot.y;
    const float dz = camera.position.z - pivot.z;
    float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    // The panel's original guard: a degenerate radius makes asin(dy/distance)
    // a NaN factory, so pick an arbitrary tiny non-zero one instead.
    if (distance < 0.0001f) distance = 0.0001f;

    OrbitFrame frame;
    frame.distance = distance;
    frame.yaw = std::atan2(dz, dx);
    // dy/distance can land a hair outside [-1,1] through float rounding when
    // the camera is exactly above the pivot; asin would return NaN and the
    // camera would vanish. The panel never clamped this because the pivot was
    // cam.target and dy/distance came from the same subtraction that produced
    // distance; with an arbitrary pivot it is worth the two fmaxes.
    const float sin_pitch = std::fmax(-1.0f, std::fmin(1.0f, dy / distance));
    frame.pitch = std::asin(sin_pitch);
    return frame;
}

// Takes and returns by value: the caller keeps the clamped frame and displays
// that, rather than clamping inside apply and showing a value that never
// happened.
OrbitFrame clamp_orbit_frame(OrbitFrame frame) {
    if (frame.pitch > kPitchLimit) frame.pitch = kPitchLimit;
    if (frame.pitch < -kPitchLimit) frame.pitch = -kPitchLimit;
    if (frame.distance < kMinDistance) frame.distance = kMinDistance;
    return frame;
}

void apply_orbit_frame(matter::CameraDesc& camera, const matter::Float3& pivot,
                       const OrbitFrame& frame, bool look_at_pivot) {
    // Order matters only for readability: target first so a reader sees the
    // camera aimed at the pivot before it is placed around it.
    if (look_at_pivot) camera.target = pivot;
    const float cos_pitch = std::cos(frame.pitch);
    camera.position.x =
        pivot.x + frame.distance * cos_pitch * std::cos(frame.yaw);
    camera.position.y = pivot.y + frame.distance * std::sin(frame.pitch);
    camera.position.z =
        pivot.z + frame.distance * cos_pitch * std::sin(frame.yaw);
}

// Viewport drag/wheel orbit. Returns false and touches nothing when all three
// inputs are zero, so a caller can invoke it unconditionally each frame and use
// the return value to decide whether the camera needs republishing.
//
// Always re-derives the frame from the camera's CURRENT position, so the orbit
// composes correctly with anything else that moved the camera since the last
// call -- there is no accumulated yaw/pitch to get out of sync.
bool orbit_camera_by_mouse(matter::CameraDesc& camera,
                           const matter::Float3& pivot, float drag_x_pixels,
                           float drag_y_pixels, float wheel_ticks,
                           float radians_per_pixel, float zoom_step) {
    if (drag_x_pixels == 0.0f && drag_y_pixels == 0.0f && wheel_ticks == 0.0f)
        return false;

    OrbitFrame frame = orbit_frame_from(camera, pivot);
    // Drag left == the Left button (yaw -= step); drag up == the Up button
    // (pitch += step, camera rises). Deliberately consistent with the buttons
    // sitting a few pixels away in the same panel rather than with any
    // particular DCC tool's convention.
    frame.yaw += drag_x_pixels * radians_per_pixel;
    frame.pitch -= drag_y_pixels * radians_per_pixel;
    if (wheel_ticks != 0.0f) {
        // pow, not a linear 1 - step*ticks: one tick must be exactly one Zoom
        // In (x (1 - step)), and a multiplicative form can never cross zero or
        // flip sign no matter how many ticks arrive in one frame.
        frame.distance *= std::pow(1.0f - zoom_step, wheel_ticks);
    }
    apply_orbit_frame(camera, pivot, clamp_orbit_frame(frame), true);
    return true;
}

}  // namespace viewer
