// Phase 4 Task 12 — Specialized component editors: adapters for the three
// component kinds that need more than generic property fields (part
// instances, physics bodies, sector streaming). Logic-only layer: no ImGui
// code, no rendering. The sector streaming editor here supersedes the
// standalone sector streaming panel retired from MatterViewer/ui.cpp.
#include "specialized_editors.h"

namespace viewer {

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
