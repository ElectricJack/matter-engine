#pragma once

// MatterEditor/src/camera_controller.h
//
// The editor's free-fly camera: WASD/Space/Ctrl movement plus mouse look,
// driven straight off GLFW key and cursor state.
//
// Three pieces, deliberately separable:
//   - `CameraPrefs`  persisted per-machine taste, exposed as the Scope::User
//                    property group "camera.prefs". main.cpp owns one instance.
//   - `apply_camera_input`  the pure math: takes an already-sampled
//                    CameraInput and a dt and rewrites a CameraDesc. No GLFW,
//                    no globals, no state between calls.
//   - `CameraController`  the stateful half: owns mouse capture, the previous
//                    cursor position, and the raw-motion input mode.
//
// Usage: call set_capture(window, true, prefs.raw_mouse_motion) when the user
// enters free-fly (TAB in the editor) and set_capture(..., false) to leave it.
// update() is a no-op while not captured, so it is safe to call every frame.
//
// Conventions. The camera is described by position/target/up (matter::CameraDesc);
// this file preserves the position-to-target distance across a look and moves
// position and target together when translating, so "target" is a look-at point
// rather than an orbit pivot. +Y is world up by default. Angles are radians,
// distances are world units (metres at editor scale), and mouse deltas are in
// PIXELS -- the pixels-to-radians conversion is CameraPrefs::look_sensitivity.
//
// Threading: main/render thread only. GLFW input functions must be called from
// the thread that owns the window.
//
// The comments on CameraPrefs' fields record which literal each preference
// replaced and, for the mouse-look fields, the remote-desktop runaway-spin
// issue (a4203d22) they came out of; camera_controller.cpp carries the rest of
// that story.

#include "matter/camera.h"

struct GLFWwindow;

namespace viewer {

// Editor camera preferences (Scope::User property group "camera.prefs"). These
// are per-machine taste, not project data: the far plane (10241 m by default)
// is a choice about this GPU, and fly speed is a choice about this mouse.
// main.cpp owns one
// instance, pushes far_plane into the live CameraDesc each frame, and hands
// move_speed to CameraController::update — the two values used to be a hand
// slider in the LOD panel and a literal at the update call site.
struct CameraPrefs {
    float far_plane = 10241.0f;  // world units; pushed into the live CameraDesc.
    float move_speed = 8.0f;     // free-fly world units per second, before boost.
    // Shift multiplier on the fly speed. Was a bare 4.0f literal inside
    // apply_camera_input; the default keeps that exact behavior.
    float boost_multiplier = 4.0f;
    // Free-fly mouse look, radians per pixel of cursor motion. Was the 0.002f
    // literal at CameraController::update's apply_camera_input call.
    float look_sensitivity = 0.002f;
    // GLFW_RAW_MOUSE_MOTION during free-fly (issue a4203d22 part 3). On by
    // default because raw device deltas are the sturdier input over an
    // indirect display path, which is what the issue is about.
    //
    // It is a PREFERENCE and not a constant precisely because it is the one
    // part of the part-3 fix that can change how free-fly FEELS locally: raw
    // motion skips the desktop pointer pipeline, so Windows' "Enhance pointer
    // precision" acceleration no longer applies and fast flicks travel a
    // constant number of radians per count. Removing the recentring warp,
    // by contrast, is behaviour-neutral. Anyone who preferred the accelerated
    // curve turns this off and gets exactly the old response; the warp stays
    // gone either way. Live — CameraController re-applies it mid-capture.
    bool raw_mouse_motion = true;
    // Camera panel orbit buttons: radians per repeat tick, and the fraction of
    // the current distance one Zoom In/Out tick adds or removes. Both were
    // literals in draw_camera_panel (0.04, and 0.96/1.04 which is 1 -/+ 0.04).
    float orbit_step = 0.04f;
    float orbit_zoom_step = 0.04f;
    // "Orbit selection" (issue a4203d22 part 1). When on AND the selection
    // resolves to bounds, the Camera panel's orbit/zoom — and viewport
    // drag/wheel — pivot on the selection's focus point instead of cam.target.
    // A persisted preference, not session state: it is a way of driving the
    // camera, and a user who works this way wants it back next launch.
    bool orbit_selection = false;
    // Discrete move/turn buttons (part 2). Deliberately coarse compared with
    // orbit_step: these exist so the viewpoint can be driven WITHOUT the mouse
    // (the panel is the workaround for the remote-desktop spin), so one press
    // has to travel a useful amount. 15 degrees and 2 m are roughly "one
    // noticeable step" at editor scale.
    float turn_step = 0.2618f;  // 15 degrees in radians
    float move_step = 2.0f;     // metres per press
};

// One frame's sampled camera input, decoupled from where it came from: the
// controller fills it from GLFW, but a test or a scripted driver can fill it by
// hand and call apply_camera_input directly.
//
// The three axes are unitless direction requests, normally -1, 0 or +1; their
// combination is normalized before it is scaled by speed, so moving diagonally
// is not faster than moving straight. The two look fields are raw cursor deltas
// in PIXELS, converted by `radians_per_pixel` at the call.
struct CameraInput {
    float forward = 0.0f;        // +1 = toward the target, -1 = away.
    float right = 0.0f;          // +1 = camera-right (forward x world up).
    float up = 0.0f;             // +1 = along world up, not camera up.
    float yaw_pixels = 0.0f;     // cursor dx; positive turns the view right.
    float pitch_pixels = 0.0f;   // cursor dy; positive (cursor down) looks down.
    bool speed_boost = false;    // shift held: multiply speed by boost_multiplier.
};

// Applies one frame of input to `camera` in place. Pure with respect to
// everything except `camera`: no GLFW, no statics, no allocation.
//
// Look happens first (yaw about world up, then pitch about the camera's right
// axis, clamped just short of the poles so the view can never flip), then
// translation moves position AND target by the same delta, so the look-at
// distance is preserved. `dt` is in seconds and `speed` in world units per
// second; `radians_per_pixel` converts the two pixel deltas.
void apply_camera_input(matter::CameraDesc& camera, const CameraInput& input,
                        float dt, float speed, float radians_per_pixel,
                        float boost_multiplier = 4.0f);

// Free-fly mouse/keyboard capture on top of apply_camera_input.
//
// Holds no reference to the window: every method takes the GLFWwindow*, so the
// controller has no lifetime relationship with it. What it does own is the
// capture flag, the previous cursor position, and a mirror of the raw-motion
// input mode it last applied.
//
// Call order matters. set_capture(window, true, ...) puts the cursor into
// GLFW_CURSOR_DISABLED and arms first_mouse_, which makes the next update()
// swallow one frame of delta instead of snapping the view. update() then does
// nothing at all until that has happened. On release, raw motion is always
// turned back off whatever the preference says.
//
// Main/render thread only (GLFW input).
class CameraController {
public:
    // `prefs` supplies move_speed, look_sensitivity and boost_multiplier — the
    // three values the camera.prefs property group describes. The default is
    // the compiled CameraPrefs, so a caller with no prefs of its own gets the
    // behavior this function had before the group existed.
    void update(GLFWwindow* window, float dt, matter::CameraDesc& camera,
                const CameraPrefs& prefs = CameraPrefs{});
    // `raw_motion` is CameraPrefs::raw_mouse_motion. It is passed here rather
    // than read from a stored prefs pointer because set_capture is also the
    // RELEASE path, which has to turn raw motion back off.
    void set_capture(GLFWwindow* window, bool capture, bool raw_motion = true);

private:
    // Applies GLFW_RAW_MOUSE_MOTION and records what was applied, so update()
    // can notice a mid-capture preference change and re-apply. Only meaningful
    // while the cursor is disabled — GLFW ignores the mode otherwise.
    void apply_raw_motion(GLFWwindow* window, bool enable);

    bool captured_ = false;    // update() is a no-op unless this is true.
    bool first_mouse_ = true;  // swallow the next delta; set by set_capture.
    // What was last handed to glfwSetInputMode(GLFW_RAW_MOUSE_MOTION), so the
    // per-frame check is a comparison and not a redundant GLFW call.
    bool raw_motion_applied_ = false;
    double last_x_ = 0.0;  // previous glfwGetCursorPos, in the unbounded
    double last_y_ = 0.0;  // virtual space GLFW_CURSOR_DISABLED reports.
};

} // namespace viewer
