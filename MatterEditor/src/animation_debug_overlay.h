#pragma once

// MatterEditor/src/animation_debug_overlay.h
//
// Viewer-only rendering of value-owned, observational animation diagnostics.
// The engine copies these snapshots at an explicit WorldSession boundary; this
// layer owns no evaluator, ECS, cache, or renderer state.
//
// Two entry points, with no owning object between them:
//
//   - `draw_animation_debug_overlay` rasterizes one animator instance's rig
//     into the viewport. It projects world-space points itself and draws into
//     ImGui's FOREGROUND draw list, i.e. 2D screen space over the composited
//     frame with no depth test -- bones behind geometry are drawn on top of it.
//     That is deliberate (an occluded joint is usually the one you want to see)
//     but it means the overlay conveys no depth ordering at all.
//   - `draw_animation_debug_overlay_controls` draws the toggles that mutate the
//     options struct. The Render tab of the Part Workbench animation panel
//     (animation_panel.cpp) calls it, so panel and viewport share one options
//     instance instead of two that can drift.
//
// The caller owns the `AnimationDebugOverlayOptions` and keeps it alive across
// frames; MatterEditor stores it on `ViewerStats::animation_overlay` and passes
// the same object to both functions (MatterEditor/src/main.cpp, the
// `stats.animation_overlay.enabled` block).
//
// Units and spaces: snapshot positions and joint radii are world space in the
// engine's world units; `framebuffer_width`/`framebuffer_height` are the 3D
// viewport's size in pixels and `viewport_x`/`viewport_y` its top-left offset
// inside the window.
//
// Both functions are ImGui calls: render thread only, and valid only between
// ImGui::NewFrame and ImGui::Render.

#include "matter/animation_debug.h"
#include "matter/camera.h"

#include <string>
#include <vector>

namespace viewer {

// One toggle per visualization layer, plus the two joint-picker fields. Held by
// the caller across frames and shared by the viewport draw and the panel's
// control block, so a checkbox in the panel cannot disagree with what the
// viewport renders.
//
// `enabled` is the master gate: draw_animation_debug_overlay returns
// immediately when it is false, and the control block greys out every other
// toggle. The three point-cloud layers (skin_weights, dominant_joint,
// cpu_reference) each subsample the LOD0 influences to a fixed budget, so their
// cost does not scale with mesh density -- see the stride note in the .cpp.
struct AnimationDebugOverlayOptions {
    bool enabled = false;
    bool bones = true;
    bool joint_axes = true;
    bool radius_envelopes = true;
    bool sockets = true;
    bool targets_and_ik = true;
    bool conservative_bounds = true;
    // Colour each sampled vertex by ONE joint's weight (red = 1, blue = 0).
    // Vertices with zero weight on that joint are not drawn at all.
    bool skin_weights = false;
    // Index into the rig's joints, selecting which joint `skin_weights` colours
    // by. Clamped against the joint count at the point of use, so a stale index
    // left over from a different rig cannot read out of range.
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

// Draws one animator instance's diagnostics over the viewport. A no-op when
// `options.enabled` is false, when the snapshot fails
// matter::valid_animation_debug_snapshot, or when the framebuffer size is
// non-positive. A malformed snapshot is dropped rather than partially drawn,
// because several indices in it (IK chain joints, socket joints, joint bounds)
// are used unchecked afterwards.
//
// Nothing is cached between calls: the view-projection is rebuilt and the joint
// world matrices are re-derived from `snapshot` every frame.
void draw_animation_debug_overlay(
    const matter::AnimationDebugInstanceSnapshot& snapshot,
    const matter::CameraDesc& camera,
    int framebuffer_width, int framebuffer_height,
    float viewport_x, float viewport_y,
    const AnimationDebugOverlayOptions& options);

// Draws the overlay's toggles as a collapsing header and writes straight back
// into `options`. Nothing reaches the viewport from here -- this is only the
// control block, which is what lets the panel and the viewport share one
// options instance.
//
// `joint_names` (optional, parallel to the rig's joints) turns the weight-joint
// picker into a named selector. Passing null falls back to a numeric input.
void draw_animation_debug_overlay_controls(
    AnimationDebugOverlayOptions& options,
    const std::vector<std::string>* joint_names = nullptr);

} // namespace viewer
