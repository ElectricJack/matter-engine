#include "animation/ozz_adapter_internal.h"

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/animation_optimizer.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"

#include <functional>
#include <cmath>
#include <unordered_map>

namespace matter::animation {
namespace {

Quaternion normalize_rotation(Quaternion value) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y +
                                   value.z * value.z + value.w * value.w);
    return length > 1e-8f ? Quaternion{value.x / length, value.y / length,
                                        value.z / length, value.w / length}
                           : Quaternion{};
}

Quaternion multiply_rotation(Quaternion left, Quaternion right) {
    return {left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
            left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
            left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
            left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z};
}

// Ozz's additive layer consumes a transform delta, not an authored absolute
// local pose.  Keep the ANIM schema ergonomic (clips still author normal
// local transforms) and make the conversion once at bake time against the
// rig's exact bind/reference pose.
AnimationTransform bind_relative_delta(const AnimationTransform& reference,
                                       const AnimationTransform& absolute) {
    AnimationTransform delta{};
    delta.translation = {absolute.translation.x - reference.translation.x,
                         absolute.translation.y - reference.translation.y,
                         absolute.translation.z - reference.translation.z};
    const Quaternion inverse_reference{-reference.rotation.x, -reference.rotation.y,
                                       -reference.rotation.z, reference.rotation.w};
    delta.rotation = normalize_rotation(multiply_rotation(inverse_reference, absolute.rotation));
    delta.scale = {reference.scale.x != 0.0f ? absolute.scale.x / reference.scale.x : 1.0f,
                   reference.scale.y != 0.0f ? absolute.scale.y / reference.scale.y : 1.0f,
                   reference.scale.z != 0.0f ? absolute.scale.z / reference.scale.z : 1.0f};
    return delta;
}

std::vector<std::size_t> canonical_order(const RigDefinition& rig, Diagnostics& diagnostics) {
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i=0; i<rig.joints.size(); ++i) index.emplace(rig.joints[i].name, i);
    std::vector<std::vector<std::size_t>> children(rig.joints.size());
    std::size_t root = rig.joints.size();
    for (std::size_t i=0; i<rig.joints.size(); ++i) {
        if (rig.joints[i].parent.empty()) { if (root == rig.joints.size()) root = i; else { diagnostics.add("ozz-skeleton", rig.joints[i].source, "rig has multiple roots"); return {}; } }
        else { const auto parent=index.find(rig.joints[i].parent); if (parent == index.end()) { diagnostics.add("ozz-skeleton", rig.joints[i].source, "rig parent is missing"); return {}; } children[parent->second].push_back(i); }
    }
    if (root == rig.joints.size()) { diagnostics.add("ozz-skeleton", rig.source, "rig has no root"); return {}; }
    std::vector<std::size_t> result;
    std::function<void(std::size_t)> visit = [&](std::size_t joint) { result.push_back(joint); for (std::size_t child : children[joint]) visit(child); };
    visit(root);
    if (result.size() != rig.joints.size()) { diagnostics.add("ozz-skeleton", rig.source, "rig hierarchy is cyclic"); return {}; }
    return result;
}

void build_raw_children(const RigDefinition& rig, const std::vector<std::vector<std::size_t>>& children, std::size_t index, ozz::animation::offline::RawSkeleton::Joint& raw) {
    const JointDef& source = rig.joints[index];
    raw.name = source.name.c_str();
    raw.transform.translation = {source.local.translation.x, source.local.translation.y, source.local.translation.z};
    raw.transform.rotation = {source.local.rotation.x, source.local.rotation.y, source.local.rotation.z, source.local.rotation.w};
    raw.transform.scale = {source.local.scale.x, source.local.scale.y, source.local.scale.z};
    for (std::size_t child : children[index]) { raw.children.emplace_back(); build_raw_children(rig, children, child, raw.children.back()); }
}

bool make_runtime_skeleton(const RigDefinition& rig, ozz::unique_ptr<ozz::animation::Skeleton>& runtime, std::vector<JointIndex>* parents, std::vector<JointRange>* subtrees, Diagnostics& diagnostics) {
    const std::vector<std::size_t> order = canonical_order(rig, diagnostics);
    if (order.empty()) return false;
    if (order.size() > kMaxJoints) { diagnostics.add("ozz-skeleton", rig.source, "rig exceeds the Matter joint limit"); return false; }
    std::unordered_map<std::string, std::size_t> authored;
    std::vector<std::vector<std::size_t>> children(rig.joints.size());
    std::size_t root = 0;
    for (std::size_t i=0; i<rig.joints.size(); ++i) authored.emplace(rig.joints[i].name, i);
    for (std::size_t i=0; i<rig.joints.size(); ++i) if (rig.joints[i].parent.empty()) root=i; else children[authored[rig.joints[i].parent]].push_back(i);
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.emplace_back();
    build_raw_children(rig, children, root, raw.roots.back());
    if (!raw.Validate()) { diagnostics.add("ozz-skeleton", rig.source, "ozz rejected raw skeleton"); return false; }
    ozz::animation::offline::SkeletonBuilder builder;
    runtime = builder(raw);
    if (!runtime) { diagnostics.add("ozz-skeleton", rig.source, "ozz skeleton builder failed"); return false; }
    if (parents && subtrees) {
        parents->assign(order.size(), kInvalidJoint); subtrees->assign(order.size(), {});
        std::unordered_map<std::size_t, JointIndex> canonical;
        for (JointIndex i=0; i<order.size(); ++i) canonical[order[i]]=i;
        for (JointIndex i=0; i<order.size(); ++i) {
            const JointDef& joint=rig.joints[order[i]];
            (*parents)[i] = joint.parent.empty() ? kInvalidJoint : canonical[authored[joint.parent]];
            (*subtrees)[i].begin=i; (*subtrees)[i].end=i+1;
        }
        for (int i=static_cast<int>(order.size())-1; i>=0; --i) if ((*parents)[i] != kInvalidJoint) (*subtrees)[(*parents)[i]].end=std::max((*subtrees)[(*parents)[i]].end, (*subtrees)[i].end);
    }
    return true;
}

} // namespace

bool build_skeleton(const RigDefinition& rig, OzzSkeleton& skeleton, Diagnostics& diagnostics) {
    ozz::unique_ptr<ozz::animation::Skeleton> runtime; std::vector<JointIndex> parents; std::vector<JointRange> subtrees;
    if (!make_runtime_skeleton(rig, runtime, &parents, &subtrees, diagnostics)) return false;
    const std::vector<std::size_t> order = canonical_order(rig, diagnostics);
    if (order.empty()) return false;
    skeleton.impl_->rest_locals.clear();
    for (std::size_t joint : order) skeleton.impl_->rest_locals.push_back(rig.joints[joint].local);
    skeleton.impl_->runtime=std::move(runtime); skeleton.impl_->parents=std::move(parents); skeleton.impl_->subtrees=std::move(subtrees); return true;
}

bool build_clip(const RigDefinition& rig, const ClipDefinition& clip, OzzAnimation& animation, Diagnostics& diagnostics) {
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    if (!make_runtime_skeleton(rig, skeleton, nullptr, nullptr, diagnostics)) return false;
    const std::vector<std::size_t> order=canonical_order(rig, diagnostics); if (order.empty()) return false;
    std::unordered_map<std::string, const ClipTrack*> tracks; for (const ClipTrack& track:clip.tracks) tracks[track.joint]=&track;
    ozz::animation::offline::RawAnimation raw; raw.name=clip.name.c_str(); raw.duration=clip.duration; raw.tracks.resize(order.size());
    for (std::size_t i=0; i<order.size(); ++i) {
        const JointDef& bind=rig.joints[order[i]]; const auto found=tracks.find(bind.name); const std::vector<ClipKey>* keys=found==tracks.end()?nullptr:&found->second->keys;
        const std::size_t key_count=keys && !keys->empty()?keys->size():1;
        for (std::size_t k=0;k<key_count;++k) { const ClipKey key=keys&& !keys->empty()?(*keys)[k]:ClipKey{0.0f,bind.local,bind.source};
            const AnimationTransform value = clip.additive ? bind_relative_delta(bind.local, key.value) : key.value;
            raw.tracks[i].translations.push_back({key.time,{value.translation.x,value.translation.y,value.translation.z}});
            raw.tracks[i].rotations.push_back({key.time,{value.rotation.x,value.rotation.y,value.rotation.z,value.rotation.w}});
            raw.tracks[i].scales.push_back({key.time,{value.scale.x,value.scale.y,value.scale.z}}); }
    }
    if (!raw.Validate()) { diagnostics.add("ozz-animation", clip.source, "ozz rejected raw animation"); return false; }
    ozz::animation::offline::RawAnimation optimized;
    if (!ozz::animation::offline::AnimationOptimizer{}(raw, *skeleton, &optimized)) { diagnostics.add("ozz-animation", clip.source, "ozz animation optimizer failed"); return false; }
    ozz::animation::offline::AnimationBuilder builder; ozz::unique_ptr<ozz::animation::Animation> runtime=builder(optimized);
    if (!runtime) { diagnostics.add("ozz-animation", clip.source, "ozz animation builder failed"); return false; }
    animation.impl_->tracks=runtime->num_tracks(); animation.impl_->runtime=std::move(runtime); return true;
}

} // namespace matter::animation
