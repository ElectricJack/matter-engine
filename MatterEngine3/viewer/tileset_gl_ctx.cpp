// tileset_gl_ctx.cpp — see header.

#include "tileset_gl_ctx.h"

#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include <limits.h>

namespace tileset {

bool tileset_gl_init(std::string& err) {
    std::string why;
    if (!viewer::gl46_available(why)) {
        err = "tileset_gl_init: " + why + "; set GALLIUM_DRIVER=d3d12 on WSLg";
        return false;
    }
    return true;
}

GLuint compile_compute_program(const std::string& source, std::string& err) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    const char* src = source.c_str();
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);

    GLint status = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint len = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 0 ? (size_t)len : 1, '\0');
        if (len > 0) glGetShaderInfoLog(sh, len, nullptr, log.data());
        err = "compile_compute_program: shader compile failed: " + log;
        glDeleteShader(sh);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glDetachShader(prog, sh);
    glDeleteShader(sh);

    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 0 ? (size_t)len : 1, '\0');
        if (len > 0) glGetProgramInfoLog(prog, len, nullptr, log.data());
        err = "compile_compute_program: link failed: " + log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ---------------------------------------------------------------------------
// Textual #include expansion (depth-limited).
// ---------------------------------------------------------------------------
static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool expand_includes(const std::string& src, const std::string& includes_dir,
                            int depth, std::unordered_set<std::string>& stack,
                            std::string& out, std::string& err)
{
    if (depth > 4) { err = "load_compute_source: include depth > 4"; return false; }

    std::istringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        // Trim leading whitespace to detect "#include" directives.
        size_t p = 0;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
        const std::string trimmed = line.substr(p);
        if (trimmed.rfind("#include", 0) == 0) {
            size_t q1 = trimmed.find('"'); size_t q2 = trimmed.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos || q2 <= q1 + 1) {
                err = "load_compute_source: malformed #include: " + line; return false;
            }
            const std::string name = trimmed.substr(q1 + 1, q2 - q1 - 1);
            const std::string path = includes_dir + "/" + name;
            if (stack.count(path)) {
                err = "load_compute_source: include cycle: " + name; return false;
            }
            std::string inc_src;
            if (!read_file(path, inc_src)) {
                err = "load_compute_source: missing include: " + path; return false;
            }
            stack.insert(path);
            if (!expand_includes(inc_src, includes_dir, depth + 1, stack, out, err)) return false;
            stack.erase(path);
            out.append("\n");
            continue;
        }
        out.append(line);
        out.append("\n");
    }
    return true;
}

// Resolve a shader-relative path against the running binary's own directory so
// callers that chdir() (e.g. LocalProvider chdirs to abs_cache_root so
// bake_source can write parts/<hash>.part relative to CWD) can still load
// shaders shipped next to the binary. Absolute paths are returned unchanged.
static std::string resolve_relative_to_exe(const std::string& p) {
    if (!p.empty() && p[0] == '/') return p;
    char exe_buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n <= 0) return p;   // fall back to CWD-relative on non-Linux
    exe_buf[n] = 0;
    std::string exe_dir(exe_buf);
    size_t slash = exe_dir.find_last_of('/');
    if (slash == std::string::npos) return p;
    return exe_dir.substr(0, slash) + "/" + p;
}

bool load_compute_source(const std::string& primary_path,
                         const std::string& includes_dir,
                         std::string& out_source,
                         std::string& err)
{
    const std::string abs_primary  = resolve_relative_to_exe(primary_path);
    const std::string abs_includes = resolve_relative_to_exe(includes_dir);

    std::string src;
    if (!read_file(abs_primary, src)) {
        err = "load_compute_source: cannot read primary: " + abs_primary;
        return false;
    }
    out_source.clear();
    std::unordered_set<std::string> stack;
    return expand_includes(src, abs_includes, /*depth*/ 0, stack, out_source, err);
}

// ---------------------------------------------------------------------------
// BVH sampler + count uniform binding for compute programs.
// ---------------------------------------------------------------------------
void bind_bvh_samplers(GLuint program, BLASManager& blas, TLASManager& tlas)
{
    // Upload any pending CPU data to GPU textures (no-op if already current).
    blas.ensure_gpu_textures_ready();
    tlas.ensure_gpu_textures_ready(blas);

    // Bind BVH sampler2Ds to known texture units.
    // bvh_tlas_common.glsl expects:
    //   trianglesTexture    → unit 0
    //   blasNodesTexture    → unit 1
    //   tlasNodesTexture    → unit 2
    //   instancesTexture    → unit 3
    struct TexBind { const char* name; GLuint id; GLenum target; };
    const TexBind bindings[] = {
        { "trianglesTexture",  blas.triangles_texture_id(),  GL_TEXTURE_2D },
        { "blasNodesTexture",  blas.blas_nodes_texture_id(), GL_TEXTURE_2D },
        { "tlasNodesTexture",  tlas.tlas_nodes_texture_id(), GL_TEXTURE_2D },
        { "instancesTexture",  tlas.instances_texture_id(),  GL_TEXTURE_2D },
    };
    for (int unit = 0; unit < 4; ++unit) {
        GLint loc = glGetUniformLocation(program, bindings[unit].name);
        if (loc < 0) continue;
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(bindings[unit].target, bindings[unit].id);
        glUniform1i(loc, unit);
    }

    // Imposter volumes unused in the bake passes; bind texture units 4..5 to 0
    // (safe — intersectScene only reads them when inst.isImposter == true).
    {
        GLint cLoc = glGetUniformLocation(program, "imposterColorVolume");
        GLint nLoc = glGetUniformLocation(program, "imposterNormalVolume");
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_3D, 0);
        if (cLoc >= 0) glUniform1i(cLoc, 4);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_3D, 0);
        if (nLoc >= 0) glUniform1i(nLoc, 5);
        glActiveTexture(GL_TEXTURE0);
    }

    // Set count uniforms directly (bind_to_shader is a no-op under GL 4.3+ for
    // samplers; fabricating a Shader{locs=nullptr} would be UB on the locs path).
    auto set_i = [&](const char* name, int value) {
        GLint loc = glGetUniformLocation(program, name);
        if (loc >= 0) glUniform1i(loc, value);
    };
    set_i("triangleCount", blas.get_total_triangle_count());
    set_i("blasNodeCount",  blas.get_total_node_count());
    set_i("tlasNodeCount",  tlas.get_node_count());
    set_i("instanceCount",  tlas.get_instance_count());
}

} // namespace tileset
