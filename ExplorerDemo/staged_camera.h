#pragma once

// StagedCamera — automatic cinematic camera sequence played during the initial bake.
//
// The staged sequence runs until BakeFinished fires OR the user takes control
// (CameraRig::has_user_input() returns true). Once either condition is met,
// user_has_control() returns true permanently.
//
// Sequence (total ~70s):
//   Shot 1 (0..40s):  slow orbit around spawn point at low altitude — good for
//                     watching terrain assemble.
//   Shot 2 (40..70s): pull-back + rise to reveal the mountain range.
//   Loop (>70s):      gentle drift, repeating indefinitely.
//
// The class moves the CameraRig's cam.position / cam.target directly; yaw/pitch
// updates go through CameraRig::set_staged_pose() to keep the rig's internal
// state consistent.

struct CameraRig;

class StagedCamera {
public:
    StagedCamera() = default;

    // Call each frame. Returns true while the staged camera is still active
    // (i.e. !user_has_control()). When it returns false the rig should handle
    // input normally.
    bool update(CameraRig& rig, float dt, bool bake_finished);

    // True once user input was detected or BakeFinished fired.
    bool user_has_control() const { return user_taken_; }

    // Signal that the user has taken control (called by main loop on any input event).
    void notify_user_input() { user_taken_ = true; }

    // Signal BakeFinished (releases the staged camera).
    void notify_bake_finished() { bake_done_ = true; }

    // Re-arm the staged sequence from the beginning (called on New-seed regenerate).
    // Resets elapsed time and clears the bake-done / user-taken latches so the
    // cinematic sequence runs again while the new world bakes.
    void reset() {
        elapsed_    = 0.0f;
        user_taken_ = false;
        bake_done_  = false;
    }

private:
    float  elapsed_    = 0.0f;   // cumulative time in staged mode (seconds)
    bool   user_taken_ = false;
    bool   bake_done_  = false;

    void apply_shot1(CameraRig& rig, float t);
    void apply_shot2(CameraRig& rig, float t);
    void apply_drift(CameraRig& rig, float t);
};
