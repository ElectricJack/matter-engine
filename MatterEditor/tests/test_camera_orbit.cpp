// Camera panel orbit / move / turn math (issue a4203d22 parts 1 and 2).
//
// Everything here is window-free on purpose: camera_orbit.cpp pulls in no
// GLFW, no ImGui and no session, and apply_camera_input is a pure function
// over CameraDesc. The two invariants the issue's acceptance list names get a
// test each -- a pivot-substituted orbit preserves distance to the pivot, and
// the turn buttons preserve cam.position EXACTLY (bitwise, not within an
// epsilon: a turn that translates the camera by a rounding error is a turn
// that drifts over a hundred presses).
//
// What is NOT covered here, and cannot be: CameraController::update's mouse
// path (part 3) needs a live GLFWwindow, and the ImGui button wiring needs a
// window and an ImGui context.

#include "../src/camera_controller.h"
#include "../src/camera_orbit.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using matter::CameraDesc;
using matter::Float3;
using viewer::CameraInput;
using viewer::OrbitFrame;

namespace {

constexpr float kQuarterPi = 0.78539816339f;

CameraDesc make_camera(Float3 position, Float3 target) {
    return CameraDesc{position, target, {0, 1, 0}, kQuarterPi, 1.0f, 5000.0f};
}

float distance3(Float3 a, Float3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool close3(Float3 a, Float3 b, float eps) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps &&
           std::fabs(a.z - b.z) < eps;
}

// Bitwise identity, not near-equality. See the file header.
bool identical3(Float3 a, Float3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// ---- Part 1: orbit about a substituted pivot -----------------------------

// The acceptance invariant. The pivot is deliberately nowhere near cam.target,
// which is the whole point of the feature: before the substitution the orbit
// was hard-wired to cam.target.
void test_pivot_orbit_preserves_distance_to_pivot() {
    const Float3 pivot{7.0f, -3.0f, 11.0f};
    CameraDesc cam = make_camera({20, 16, 34}, {0, 9, 0});
    const float before = distance3(cam.position, pivot);

    OrbitFrame frame = viewer::orbit_frame_from(cam, pivot);
    frame.yaw += 0.37f;
    frame.pitch += 0.11f;
    viewer::apply_orbit_frame(cam, pivot, viewer::clamp_orbit_frame(frame), true);

    const float after = distance3(cam.position, pivot);
    assert(std::fabs(after - before) < 1e-3f);
    // look_at_pivot: the object stays in frame only because the camera is
    // re-aimed at it, not merely displaced around it.
    assert(close3(cam.target, pivot, 1e-5f));
    std::printf("  pivot orbit preserves distance to pivot: OK (%.4f -> %.4f)\n",
                static_cast<double>(before), static_cast<double>(after));
}

// Acceptance item 3, the part that can be checked without a window: orbiting a
// full circle returns the camera to where it started, so the selection is
// framed identically at 0 and at 360 degrees.
void test_full_circle_returns_to_start() {
    const Float3 pivot{2.0f, 1.5f, -4.0f};
    CameraDesc cam = make_camera({20, 16, 34}, {0, 9, 0});
    // First orbit tick also snaps the target onto the pivot; take the
    // reference AFTER that so the comparison is orbit-vs-orbit.
    OrbitFrame seed = viewer::orbit_frame_from(cam, pivot);
    viewer::apply_orbit_frame(cam, pivot, viewer::clamp_orbit_frame(seed), true);
    const Float3 start = cam.position;

    constexpr int kSteps = 157;  // 2*pi / 0.04, the default orbit_step
    for (int i = 0; i < kSteps; ++i) {
        OrbitFrame frame = viewer::orbit_frame_from(cam, pivot);
        frame.yaw += 6.283185307179586f / kSteps;
        viewer::apply_orbit_frame(cam, pivot, viewer::clamp_orbit_frame(frame),
                                  true);
    }
    assert(close3(cam.position, start, 1e-2f));
    assert(close3(cam.target, pivot, 1e-5f));
    std::printf("  full circle returns to start: OK\n");
}

// One wheel tick must be exactly one Zoom In press, and the wheel must never
// be able to push the radius through zero however many ticks arrive at once.
void test_wheel_zoom_matches_one_zoom_button() {
    const Float3 pivot{0, 0, 0};
    CameraDesc cam = make_camera({0, 0, 50}, {0, 0, 0});
    viewer::orbit_camera_by_mouse(cam, pivot, 0.0f, 0.0f, 1.0f, 0.002f, 0.04f);
    assert(std::fabs(distance3(cam.position, pivot) - 50.0f * 0.96f) < 1e-3f);

    CameraDesc spun = make_camera({0, 0, 50}, {0, 0, 0});
    viewer::orbit_camera_by_mouse(spun, pivot, 0.0f, 0.0f, 400.0f, 0.002f, 0.04f);
    const float d = distance3(spun.position, pivot);
    assert(d >= 1.0f);   // clamped, not negative and not NaN
    assert(d == d);
    std::printf("  wheel zoom == one Zoom In press: OK\n");
}

// A no-op frame must be reported as a no-op so the caller can skip the write.
void test_idle_mouse_does_not_move_the_camera() {
    CameraDesc cam = make_camera({20, 16, 34}, {0, 9, 0});
    const CameraDesc before = cam;
    const bool moved = viewer::orbit_camera_by_mouse(cam, {1, 2, 3}, 0.0f, 0.0f,
                                                     0.0f, 0.002f, 0.04f);
    assert(!moved);
    assert(identical3(cam.position, before.position));
    assert(identical3(cam.target, before.target));
    std::printf("  idle mouse is a no-op: OK\n");
}

// The pole and the degenerate-radius cases the panel's inline math only
// survived because its pivot was always cam.target.
void test_clamps_and_degenerate_pivot() {
    OrbitFrame frame{0.0f, 3.0f, 0.01f};
    frame = viewer::clamp_orbit_frame(frame);
    assert(frame.pitch < 1.5708f && frame.pitch > 1.55f);
    assert(frame.distance == 1.0f);

    // Camera exactly above the pivot: dy/distance is 1 to the last bit, and an
    // unclamped asin of 1+epsilon would be NaN.
    CameraDesc cam = make_camera({5, 25, 5}, {5, 0, 5});
    const OrbitFrame straight_up = viewer::orbit_frame_from(cam, {5, 0, 5});
    assert(straight_up.pitch == straight_up.pitch);
    assert(std::fabs(straight_up.distance - 25.0f) < 1e-4f);

    // Pivot coincident with the camera: no NaN, no divide by zero.
    const OrbitFrame degenerate = viewer::orbit_frame_from(cam, cam.position);
    assert(degenerate.distance > 0.0f);
    assert(degenerate.pitch == degenerate.pitch);
    std::printf("  pole / degenerate pivot clamps: OK\n");
}

// ---- Part 2: discrete move and turn --------------------------------------

// The acceptance invariant: a turn rotates the look direction and leaves the
// eye where it was, to the bit.
void test_turn_preserves_position_exactly() {
    CameraDesc cam = make_camera({20, 16, 34}, {0, 9, 0});
    const Float3 eye = cam.position;
    const float view_length = distance3(cam.position, cam.target);

    // Exactly what the Turn buttons do: one "pixel" of yaw at
    // radians_per_pixel = turn_step, zero move speed.
    constexpr float kTurnStep = 0.2618f;  // 15 degrees
    for (int i = 0; i < 24; ++i) {
        CameraInput input{};
        input.yaw_pixels = 1.0f;
        viewer::apply_camera_input(cam, input, 1.0f, 0.0f, kTurnStep);
    }
    assert(identical3(cam.position, eye));
    // 24 * 15 degrees = 360: the look direction is back where it started, and
    // the view length never changed.
    assert(std::fabs(distance3(cam.position, cam.target) - view_length) < 1e-3f);
    std::printf("  turn preserves position exactly: OK\n");
}

// One press must turn by exactly turn_step radians, otherwise "15 degrees" in
// the schema is a lie.
void test_turn_step_is_the_requested_angle() {
    CameraDesc cam = make_camera({0, 0, 0}, {0, 0, -5});
    constexpr float kTurnStep = 0.2618f;
    CameraInput input{};
    input.yaw_pixels = 1.0f;
    viewer::apply_camera_input(cam, input, 1.0f, 0.0f, kTurnStep);

    // forward went from (0,0,-1) to (sin, 0, -cos) of the turn angle.
    const float angle = std::atan2(std::fabs(cam.target.x),
                                   std::fabs(cam.target.z));
    assert(std::fabs(angle - kTurnStep) < 1e-4f);
    std::printf("  one turn press == turn_step radians: OK\n");
}

// A move press translates BOTH position and target by exactly move_step along
// the camera basis -- the framing does not change, the viewpoint does.
void test_move_step_translates_by_exactly_move_step() {
    CameraDesc cam = make_camera({0, 0, 5}, {0, 0, 0});
    const Float3 eye = cam.position;
    const Float3 look = cam.target;
    constexpr float kMoveStep = 2.0f;

    CameraInput input{};
    input.forward = 1.0f;
    viewer::apply_camera_input(cam, input, 1.0f, kMoveStep, 0.002f);
    assert(close3(cam.position, {0, 0, 3}, 1e-5f));
    assert(close3(cam.target, {0, 0, -2}, 1e-5f));
    assert(std::fabs(distance3(cam.position, eye) - kMoveStep) < 1e-5f);
    assert(std::fabs(distance3(cam.target, look) - kMoveStep) < 1e-5f);

    // Strafe right is the same magnitude on the perpendicular axis.
    CameraDesc side = make_camera({0, 0, 5}, {0, 0, 0});
    CameraInput strafe{};
    strafe.right = 1.0f;
    viewer::apply_camera_input(side, strafe, 1.0f, kMoveStep, 0.002f);
    assert(std::fabs(distance3(side.position, {0, 0, 5}) - kMoveStep) < 1e-5f);
    assert(std::fabs(side.position.z - 5.0f) < 1e-5f);
    std::printf("  move press == move_step metres: OK\n");
}

// The default steps the schema ships. If someone retunes these, the schema doc
// strings ("15 degrees", "2 m") must be retuned with them.
void test_defaults_match_the_documented_steps() {
    const viewer::CameraPrefs prefs;
    assert(std::fabs(prefs.turn_step - 15.0f * 3.14159265358979323846f / 180.0f) <
           1e-4f);
    assert(prefs.move_step == 2.0f);
    assert(prefs.orbit_selection == false);
    std::printf("  camera.prefs defaults: OK\n");
}

}  // namespace

int main() {
    std::printf("test_camera_orbit\n");
    test_pivot_orbit_preserves_distance_to_pivot();
    test_full_circle_returns_to_start();
    test_wheel_zoom_matches_one_zoom_button();
    test_idle_mouse_does_not_move_the_camera();
    test_clamps_and_degenerate_pivot();
    test_turn_preserves_position_exactly();
    test_turn_step_is_the_requested_angle();
    test_move_step_translates_by_exactly_move_step();
    test_defaults_match_the_documented_steps();
    std::printf("test_camera_orbit: all OK\n");
    return 0;
}
