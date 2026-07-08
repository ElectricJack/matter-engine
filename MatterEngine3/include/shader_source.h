#pragma once
// Embedded-shader lookup with disk override. Internal kernel header (moves to
// src/ in the Stage 5 file moves).
#include <string>

namespace matter {

// Lookup order: MATTER_SHADER_DIR env, then override dir set below, then the
// embedded table. Returns false + err if the logical path is unknown everywhere.
bool shader_text(const char* logical_path, std::string& out, std::string& err);

// EngineDesc::shader_dir plumbs through here; nullptr clears the override.
void set_shader_override_dir(const char* dir_or_null);

} // namespace matter
