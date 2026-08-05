#ifndef VIEWER_WIREFRAME_CONTROLS_H
#define VIEWER_WIREFRAME_CONTROLS_H

#include <string_view>

namespace viewer {

enum class WireframeConsoleCommandResult {
    Unrecognized,
    Unavailable,
    Applied,
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
