#pragma once

// Filesystem-only project discovery shared by the engine, editor and tests.
// Folders organize content; scene and object identities remain filename stems.
#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace matter::project_layout {
namespace fs = std::filesystem;

inline bool hidden(const fs::path& path) {
    const auto name = path.filename().string();
    return !name.empty() && name.front() == '.';
}

// Recognized scene directories are leaves: their objects, helpers and fixtures
// cannot accidentally become scenes. Symlink directories are not followed.
inline std::vector<fs::path> scene_scripts(const fs::path& scenes_dir) {
    std::vector<fs::path> scripts;
    std::map<std::string, fs::path> names;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(scenes_dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        if (hidden(it->path()) || it->path().filename() == "objects" || it->is_symlink(ec)) {
            it.disable_recursion_pending();
            continue;
        }
        const auto name = it->path().filename().string();
        const auto script = it->path() / (name + ".js");
        if (!fs::is_regular_file(script, ec)) { ec.clear(); continue; }
        const auto prior = names.emplace(name, script);
        if (!prior.second)
            throw std::runtime_error("Duplicate scene '" + name + "': " +
                prior.first->second.string() + " and " + script.string());
        scripts.push_back(script);
        it.disable_recursion_pending();
    }
    std::sort(scripts.begin(), scripts.end());
    return scripts;
}

inline fs::path scene_script(const fs::path& project, const std::string& name) {
    for (const auto& script : scene_scripts(project / "scenes"))
        if (script.stem() == name) return script;
    return {};
}

// Expand each tier separately, retaining scene-before-project precedence.
// Duplicate module names INSIDE a tier are errors, not directory-order choices.
// Empty/nonexistent roots retain the legacy behavior (no cwd lookup).
inline std::vector<std::string> object_roots(const std::vector<std::string>& tiers) {
    std::vector<std::string> roots;
    std::set<fs::path> visited;
    for (const auto& tier : tiers) {
        if (tier.empty()) continue;
        const fs::path root(tier);
        if (!visited.insert(root.lexically_normal()).second) continue;
        std::vector<fs::path> dirs{root};
        std::map<std::string, fs::path> names;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (hidden(it->path()) || it->is_symlink(ec)) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_directory(ec)) {
                dirs.push_back(it->path());
            } else if (it->is_regular_file(ec) && it->path().extension() == ".js") {
                const auto name = it->path().stem().string();
                const auto prior = names.emplace(name, it->path());
                if (!prior.second)
                    throw std::runtime_error("Duplicate object '" + name + "' in tier " + tier +
                        ": " + prior.first->second.string() + " and " + it->path().string());
            }
        }
        std::sort(dirs.begin() + 1, dirs.end());
        for (const auto& dir : dirs) {
            visited.insert(dir.lexically_normal());
            roots.push_back(dir.string());
        }
    }
    return roots;
}

inline std::vector<fs::path> object_files(const fs::path& tier) {
    std::vector<fs::path> files;
    for (const auto& dir : object_roots({tier.string()})) {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (!hidden(it->path()) && !it->is_symlink(ec) &&
                it->is_regular_file(ec) && it->path().extension() == ".js")
                files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

inline fs::path object_source(const std::vector<std::string>& tiers, const std::string& module) {
    for (const auto& root : object_roots(tiers)) {
        const auto file = fs::path(root) / (module + ".js");
        std::error_code ec;
        if (fs::is_regular_file(file, ec)) return file;
    }
    return {};
}
} // namespace matter::project_layout
