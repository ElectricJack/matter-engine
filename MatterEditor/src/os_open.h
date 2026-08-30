#pragma once
// MatterEditor/src/os_open.h
//
// Task 9 — small cross-platform helper to open a file in the OS default
// editor/viewer (used by the Properties panel's "Open Source" button for
// baked-root part scripts).
#include <string>

namespace viewer {

// Hand `path` to the OS default handler (ShellExecute on Windows,
// `xdg-open` elsewhere) and return immediately.
//
// Fire-and-forget: there is no return value and no error reporting. A path
// that does not exist, a desktop with no registered handler, or a missing
// xdg-open are all silent no-ops from the caller's point of view — check the
// path yourself if that matters. Main/UI thread only in practice; it is
// called straight out of an ImGui button handler.
void os_open_file(const std::string& path);

} // namespace viewer
