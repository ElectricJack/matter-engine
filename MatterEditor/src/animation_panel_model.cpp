#include "animation_panel_model.h"

#include <algorithm>
#include <utility>

namespace viewer {
namespace {

// Authored names are the useful label, but a rig that somehow reached the panel
// without one still has to render as something stable and unambiguous.
std::string label_or_index(const std::string& name, const char* kind, size_t index) {
    if (!name.empty()) return name;
    return std::string(kind) + "[" + std::to_string(index) + "]";
}

} // namespace

void AnimationPanelModel::update(
    const std::vector<matter::AnimationDebugInstanceSnapshot>& snapshots, bool query_ok) {
    rejected_ = 0;
    if (!query_ok) {
        // The engine could not produce a consistent view. Drop the stale rows
        // rather than showing values that no longer describe anything, but keep
        // the status distinct from "nothing is animated" so the failure is
        // visible instead of looking like an idle scene.
        instances_.clear();
        selected_ = 0;
        status_ = AnimationPanelStatus::QueryFailed;
        rebuild_rows();
        return;
    }

    // Remember what was selected so an ordinary refresh does not move it. The
    // asset identity is the stable key; the index is not (an animator ahead of
    // the selection can disappear on reload).
    const uint64_t previous_hash = selected_resolved_hash();

    std::vector<matter::AnimationDebugInstanceSnapshot> accepted;
    accepted.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) {
        // Strict draw-boundary validation: one bad index or non-finite value
        // rejects the whole snapshot. A partially trusted snapshot is how a
        // diagnostic overlay turns into an out-of-bounds read.
        if (!matter::valid_animation_debug_snapshot(snapshot)) { ++rejected_; continue; }
        accepted.push_back(snapshot);
    }
    instances_ = std::move(accepted);

    selected_ = 0;
    if (previous_hash != 0) {
        for (size_t i = 0; i < instances_.size(); ++i) {
            if (instances_[i].asset.resolved_hash == previous_hash) { selected_ = i; break; }
        }
    }
    if (selected_ >= instances_.size()) selected_ = 0;

    status_ = instances_.empty() ? AnimationPanelStatus::NoBindings
                                 : AnimationPanelStatus::Ready;
    rebuild_rows();
}

void AnimationPanelModel::set_diagnostics(std::vector<matter::AnimationDiagnostic> diagnostics) {
    diagnostics_ = std::move(diagnostics);
}

void AnimationPanelModel::select_instance(std::size_t index) {
    if (index >= instances_.size()) return;   // stale UI index: refuse, do not store
    if (index == selected_) return;
    selected_ = index;
    rebuild_rows();
}

uint64_t AnimationPanelModel::selected_resolved_hash() const {
    if (selected_ >= instances_.size()) return 0;
    return instances_[selected_].asset.resolved_hash;
}

bool AnimationPanelModel::selected_visible() const {
    if (selected_ >= instances_.size()) return false;
    return instances_[selected_].visible;
}

matter::AnimatorInstanceHandle AnimationPanelModel::selected_animator() const {
    if (selected_ >= instances_.size()) return {};
    return instances_[selected_].pose.instance;
}

void AnimationPanelModel::rebuild_rows() {
    joint_rows_.clear();
    socket_rows_.clear();
    target_rows_.clear();
    skin_ = {};
    if (selected_ >= instances_.size()) return;

    const auto& asset = instances_[selected_].asset;
    const auto& pose = instances_[selected_].pose;

    // Depth from the parent chain. Joints are stored parent-before-child (the
    // rig compiler guarantees it), so a single forward pass suffices; the guard
    // keeps a malformed order from reading an unwritten depth.
    joint_rows_.reserve(asset.joints.size());
    for (size_t i = 0; i < asset.joints.size(); ++i) {
        const auto& joint = asset.joints[i];
        AnimationJointRow row;
        row.index = static_cast<uint16_t>(i);
        row.parent = joint.parent;
        row.radius = joint.radius;
        row.label = label_or_index(joint.name, "joint", i);
        row.depth = (joint.parent < i) ? joint_rows_[joint.parent].depth + 1 : 0;
        joint_rows_.push_back(std::move(row));
    }

    const auto joint_label = [this](uint16_t index) -> std::string {
        if (index < joint_rows_.size()) return joint_rows_[index].label;
        return "<unbound>";
    };

    socket_rows_.reserve(asset.sockets.size());
    for (size_t i = 0; i < asset.sockets.size(); ++i) {
        const auto& socket = asset.sockets[i];
        AnimationSocketRow row;
        row.index = static_cast<uint16_t>(i);
        row.joint = socket.joint;
        row.label = label_or_index(socket.name, "socket", i);
        row.joint_label = joint_label(socket.joint);
        socket_rows_.push_back(std::move(row));
    }

    target_rows_.reserve(asset.targets.size());
    for (size_t i = 0; i < asset.targets.size(); ++i) {
        const auto& definition = asset.targets[i];
        AnimationTargetRow row;
        row.index = static_cast<uint16_t>(i);
        row.label = label_or_index(definition.name, "target", i);
        row.chain_length = static_cast<uint32_t>(definition.chain.size());
        row.has_pole = definition.has_pole;
        row.driver_is_controller = definition.driver_is_controller;
        row.controller = definition.controller;
        row.cadence_is_fixed = definition.cadence_is_fixed;
        // update() validated that pose.targets is parallel to asset.targets.
        if (i < pose.targets.size()) {
            row.enabled = pose.targets[i].enabled;
            row.available = pose.targets[i].available;
            row.weight = pose.targets[i].weight;
            row.evaluated = pose.targets[i].evaluated;
        }
        // One-driver arbitration owns the real decision; mirroring it here lets
        // the panel grey the gizmo out instead of offering a drag the engine
        // will reject.
        row.gizmo_enabled = !definition.driver_is_controller;
        target_rows_.push_back(std::move(row));
    }

    skin_.joint_count = static_cast<uint32_t>(asset.joints.size());
    skin_.influence_count = static_cast<uint32_t>(asset.lod0_influences.size());
    skin_.has_skin = !asset.lod0_influences.empty();
    skin_.rigid_part_count = static_cast<uint32_t>(asset.rigid_part_hashes.size());
}

} // namespace viewer
