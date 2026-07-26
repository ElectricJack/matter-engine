#pragma once

#include "matter/animation.h"

#include <cstdint>
#include <vector>

namespace matter {

// Public, value-owned diagnostics only. These snapshots deliberately expose
// no Ozz, Flecs, QuickJS, renderer, or cache-provider types and can therefore
// cross the engine/viewer boundary without extending any internal lifetime.
struct AnimationDebugJoint {
    uint16_t parent = UINT16_MAX;
    float radius = 0.0f;
};

struct AnimationDebugSocket {
    uint16_t joint = UINT16_MAX;
    AnimationTransform local{};
};

struct AnimationDebugTargetDefinition {
    std::vector<uint16_t> chain;
    Float3 pole{};
    bool has_pole = false;
};

struct AnimationDebugTargetState {
    AnimationTransform evaluated{};
    float weight = 0.0f;
    bool enabled = false;
    bool available = false;
};

struct AnimationDebugJointBound {
    uint16_t joint = UINT16_MAX;
    Float3 minimum{};
    Float3 maximum{};
};

struct AnimationDebugVertexInfluence {
    Float3 bind_position{};
    uint16_t joints[4] = {UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX};
    uint16_t weights[4] = {};
};

struct AnimationDebugAssetSnapshot {
    uint64_t resolved_hash = 0;
    uint64_t nonce_high = 0;
    uint64_t nonce_low = 0;
    std::vector<AnimationDebugJoint> joints;
    std::vector<AnimationDebugSocket> sockets;
    std::vector<AnimationDebugTargetDefinition> targets;
    std::vector<AnimationDebugJointBound> joint_bounds;
    std::vector<AnimationDebugVertexInfluence> lod0_influences;
};

struct AnimationDebugPoseSnapshot {
    AnimatorInstanceHandle instance{};
    uint64_t fixed_tick = 0;
    uint64_t frame_serial = 0;
    std::vector<AnimationTransform> local_pose;
    std::vector<Mat4f> model_pose;
    std::vector<Mat4f> skin_palette;
    std::vector<AnimationDebugTargetState> targets;
};

struct AnimationDebugInstanceSnapshot {
    AnimationDebugAssetSnapshot asset;
    AnimationDebugPoseSnapshot pose;
};

} // namespace matter
