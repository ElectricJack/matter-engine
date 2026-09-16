// MatterEngine3/src/export/part_export.cpp — see part_export.h for the
// artifact-in/files-out contract, the subtree rule and the LOD rule.

#include "export/part_export.h"

#include "export/export_text.h"
#include "tlas_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace matter_export {
namespace {

// Row-major, column-vector composition: the child's local transform applied
// first, then the parent's. Matches mat4::TransformPoint's convention (see
// tri.h), which is what BVHInstance and the flatten stage already speak.
mat4 concat(const mat4& parent, const mat4& child) {
    mat4 out;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += parent.cell[row * 4 + k] * child.cell[k * 4 + col];
            out.cell[row * 4 + col] = sum;
        }
    }
    return out;
}

bool is_identity(const mat4& m) {
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if (m.cell[row * 4 + col] != (row == col ? 1.0f : 0.0f)) return false;
    return true;
}

// Inverse-transpose of the upper 3x3, so a non-uniform scale does not skew
// normals. mat4::Inverted() returns identity for a singular matrix, which
// degrades to "leave the normals alone" rather than producing NaNs.
void normal_matrix(const mat4& m, float out[9]) {
    const mat4 inv = m.Inverted();
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out[row * 3 + col] = inv.cell[col * 4 + row];
}

float3 apply_normal(const float out[9], const float3& n) {
    float3 r = make_float3(out[0] * n.x + out[1] * n.y + out[2] * n.z,
                           out[3] * n.x + out[4] * n.y + out[5] * n.z,
                           out[6] * n.x + out[7] * n.y + out[8] * n.z);
    const float len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    if (len > 1e-20f) { r.x /= len; r.y /= len; r.z /= len; }
    return r;
}

void transform_range(std::vector<Tri>& tris, std::vector<TriEx>& triex,
                     size_t first, const mat4& xform) {
    if (is_identity(xform)) return;
    float nm[9];
    normal_matrix(xform, nm);
    for (size_t i = first; i < tris.size(); ++i) {
        Tri& t = tris[i];
        t.vertex0 = xform.TransformPoint(t.vertex0);
        t.vertex1 = xform.TransformPoint(t.vertex1);
        t.vertex2 = xform.TransformPoint(t.vertex2);
    }
    for (size_t i = first; i < triex.size(); ++i) {
        TriEx& e = triex[i];
        e.N0 = apply_normal(nm, e.N0);
        e.N1 = apply_normal(nm, e.N1);
        e.N2 = apply_normal(nm, e.N2);
    }
}

std::string join_path(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + leaf;
    return dir + "/" + leaf;
}

// Everything the recursive gather needs. `path` is the chain of hashes
// currently being expanded, which is the cycle guard.
struct GatherState {
    std::string cache_root;
    uint32_t requested_lod = 0;
    std::vector<Tri>* tris = nullptr;
    std::vector<TriEx>* triex = nullptr;
    PartExportSummary* summary = nullptr;
    std::vector<uint64_t> path;
};

void warn(GatherState& state, const std::string& message) {
    // Cap the list: a badly broken cache could otherwise produce one warning
    // per instance and swamp the manifest.
    if (state.summary->warnings.size() < 32u)
        state.summary->warnings.push_back(message);
    else if (state.summary->warnings.size() == 32u)
        state.summary->warnings.push_back("... further warnings suppressed");
}

bool gather_subtree(uint64_t hash, const mat4& xform, uint32_t depth, bool is_root,
                    GatherState& state, uint32_t& lod_count_out, std::string& error);

// Pull one rung out of a v6 FLAT artifact: every cluster contributes its
// min(lod, its own last) rung, so a cluster with a shorter ladder stays visible
// at its coarsest instead of vanishing.
bool gather_flat(const std::vector<part_asset::FlatCluster>& clusters,
                 const std::vector<part_asset::FlatInstanceRef>& refs,
                 const BLASManager& blas, uint64_t hash, const mat4& xform,
                 uint32_t depth, bool is_root, GatherState& state,
                 uint32_t& lod_count_out, std::string& error) {
    size_t max_rungs = 0;
    for (const part_asset::FlatCluster& cluster : clusters)
        max_rungs = std::max(max_rungs, cluster.lods.size());
    lod_count_out = static_cast<uint32_t>(max_rungs);
    if (max_rungs == 0u) {
        error = "flat artifact " + format_hex64(hash) + " carries no LOD rungs";
        return false;
    }
    const uint32_t lod = is_root ? state.requested_lod
                                 : std::min<uint32_t>(state.requested_lod,
                                                      static_cast<uint32_t>(max_rungs) - 1u);
    if (lod >= max_rungs) {
        error = "LOD " + std::to_string(lod) + " does not exist; part " +
                format_hex64(hash) + " has " + std::to_string(max_rungs) + " rung(s)";
        return false;
    }

    const size_t first = state.tris->size();
    for (const part_asset::FlatCluster& cluster : clusters) {
        if (cluster.lods.empty()) continue;
        const size_t use = std::min(static_cast<size_t>(lod), cluster.lods.size() - 1u);
        gather_rung_triangles(blas, cluster.lods[use].blas_indices, -1,
                              *state.tris, *state.triex);
    }
    transform_range(*state.tris, *state.triex, first, xform);

    for (const part_asset::FlatInstanceRef& ref : refs) {
        mat4 child_local;
        std::memcpy(child_local.cell, ref.transform, sizeof child_local.cell);
        uint32_t child_rungs = 0;
        std::string child_error;
        if (!gather_subtree(ref.child_resolved_hash, concat(xform, child_local),
                            depth + 1u, false, state, child_rungs, child_error)) {
            ++state.summary->skipped_subtree_refs;
            warn(state, "instance ref " + format_hex64(ref.child_resolved_hash) +
                            " not merged: " + child_error);
            continue;
        }
        ++state.summary->inlined_subtree_refs;
    }
    return true;
}

// Pull one rung out of a v2 compositional artifact and recurse into its child
// table. A compositional part's own LOD ladder is the `lods` block.
bool gather_v2(const part_asset::LodLevels& lods,
               const std::vector<part_asset::ChildInstance>& children,
               const BLASManager& blas, uint64_t hash, const mat4& xform,
               uint32_t depth, bool is_root, GatherState& state,
               uint32_t& lod_count_out, std::string& error) {
    lod_count_out = static_cast<uint32_t>(lods.size());
    const size_t first = state.tris->size();
    if (!lods.empty()) {
        const uint32_t lod =
            is_root ? state.requested_lod
                    : std::min<uint32_t>(state.requested_lod,
                                         static_cast<uint32_t>(lods.size()) - 1u);
        if (lod >= lods.size()) {
            error = "LOD " + std::to_string(lod) + " does not exist; part " +
                    format_hex64(hash) + " has " + std::to_string(lods.size()) + " rung(s)";
            return false;
        }
        gather_rung_triangles(blas, lods[lod].blas_indices, -1, *state.tris, *state.triex);
    } else {
        // No ladder at all: a leaf whose bake registered geometry without
        // recording rungs. Take every BLAS entry, which is what the artifact
        // holds and what the renderer would place.
        if (is_root && state.requested_lod != 0u) {
            error = "LOD " + std::to_string(state.requested_lod) +
                    " does not exist; part " + format_hex64(hash) +
                    " has a single unladdered mesh";
            return false;
        }
        std::vector<uint32_t> all;
        all.reserve(blas.get_entries().size());
        for (uint32_t i = 0; i < blas.get_entries().size(); ++i) all.push_back(i);
        gather_rung_triangles(blas, all, -1, *state.tris, *state.triex);
        lod_count_out = 1u;
    }
    transform_range(*state.tris, *state.triex, first, xform);

    for (const part_asset::ChildInstance& child : children) {
        mat4 child_local;
        std::memcpy(child_local.cell, child.transform, sizeof child_local.cell);
        uint32_t child_rungs = 0;
        std::string child_error;
        if (!gather_subtree(child.child_resolved_hash, concat(xform, child_local),
                            depth + 1u, false, state, child_rungs, child_error)) {
            ++state.summary->skipped_subtree_refs;
            warn(state, "child " + format_hex64(child.child_resolved_hash) +
                            " not merged: " + child_error);
            continue;
        }
        ++state.summary->inlined_subtree_refs;
    }
    return true;
}

bool gather_subtree(uint64_t hash, const mat4& xform, uint32_t depth, bool is_root,
                    GatherState& state, uint32_t& lod_count_out, std::string& error) {
    error.clear();
    lod_count_out = 0;
    if (depth > kMaxSubtreeDepth) {
        error = "subtree deeper than " + std::to_string(kMaxSubtreeDepth) + " levels";
        return false;
    }
    if (std::find(state.path.begin(), state.path.end(), hash) != state.path.end()) {
        error = "cycle: hash already on the expansion path";
        return false;
    }
    state.path.push_back(hash);
    struct PopGuard {
        std::vector<uint64_t>& path;
        ~PopGuard() { path.pop_back(); }
    } pop_guard{state.path};

    // A flat and a compositional artifact are two SECTIONS of the same bundle
    // file, so both paths name the same file and the loader picks the section.
    // Flat first: it is the merged subtree and the cheaper, more complete
    // answer whenever the flatten stage has run.
    const std::string flat_path =
        join_path(state.cache_root, part_asset::cache_path_flat(hash));
    {
        BLASManager blas;
        TLASManager tlas;
        std::vector<part_asset::FlatCluster> clusters;
        std::vector<part_asset::FlatInstanceRef> refs;
        if (part_asset::load_flat_v3(flat_path, hash, blas, tlas, clusters, refs)) {
            return gather_flat(clusters, refs, blas, hash, xform, depth, is_root, state,
                               lod_count_out, error);
        }
    }

    const std::string part_path =
        join_path(state.cache_root, part_asset::cache_path_resolved(hash));
    BLASManager blas;
    TLASManager tlas;
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    part_asset::PartAssetLoadFailure failure = part_asset::PartAssetLoadFailure::None;
    std::string reason;
    if (!part_asset::load_v2(part_path, hash, blas, tlas, children, lods, &failure, &reason)) {
        error = "no loadable artifact at '" + part_path + "'";
        if (!reason.empty()) error += " (" + reason + ")";
        return false;
    }
    return gather_v2(lods, children, blas, hash, xform, depth, is_root, state,
                     lod_count_out, error);
}

void append_json_string(std::string& out, const std::string& value) {
    out.push_back('"');
    for (char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                    out.push_back(kHex[static_cast<unsigned char>(c) & 0xF]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void append_float_array(std::string& out, const float* values, size_t count, int decimals) {
    out.push_back('[');
    for (size_t i = 0; i < count; ++i) {
        if (i != 0u) out += ", ";
        out += format_fixed(values[i], decimals);
    }
    out.push_back(']');
}

const char* format_name(TextureFormat format) {
    switch (format) {
        case TextureFormat::Png:  return "png";
        case TextureFormat::Ktx2: return "ktx2";
        default:                  return "none";
    }
}

} // namespace

bool ensure_directory(const std::string& path, std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "output directory must not be empty";
        return false;
    }
    std::error_code code;
    std::filesystem::create_directories(std::filesystem::path(path), code);
    if (!std::filesystem::is_directory(std::filesystem::path(path), code)) {
        error = "could not create output directory '" + path + "'";
        return false;
    }
    return true;
}

bool export_part(const PartExportRequest& request, const ExportSettings& settings,
                 PartExportSummary& summary, std::string& error) {
    error.clear();
    summary = PartExportSummary{};
    summary.name = request.name;
    summary.resolved_hash = request.resolved_hash;
    summary.lod = settings.lod;
    summary.instances = request.instances;

    std::vector<Tri> tris;
    std::vector<TriEx> triex;
    GatherState state;
    state.cache_root = request.cache_root;
    state.requested_lod = settings.lod;
    state.tris = &tris;
    state.triex = &triex;
    state.summary = &summary;

    uint32_t lod_count = 0;
    if (!gather_subtree(request.resolved_hash, mat4::Identity(), 0u, true, state,
                        lod_count, error)) {
        return false;
    }
    summary.lod_count = lod_count;
    if (tris.empty()) {
        error = "part " + format_hex64(request.resolved_hash) + " LOD " +
                std::to_string(settings.lod) + " contains no triangles";
        return false;
    }

    ExportOptions build_options;
    build_options.texture_size = settings.texture_size;
    build_options.gutter_texels = settings.gutter_texels;
    build_options.chart_cone_deg = settings.chart_cone_deg;
    // A geometry-only export still needs a UV set to be useful to a consumer
    // that later assigns its own textures, and the chart pass is cheap next to
    // the bake, so UVs are always generated.
    build_options.generate_uvs = true;

    ExportModel model;
    if (!build_export_model(tris, triex, request.name, request.resolved_hash,
                            settings.lod, lod_count, build_options, model, error)) {
        return false;
    }

    if (settings.texture_format != TextureFormat::None) {
        TextureBakeOptions bake_options;
        bake_options.size = settings.texture_size;
        bake_options.dilate_texels = settings.gutter_texels;
        bake_options.normal_space_flat = settings.normal_space_flat;
        // An all-black emissive map is a file nobody needs and an MTL key that
        // misrenders on a consumer without map_Ke support (see obj_writer.h),
        // so a part where nothing emits gets neither.
        bool any_emissive = false;
        for (const ExportMaterial& material : model.materials)
            for (int c = 0; c < 3; ++c)
                any_emissive = any_emissive || material.emissive[c] > 0.0f;
        bake_options.bake[static_cast<uint32_t>(MapKind::Emissive)] = any_emissive;
        if (!bake_maps(model, bake_options, error)) return false;
    }

    ObjWriteOptions write_options;
    write_options.directory = settings.out_dir;
    write_options.base_name = model.name;
    write_options.texture_format = settings.texture_format;
    if (!write_obj(model, write_options, summary.files, error)) return false;

    summary.name = model.name;
    summary.vertex_count = model.mesh.vertex_count;
    summary.triangle_count = model.mesh.triangle_count();
    summary.charts = model.charts;
    for (uint32_t m = 0; m < kMapCount; ++m)
        summary.map_hashes[m] = model.maps[m].content_hash;
    for (const ExportSubmesh& sub : model.mesh.submeshes) {
        const ExportMaterial& material = model.materials[sub.material_slot];
        MaterialSummary entry;
        entry.id = material.id;
        entry.name = material.name;
        for (int c = 0; c < 3; ++c) {
            entry.albedo[c] = material.albedo[c];
            entry.emissive[c] = material.emissive[c];
        }
        entry.roughness = material.roughness;
        entry.metallic = material.metallic;
        entry.triangles = sub.index_count / 3u;
        summary.materials.push_back(entry);
    }
    return true;
}

std::string build_manifest_json(const std::vector<PartExportSummary>& parts,
                                const ExportSettings& settings) {
    std::string out;
    out.reserve(1024u + parts.size() * 1024u);
    out += "{\n";
    out += "  \"format\": \"matter-obj-export\",\n";
    out += "  \"version\": 1,\n";
    out += "  \"units\": \"metres\",\n";
    out += "  \"up_axis\": \"Y\",\n";
    out += "  \"handedness\": \"right\",\n";
    out += "  \"front_face\": \"ccw\",\n";
    out += "  \"lod\": " + std::to_string(settings.lod) + ",\n";
    out += "  \"texture_size\": " + std::to_string(settings.texture_size) + ",\n";
    out += std::string("  \"texture_format\": \"") + format_name(settings.texture_format) +
           "\",\n";
    out += std::string("  \"normal_space\": \"") +
           (settings.normal_space_flat ? "flat" : "smooth") + "\",\n";
    out += "  \"parts\": [";
    for (size_t p = 0; p < parts.size(); ++p) {
        const PartExportSummary& part = parts[p];
        out += (p == 0u ? "\n" : ",\n");
        out += "    {\n";
        out += "      \"name\": ";
        append_json_string(out, part.name);
        out += ",\n";
        out += "      \"resolved_hash\": \"" + format_hex64(part.resolved_hash) + "\",\n";
        out += "      \"lod\": " + std::to_string(part.lod) + ",\n";
        out += "      \"lod_count\": " + std::to_string(part.lod_count) + ",\n";
        out += "      \"vertices\": " + std::to_string(part.vertex_count) + ",\n";
        out += "      \"triangles\": " + std::to_string(part.triangle_count) + ",\n";
        out += "      \"inlined_subtree_refs\": " +
               std::to_string(part.inlined_subtree_refs) + ",\n";
        out += "      \"skipped_subtree_refs\": " +
               std::to_string(part.skipped_subtree_refs) + ",\n";
        out += "      \"charts\": {\"count\": " + std::to_string(part.charts.chart_count) +
               ", \"atlas\": " + std::to_string(part.charts.atlas_size) +
               ", \"texels_per_meter\": " +
               format_fixed(part.charts.texels_per_meter, 3) +
               ", \"fill\": " + format_fixed(part.charts.packing_fill, 4) +
               ", \"max_distortion\": " + format_fixed(part.charts.max_distortion, 3) + "},\n";
        out += "      \"materials\": [";
        for (size_t m = 0; m < part.materials.size(); ++m) {
            const MaterialSummary& material = part.materials[m];
            out += (m == 0u ? "\n" : ",\n");
            out += "        {\"name\": ";
            append_json_string(out, material.name);
            out += ", \"id\": " +
                   (material.id == UINT32_MAX ? std::string("null")
                                              : std::to_string(material.id));
            out += ", \"albedo\": ";
            append_float_array(out, material.albedo, 3, kScalarDecimals);
            out += ", \"emissive\": ";
            append_float_array(out, material.emissive, 3, kScalarDecimals);
            out += ", \"roughness\": " + format_fixed(material.roughness, kScalarDecimals);
            out += ", \"metallic\": " + format_fixed(material.metallic, kScalarDecimals);
            out += ", \"triangles\": " + std::to_string(material.triangles) + "}";
        }
        out += part.materials.empty() ? "],\n" : "\n      ],\n";
        out += "      \"maps\": {";
        {
            bool first = true;
            for (uint32_t m = 0; m < kMapCount; ++m) {
                if (part.map_hashes[m] == 0u) continue;
                out += first ? "\n" : ",\n";
                first = false;
                out += std::string("        \"") + map_suffix(static_cast<MapKind>(m)) +
                       "\": \"" + format_hex64(part.map_hashes[m]) + "\"";
            }
            out += first ? "},\n" : "\n      },\n";
        }
        out += "      \"files\": [";
        for (size_t f = 0; f < part.files.files.size(); ++f) {
            const WrittenFile& file = part.files.files[f];
            out += (f == 0u ? "\n" : ",\n");
            out += "        {\"name\": ";
            append_json_string(out, file.name);
            out += ", \"bytes\": " + std::to_string(file.bytes);
            out += ", \"hash\": \"" + format_hex64(file.content_hash) + "\"}";
        }
        out += part.files.files.empty() ? "]" : "\n      ]";
        if (!part.instances.empty()) {
            out += ",\n      \"instances\": [";
            for (size_t i = 0; i < part.instances.size(); ++i) {
                out += (i == 0u ? "\n" : ",\n");
                out += "        ";
                append_float_array(out, part.instances[i].data(), 16, kPositionDecimals);
            }
            out += "\n      ]";
        }
        if (!part.warnings.empty()) {
            out += ",\n      \"warnings\": [";
            for (size_t w = 0; w < part.warnings.size(); ++w) {
                out += (w == 0u ? "\n" : ",\n");
                out += "        ";
                append_json_string(out, part.warnings[w]);
            }
            out += "\n      ]";
        }
        out += "\n    }";
    }
    out += parts.empty() ? "]\n" : "\n  ]\n";
    out += "}\n";
    return out;
}

bool write_manifest(const std::vector<PartExportSummary>& parts,
                    const ExportSettings& settings, std::string& error) {
    const std::string text = build_manifest_json(parts, settings);
    return write_file_text(join_path(settings.out_dir, "manifest.json"), text, error);
}

} // namespace matter_export
