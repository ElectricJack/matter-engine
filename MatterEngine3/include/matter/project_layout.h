#pragma once

// Filesystem-only project discovery shared by the engine, editor and tests.
// Folders organize content; scene and object identities remain filename stems.
#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace matter::project_layout {
namespace fs = std::filesystem;

inline bool hidden(const fs::path& path) {
    const auto name = path.filename().string();
    return !name.empty() && name.front() == '.';
}

// Discovery never throws. A duplicate identity inside one tier is a content
// error the caller decides how to surface; the first entry in sorted directory
// order wins so the choice is stable across machines.
struct Diagnostics { std::vector<std::string> duplicates; };
inline void note_duplicate(Diagnostics* d, std::string message) {
    if (d) d->duplicates.push_back(std::move(message));
}

// Recognized scene directories are leaves: their objects, helpers and fixtures
// cannot accidentally become scenes. Symlink directories are not followed.
inline std::vector<fs::path> scene_scripts(const fs::path& scenes_dir, Diagnostics* diagnostics = nullptr) {
    // Directory iteration order is unspecified, so collect every candidate
    // first and dedupe in sorted path order.
    std::vector<std::pair<std::string, fs::path>> candidates;
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
        candidates.emplace_back(name, script);
        it.disable_recursion_pending();
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<fs::path> scripts;
    std::map<std::string, fs::path> names;
    for (const auto& [name, script] : candidates) {
        const auto prior = names.emplace(name, script);
        if (!prior.second) {
            note_duplicate(diagnostics, "Duplicate scene '" + name + "': " +
                prior.first->second.string() + " and " + script.string());
            continue;
        }
        scripts.push_back(script);
    }
    std::sort(scripts.begin(), scripts.end());
    return scripts;
}

inline fs::path scene_script(const fs::path& project, const std::string& name, Diagnostics* diagnostics = nullptr) {
    for (const auto& script : scene_scripts(project / "scenes", diagnostics))
        if (script.stem() == name) return script;
    return {};
}

// Expand each tier separately, retaining scene-before-project precedence.
// Duplicate module names INSIDE a tier are content errors: they are reported
// through `diagnostics`, and resolution takes the copy in the first directory
// of the returned (sorted) root order.
// Empty/nonexistent roots retain the legacy behavior (no cwd lookup).
inline std::vector<std::string> object_roots(const std::vector<std::string>& tiers, Diagnostics* diagnostics = nullptr) {
    std::vector<std::string> roots;
    std::set<fs::path> visited;
    for (const auto& tier : tiers) {
        if (tier.empty()) continue;
        const fs::path root(tier);
        if (!visited.insert(root.lexically_normal()).second) continue;
        std::vector<fs::path> dirs{root};
        std::vector<fs::path> files;
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
                files.push_back(it->path());
            }
        }
        std::sort(dirs.begin() + 1, dirs.end());
        // Same order as `dirs` (the tier root sorts first as everyone's
        // prefix), so the reported winner is the copy resolution picks.
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return a.parent_path() != b.parent_path() ? a.parent_path() < b.parent_path() : a < b;
        });
        std::map<std::string, fs::path> names;
        for (const auto& file : files) {
            const auto name = file.stem().string();
            const auto prior = names.emplace(name, file);
            if (!prior.second)
                note_duplicate(diagnostics, "Duplicate object '" + name + "' in tier " + tier +
                    ": " + prior.first->second.string() + " and " + file.string());
        }
        for (const auto& dir : dirs) {
            visited.insert(dir.lexically_normal());
            roots.push_back(dir.string());
        }
    }
    return roots;
}

inline std::vector<fs::path> object_files(const fs::path& tier, Diagnostics* diagnostics = nullptr) {
    std::vector<fs::path> files;
    for (const auto& dir : object_roots({tier.string()}, diagnostics)) {
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
