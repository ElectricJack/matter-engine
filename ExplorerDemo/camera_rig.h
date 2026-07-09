#pragma once
#include "raylib.h"

// CameraRig — hand-rolled free-camera with composed keyboard/mouse + gamepad input.
// WASD/mouse: CAMERA_FREE semantics (mouselook when cursor is disabled).
// Gamepad: left stick = move (XZ), right stick = look, triggers = speed boost.
// All inputs compose (sum), no mode switching.
struct CameraRig {
    Camera3D cam{};

    // Called once to set up the camera at the Meadow Valley spawn point.
    void init();

    // Update camera from input. dt = frame time in seconds.
    void update(float dt);

    // True when the user has grabbed mouse control (cursor disabled).
    bool user_has_control() const;

    // Programmatic camera shot for smoke/test sequences.
    // shot: reserved for future named-shot index.
    void play_staged(int shot);

private:
    bool cursor_captured_ = false;
    float yaw_   = 0.0f;   // radians, updated from look deltas
    float pitch_ = 0.0f;   // radians, clamped to ±85°
    float move_speed_   = 20.0f;  // m/s base
    float look_sensitivity_ = 0.003f;  // rad/pixel (mouse)
    float stick_look_speed_ = 1.8f;    // rad/s (gamepad right stick)
};
