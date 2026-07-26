// Part Workbench animation panel model — headless (no ImGui, no engine).
//
// Everything asserted here is behaviour an author notices when it is wrong:
// a selection that jumps to a different rig on reload, an empty panel that
// cannot distinguish "nothing is animated" from "the query failed", a gizmo
// that looks draggable but is silently rejected, and a malformed snapshot that
// half-draws instead of being refused.
#include "animation_panel_model.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

namespace {

using namespace viewer;
using matter::AnimationDebugInstanceSnapshot;

// A minimal but VALID snapshot: three joints (root -> mid -> tip), one socket,
// one target over the whole chain. Sizes must satisfy
// valid_animation_debug_snapshot, which is what the model filters on.
AnimationDebugInstanceSnapshot make_snapshot(uint64_t hash, bool controller_driven) {
    AnimationDebugInstanceSnapshot s{};
    s.asset.resolved_hash = hash;
    s.asset.joints = {
        {UINT16_MAX, 1.0f, "root"},
        {0, 0.5f, "mid"},
        {1, 0.25f, "tip"},
    };
    s.asset.sockets = {{2, matter::AnimationTransform{}, "hand"}};

    matter::AnimationDebugTargetDefinition target{};
    target.chain = {0, 1, 2};
    target.has_pole = true;
    target.name = "foot";
    target.driver_is_controller = controller_driven;
    if (controller_driven) target.controller = "gait";
    target.cadence_is_fixed = true;
    s.asset.targets.push_back(std::move(target));

    s.pose.instance = matter::AnimatorInstanceHandle{1, 1};
    s.pose.local_pose.resize(3);
    s.pose.model_pose.resize(3);
    s.pose.skin_palette.resize(3);
    matter::AnimationDebugTargetState state{};
    state.enabled = true;
    state.available = true;
    state.weight = 1.0f;
    s.pose.targets.push_back(state);
    return s;
}

void test_empty_and_failed_are_distinguishable() {
    AnimationPanelModel model;
    model.update({}, true);
    check(model.status() == AnimationPanelStatus::NoBindings,
          "no snapshots with a successful query reads as NoBindings");
    check(model.instance_count() == 0, "NoBindings exposes no instances");

    model.update({}, false);
    check(model.status() == AnimationPanelStatus::QueryFailed,
          "a failed query is distinct from having nothing to show");
}

void test_ready_builds_named_rows() {
    AnimationPanelModel model;
    model.update({make_snapshot(0xAAAA, false)}, true);
    check(model.status() == AnimationPanelStatus::Ready, "a valid snapshot reads as Ready");
    check(model.instance_count() == 1, "one live binding");

    const auto& joints = model.joint_rows();
    check(joints.size() == 3, "every joint produces a row");
    if (joints.size() == 3) {
        check(joints[0].label == "root" && joints[1].label == "mid" && joints[2].label == "tip",
              "joint rows carry their authored names");
        check(joints[0].depth == 0 && joints[1].depth == 1 && joints[2].depth == 2,
              "joint depth is derived from the parent chain");
    }
    const auto& sockets = model.socket_rows();
    check(sockets.size() == 1 && sockets[0].label == "hand" && sockets[0].joint_label == "tip",
          "socket rows name both the socket and its joint");
    check(model.selected_resolved_hash() == 0xAAAA, "the selected asset identity is reported");
}

void test_controller_driven_target_disables_the_gizmo() {
    AnimationPanelModel external;
    external.update({make_snapshot(1, false)}, true);
    check(external.target_rows().size() == 1, "one target row");
    if (!external.target_rows().empty()) {
        const auto& row = external.target_rows()[0];
        check(row.label == "foot", "target row carries its authored name");
        check(row.chain_length == 3, "target row reports its chain length");
        check(row.gizmo_enabled,
              "an externally driven target may be dragged");
    }

    AnimationPanelModel driven;
    driven.update({make_snapshot(1, true)}, true);
    if (!driven.target_rows().empty()) {
        const auto& row = driven.target_rows()[0];
        check(!row.gizmo_enabled,
              "a controller-driven target disables its gizmo rather than silently ignoring drags");
        check(row.driver_is_controller && row.controller == "gait",
              "the panel can say WHICH controller owns the target");
    }
}

void test_selection_survives_and_clamps_across_updates() {
    AnimationPanelModel model;
    model.update({make_snapshot(1, false), make_snapshot(2, false), make_snapshot(3, false)}, true);
    model.select_instance(2);
    check(model.selected_instance() == 2, "selection follows the author's click");
    check(model.selected_resolved_hash() == 3, "selection resolves to the right asset");

    // Same set again (an ordinary refresh): selection must not move.
    model.update({make_snapshot(1, false), make_snapshot(2, false), make_snapshot(3, false)}, true);
    check(model.selected_instance() == 2, "a refresh does not move the selection");

    // The world reloads with fewer animators: the selection must clamp into
    // range rather than index off the end.
    model.update({make_snapshot(1, false)}, true);
    check(model.selected_instance() == 0, "selection clamps when instances disappear");
    check(model.selected_resolved_hash() == 1, "clamped selection still resolves");

    model.update({}, true);
    check(model.selected_instance() == 0 && model.instance_count() == 0,
          "losing every instance leaves a safe selection");
    check(model.selected_resolved_hash() == 0, "no selection resolves to no asset");
}

void test_out_of_range_selection_is_ignored() {
    AnimationPanelModel model;
    model.update({make_snapshot(1, false)}, true);
    model.select_instance(99);
    check(model.selected_instance() == 0, "an out-of-range selection is refused, not stored");
}

void test_malformed_snapshot_is_rejected_not_half_drawn() {
    // model_pose shorter than the joint list: a draw would index off the end.
    AnimationDebugInstanceSnapshot bad = make_snapshot(7, false);
    bad.pose.model_pose.resize(1);

    AnimationPanelModel model;
    model.update({bad}, true);
    check(model.instance_count() == 0, "a malformed snapshot is dropped entirely");
    check(model.rejected_snapshot_count() == 1, "the rejection is counted, not silent");
    check(model.status() == AnimationPanelStatus::NoBindings,
          "dropping the only snapshot leaves nothing to show");

    // A good snapshot alongside a bad one must still be shown.
    AnimationPanelModel mixed;
    mixed.update({bad, make_snapshot(8, false)}, true);
    check(mixed.instance_count() == 1 && mixed.rejected_snapshot_count() == 1,
          "one bad snapshot does not suppress the good ones");
    check(mixed.selected_resolved_hash() == 8, "the surviving snapshot is selectable");
}

// A rigidly-bound influence: one joint at full weight. valid_animation_debug_snapshot
// requires at least one non-zero lane whose weights sum to exactly 65535, so an
// all-zero influence is (correctly) rejected as malformed.
matter::AnimationDebugVertexInfluence make_influence(uint16_t joint) {
    matter::AnimationDebugVertexInfluence influence{};
    influence.joints[0] = joint;
    influence.weights[0] = 65535u;
    return influence;
}

void test_skin_summary_and_diagnostics() {
    AnimationDebugInstanceSnapshot s = make_snapshot(9, false);
    s.asset.lod0_influences = {make_influence(0), make_influence(1),
                               make_influence(2), make_influence(2)};
    s.asset.rigid_part_hashes = {11, 22};

    AnimationPanelModel model;
    model.update({s}, true);
    check(model.skin_summary().has_skin, "a snapshot with influences reports skin");
    check(model.skin_summary().influence_count == 4, "influence count is reported");
    check(model.skin_summary().joint_count == 3, "joint count is reported");
    check(model.skin_summary().rigid_part_count == 2, "rigid segment count is reported");

    model.set_diagnostics({{"binding-validation", "writable target chains overlap", "<part>", 2, 291, "target"}});
    check(model.diagnostics().size() == 1, "bake diagnostics reach the panel");
    if (!model.diagnostics().empty())
        check(model.diagnostics()[0].source_location() == "<part>:2:291 (target)",
              "a diagnostic formats its source location for display");
}

// The write path needs the live animator handle and a transform to seed its
// fields from. Seeding from identity instead of the evaluated transform would
// yank the target the moment an author opened the editor.
void test_write_path_inputs_are_exposed() {
    AnimationPanelModel model;
    AnimationDebugInstanceSnapshot s = make_snapshot(5, false);
    s.pose.targets[0].evaluated.translation = {1.5f, -2.0f, 0.25f};
    model.update({s}, true);

    check(model.selected_animator().valid(),
          "the selected animator handle is exposed for writes");
    if (!model.target_rows().empty()) {
        const auto& row = model.target_rows()[0];
        check(row.evaluated.translation.x == 1.5f && row.evaluated.translation.y == -2.0f,
              "target rows carry the evaluated transform to seed an editor from");
    }

    model.update({}, true);
    check(!model.selected_animator().valid(),
          "no selection exposes no animator, so a write path cannot target a dead handle");
}

void test_tab_selection_round_trips() {
    AnimationPanelModel model;
    check(model.tab() == AnimationTab::Rig, "the panel opens on the Rig tab");
    model.set_tab(AnimationTab::Targets);
    check(model.tab() == AnimationTab::Targets, "tab selection round-trips");
    model.update({}, false);
    check(model.tab() == AnimationTab::Targets, "a failed query does not reset the tab");
}

} // namespace

int main() {
    test_empty_and_failed_are_distinguishable();
    test_ready_builds_named_rows();
    test_controller_driven_target_disables_the_gizmo();
    test_selection_survives_and_clamps_across_updates();
    test_out_of_range_selection_is_ignored();
    test_malformed_snapshot_is_rejected_not_half_drawn();
    test_skin_summary_and_diagnostics();
    test_write_path_inputs_are_exposed();
    test_tab_selection_round_trips();
    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
