// Phase 4 Task 12 — Specialized component editors: adapters for the three
// component kinds that need more than generic property fields (part
// instances, physics bodies, sector streaming). Logic-only layer: no ImGui
// code, no rendering. The sector streaming editor here supersedes the
// standalone sector streaming panel retired from MatterEditor/src/ui.cpp.
// This translation unit is intentionally almost empty: the command structs are
// declarations, the state is POD, and the drawing lives in
// `MatterEditor/src/properties_panel.cpp`. What is left is the one predicate
// that decides which component kinds get extra UI at all.
//
// Tests: `MatterEngine3/tests/specialized_editors_tests.cpp`
// (`make -C MatterEngine3/tests run-specialized-editors`).
#include "specialized_editors.h"

namespace viewer {

// The gate the Properties panel asks before drawing anything beyond the
// auto-generated fields. It must agree with the switch in
// properties_panel.cpp's draw_specialized_editor: a kind that returns true
// here with no case there draws an empty extra section instead of failing, so
// the disagreement is silent. Add kinds to both or neither.
bool SpecializedEditors::has_specialized_editor(matter::scene::ComponentKind kind) const {
    using matter::scene::ComponentKind;
    switch (kind) {
        case ComponentKind::PartInstance:
        case ComponentKind::RigidBody:
        case ComponentKind::SectorStreaming:
            return true;
        default:
            return false;
    }
}

} // namespace viewer
