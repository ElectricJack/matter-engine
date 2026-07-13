#include "camera_controller.h"

#include <cmath>

#define GLFW_INCLUDE_NONE
#include "../Libraries/raylib/src/external/glfw/include/GLFW/glfw3.h"

namespace viewer {
namespace {

matter::Float3 add(matter::Float3 a, matter::Float3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

matter::Float3 sub(matter::Float3 a, matter::Float3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

matter::Float3 mul(matter::Float3 v, float scale) {
    return {v.x * scale, v.y * scale, v.z * scale};
}

float dot(matter::Float3 a, matter::Float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

matter::Float3 cross(matter::Float3 a, matter::Float3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

matter::Float3 normalized(matter::Float3 v, matter::Float3 fallback) {
    const float length_squared = dot(v, v);
    if (length_squared <= 1e-12f) return fallback;
    return mul(v, 1.0f / std::sqrt(length_squared));
}

matter::Float3 rotate_around_axis(matter::Float3 v, matter::Float3 axis, float angle) {
    axis = normalized(axis, {0.0f, 1.0f, 0.0f});
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return add(add(mul(v, c), mul(cross(axis, v), s)),
               mul(axis, dot(axis, v) * (1.0f - c)));
}

} // namespace

void apply_camera_input(matter::CameraDesc& camera, const CameraInput& input,
                        float dt, float speed, float radians_per_pixel) {
    matter::Float3 view = sub(camera.target, camera.position);
    const float view_length = std::sqrt(dot(view, view));
    matter::Float3 forward = normalized(view, {0.0f, 0.0f, -1.0f});
    const matter::Float3 world_up = normalized(camera.up, {0.0f, 1.0f, 0.0f});
    matter::Float3 right = normalized(cross(forward, world_up), {1.0f, 0.0f, 0.0f});

    if (input.yaw_pixels != 0.0f)
        forward = rotate_around_axis(forward, world_up,
                                     -input.yaw_pixels * radians_per_pixel);
    right = normalized(cross(forward, world_up), right);
    if (input.pitch_pixels != 0.0f) {
        const matter::Float3 pitched = rotate_around_axis(
            forward, right, -input.pitch_pixels * radians_per_pixel);
        // Keep a small margin from the poles so the right vector remains stable.
        if (std::fabs(dot(pitched, world_up)) < 0.99985f) forward = pitched;
    }

    camera.target = add(camera.position, mul(forward, view_length > 1e-6f ? view_length : 1.0f));

    const float distance = speed * dt * (input.speed_boost ? 4.0f : 1.0f);
    const matter::Float3 movement = add(add(mul(forward, input.forward),
                                            mul(right, input.right)),
                                        mul(world_up, input.up));
    const matter::Float3 delta = mul(movement, distance);
    camera.position = add(camera.position, delta);
    camera.target = add(camera.target, delta);
}

void CameraController::update(GLFWwindow* window, float dt, matter::CameraDesc& camera) {
    if (!window || !captured_) return;

    CameraInput input{};
    input.forward = (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS ? 1.0f : 0.0f) -
                    (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ? 1.0f : 0.0f);
    input.right = (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ? 1.0f : 0.0f) -
                  (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ? 1.0f : 0.0f);
    input.up = (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS ? 1.0f : 0.0f) -
               (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ? 1.0f : 0.0f);
    input.speed_boost = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                        glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (first_mouse_) {
        last_x_ = x;
        last_y_ = y;
        first_mouse_ = false;
    }
    input.yaw_pixels = static_cast<float>(x - last_x_);
    input.pitch_pixels = static_cast<float>(y - last_y_);
    last_x_ = x;
    last_y_ = y;

    apply_camera_input(camera, input, dt, 8.0f, 0.002f);
}

void CameraController::set_capture(GLFWwindow* window, bool capture) {
    captured_ = capture;
    first_mouse_ = true;
    if (window)
        glfwSetInputMode(window, GLFW_CURSOR,
                         capture ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

} // namespace viewer
