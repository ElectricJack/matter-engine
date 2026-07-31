#include "camera_controller.h"

#include <cmath>

#define GLFW_INCLUDE_NONE
#include "../../third_party/raylib/src/external/glfw/include/GLFW/glfw3.h"

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
                        float dt, float speed, float radians_per_pixel,
                        float boost_multiplier) {
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
        constexpr float kPoleDotLimit = 0.99985f;
        const float current_up_dot = std::fmax(-1.0f, std::fmin(1.0f, dot(forward, world_up)));
        const float current_pitch = std::asin(current_up_dot);
        const float max_pitch = std::asin(kPoleDotLimit);
        const float requested_pitch = current_pitch - input.pitch_pixels * radians_per_pixel;
        const float clamped_pitch = std::fmax(-max_pitch, std::fmin(max_pitch, requested_pitch));
        forward = rotate_around_axis(forward, right, clamped_pitch - current_pitch);
    }

    camera.target = add(camera.position, mul(forward, view_length > 1e-6f ? view_length : 1.0f));

    // CameraPrefs::boost_multiplier (default 4.0) — a described property now,
    // not a literal.
    const float distance =
        speed * dt * (input.speed_boost ? boost_multiplier : 1.0f);
    const matter::Float3 movement = normalized(
        add(add(mul(forward, input.forward), mul(right, input.right)),
            mul(world_up, input.up)),
        {0.0f, 0.0f, 0.0f});
    const matter::Float3 delta = mul(movement, distance);
    camera.position = add(camera.position, delta);
    camera.target = add(camera.target, delta);
}

void CameraController::update(GLFWwindow* window, float dt,
                              matter::CameraDesc& camera,
                              const CameraPrefs& prefs) {
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

    // Mouse look. NO recentring warp here — see set_capture: the cursor is in
    // GLFW_CURSOR_DISABLED, which already hides the pointer and reports an
    // UNBOUNDED virtual position, so successive glfwGetCursorPos values differ
    // by exactly the motion since the last poll. That is the documented way to
    // read disabled-cursor mode.
    //
    // The old code additionally did glfwSetCursorPos(window, centre) every
    // frame and differenced against the centre. Locally that is merely
    // redundant. Over Remote Desktop it is the runaway-spin bug (issue
    // a4203d22): the warp is asynchronous and the RDP pointer channel may
    // delay or drop it entirely, so the next poll still reports the
    // pre-warp position — the same displacement is differenced against the
    // centre again and again and the view accelerates without the user
    // moving the mouse.
    // Pick up a Raw mouse motion toggle without needing a capture cycle: the
    // whole reason it is a preference is so its effect on feel can be A/B'd,
    // and an A/B that requires two TAB presses between samples is not one.
    if (raw_motion_applied_ != prefs.raw_mouse_motion)
        apply_raw_motion(window, prefs.raw_mouse_motion);

    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (first_mouse_) {
        // Still needed: the first poll after capture reports wherever the
        // pointer happened to be, and differencing that against a stale
        // last_x_/last_y_ would snap the view. Swallow one frame's delta.
        last_x_ = x;
        last_y_ = y;
        first_mouse_ = false;
    } else {
        const double dx = x - last_x_;
        const double dy = y - last_y_;
        last_x_ = x;
        last_y_ = y;

        // Belt and braces on top of removing the warp, NOT instead of it (a
        // clamp alone would only turn a spin into a drift). Any single frame
        // whose delta exceeds a third of the window is not a hand movement —
        // it is a mode switch, a display-scale change, or a resynchronising
        // remote pointer. Drop the whole frame's look rather than clamping
        // it: a clamped kick is still a visible lurch, and last_x_/last_y_
        // are already re-anchored above so the next frame resumes cleanly.
        int win_w = 0, win_h = 0;
        glfwGetWindowSize(window, &win_w, &win_h);
        const double limit_x = win_w > 0 ? win_w / 3.0 : 1.0e9;
        const double limit_y = win_h > 0 ? win_h / 3.0 : 1.0e9;
        if (std::fabs(dx) <= limit_x && std::fabs(dy) <= limit_y) {
            input.yaw_pixels = static_cast<float>(dx);
            input.pitch_pixels = static_cast<float>(dy);
        }
    }

    apply_camera_input(camera, input, dt, prefs.move_speed,
                       prefs.look_sensitivity, prefs.boost_multiplier);
}

void CameraController::apply_raw_motion(GLFWwindow* window, bool enable) {
    // Raw motion takes the device's own deltas and skips the desktop pointer
    // pipeline (acceleration curves, the RDP pointer channel, per-monitor
    // scaling), so free-fly look stops depending on how faithfully an indirect
    // display reproduces cursor positions. Unsupported on some
    // platforms/backends; glfwRawMouseMotionSupported reports that, and there
    // we simply never turn it on.
    if (!window || !glfwRawMouseMotionSupported()) return;
    glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION,
                     enable ? GLFW_TRUE : GLFW_FALSE);
    raw_motion_applied_ = enable;
}

void CameraController::set_capture(GLFWwindow* window, bool capture,
                                   bool raw_motion) {
    captured_ = capture;
    first_mouse_ = true;
    if (!window) return;
    glfwSetInputMode(window, GLFW_CURSOR,
                     capture ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    // ORDER IS LOAD-BEARING: raw motion is applied AFTER the cursor mode,
    // because GLFW only honours GLFW_RAW_MOUSE_MOTION while the cursor is
    // disabled. On release we always clear it, whatever the preference says.
    apply_raw_motion(window, capture && raw_motion);
}

} // namespace viewer
