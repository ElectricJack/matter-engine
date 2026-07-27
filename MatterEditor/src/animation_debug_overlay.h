#pragma once

// Viewer-only rendering of value-owned, observational animation diagnostics.
// The engine copies these snapshots at an explicit WorldSession boundary; this
// layer owns no evaluator, ECS, cache, or renderer state.

#include "matter/animation_debug.h"
#include "matter/camera.h"

#include <string>
#include <vector>

namespace viewer {

struct AnimationDebugOverlayOptions {
    bool enabled = false;
    bool bones = true;
    bool joint_axes = true;
    bool radius_envelopes = true;
    bool sockets = true;
    bool targets_and_ik = true;
    bool conservative_bounds = true;
    bool skin_weights = false;
    int weight_joint = 0;
    // Colour each sampled vertex by its HIGHEST-weight joint instead of by one
    // joint's weight. Shows the whole weight partition at once, so a vertex
    // bound to the wrong limb stands out as an off-colour speck in a solid
    // region rather than having to be hunted joint by joint.
    bool dominant_joint = false;
    // Draw every sampled vertex at the position the CPU computes from the same
    // immutable pose the GPU was handed. Any divergence between these points
    // and the rendered surface is a fault in the GPU skinning path, not in the
    // weights or the pose -- which is otherwise very hard to tell apart.
    bool cpu_reference = false;
};

void draw_animation_debug_overlay(
    const matter::AnimationDebugInstanceSnapshot& snapshot,
    const matter::CameraDesc& camera,
    int framebuffer_width, int framebuffer_height,
    float viewport_x, float viewport_y,
    const AnimationDebugOverlayOptions& options);

// `joint_names` (optional, parallel to the rig's joints) turns the weight-joint
// picker into a named selector. Passing null falls back to a numeric input.
void draw_animation_debug_overlay_controls(
    AnimationDebugOverlayOptions& options,
    const std::vector<std::string>* joint_names = nullptr);

} // namespace viewer
