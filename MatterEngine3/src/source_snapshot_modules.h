#pragma once
#include "matter/fixture_io.h"
#include "module_resolver.h"

namespace matter {
// Bind the selected source generation to both hashing and evaluation. The
// scripting owner validates imports and owns transitive/alias/cycle folding;
// this adapter supplies the same first-root file selection as the disk path.
inline std::vector<module_resolver::ResolvedModule> source_snapshot_modules(
    const SourceSnapshot& sources, const std::vector<std::string>& ordered_roots) {
    std::map<std::string, module_resolver::ResolvedModule> selected;
    for (const auto& root : ordered_roots) {
        const std::filesystem::path base(SourceSnapshot::key(root));
        for (const auto& path : sources.files_under(root)) {
            const auto relative = std::filesystem::path(path).lexically_relative(base);
            if (relative.has_parent_path() || relative.extension() != ".js") continue;
            const auto name = relative.stem().string();
            if (name.empty() || name.front() == '.' || name.find("..") != std::string::npos)
                continue;
            const std::string specifier = "shared-lib/" + name;
            if (selected.count(specifier)) continue;
            std::string bytes;
            if (sources.read(path, bytes))
                selected.emplace(specifier,
                    module_resolver::ResolvedModule{specifier, std::move(bytes), path});
        }
    }
    std::vector<module_resolver::ResolvedModule> result;
    result.reserve(selected.size());
    for (auto& entry : selected) result.push_back(std::move(entry.second));
    return result;
}
} // namespace matter
