#include "camera_rig.h"
#include "raylib.h"
#include "raymath.h"

#include <cmath>

static constexpr float SPAWN_X =    0.0f;
static constexpr float SPAWN_Y =   25.0f;
static constexpr float SPAWN_Z =    0.0f;

static constexpr float SPAWN_YAW   = 3.14159265f;
static constexpr float SPAWN_PITCH = -0.15f;

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
        user_input_seen_ = true;
        if (cursor_captured_) DisableCursor();
        else                  EnableCursor();
    }

    // --- Accumulate look delta (mouse + right gamepad stick) ---
    float dyaw   = 0.0f;
    float dpitch = 0.0f;

    if (cursor_captured_) {
        Vector2 md = GetMouseDelta();
        if (fabsf(md.x) > 0.5f || fabsf(md.y) > 0.5f) {
            dyaw   += md.x * look_sensitivity_;
            dpitch -= md.y * look_sensitivity_;   // screen Y inverted from world pitch
            user_input_seen_ = true;
        }
    }

    if (IsGamepadAvailable(0)) {
        float rx = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_Y);
        // Apply a small dead-zone.
        if (fabsf(rx) > 0.1f) { dyaw   += rx * stick_look_speed_ * dt; user_input_seen_ = true; }
        if (fabsf(ry) > 0.1f) { dpitch -= ry * stick_look_speed_ * dt; user_input_seen_ = true; }
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

    float move_x = 0.0f, move_y = 0.0f, move_z = 0.0f;

    // Keyboard (WASD + QE for vertical).
    if (IsKeyDown(KEY_W)) { move_x += forward.x; move_z += forward.z; user_input_seen_ = true; }
    if (IsKeyDown(KEY_S)) { move_x -= forward.x; move_z -= forward.z; user_input_seen_ = true; }
    if (IsKeyDown(KEY_D)) { move_x += right.x;   move_z += right.z;   user_input_seen_ = true; }
    if (IsKeyDown(KEY_A)) { move_x -= right.x;   move_z -= right.z;   user_input_seen_ = true; }
    if (IsKeyDown(KEY_E)) { move_y += 1.0f; user_input_seen_ = true; }
    if (IsKeyDown(KEY_Q)) { move_y -= 1.0f; user_input_seen_ = true; }

    // Gamepad left stick (XZ movement).
    if (IsGamepadAvailable(0)) {
        float lx = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        // Dead-zone.
        if (fabsf(lx) > 0.1f) { move_x += right.x * lx;   move_z += right.z * lx; user_input_seen_ = true; }
        if (fabsf(ly) > 0.1f) { move_x += forward.x * (-ly); move_z += forward.z * (-ly); user_input_seen_ = true; }

        // Triggers for vertical.
        float lt = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_TRIGGER);
        float rt = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_TRIGGER);
        // Triggers report [-1..1]; normalize to [0..1].
        float lt01 = (lt + 1.0f) * 0.5f;
        float rt01 = (rt + 1.0f) * 0.5f;
        if (lt01 > 0.05f || rt01 > 0.05f) { move_y += rt01 - lt01; user_input_seen_ = true; }
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

bool CameraRig::has_user_input() const {
    return user_input_seen_;
}

void CameraRig::set_staged_pose(float px, float py, float pz, float yaw, float pitch) {
    cam.position = { px, py, pz };
    yaw_   = yaw;
    pitch_ = pitch;
    Vector3 dir = direction_from_yaw_pitch(yaw_, pitch_);
    cam.target = {
        cam.position.x + dir.x,
        cam.position.y + dir.y,
        cam.position.z + dir.z
    };
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
