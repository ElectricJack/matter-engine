// Headless proof for issue editor-workbench-actions-noop ("Open in Workbench,
// Reveal and Go do nothing"), covering the three per-control repairs:
//
//   1. The Workbench isolation world places its root UNEXPANDED. The W2
//      generator hardcoded `expand: true`, which hard-fails on leaf parts
//      ("expand: root has no children") and published an empty world — the
//      viewport showed nothing, so Open in Workbench (and Go, which now
//      routes through it) looked like dead buttons.
//   2. "Reveal" selects the module's baked root in the active world
//      (reveal_part_in_world) instead of shelling out to the OS file browser.
//   3. Revealing focuses the camera on the root's real world-space bounds
//      (focus_camera_on_selection's BakedRootBoundsFn path), so the selection
//      is looked at, not just recorded off-screen.
//
// Pure CPU: no GL/Vulkan/ImGui. Runs via `make -C MatterEditor
// run-test-workbench-actions`.

#include "camera_focus.h"
#include "part_workbench_iso.h"
#include "reveal_part.h"
#include "selection_set.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) {                                                 \
            std::printf("  ok: %s\n", msg);                         \
        } else {                                                    \
            std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++failures;                                             \
        }                                                           \
    } while (0)

bool near_f(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) < tol; }

part_graph_snapshot::Snapshot demo_snapshot() {
    part_graph_snapshot::Snapshot snap;
    part_graph_snapshot::Node root;
    root.module = "Crate";
    root.resolved_hash = 0xC0FFEEull;
    root.is_root = true;
    snap.nodes.emplace("Crate", root);

    part_graph_snapshot::Node child;
    child.module = "TreeBranch";
    child.resolved_hash = 0xBEEFull;
    child.is_root = false;  // composed inside a root; no world instance
    snap.nodes.emplace("TreeBranch", child);
    return snap;
}

void test_reveal_selects_loaded_root() {
    std::printf("-- reveal selects the loaded root\n");
    auto snap = demo_snapshot();
    viewer::SelectionSet selection;
    const uint64_t hash = viewer::reveal_part_in_world(snap, "Crate", selection);
    CHECK(hash == 0xC0FFEEull, "returns the root's resolved hash");
    CHECK(selection.size() == 1, "selection replaced with one object");
    const viewer::SelectedObject* primary = selection.primary();
    CHECK(primary != nullptr && primary->kind == viewer::SelectedObject::BakedRoot,
          "primary is a BakedRoot");
    CHECK(primary != nullptr && primary->id == 0xC0FFEEull,
          "primary carries the resolved hash");
}

void test_reveal_leaves_selection_when_not_loaded() {
    std::printf("-- reveal no-ops for unloaded / non-root modules\n");
    auto snap = demo_snapshot();
    viewer::SelectionSet selection;
    selection.replace(viewer::SelectedObject{viewer::SelectedObject::Entity, 42});

    CHECK(viewer::reveal_part_in_world(snap, "Pebble", selection) == 0,
          "module absent from the world returns 0");
    CHECK(viewer::reveal_part_in_world(snap, "TreeBranch", selection) == 0,
          "child-only module returns 0 (no world instance to select)");
    const viewer::SelectedObject* primary = selection.primary();
    CHECK(selection.size() == 1 && primary != nullptr &&
              primary->kind == viewer::SelectedObject::Entity && primary->id == 42,
          "prior selection untouched on both no-op paths");
}

void test_focus_frames_baked_root_bounds() {
    std::printf("-- focus frames the baked root's world-space bounds\n");
    viewer::SelectionSet selection;
    selection.replace(viewer::SelectedObject{viewer::SelectedObject::BakedRoot, 7});

    // Local AABB [-1,1]^3 translated to (10, 2, -4): focus must aim at the
    // translated center, from the preserved view direction.
    auto bounds = [](uint64_t part_hash, viewer::SelectionBounds& out) {
        if (part_hash != 7) return false;
        for (int a = 0; a < 3; ++a) { out.local_min[a] = -1.0f; out.local_max[a] = 1.0f; }
        for (int i = 0; i < 16; ++i) out.world_matrix[i] = 0.0f;
        out.world_matrix[0] = out.world_matrix[5] = out.world_matrix[10] =
            out.world_matrix[15] = 1.0f;
        out.world_matrix[3] = 10.0f;
        out.world_matrix[7] = 2.0f;
        out.world_matrix[11] = -4.0f;
        return true;
    };

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 10.0f};
    camera.target = {0.0f, 0.0f, 0.0f};
    viewer::FieldCommands fields;  // no entity getters needed for baked roots
    viewer::focus_camera_on_selection(camera, selection, fields, bounds);

    CHECK(near_f(camera.target.x, 10.0f) && near_f(camera.target.y, 2.0f) &&
              near_f(camera.target.z, -4.0f),
          "camera targets the instance's world-space center");
    const float dx = camera.position.x - camera.target.x;
    const float dy = camera.position.y - camera.target.y;
    const float dz = camera.position.z - camera.target.z;
    CHECK(near_f(dx, 0.0f) && near_f(dy, 0.0f) && dz > 0.0f,
          "view direction preserved (+Z), distance positive");
    const float radius = std::sqrt(3.0f);  // half-diagonal of the 2m cube
    const float expected = radius / std::tan(0.5f * 35.0f * 3.14159265f / 180.0f);
    CHECK(near_f(dz, expected, 0.05f), "distance follows the framing FOV math");

    // Without a bounds provider a baked-root-only selection must not yank the
    // camera (the pre-fix Scene-tree behavior, preserved for its callers).
    matter::CameraDesc untouched{};
    untouched.position = {1.0f, 2.0f, 3.0f};
    untouched.target = {4.0f, 5.0f, 6.0f};
    matter::CameraDesc copy = untouched;
    viewer::focus_camera_on_selection(copy, selection, fields);
    CHECK(near_f(copy.position.x, untouched.position.x) &&
              near_f(copy.target.z, untouched.target.z),
          "no provider -> camera unchanged");
}

void test_iso_world_places_root_unexpanded() {
    std::printf("-- workbench iso world places the root unexpanded\n");
    const std::string src = viewer::workbench_iso_world_source("Crate", "{\"seed\":3}");
    CHECK(src.find("module: \"Crate\"") != std::string::npos, "root module embedded");
    CHECK(src.find("params: {\"seed\":3}") != std::string::npos, "params embedded");
    CHECK(src.find("expand: true") == std::string::npos,
          "expand: true is gone (it hard-fails on leaf parts and publishes nothing)");
    CHECK(src.find("expand: false") != std::string::npos, "root placed unexpanded");
    CHECK(src.find("class IsoWorld_Crate extends World") != std::string::npos,
          "class name derives from the module");
}

void test_iso_identifier_sanitizes() {
    std::printf("-- iso identifier sanitization\n");
    CHECK(viewer::workbench_iso_identifier("Tree-2x") == "Tree_2x",
          "punctuation becomes underscores");
    CHECK(viewer::workbench_iso_identifier("9Lives") == "_9Lives",
          "leading digit gets a guard underscore");
    CHECK(viewer::workbench_iso_identifier("") == "_", "empty module stays a valid identifier");
}

}  // namespace

int main() {
    std::printf("test_workbench_actions\n");
    test_reveal_selects_loaded_root();
    test_reveal_leaves_selection_when_not_loaded();
    test_focus_frames_baked_root_bounds();
    test_iso_world_places_root_unexpanded();
    test_iso_identifier_sanitizes();
    if (failures) {
        std::printf("FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
