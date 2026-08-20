#pragma once
// MatterEngine3/src/shader_source.h
//
// Embedded-shader lookup with disk override. Internal kernel header (moves to
// src/ in the Stage 5 file moves).
#include <string>

namespace matter {

// Lookup order: MATTER_SHADER_DIR env, then override dir set below, then the
// embedded table. Returns false + err if the logical path is unknown everywhere.
// `logical_path` is the shader's path as the build recorded it in the
// generated table (shaders_gen/embedded_shaders.h), and the SAME relative path
// is appended to an override directory -- so an override tree must mirror the
// source tree's layout, not just its filenames.
//
// GOTCHAS:
//   * An override file that EXISTS wins even when it is empty: a zero-byte
//     file is a successful read, so `out` comes back empty and true is
//     returned. A stray empty file therefore silently blanks a shader rather
//     than falling through to the embedded copy.
//   * `out` is only meaningful when the call returns true; `err` is only set
//     when it returns false.
//   * Every hit copies the whole source text into `out`. Call it at pipeline
//     build time, not per frame.
bool shader_text(const char* logical_path, std::string& out, std::string& err);

// EngineDesc::shader_dir plumbs through here; nullptr clears the override.
// PROCESS-WIDE and unsynchronized: the override directory is a single file
// static, so this affects every subsequent shader_text() call from any thread
// and must be set during startup, before shaders begin loading. Note the
// MATTER_SHADER_DIR environment variable is consulted FIRST and this override
// cannot displace it.
void set_shader_override_dir(const char* dir_or_null);

} // namespace matter
