#pragma once

// MatterEngine3/tests/obj_parser.h
//
// A small, self-contained Wavefront OBJ/MTL reader, vendored HERE (in tests/)
// rather than in the engine because it exists for exactly one purpose: to read
// back what MatterEngine3/src/export wrote and check it, WITHOUT sharing any
// code with the writer. A round-trip test that parses with the writer's own
// helpers proves nothing; this parser was written from the format, not from
// obj_writer.cpp.
//
// The spec this exporter targets asked for "a small vendored OBJ parser, or
// three.js under node". Node is not an option here: the repo has no
// package.json anywhere (CLAUDE.md forbids adding one, because the same .js
// files are loaded by the engine's QuickJS host), so there is nowhere to
// install three from. Hence the parser.
//
// WHAT IT SUPPORTS: comments, `mtllib`, `o`, `g`, `v`, `vn`, `vt`, `usemtl`,
// and `f` with 1-based or negative-relative indices in the `v`, `v/vt`,
// `v//vn` and `v/vt/vn` spellings, with 3 or more corners (fanned into
// triangles). MTL: `newmtl` plus every other line as a key with its raw
// remainder, which is all a reference check needs.
//
// WHAT IT DOES NOT: smoothing groups, free-form surfaces, line elements,
// per-face material inheritance across files. None of those appear in the
// exporter's output, and a parser that silently accepted them would weaken the
// test.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace obj_parse {

struct Corner {
    int position = 0;   // 0-based, resolved; -1 when absent
    int uv = -1;
    int normal = -1;
};

struct Face {
    Corner corner[3];
    std::string material;   // usemtl in effect
};

struct ObjFile {
    std::string mtllib;
    std::vector<std::string> objects;
    std::vector<float> positions;   // 3 per vertex
    std::vector<float> normals;     // 3 per vertex
    std::vector<float> uvs;         // 2 per vertex
    std::vector<Face> faces;
    std::vector<std::string> materials_used;   // in first-use order
    std::string error;

    size_t position_count() const { return positions.size() / 3u; }
    size_t normal_count() const { return normals.size() / 3u; }
    size_t uv_count() const { return uvs.size() / 2u; }
    bool ok() const { return error.empty(); }
};

struct MtlMaterial {
    std::string name;
    std::vector<std::pair<std::string, std::string>> entries;   // key -> remainder

    const std::string* find(const std::string& key) const {
        for (const auto& entry : entries)
            if (entry.first == key) return &entry.second;
        return nullptr;
    }
    bool has(const std::string& key) const { return find(key) != nullptr; }
};

struct MtlFile {
    std::vector<MtlMaterial> materials;
    std::string error;

    const MtlMaterial* find(const std::string& name) const {
        for (const MtlMaterial& material : materials)
            if (material.name == name) return &material;
        return nullptr;
    }
    bool ok() const { return error.empty(); }
};

inline bool read_text_file(const std::string& path, std::string& out) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    out.clear();
    char buffer[8192];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0)
        out.append(buffer, read);
    std::fclose(file);
    return true;
}

namespace detail {

inline std::vector<std::string> split_tokens(const std::string& line) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        const size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i > start) tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

inline std::string trim(const std::string& text) {
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

// Resolve an OBJ index string: 1-based positive, or negative relative to the
// current count. Returns false on an unparseable or out-of-range value.
inline bool resolve_index(const std::string& token, size_t count, int& out) {
    if (token.empty()) { out = -1; return true; }
    char* end = nullptr;
    const long value = std::strtol(token.c_str(), &end, 10);
    if (end == token.c_str() || *end != '\0' || value == 0) return false;
    const long resolved = (value > 0) ? (value - 1) : (static_cast<long>(count) + value);
    if (resolved < 0 || resolved >= static_cast<long>(count)) return false;
    out = static_cast<int>(resolved);
    return true;
}

inline bool parse_corner(const std::string& token, const ObjFile& file, Corner& out) {
    std::string parts[3];
    int slot = 0;
    for (char c : token) {
        if (c == '/') {
            if (++slot > 2) return false;
            continue;
        }
        parts[slot].push_back(c);
    }
    if (!resolve_index(parts[0], file.position_count(), out.position) || out.position < 0)
        return false;
    if (!resolve_index(parts[1], file.uv_count(), out.uv)) return false;
    if (!resolve_index(parts[2], file.normal_count(), out.normal)) return false;
    return true;
}

} // namespace detail

inline ObjFile parse_obj(const std::string& text) {
    ObjFile file;
    std::string current_material;
    size_t line_number = 0;
    size_t cursor = 0;
    while (cursor <= text.size()) {
        const size_t newline = text.find('\n', cursor);
        const std::string raw = text.substr(
            cursor, (newline == std::string::npos ? text.size() : newline) - cursor);
        cursor = (newline == std::string::npos) ? text.size() + 1u : newline + 1u;
        ++line_number;

        const std::string line = detail::trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> tokens = detail::split_tokens(line);
        if (tokens.empty()) continue;
        const std::string& verb = tokens[0];

        auto fail = [&](const char* what) {
            if (file.error.empty())
                file.error = std::string("line ") + std::to_string(line_number) + ": " + what;
        };

        if (verb == "v" || verb == "vn") {
            if (tokens.size() < 4u) { fail("expected 3 numbers"); continue; }
            std::vector<float>& target = (verb == "v") ? file.positions : file.normals;
            for (int k = 1; k <= 3; ++k) target.push_back(std::strtof(tokens[k].c_str(), nullptr));
        } else if (verb == "vt") {
            if (tokens.size() < 3u) { fail("expected 2 numbers"); continue; }
            for (int k = 1; k <= 2; ++k) file.uvs.push_back(std::strtof(tokens[k].c_str(), nullptr));
        } else if (verb == "f") {
            if (tokens.size() < 4u) { fail("face needs at least 3 corners"); continue; }
            std::vector<Corner> corners;
            bool ok = true;
            for (size_t k = 1; k < tokens.size(); ++k) {
                Corner corner;
                if (!detail::parse_corner(tokens[k], file, corner)) { ok = false; break; }
                corners.push_back(corner);
            }
            if (!ok) { fail("bad face corner"); continue; }
            for (size_t k = 2; k < corners.size(); ++k) {
                Face face;
                face.corner[0] = corners[0];
                face.corner[1] = corners[k - 1u];
                face.corner[2] = corners[k];
                face.material = current_material;
                file.faces.push_back(face);
            }
        } else if (verb == "usemtl") {
            if (tokens.size() < 2u) { fail("usemtl needs a name"); continue; }
            current_material = tokens[1];
            bool seen = false;
            for (const std::string& name : file.materials_used)
                if (name == current_material) { seen = true; break; }
            if (!seen) file.materials_used.push_back(current_material);
        } else if (verb == "mtllib") {
            if (tokens.size() < 2u) { fail("mtllib needs a name"); continue; }
            file.mtllib = tokens[1];
        } else if (verb == "o" || verb == "g") {
            if (tokens.size() >= 2u) file.objects.push_back(tokens[1]);
        } else {
            fail(("unexpected verb '" + verb + "'").c_str());
        }
    }
    return file;
}

inline MtlFile parse_mtl(const std::string& text) {
    MtlFile file;
    size_t line_number = 0;
    size_t cursor = 0;
    while (cursor <= text.size()) {
        const size_t newline = text.find('\n', cursor);
        const std::string raw = text.substr(
            cursor, (newline == std::string::npos ? text.size() : newline) - cursor);
        cursor = (newline == std::string::npos) ? text.size() + 1u : newline + 1u;
        ++line_number;

        const std::string line = detail::trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const size_t space = line.find_first_of(" \t");
        const std::string key = line.substr(0, space);
        const std::string value =
            (space == std::string::npos) ? std::string() : detail::trim(line.substr(space + 1u));

        if (key == "newmtl") {
            if (value.empty()) {
                if (file.error.empty())
                    file.error = "line " + std::to_string(line_number) + ": newmtl needs a name";
                continue;
            }
            MtlMaterial material;
            material.name = value;
            file.materials.push_back(material);
            continue;
        }
        if (file.materials.empty()) {
            if (file.error.empty())
                file.error = "line " + std::to_string(line_number) + ": key before any newmtl";
            continue;
        }
        file.materials.back().entries.emplace_back(key, value);
    }
    return file;
}

// The value of a map_* style key with any leading option flags stripped
// ("-bm 1.0 foo.png" -> "foo.png"). Returns "" when the key is absent.
inline std::string map_path(const MtlMaterial& material, const std::string& key) {
    const std::string* value = material.find(key);
    if (!value) return std::string();
    const std::vector<std::string> tokens = detail::split_tokens(*value);
    return tokens.empty() ? std::string() : tokens.back();
}

} // namespace obj_parse
