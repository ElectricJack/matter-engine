#pragma once

// Part Workbench animation panel — presentation logic, no ImGui.
//
// Split for the same reason console_log.cpp is split from console_panel.cpp:
// the interesting behaviour (selection stability across a reload, rejecting a
// malformed snapshot, deciding whether a gizmo may be dragged) is worth testing
// headlessly, and none of it needs a draw context. animation_panel.cpp renders
// this model; this file must never include ImGui.
//
// Everything here is OBSERVATIONAL. The model derives rows from value-owned
// snapshots the engine copied at a WorldSession boundary. It owns no evaluator,
// ECS, cache, or renderer state, and it never writes animation state -- the one
// interactive affordance (a target gizmo) is executed by the panel through
// AnimationService, not by this model.

#include "matter/animation_debug.h"
#include "matter/animation_diagnostic.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace viewer {

enum class AnimationTab { Rig, Skin, Clips, Graph, Targets, Render };

// Why the panel has nothing to show. Worth distinguishing: NoBindings is the
// ordinary case (nothing animated is loaded), QueryFailed means the engine
// refused to produce a consistent snapshot and is a bug to chase. Collapsing
// them into one empty state is how "the overlay is broken" goes unnoticed.
enum class AnimationPanelStatus { Ready, NoBindings, QueryFailed };

struct AnimationJointRow {
    uint16_t index = 0;
    uint16_t parent = UINT16_MAX;
    uint32_t depth = 0;
    float radius = 0.0f;
    std::string label;
};

struct AnimationSocketRow {
    uint16_t index = 0;
    uint16_t joint = UINT16_MAX;
    std::string label;
    std::string joint_label;
};

struct AnimationTargetRow {
    uint16_t index = 0;
    std::string label;
    uint32_t chain_length = 0;
    bool has_pole = false;
    bool enabled = false;
    bool available = false;
    float weight = 0.0f;
    bool driver_is_controller = false;
    std::string controller;
    bool cadence_is_fixed = false;
    // Last evaluated transform, so an editor can seed its fields from what the
    // target is actually doing rather than from identity.
    matter::AnimationTransform evaluated{};
    // False when a controller owns this target: one-driver arbitration would
    // reject an external write, so the panel disables the gizmo instead of
    // letting an author drag something that silently does nothing.
    bool gizmo_enabled = false;
};

struct AnimationSkinSummary {
    bool has_skin = false;
    uint32_t influence_count = 0;
    uint32_t joint_count = 0;
    uint32_t rigid_part_count = 0;
};

class AnimationPanelModel {
public:
    // `query_ok` is the result of WorldSession::animation_debug_snapshots.
    // Snapshots failing valid_animation_debug_snapshot are dropped and counted
    // rather than partially displayed -- a bad index must never reach a draw.
    void update(const std::vector<matter::AnimationDebugInstanceSnapshot>& snapshots,
                bool query_ok);
    void set_diagnostics(std::vector<matter::AnimationDiagnostic> diagnostics);

    AnimationPanelStatus status() const { return status_; }
    std::size_t instance_count() const { return instances_.size(); }
    std::size_t selected_instance() const { return selected_; }
    // Ignored when out of range, so a stale index from the UI cannot desync.
    void select_instance(std::size_t index);

    AnimationTab tab() const { return tab_; }
    void set_tab(AnimationTab tab) { tab_ = tab; }

    const std::vector<AnimationJointRow>& joint_rows() const { return joint_rows_; }
    const std::vector<AnimationSocketRow>& socket_rows() const { return socket_rows_; }
    const std::vector<AnimationTargetRow>& target_rows() const { return target_rows_; }
    const AnimationSkinSummary& skin_summary() const { return skin_; }
    const std::vector<matter::AnimationDiagnostic>& diagnostics() const { return diagnostics_; }

    // Snapshots the engine produced but that failed draw-boundary validation.
    // Surfaced so a malformed rig is visible rather than silently absent.
    uint32_t rejected_snapshot_count() const { return rejected_; }

    uint64_t selected_resolved_hash() const;
    bool selected_visible() const;
    // The live animator the rows describe. Invalid when nothing is selected;
    // a write path must check valid() rather than assume a selection exists.
    matter::AnimatorInstanceHandle selected_animator() const;

private:
    void rebuild_rows();

    std::vector<matter::AnimationDebugInstanceSnapshot> instances_;
    std::vector<AnimationJointRow> joint_rows_;
    std::vector<AnimationSocketRow> socket_rows_;
    std::vector<AnimationTargetRow> target_rows_;
    std::vector<matter::AnimationDiagnostic> diagnostics_;
    AnimationSkinSummary skin_{};
    AnimationPanelStatus status_ = AnimationPanelStatus::NoBindings;
    AnimationTab tab_ = AnimationTab::Rig;
    std::size_t selected_ = 0;
    uint32_t rejected_ = 0;
};

} // namespace viewer
