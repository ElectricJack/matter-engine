// File-backed persistence for matter::props.
//
// Split out of props.cpp so the core (schema, registry, accessors, JSON
// round-trip) stays linkable without part_asset_v2.cpp and its BLAS/TLAS
// closure — the headless props test suite links only the core.

#include "matter/props.h"
#include "matter/log.h"

#include "part_asset_v2.h"  // part_asset::replace_file_atomic_detailed

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace matter {
namespace props {
namespace {

namespace fs = std::filesystem;

bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

bool save_scope_file(const Registry& r, Scope scope, const std::string& path) {
    jsondoc::Value doc;
    std::string existing;
    const bool had_file = read_text(path, existing);
    if (had_file && !jsondoc::parse_json(existing, doc)) {
        MATTER_LOGW("props", "%s: unparsable, rewriting from scratch\n", path.c_str());
        doc = jsondoc::Value();
    }

    save_scope(r, scope, doc);

    const jsondoc::Value* groups = doc.find("groups");
    const bool empty = !groups || groups->obj.empty();
    if (empty && !had_file) return true;  // no edits and no file — write nothing

    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);

    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.good()) {
            MATTER_LOGE("props", "%s: cannot open temp file for write\n", tmp.c_str());
            return false;
        }
        f << jsondoc::write_json(doc);
        if (!f.good()) {
            MATTER_LOGE("props", "%s: write failed\n", tmp.c_str());
            return false;
        }
    }

    if (part_asset::replace_file_atomic_detailed(tmp, path) ==
        part_asset::FileReplaceOutcome::NotReplaced) {
        fs::remove(tmp, ec);
        MATTER_LOGE("props", "%s: atomic replace failed\n", path.c_str());
        return false;
    }
    return true;
}

bool load_scope_file(Registry& r, Scope scope, const std::string& path) {
    std::string text;
    if (!read_text(path, text)) return false;
    jsondoc::Value doc;
    if (!jsondoc::parse_json(text, doc)) {
        MATTER_LOGW("props", "%s: unparsable, ignored\n", path.c_str());
        return false;
    }
    load_scope(r, scope, doc);
    return true;
}

bool load_group_file(Binding& b, const std::string& path) {
    std::string text;
    if (!read_text(path, text)) return false;
    jsondoc::Value doc;
    if (!jsondoc::parse_json(text, doc)) {
        MATTER_LOGW("props", "%s: unparsable, ignored\n", path.c_str());
        return false;
    }
    load_group(b, doc);
    return true;
}

}  // namespace props
}  // namespace matter
