#pragma once

#include "matter/animation.h"

#include <cmath>
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
    // ECS owner transform for model-space skeleton, skin, target, and pole
    // visualization.
    Mat4f world_transform{{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f}};
};

inline bool animation_debug_finite(const Float3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

inline bool animation_debug_finite(const AnimationTransform& value) {
    return animation_debug_finite(value.translation) &&
           std::isfinite(value.rotation.x) &&
           std::isfinite(value.rotation.y) &&
           std::isfinite(value.rotation.z) &&
           std::isfinite(value.rotation.w) &&
           animation_debug_finite(value.scale);
}

inline bool animation_debug_finite(const Mat4f& value) {
    for (float element : value.m)
        if (!std::isfinite(element)) return false;
    return true;
}

// Strict draw-boundary validation. Diagnostics are disposable, so one invalid
// index/non-finite value rejects the value-owned snapshot instead of allowing
// a partially trusted primitive to index another array.
inline bool valid_animation_debug_snapshot(
    const AnimationDebugInstanceSnapshot& snapshot) {
    const auto& asset = snapshot.asset;
    const auto& pose = snapshot.pose;
    if (!pose.instance.valid() || asset.resolved_hash == 0 ||
        asset.joints.empty() ||
        pose.model_pose.size() != asset.joints.size() ||
        (!asset.lod0_influences.empty() &&
         pose.skin_palette.size() != asset.joints.size()) ||
        pose.targets.size() != asset.targets.size() ||
        !animation_debug_finite(snapshot.world_transform)) return false;
    for (size_t i = 0; i < asset.joints.size(); ++i) {
        const AnimationDebugJoint& joint = asset.joints[i];
        if ((joint.parent != UINT16_MAX && joint.parent >= i) ||
            !std::isfinite(joint.radius) || joint.radius <= 0.0f ||
            !animation_debug_finite(pose.model_pose[i])) return false;
    }
    for (const Mat4f& matrix : pose.skin_palette)
        if (!animation_debug_finite(matrix)) return false;
    for (const AnimationDebugSocket& socket : asset.sockets)
        if (socket.joint >= asset.joints.size() ||
            !animation_debug_finite(socket.local)) return false;
    for (const AnimationDebugTargetDefinition& target : asset.targets) {
        if (target.chain.size() != 3 ||
            !animation_debug_finite(target.pole)) return false;
        for (uint16_t joint : target.chain)
            if (joint >= asset.joints.size()) return false;
    }
    for (const AnimationDebugTargetState& target : pose.targets)
        if (!animation_debug_finite(target.evaluated) ||
            !std::isfinite(target.weight) || target.weight < 0.0f ||
            target.weight > 1.0f) return false;
    for (const AnimationDebugJointBound& bound : asset.joint_bounds)
        if (bound.joint >= asset.joints.size() ||
            !animation_debug_finite(bound.minimum) ||
            !animation_debug_finite(bound.maximum) ||
            bound.minimum.x > bound.maximum.x ||
            bound.minimum.y > bound.maximum.y ||
            bound.minimum.z > bound.maximum.z) return false;
    for (const AnimationDebugVertexInfluence& vertex :
         asset.lod0_influences) {
        if (!animation_debug_finite(vertex.bind_position)) return false;
        uint32_t sum = 0;
        uint32_t count = 0;
        for (size_t lane = 0; lane < 4; ++lane) {
            if (!vertex.weights[lane]) continue;
            if (vertex.joints[lane] >= asset.joints.size()) return false;
            sum += vertex.weights[lane];
            ++count;
        }
        if (count == 0 || count > 4 || sum != 65535u) return false;
    }
    return true;
}

// Canonical poles are model-space directions. Match the solver/debug contract
// by rotating through the solved chain root, then the ECS owner; translation
// is deliberately ignored. The normalized result is world-space.
inline bool animation_debug_world_pole_direction(
    const Mat4f& owner_world, const Mat4f& chain_root_model,
    const Float3& authored_pole, Float3& out) {
    out = {};
    if (!animation_debug_finite(owner_world) ||
        !animation_debug_finite(chain_root_model) ||
        !animation_debug_finite(authored_pole)) return false;
    const Float3 model{
        chain_root_model.m[0] * authored_pole.x +
            chain_root_model.m[1] * authored_pole.y +
            chain_root_model.m[2] * authored_pole.z,
        chain_root_model.m[4] * authored_pole.x +
            chain_root_model.m[5] * authored_pole.y +
            chain_root_model.m[6] * authored_pole.z,
        chain_root_model.m[8] * authored_pole.x +
            chain_root_model.m[9] * authored_pole.y +
            chain_root_model.m[10] * authored_pole.z};
    out = {
        owner_world.m[0] * model.x + owner_world.m[1] * model.y +
            owner_world.m[2] * model.z,
        owner_world.m[4] * model.x + owner_world.m[5] * model.y +
            owner_world.m[6] * model.z,
        owner_world.m[8] * model.x + owner_world.m[9] * model.y +
            owner_world.m[10] * model.z};
    const float length =
        std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z);
    if (!std::isfinite(length) || length <= 1e-6f) {
        out = {};
        return false;
    }
    out.x /= length;
    out.y /= length;
    out.z /= length;
    return true;
}

} // namespace matter
