// File-backed persistence for matter::props.
//
// Split out of props.cpp so the core (schema, registry, accessors, JSON
// round-trip) stays linkable without part_asset_v2.cpp and its BLAS/TLAS
// closure — the headless props test suite links only the core.

#include "matter/props.h"

#include "part_asset_v2.h"  // part_asset::replace_file_atomic_detailed

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace matter {
namespace props {
namespace {

namespace fs = std::filesystem;

// Whole-file read as binary. Returns false when the file cannot be opened, which
// every caller treats as "no file yet" rather than as an error.
bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

// Writes one scope's modified fields to `path`, READ-MODIFY-WRITE: the existing
// file is parsed first so keys this build does not know about (other scopes,
// other groups, future schema fields) survive the rewrite. An unparsable
// existing file is reported and rewritten from scratch.
//
// No file is created when there is nothing to persist and none existed before,
// so a clean session leaves no artifact. Parent directories are created as
// needed. The write goes to `path + ".tmp"` and is then atomically swapped into
// place, so a crash mid-write cannot leave a truncated settings file.
//
// Returns false on any write or replace failure; the caller's in-memory state is
// unaffected either way, and every failure path removes the temp file rather
// than leaving it beside the settings file.
bool save_scope_file(const Registry& r, Scope scope, const std::string& path) {
    jsondoc::Value doc;
    std::string existing;
    const bool had_file = read_text(path, existing);
    if (had_file && !jsondoc::parse_json(existing, doc)) {
        fprintf(stderr, "[props] %s: unparsable, rewriting from scratch\n", path.c_str());
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
    // `ok` rather than an early return: the stream has to be CLOSED (end of this
    // block) before the temp file can be removed on Windows, and every failure
    // path must remove it or a half-written .tmp is left beside the settings
    // file forever.
    bool ok = true;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.good()) {
            fprintf(stderr, "[props] %s: cannot open temp file for write\n", tmp.c_str());
            ok = false;
        } else {
            f << jsondoc::write_json(doc);
            if (!f.good()) {
                fprintf(stderr, "[props] %s: write failed\n", tmp.c_str());
                ok = false;
            }
        }
    }
    if (!ok) {
        fs::remove(tmp, ec);
        return false;
    }

    if (part_asset::replace_file_atomic_detailed(tmp, path) ==
        part_asset::FileReplaceOutcome::NotReplaced) {
        fs::remove(tmp, ec);
        fprintf(stderr, "[props] %s: atomic replace failed\n", path.c_str());
        return false;
    }
    return true;
}

// Applies a scope file to every binding in `scope`. Returns false for a missing
// or unparsable file — both are ordinary outcomes (no settings saved yet, or a
// corrupt file being ignored), not errors the caller must handle. Per-field
// semantics are load_group's: a sparse overlay that leaves absent and
// type-mismatched fields at their current values.
bool load_scope_file(Registry& r, Scope scope, const std::string& path) {
    std::string text;
    if (!read_text(path, text)) return false;
    jsondoc::Value doc;
    if (!jsondoc::parse_json(text, doc)) {
        fprintf(stderr, "[props] %s: unparsable, ignored\n", path.c_str());
        return false;
    }
    load_scope(r, scope, doc);
    return true;
}

// Same as load_scope_file but for a single binding, and without the scope
// filter: it applies whatever the document holds under that binding's group
// path.
bool load_group_file(Binding& b, const std::string& path) {
    std::string text;
    if (!read_text(path, text)) return false;
    jsondoc::Value doc;
    if (!jsondoc::parse_json(text, doc)) {
        fprintf(stderr, "[props] %s: unparsable, ignored\n", path.c_str());
        return false;
    }
    load_group(b, doc);
    return true;
}

}  // namespace props
}  // namespace matter
