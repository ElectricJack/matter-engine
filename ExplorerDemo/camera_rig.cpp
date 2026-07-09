#include "camera_rig.h"
#include "raylib.h"
#include "raymath.h"

#include <cmath>

// World-center spawn: Meadow Valley is a 10x world (51x51 tiles).
// The tile grid spans roughly 0..816 in X and Z (51*16 = 816).
// Centre is (408, ?, 408). Terrain height at (408,408) for the default seed
// (20260709) is ~0.0 units (world center sits at the flat meadow bottom;
// precomputed from terrain_noise.js heightField). We add 8 m for near-ground
// immersive start facing the mountain range.
static constexpr float SPAWN_X =  408.0f;
static constexpr float SPAWN_Y =    8.0f;   // precomputed heightAt(408,408)≈0 + 8 m above terrain
static constexpr float SPAWN_Z =  408.0f;

// Direction the camera faces on spawn: toward the mountain range (roughly -Z).
// yaw=PI faces -Z.
static constexpr float SPAWN_YAW   = 3.14159265f;   // PI  → face -Z
static constexpr float SPAWN_PITCH = -0.15f;         // slight downward tilt

static constexpr float DEG2RAD_F = 0.01745329251f;
static constexpr float PI_F      = 3.14159265358979f;

// Recompute target from position + yaw/pitch.
static Vector3 direction_from_yaw_pitch(float yaw, float pitch) {
    return {
        cosf(pitch) * sinf(yaw),
        sinf(pitch),
        cosf(pitch) * cosf(yaw)
    };
}

void CameraRig::init() {
    yaw_   = SPAWN_YAW;
    pitch_ = SPAWN_PITCH;

    cam.position   = { SPAWN_X, SPAWN_Y, SPAWN_Z };
    cam.up         = { 0.0f, 1.0f, 0.0f };
    cam.fovy       = 70.0f;
    cam.projection = CAMERA_PERSPECTIVE;

    Vector3 dir = direction_from_yaw_pitch(yaw_, pitch_);
    cam.target = {
        cam.position.x + dir.x,
        cam.position.y + dir.y,
        cam.position.z + dir.z
    };
}

void CameraRig::update(float dt) {
    // --- Cursor capture toggle (Tab key) ---
    if (IsKeyPressed(KEY_TAB)) {
        cursor_captured_ = !cursor_captured_;
        if (cursor_captured_) DisableCursor();
        else                  EnableCursor();
    }

    // --- Accumulate look delta (mouse + right gamepad stick) ---
    float dyaw   = 0.0f;
    float dpitch = 0.0f;

    if (cursor_captured_) {
        Vector2 md = GetMouseDelta();
        dyaw   += md.x * look_sensitivity_;
        dpitch -= md.y * look_sensitivity_;   // screen Y inverted from world pitch
    }

    if (IsGamepadAvailable(0)) {
        float rx = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_Y);
        // Apply a small dead-zone.
        if (fabsf(rx) > 0.1f) dyaw   += rx * stick_look_speed_ * dt;
        if (fabsf(ry) > 0.1f) dpitch -= ry * stick_look_speed_ * dt;
    }

    yaw_   += dyaw;
    pitch_ += dpitch;
    // Clamp pitch to ±85°.
    const float MAX_PITCH = 85.0f * DEG2RAD_F;
    if (pitch_ >  MAX_PITCH) pitch_ =  MAX_PITCH;
    if (pitch_ < -MAX_PITCH) pitch_ = -MAX_PITCH;

    // --- Compute move direction from yaw (ignore pitch for movement) ---
    Vector3 forward = {  sinf(yaw_), 0.0f,  cosf(yaw_) };
    Vector3 right   = {  cosf(yaw_), 0.0f, -sinf(yaw_) };
    Vector3 up_vec  = {  0.0f,       1.0f,  0.0f        };

    float move_x = 0.0f, move_y = 0.0f, move_z = 0.0f;

    // Keyboard (WASD + QE for vertical).
    if (IsKeyDown(KEY_W)) { move_x += forward.x; move_z += forward.z; }
    if (IsKeyDown(KEY_S)) { move_x -= forward.x; move_z -= forward.z; }
    if (IsKeyDown(KEY_D)) { move_x += right.x;   move_z += right.z;   }
    if (IsKeyDown(KEY_A)) { move_x -= right.x;   move_z -= right.z;   }
    if (IsKeyDown(KEY_E)) { move_y += 1.0f; }
    if (IsKeyDown(KEY_Q)) { move_y -= 1.0f; }

    // Gamepad left stick (XZ movement).
    if (IsGamepadAvailable(0)) {
        float lx = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        // Dead-zone.
        if (fabsf(lx) > 0.1f) { move_x += right.x * lx;   move_z += right.z * lx; }
        if (fabsf(ly) > 0.1f) { move_x += forward.x * (-ly); move_z += forward.z * (-ly); }

        // Triggers for vertical.
        float lt = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_TRIGGER);
        float rt = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_TRIGGER);
        // Triggers report [-1..1]; normalize to [0..1].
        float lt01 = (lt + 1.0f) * 0.5f;
        float rt01 = (rt + 1.0f) * 0.5f;
        move_y += rt01 - lt01;
    }

    // Speed multiplier: Shift key or left-trigger partial hold.
    float speed = move_speed_;
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) speed *= 4.0f;

    // Apply movement.
    cam.position.x += move_x * speed * dt;
    cam.position.y += move_y * speed * dt;
    cam.position.z += move_z * speed * dt;

    // Update target from position + look direction.
    Vector3 dir = direction_from_yaw_pitch(yaw_, pitch_);
    cam.target = {
        cam.position.x + dir.x,
        cam.position.y + dir.y,
        cam.position.z + dir.z
    };
}

bool CameraRig::user_has_control() const {
    return cursor_captured_;
}

void CameraRig::play_staged(int /*shot*/) {
    // Restore to spawn position and face the valley.
    yaw_   = SPAWN_YAW;
    pitch_ = SPAWN_PITCH;
    cam.position = { SPAWN_X, SPAWN_Y, SPAWN_Z };
    Vector3 dir = direction_from_yaw_pitch(yaw_, pitch_);
    cam.target = {
        cam.position.x + dir.x,
        cam.position.y + dir.y,
        cam.position.z + dir.z
    };
}
