#pragma once
// tileset_gl_ctx.h — headless GL 4.6 check + compute program helpers.
#include "gl46.h"     // GLAD types + gl46_available
#include <string>

class BLASManager;
class TLASManager;

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

// Bind the 4 BVH sampler2Ds (trianglesTexture, blasNodesTexture, tlasNodesTexture,
// instancesTexture) at texture units 0..3, and the 2 imposter sampler3Ds
// (imposterColorVolume, imposterNormalVolume) at units 4..5 (both bound to 0 as
// placeholders — the tileset bake does not use imposters). Required because
// SetShaderValueTexture is a no-op under GRAPHICS_API_OPENGL_43, so raylib's
// bind_to_shader path cannot wire samplers to a compute program; we do it ourselves.
// Also sets the 4 count uniforms (triangleCount, blasNodeCount, tlasNodeCount,
// instanceCount) via glGetUniformLocation + glUniform1i directly, since the
// fabricated Shader{locs=nullptr} path would be UB.
// Calls ensure_gpu_textures_ready on both blas and tlas internally, so callers
// do not need to do so separately. program must be a linked compute program.
// Uniform names that are absent in the program are silently skipped
// (glGetUniformLocation returns -1).
void bind_bvh_samplers(GLuint program, BLASManager& blas, TLASManager& tlas);

} // namespace tileset
