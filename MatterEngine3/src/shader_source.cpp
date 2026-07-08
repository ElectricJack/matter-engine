#include "shader_source.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../shaders_gen/embedded_shaders.h"

namespace matter {

static std::string g_override_dir;

void set_shader_override_dir(const char* dir_or_null) {
    g_override_dir = dir_or_null ? dir_or_null : "";
}

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
