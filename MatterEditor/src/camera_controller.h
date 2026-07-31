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
    void set_capture(GLFWwindow* window, bool capture);

private:
    bool captured_ = false;
    bool first_mouse_ = true;
    double last_x_ = 0.0;
    double last_y_ = 0.0;
};

} // namespace viewer
