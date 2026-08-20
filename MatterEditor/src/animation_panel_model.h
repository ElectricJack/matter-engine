#pragma once

// MatterEditor/src/animation_panel_model.h
//
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
//
// Lifecycle: one model per panel, owned by BakeLab (`animation_model_` in
// MatterEditor/src/bake_lab.h). The host calls update() once per frame with the
// engine's snapshot vector, then set_diagnostics(), then draws. Every accessor
// below returns a reference into state that update()/select_instance() rebuild,
// so a stored reference does not survive the next update().
//
// Threading: no synchronization of any kind. Refresh and read happen on the
// same (render) thread, and the snapshots stored here are deep copies the
// engine made at a WorldSession boundary, so nothing in the model aliases live
// engine state.
//
// Selection is keyed on the asset's resolved hash rather than on the instance
// index, so an ordinary refresh -- or a reload that removes an animator ahead
// of the selection -- keeps the same rig selected instead of silently sliding
// onto a different one.

#include "matter/animation_debug.h"
#include "matter/animation_diagnostic.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace viewer {

// Which tab the panel last drew. Recorded on the model so the choice is
// inspectable and testable; nothing in this file changes behaviour based on it.
// Clips and Graph have no data behind them today -- the debug snapshot carries
// rig, skin, target and pose data only.
enum class AnimationTab { Rig, Skin, Clips, Graph, Targets, Render };

// Why the panel has nothing to show. Worth distinguishing: NoBindings is the
// ordinary case (nothing animated is loaded), QueryFailed means the engine
// refused to produce a consistent snapshot and is a bug to chase. Collapsing
// them into one empty state is how "the overlay is broken" goes unnoticed.
enum class AnimationPanelStatus { Ready, NoBindings, QueryFailed };

// One row of the Rig tab's joint table, flattened from the snapshot's joint
// array in storage order (parent before child, guaranteed by the rig compiler).
struct AnimationJointRow {
    uint16_t index = 0;            // index into the snapshot's joints array.
    uint16_t parent = UINT16_MAX;  // UINT16_MAX = root, i.e. no parent.
    uint32_t depth = 0;            // hops to the root; used only to indent.
    float radius = 0.0f;           // authored joint radius, world units.
    std::string label;             // authored name, or "joint[i]" if unnamed.
};

// One attachment socket: a named frame parented to a joint.
struct AnimationSocketRow {
    uint16_t index = 0;            // index into the snapshot's sockets array.
    uint16_t joint = UINT16_MAX;   // parent joint index; out of range = unbound.
    std::string label;             // authored name, or "socket[i]" if unnamed.
    std::string joint_label;       // parent joint's label, or "<unbound>".
};

// One IK / attachment target: its static definition from the asset joined with
// the live per-frame state from the pose. The enabled/available/weight/evaluated
// group comes from the pose and stays at its default when the pose carries no
// entry for this target.
struct AnimationTargetRow {
    uint16_t index = 0;              // index into the snapshot's targets array.
    std::string label;               // authored name, or "target[i]".
    uint32_t chain_length = 0;       // joints in the IK chain (two-bone IK == 3).
    bool has_pole = false;
    bool enabled = false;            // the target is switched on this frame.
    bool available = false;          // the evaluator produced a usable result.
    float weight = 0.0f;             // blend weight, 0-1.
    bool driver_is_controller = false;
    std::string controller;          // driving controller when the flag above is set.
    bool cadence_is_fixed = false;   // evaluated on the fixed tick, not per frame.
    // Last evaluated transform, so an editor can seed its fields from what the
    // target is actually doing rather than from identity.
    matter::AnimationTransform evaluated{};
    // False when a controller owns this target: one-driver arbitration would
    // reject an external write, so the panel disables the gizmo instead of
    // letting an author drag something that silently does nothing.
    bool gizmo_enabled = false;
};

// Counts for the Skin tab, all derived from the selected snapshot's asset and
// all reset to zero when nothing is selected.
struct AnimationSkinSummary {
    bool has_skin = false;          // true iff there are LOD0 influences.
    uint32_t influence_count = 0;   // per-vertex influence records at LOD0.
    uint32_t joint_count = 0;       // joints in the rig == skin palette size.
    uint32_t rigid_part_count = 0;  // rigidly bound sub-parts, i.e. not skinned.
};

// Panel state derived from the engine's animation debug snapshots: the row
// vectors the Rig/Skin/Targets tabs render, the current selection, and the
// reason the panel might have nothing to show.
//
// Owned by the panel host (BakeLab) and refreshed once per frame. Copyable and
// movable by default -- it holds only value copies, no handles into engine
// memory. Not thread-safe and not meant to be: update() and every accessor run
// on the same thread.
//
// Call order per frame: update(snapshots, query_ok), which rebuilds every row
// vector, then set_diagnostics() if there are any, then read. select_instance
// and set_tab may be called from the draw in response to input;
// select_instance rebuilds the rows immediately.
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
    // Recomputes the joint/socket/target rows and the skin summary from the
    // selected instance. Clears all four first, so it doubles as the "nothing
    // selected" path. Called by update() and select_instance(); nothing else
    // needs it, because nothing else changes the selection.
    void rebuild_rows();

    // Accepted snapshots only -- update() drops any that fail
    // valid_animation_debug_snapshot -- so indices here do not correspond to
    // the engine's own instance ordering.
    std::vector<matter::AnimationDebugInstanceSnapshot> instances_;
    std::vector<AnimationJointRow> joint_rows_;
    std::vector<AnimationSocketRow> socket_rows_;
    std::vector<AnimationTargetRow> target_rows_;
    std::vector<matter::AnimationDiagnostic> diagnostics_;
    AnimationSkinSummary skin_{};
    AnimationPanelStatus status_ = AnimationPanelStatus::NoBindings;
    AnimationTab tab_ = AnimationTab::Rig;
    std::size_t selected_ = 0;  // index into instances_; 0 when nothing valid.
    uint32_t rejected_ = 0;     // snapshots dropped by the most recent update().
};

} // namespace viewer
