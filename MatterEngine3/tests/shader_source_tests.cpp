// Shader lookup behavior uses the production lookup with in-memory overrides.
#include "shader_source.h"
#include "check.h"
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {
void environment_override(const char* value) {
#ifdef _WIN32
    _putenv_s("MATTER_SHADER_DIR", value ? value : "");
#else
    if (value) setenv("MATTER_SHADER_DIR", value, 1);
    else unsetenv("MATTER_SHADER_DIR");
#endif
}
}

int main() {
    const char* prior = std::getenv("MATTER_SHADER_DIR");
    const bool had_prior = prior != nullptr;
    const std::string saved = prior ? prior : "";
    struct Restore {
        bool present;
        std::string value;
        ~Restore() {
            environment_override(present ? value.c_str() : nullptr);
            matter::set_shader_override_dir(nullptr);
        }
    } restore{had_prior, saved};
    environment_override(nullptr);
    matter::set_shader_override_dir(nullptr);

    std::map<std::string, std::string> sources;
    std::vector<std::string> probes;
    const matter::ShaderSourceReader reader = [&](const std::string& path,
                                                   std::string& out) {
        probes.push_back(path);
        const auto found = sources.find(path);
        if (found == sources.end()) return false;
        out = found->second;
        return true;
    };
    const char* key = "shaders/bvh_tlas_common.glsl";
    std::string text, error;
    CHECK(matter::shader_text(key, text, error, reader) &&
              text.find("layout") != std::string::npos && probes.empty(),
          "embedded lookup works without external source probes");
    const auto embedded = text;
    CHECK(!matter::shader_text("shaders/nope.fs", text, error, reader) &&
              error.find("nope.fs") != std::string::npos,
          "unknown logical path fails with its name");

    matter::set_shader_override_dir("override");
    sources[std::string("override/") + key] = "// OVERRIDE MARKER\n";
    CHECK(matter::shader_text(key, text, error, reader) &&
              text == "// OVERRIDE MARKER\n",
          "explicit source override shadows embedded source");
    sources[std::string("override/") + key].clear();
    CHECK(matter::shader_text(key, text, error, reader) && text.empty(),
          "a readable empty override shadows embedded source");
    sources.clear();
    CHECK(matter::shader_text(key, text, error, reader) && text == embedded,
          "unreadable override falls through to embedded source");

    environment_override("environment");
    sources[std::string("environment/") + key] = "environment source";
    sources[std::string("override/") + key] = "explicit source";
    probes.clear();
    CHECK(matter::shader_text(key, text, error, reader) &&
              text == "environment source" && probes.size() == 1 &&
              probes.front() == std::string("environment/") + key,
          "environment source wins without probing the explicit override");
    sources.erase(std::string("environment/") + key);
    probes.clear();
    CHECK(matter::shader_text(key, text, error, reader) &&
              text == "explicit source" && probes.size() == 2,
          "unreadable environment source falls through to explicit override");
    sources[std::string("environment/") + key] = "edited source";
    CHECK(matter::shader_text(key, text, error, reader) && text == "edited source",
          "environment source is reread on each lookup");
    return check_summary();
}
