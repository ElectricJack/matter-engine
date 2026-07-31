#pragma once

#include "matter/camera.h"

struct GLFWwindow;

namespace viewer {

// Editor camera preferences (Scope::User property group "camera.prefs"). These
// are per-machine taste, not project data: a 20 km far plane is a choice about
// this GPU, and fly speed is a choice about this mouse. main.cpp owns one
// instance, pushes far_plane into the live CameraDesc each frame, and hands
// move_speed to CameraController::update — the two values used to be a hand
// slider in the LOD panel and a literal at the update call site.
struct CameraPrefs {
    float far_plane = 10241.0f;
    float move_speed = 8.0f;
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

struct CameraInput {
    float forward = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float yaw_pixels = 0.0f;
    float pitch_pixels = 0.0f;
    bool speed_boost = false;
};

void apply_camera_input(matter::CameraDesc& camera, const CameraInput& input,
                        float dt, float speed, float radians_per_pixel,
                        float boost_multiplier = 4.0f);

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

    bool captured_ = false;
    bool first_mouse_ = true;
    // What was last handed to glfwSetInputMode(GLFW_RAW_MOUSE_MOTION), so the
    // per-frame check is a comparison and not a redundant GLFW call.
    bool raw_motion_applied_ = false;
    double last_x_ = 0.0;
    double last_y_ = 0.0;
};

} // namespace viewer
