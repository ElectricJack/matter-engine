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
                        float dt, float speed, float radians_per_pixel);

class CameraController {
public:
    void update(GLFWwindow* window, float dt, matter::CameraDesc& camera,
                float move_speed = 8.0f);
    void set_capture(GLFWwindow* window, bool capture);

private:
    bool captured_ = false;
    bool first_mouse_ = true;
    double last_x_ = 0.0;
    double last_y_ = 0.0;
};

} // namespace viewer
