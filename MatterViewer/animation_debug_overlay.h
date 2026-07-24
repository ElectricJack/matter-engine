#pragma once

// Viewer-only, observational animation diagnostics.  This deliberately has
// no WorldSession, renderer, ECS, or editor-event dependency: the caller
// hands it a committed immutable .anim asset and a published pose snapshot for
// the duration of one UI frame.

#include "matter/animation.h"
#include "matter/camera.h"

#include <cstdint>
#include <string>
#include <vector>

namespace matter::animation {
struct AnimAsset;
struct AnimationPoseSnapshot;
}

namespace viewer {

struct AnimationDebugJoint {
    uint16_t parent = UINT16_MAX;
    float radius = 0.0f;
};

struct AnimationDebugSocket {
    uint16_t joint = UINT16_MAX;
    matter::AnimationTransform local{};
};

struct AnimationDebugTarget {
    std::vector<uint16_t> chain;
    matter::Float3 pole{};
    bool has_pole = false;
};

struct AnimationDebugJointBound {
    uint16_t joint = UINT16_MAX;
    matter::Float3 minimum{};
    matter::Float3 maximum{};
};

// Parsed once from the committed .anim RigSchema/GeometryBindings sections.
// It is intentionally value-owned so no part-store or script-host pointer can
// be retained by the debug draw path.
struct AnimationDebugAsset {
    uint64_t resolved_hash = 0;
    uint64_t nonce_high = 0;
    uint64_t nonce_low = 0;
    std::vector<AnimationDebugJoint> joints;
    std::vector<AnimationDebugSocket> sockets;
    std::vector<AnimationDebugTarget> targets;
    std::vector<AnimationDebugJointBound> joint_bounds;
    uint32_t lod0_influence_count = 0;
};

// Loads only data that is already committed in an AnimAsset.  Invalid or
// incomplete payloads fail closed and leave `out` empty.
bool make_animation_debug_asset(const matter::animation::AnimAsset& committed,
                                AnimationDebugAsset& out);

struct AnimationDebugOverlayOptions {
    bool enabled = false;
    bool bones = true;
    bool joint_axes = true;
    bool radius_envelopes = true;
    bool sockets = true;
    bool targets_and_ik = true;
    bool conservative_bounds = true;
    bool skin_weights = false;
};

// Projects transient overlay primitives into the existing ImGui foreground
// list.  It performs no cache, renderer, culling, checkpoint, or event-hub
// operation. `asset` and `pose` must both describe the same committed bundle.
void draw_animation_debug_overlay(const AnimationDebugAsset& asset,
                                  const matter::animation::AnimationPoseSnapshot& pose,
                                  const matter::CameraDesc& camera,
                                  int framebuffer_width, int framebuffer_height,
                                  float viewport_x, float viewport_y,
                                  const AnimationDebugOverlayOptions& options);

// Keeps the viewer's controls local to the viewer.  It never writes animation
// inputs, targets, or simulation state.
void draw_animation_debug_overlay_controls(AnimationDebugOverlayOptions& options);

} // namespace viewer
