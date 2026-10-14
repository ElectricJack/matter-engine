#pragma once

// Optional generated-world inputs. These objects never intercept filesystem
// calls: consumers explicitly select them instead of their normal disk IO.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace matter {

class SourceSnapshot {
public:
    using Files = std::map<std::string, std::string>;
    explicit SourceSnapshot(Files files, std::vector<std::string> directories = {}) {
        for (auto& entry : files) {
            const auto path = key(entry.first);
            files_.emplace(path, std::move(entry.second));
            add_directory(std::filesystem::path(path).parent_path());
        }
        for (const auto& directory : directories) add_directory(directory);
    }
    static std::string key(const std::string& path) {
        return std::filesystem::path(path).lexically_normal().generic_string();
    }
    bool read(const std::string& path, std::string& out) const {
        const auto found = files_.find(key(path));
        if (found == files_.end()) return false;
        out = found->second;
        return true;
    }
    bool exists(const std::string& path) const {
        return files_.find(key(path)) != files_.end();
    }
    bool is_directory(const std::string& path) const {
        return directories_.count(key(path)) != 0;
    }
    // Scene directories are leaves; their helpers/objects are not scenes.
    std::string scene_script(const std::string& project, const std::string& name) const {
        const std::filesystem::path base = std::filesystem::path(project) / "scenes";
        for (const auto& file : files_under(base.generic_string())) {
            const std::filesystem::path script(file);
            if (script.filename() != name + ".js" || script.parent_path().filename() != name)
                continue;
            bool nested_scene_or_objects = false;
            for (auto dir = script.parent_path(); dir != base && !dir.empty();) {
                if (dir.filename() == "objects") { nested_scene_or_objects = true; break; }
                dir = dir.parent_path();
                if (dir != base && exists((dir / (dir.filename().string() + ".js")).generic_string())) {
                    nested_scene_or_objects = true;
                    break;
                }
            }
            if (!nested_scene_or_objects) return file;
        }
        return {};
    }
    std::vector<std::string> files_under(const std::string& root) const {
        std::vector<std::string> result;
        if (root.empty()) return result;
        const std::filesystem::path base(key(root));
        for (const auto& entry : files_) {
            const auto relative = std::filesystem::path(entry.first).lexically_relative(base);
            if (relative.empty() || relative == ".") continue;
            bool outside_or_hidden = false;
            for (const auto& segment : relative) {
                const auto name = segment.string();
                if (!name.empty() && name.front() == '.') {
                    outside_or_hidden = true;
                    break;
                }
            }
            if (!outside_or_hidden) result.push_back(entry.first);
        }
        return result;
    }
    // Same tier precedence/root-first/sorted-group ordering as project_layout.
    // Generated inputs include directory metadata, without symlinks or IO errors.
    std::vector<std::string> object_roots(const std::vector<std::string>& tiers) const {
        std::vector<std::string> result;
        std::set<std::string> visited;
        for (const auto& tier : tiers) {
            if (tier.empty()) continue;
            const auto root = key(tier);
            if (!visited.insert(root).second) continue;
            std::set<std::string> groups;
            for (const auto& directory : directories_) {
                const auto relative = std::filesystem::path(directory).lexically_relative(root);
                if (relative.empty() || relative == ".") continue;
                bool hidden_or_outside = false;
                for (const auto& segment : relative)
                    if (!segment.string().empty() && segment.string().front() == '.')
                        hidden_or_outside = true;
                if (!hidden_or_outside) groups.insert(directory);
            }
            result.push_back(root);
            for (const auto& group : groups) {
                visited.insert(group);
                result.push_back(group);
            }
        }
        return result;
    }
private:
    void add_directory(std::filesystem::path path) {
        path = path.lexically_normal();
        while (!path.empty()) {
            directories_.insert(path.generic_string());
            const auto parent = path.parent_path();
            if (parent == path) break;
            path = parent;
        }
    }
    Files files_;
    std::set<std::string> directories_;
};

// Cache bytes still run through the production serializers and validators.
// An open handle observes one immutable generation for its entire lifetime.
// Failed publication must preserve the previous generation. Implementations
// synchronize open/publish; the bundle's existing lock still protects merges.
class CacheByteStorage {
public:
    using Bytes = std::vector<std::uint8_t>;
    using ReadHandle = std::shared_ptr<const Bytes>;
    virtual ~CacheByteStorage() = default;
    virtual ReadHandle open(const std::string& path) const = 0;
    virtual bool publish(const std::string& path, const Bytes& bytes) = 0;
    virtual bool erase(const std::string& path) = 0;
    bool exists(const std::string& path) const { return bool(open(path)); }
};

// Shared explicitly by the session, provider and each scheduled operation;
// cancellation/join retain its lifetime through the last worker access.
struct WorldFixtureIO {
    std::shared_ptr<const SourceSnapshot> sources;
    std::shared_ptr<CacheByteStorage> cache;
};

} // namespace matter
