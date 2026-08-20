// MatterEngine3/src/shader_source.cpp
//
// Resolves a shader's logical path to its GLSL source text. Three sources are
// tried in a fixed order and the FIRST that yields a readable file wins:
//
//   1. $MATTER_SHADER_DIR/<logical_path>   -- the environment override, read
//      fresh on every call so a shader can be re-read after an edit without
//      restarting;
//   2. <override dir>/<logical_path>       -- EngineDesc::shader_dir, plumbed
//      in through set_shader_override_dir();
//   3. the generated embedded table        -- shaders_gen/embedded_shaders.h,
//      produced by the build and linked into the binary, which is what makes a
//      shipped exe self-contained.
//
// Only the disk paths can be edited at runtime; the embedded table is the
// fallback and is always present. A miss in all three is an error, not an
// empty string -- see shader_text().
//
// This is the GLSL SOURCE path (used where a shader is compiled at runtime).
// It is distinct from the SPIR-V that shaders_vk/ compiles into
// shaders_gen/embedded_spirv.h at build time.
//
// Threading: the override directory is a file-static string with no lock, so
// set it during startup and treat lookups afterwards as read-only.
#include "shader_source.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../shaders_gen/embedded_shaders.h"

namespace matter {

// Process-wide override directory, empty when unset. Deliberately a plain
// static: it is written once at startup by set_shader_override_dir() and read
// by every later lookup, with no synchronization for either.
static std::string g_override_dir;

void set_shader_override_dir(const char* dir_or_null) {
    g_override_dir = dir_or_null ? dir_or_null : "";
}

// Slurp a whole file into `out`. Returns false ONLY when the file cannot be
// opened -- which is what makes it usable as an "is there an override here?"
// probe.
//
// Two consequences worth knowing:
//   * a zero-byte file returns TRUE with `out` empty, so an empty override
//     file shadows the embedded shader with nothing;
//   * a short read is not an error: `out` is resized down to the bytes
//     actually read, so a truncated file yields truncated source rather than a
//     failure.
static bool read_file(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)(n < 0 ? 0 : n));
    size_t rd = n > 0 ? fread(&out[0], 1, (size_t)n, f) : 0;
    fclose(f);
    out.resize(rd);
    return true;
}

// Env override, then the set override dir, then the embedded table -- see the
// file header for why that order. The embedded lookup is a linear scan with a
// strcmp per entry; the table has a few dozen rows and this runs at pipeline
// build time, so it has never been worth indexing.
//
// Returns false with `err` set only when the path matches nowhere, which is a
// genuine error (a typo in a logical path, or a shader the build did not
// embed) and never a normal outcome.
bool shader_text(const char* logical_path, std::string& out, std::string& err) {
    if (const char* env = getenv("MATTER_SHADER_DIR")) {
        if (read_file(std::string(env) + "/" + logical_path, out)) return true;
    }
    if (!g_override_dir.empty()) {
        if (read_file(g_override_dir + "/" + logical_path, out)) return true;
    }
    for (int i = 0; i < matter_embedded::kEmbeddedShaderCount; ++i) {
        if (strcmp(matter_embedded::kEmbeddedShaders[i].path, logical_path) == 0) {
            out = matter_embedded::kEmbeddedShaders[i].text;
            return true;
        }
    }
    err = std::string("shader_text: unknown shader '") + logical_path +
          "' (not on disk override, not embedded)";
    return false;
}

} // namespace matter
