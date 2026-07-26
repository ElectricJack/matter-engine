#pragma once

// Viewer-only rendering of value-owned, observational animation diagnostics.
// The engine copies these snapshots at an explicit WorldSession boundary; this
// layer owns no evaluator, ECS, cache, or renderer state.

#include "matter/animation_debug.h"
#include "matter/camera.h"

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
};

void draw_animation_debug_overlay(
    const matter::AnimationDebugInstanceSnapshot& snapshot,
    const matter::CameraDesc& camera,
    int framebuffer_width, int framebuffer_height,
    float viewport_x, float viewport_y,
    const AnimationDebugOverlayOptions& options);

void draw_animation_debug_overlay_controls(
    AnimationDebugOverlayOptions& options);

} // namespace viewer
