// staged_camera.cpp — cinematic camera sequence during the initial bake (Task 9).
//
// Three stages:
//   Shot 1 ( 0–40s):  slow orbit around the valley centre at ~30m altitude.
//   Shot 2 (40–70s):  pull-back + rise to ~200m altitude revealing the mountain range.
//   Loop  (>70s):     gentle slow drift, looping back to orbit.

#include "staged_camera.h"
#include "camera_rig.h"
#include "raymath.h"

#include <cmath>

// Meadow Valley centre (same constants as camera_rig.cpp).
static constexpr float CX = 408.0f;
static constexpr float CZ = 408.0f;

static constexpr float PI_F = 3.14159265358979f;

// ---------------------------------------------------------------------------
// update() — advance staged sequence; return true while active.
// ---------------------------------------------------------------------------
bool StagedCamera::update(CameraRig& rig, float dt, bool bake_finished) {
    // Latch bake-done signal.
    if (bake_finished) bake_done_ = true;

    // Check for user input from the rig.
    if (rig.has_user_input()) user_taken_ = true;

    if (user_taken_ || bake_done_) {
        return false;  // hand control back to main loop
    }

    elapsed_ += dt;
    float t = elapsed_;

    if (t < 40.0f) {
        apply_shot1(rig, t);
    } else if (t < 70.0f) {
        apply_shot2(rig, t - 40.0f);
    } else {
        // Loop: map elapsed back into a 40s orbit cycle.
        float loop_t = fmodf(t - 70.0f, 40.0f);
        apply_drift(rig, loop_t);
    }

    return true;   // still in staged mode
}

// ---------------------------------------------------------------------------
// Shot 1: slow low-altitude orbit around the valley centre.
// Radius 120m, altitude 30m, full revolution over 40s.
// Camera always looks at the valley centre (CX, 0, CZ) with a slight upward pitch.
// ---------------------------------------------------------------------------
void StagedCamera::apply_shot1(CameraRig& rig, float t) {
    float angle  = (t / 40.0f) * 2.0f * PI_F;   // 0..2π over 40s
    float radius = 120.0f;
    float alt    = 30.0f;

    float px = CX + radius * cosf(angle);
    float pz = CZ + radius * sinf(angle);
    float py = alt;

    // Look at the centre of the valley slightly above ground.
    float tx = CX;
    float ty = 5.0f;
    float tz = CZ;

    // Derive yaw/pitch from direction vector so the rig's internal state stays correct.
    float dx = tx - px;
    float dy = ty - py;
    float dz = tz - pz;
    float horiz = sqrtf(dx*dx + dz*dz);

    float yaw   =  atan2f(dx, dz);   // +Z is forward; camera_rig.cpp uses sinf(yaw)=dx, cosf(yaw)=dz
    float pitch =  atan2f(dy, horiz);

    rig.set_staged_pose(px, py, pz, yaw, pitch);
}

// ---------------------------------------------------------------------------
// Shot 2: pull-back + rise. Over 30 seconds the camera retreats from the near
// orbit to a high vantage point (radius 350m, altitude 200m).
// ---------------------------------------------------------------------------
void StagedCamera::apply_shot2(CameraRig& rig, float t) {
    float frac = t / 30.0f;  // 0..1 over shot 2
    // Smooth ease-in/out.
    float ease = frac * frac * (3.0f - 2.0f * frac);

    float radius = 120.0f + ease * 230.0f;   // 120→350m
    float alt    = 30.0f  + ease * 170.0f;   // 30→200m

    // Continue orbiting slowly during pull-back (quarter revolution).
    float angle = (PI_F * 0.5f) * ease;      // advance 90° over shot 2

    float px = CX + radius * cosf(angle);
    float pz = CZ + radius * sinf(angle);
    float py = alt;

    float tx = CX;
    float ty = 0.0f;
    float tz = CZ;

    float dx = tx - px;
    float dy = ty - py;
    float dz = tz - pz;
    float horiz = sqrtf(dx*dx + dz*dz);

    float yaw   = atan2f(dx, dz);
    float pitch = atan2f(dy, horiz);

    rig.set_staged_pose(px, py, pz, yaw, pitch);
}

// ---------------------------------------------------------------------------
// Loop drift: gentle orbit at the high vantage point (radius 350m, alt 200m).
// Full revolution over 40s, repeating.
// ---------------------------------------------------------------------------
void StagedCamera::apply_drift(CameraRig& rig, float t) {
    float angle  = (t / 40.0f) * 2.0f * PI_F + (PI_F * 0.5f);
    float radius = 350.0f;
    float alt    = 200.0f;

    float px = CX + radius * cosf(angle);
    float pz = CZ + radius * sinf(angle);
    float py = alt;

    float tx = CX;
    float ty = 0.0f;
    float tz = CZ;

    float dx = tx - px;
    float dy = ty - py;
    float dz = tz - pz;
    float horiz = sqrtf(dx*dx + dz*dz);

    float yaw   = atan2f(dx, dz);
    float pitch = atan2f(dy, horiz);

    rig.set_staged_pose(px, py, pz, yaw, pitch);
}
