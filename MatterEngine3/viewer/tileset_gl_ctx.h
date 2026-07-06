#pragma once
// tileset_gl_ctx.h — headless GL 4.6 check + compute program helpers.
#include "gl46.h"     // GLAD types + gl46_available
#include <string>

namespace tileset {

// Verify the live raylib GL context exposes the compute + SSBO surface we need.
// Must be called AFTER raylib InitWindow. On failure fills err with a message
// that includes the phrase "set GALLIUM_DRIVER=d3d12".
bool tileset_gl_init(std::string& err);

// Compile + link a single compute shader. Returns the program id (nonzero) on
// success; 0 on failure with the shader / program info log in err.
GLuint compile_compute_program(const std::string& source, std::string& err);

// Textually expand `#include "name.glsl"` lines against files in `includes_dir`.
// Supports one level of nesting (each included file may itself #include others,
// bounded by a small depth limit to guard against cycles).
bool load_compute_source(const std::string& primary_path,
                         const std::string& includes_dir,
                         std::string& out_source,
                         std::string& err);

} // namespace tileset
