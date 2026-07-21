#pragma once
// Task 9 — small cross-platform helper to open a file in the OS default
// editor/viewer (used by the Properties panel's "Open Source" button for
// baked-root part scripts).
#include <string>

namespace viewer {

void os_open_file(const std::string& path);

} // namespace viewer
