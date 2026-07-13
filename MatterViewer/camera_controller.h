#pragma once

#include "matter/camera.h"

struct GLFWwindow;

namespace viewer {

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
    void update(GLFWwindow* window, float dt, matter::CameraDesc& camera);
    void set_capture(GLFWwindow* window, bool capture);

private:
    bool captured_ = false;
    bool first_mouse_ = true;
    double last_x_ = 0.0;
    double last_y_ = 0.0;
};

} // namespace viewer
