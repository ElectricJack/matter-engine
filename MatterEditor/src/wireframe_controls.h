#ifndef VIEWER_WIREFRAME_CONTROLS_H
#define VIEWER_WIREFRAME_CONTROLS_H

// MatterEditor/src/wireframe_controls.h
//
// The wireframe debug view's decision logic, kept apart from the UI that
// triggers it. Three inputs reach one output:
//   - the Debug View panel's "Wireframe overlay" checkbox,
//   - the debug-view combo's own "Wireframe" entry (the persistable single-int
//     form that shot descriptors and issue state.json store),
//   - the console / MATTER_CMD_FIFO `wireframe [on|off|toggle]` verbs,
// all resolving to RenderOptions::wireframe via resolve_wireframe_request.
//
// The invariant both functions exist to enforce: a device without
// VK_POLYGON_MODE_LINE (fillModeNonSolid) can never be left claiming a
// wireframe view. Availability is a device fact passed in by the caller, not a
// preference, and it FAILS CLOSED at both entry points.
//
// Header-only, dependency-free (`<string_view>` and nothing else), pure and
// noexcept — no ImGui, no Vulkan, no engine types — so the truth table is
// unit-testable. It is: MatterEngine3/tests/vk_scene_renderer_tests.cpp.
// The live caller is MatterEditor/src/main.cpp, which reads the editor state
// each frame and writes the result into RenderOptions.

#include <string_view>

namespace viewer {

// Outcome of a console/FIFO `wireframe ...` line. The caller prints a
// different message for each, which is why "the device cannot do this" is a
// separate value from "applied" rather than a silently ignored request.
enum class WireframeConsoleCommandResult {
    Unrecognized,  // not a wireframe verb at all; try the next parser
    Unavailable,   // recognized, but the device cannot; the flag was CLEARED
    Applied,       // the flag now holds what the line asked for
};

// Applies the legacy FIFO verbs to the same session-local flag the Debug View
// checkbox edits. Unsupported devices FAIL CLOSED -- the flag is cleared and
// the caller is told it is unavailable -- rather than being left set over a
// filled frame, which would make the console report a wireframe view that the
// GPU is not drawing.
inline WireframeConsoleCommandResult apply_wireframe_console_command(
    std::string_view command, bool available, bool& enabled) noexcept {
    const bool recognized = command == "wireframe" ||
                            command == "wireframe toggle" ||
                            command == "wireframe on" ||
                            command == "wireframe off";
    if (!recognized) return WireframeConsoleCommandResult::Unrecognized;
    if (!available) {
        enabled = false;
        return WireframeConsoleCommandResult::Unavailable;
    }
    if (command == "wireframe" || command == "wireframe toggle")
        enabled = !enabled;
    else
        enabled = command == "wireframe on";
    return WireframeConsoleCommandResult::Applied;
}

// The one place that turns editor state into RenderOptions::wireframe.
//
// Two controls reach the same flag on purpose. `checkbox` composes with any
// debug view (notably "LOD levels", so triangle density and rung colour can be
// read at once), while the combo's own "Wireframe" entry is the persistable
// single-int form that shot descriptors and issue state.json can store. Either
// asks for lines; neither may produce them on a device without the feature.
inline bool resolve_wireframe_request(bool checkbox, int debug_view_mode,
                                      int wireframe_view_index,
                                      bool available) noexcept {
    if (!available) return false;
    return checkbox || debug_view_mode == wireframe_view_index;
}

}  // namespace viewer

#endif  // VIEWER_WIREFRAME_CONTROLS_H
