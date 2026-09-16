// MatterEngine3/src/export/mesh_export.cpp — see mesh_export.h for what this
// layer is and why it is format-agnostic. This file is the four stages that
// header describes, in order: gather, chart, weld, group.

#include "export/mesh_export.h"

// Relative, the way lod_bake.cpp reaches the same header: MeshChartingLib is
// compiled from source by its consumers and its include dir is not on every
// build's search path (MatterEngine3/tests/Makefile in particular).
#include "../../../libs/MeshChartingLib/include/mesh_charting.h"
#include "render/indexed_part_geometry.h"   // viewer::build_indexed_part_geometry

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace matter_export {
namespace {

inline float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// -0.0f and +0.0f are the same number but different bits, and the engine's weld
// (build_indexed_part_geometry) compares attribute BYTES. A mesher that produces
// a normal of (1, -0.0, 0) on one triangle and (1, +0.0, 0) on its neighbour
// therefore splits a vertex that should have merged, and — worse for this
// exporter — which of the two a cross product lands on can differ between
// compilers and optimisation levels, which would make the "byte-identical
// export" claim false. Normalising the sign of zero before the weld costs one
// compare per float and removes both problems.
inline void canonicalize_zero(float& v) {
    if (v == 0.0f) v = 0.0f;
}

// A chart whose projected bounding box collapses in one axis (a fan of
// triangles seen exactly edge-on, which segment_charts can produce for a
// one-triangle chart) would pack to a zero-texel block and divide by zero in
// the UV map. One micrometre of extent costs nothing and keeps every chart at
// least one texel wide.
constexpr float kMinChartExtentMeters = 1e-6f;

std::string sanitize_identifier(const std::string& raw, const std::string& fallback) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        out.push_back(ok ? c : '_');
    }
    // Leading digits and dots make poor file-name stems and poor OBJ object
    // names; a leading underscore is both legal and obviously synthetic.
    while (!out.empty() && (out.front() == '.' || out.front() == '-')) out.erase(out.begin());
    if (out.empty()) return fallback;
    return out;
}

// Neutral TriEx for a BLAS entry that carries none: face normal on all three
// corners, no tint, unoccluded. Mirrors part_flatten.cpp's neutral_triex_for so
// a chartless/attribute-less entry exports the same way it flattens.
TriEx neutral_triex_for(const Tri& t, int material) {
    TriEx ex{};
    const float e1[3] = {t.vertex1.x - t.vertex0.x, t.vertex1.y - t.vertex0.y,
                         t.vertex1.z - t.vertex0.z};
    const float e2[3] = {t.vertex2.x - t.vertex0.x, t.vertex2.y - t.vertex0.y,
                         t.vertex2.z - t.vertex0.z};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                  e1[2] * e2[0] - e1[0] * e2[2],
                  e1[0] * e2[1] - e1[1] * e2[0]};
    const float len = std::sqrt(dot3(n, n));
    if (len > 0.0f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    else { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
    ex.N0 = make_float3(n[0], n[1], n[2]);
    ex.N1 = ex.N0;
    ex.N2 = ex.N0;
    ex.uv0 = float2(0.0f, 0.0f);
    ex.uv1 = float2(0.0f, 0.0f);
    ex.uv2 = float2(0.0f, 0.0f);
    ex.materialId = material;
    ex.tint = float4(1.0f, 1.0f, 1.0f, 0.0f);
    ex.ao0 = ex.ao1 = ex.ao2 = 1.0f;
    return ex;
}

} // namespace

const char* map_suffix(MapKind kind) {
    switch (kind) {
        case MapKind::Albedo:    return "albedo";
        case MapKind::Normal:    return "normal";
        case MapKind::Roughness: return "roughness";
        case MapKind::Metallic:  return "metallic";
        case MapKind::Emissive:  return "emissive";
        case MapKind::Occlusion: return "ao";
        default:                 return "unknown";
    }
}

uint64_t fnv1a64_bytes(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

void finalize_image_hash(ExportImage& image) {
    if (image.empty()) { image.content_hash = 0; return; }
    const uint32_t header[3] = {image.width, image.height, image.channels};
    uint64_t h = fnv1a64_bytes(header, sizeof header);
    image.content_hash = fnv1a64_bytes(image.texels.data(), image.texels.size(), h);
}

bool describe_material(uint32_t material_id, ExportMaterial& out) {
    out = ExportMaterial{};
    out.id = material_id;

    const int count = MaterialRegistryCount();
    const int id = static_cast<int>(material_id);
    const MaterialDef* def =
        (id >= 0 && id < count) ? MaterialRegistryGet(id) : nullptr;
    if (!def) {
        out.name = "mat_" + std::to_string(material_id) + "_missing";
        out.albedo[0] = out.albedo[1] = out.albedo[2] = 0.5f;
        out.roughness = 1.0f;
        out.metallic = 0.0f;
        return false;
    }

    const char* registry_name = MaterialRegistryNameOf(id);
    out.name = registry_name
                   ? sanitize_identifier(registry_name, "mat") + "_" + std::to_string(material_id)
                   : "mat_" + std::to_string(material_id);
    out.albedo[0] = def->albedo[0];
    out.albedo[1] = def->albedo[1];
    out.albedo[2] = def->albedo[2];
    for (int c = 0; c < 3; ++c) out.emissive[c] = def->emissionColor[c] * def->emission;
    out.roughness = def->roughness;
    out.metallic = def->metallic;
    out.opacity = def->opacity;
    out.ior = def->ior;
    out.alpha_cutoff = def->alphaCutoff;
    out.alpha_tested = (def->surfaceFlags & MATERIAL_ALPHA_TESTED) != 0u;
    out.double_sided = (def->surfaceFlags & MATERIAL_DOUBLE_SIDED) != 0u;
    return true;
}

uint32_t gather_rung_triangles(const BLASManager& blas,
                               const std::vector<uint32_t>& blas_indices,
                               int default_material,
                               std::vector<Tri>& tris_out,
                               std::vector<TriEx>& triex_out) {
    const auto& entries = blas.get_entries();
    const size_t before = tris_out.size();
    for (uint32_t bi : blas_indices) {
        if (bi >= entries.size()) continue;
        const BLASManager::BLASEntry& e = *entries[bi];
        tris_out.insert(tris_out.end(), e.triangles.begin(), e.triangles.end());
        if (e.tri_extra.size() == e.triangles.size()) {
            triex_out.insert(triex_out.end(), e.tri_extra.begin(), e.tri_extra.end());
        } else {
            for (const Tri& t : e.triangles)
                triex_out.push_back(neutral_triex_for(t, default_material));
        }
    }
    return static_cast<uint32_t>(tris_out.size() - before);
}

// ---------------------------------------------------------------------------
// Chart + UV generation
// ---------------------------------------------------------------------------
//
// The soup handed to MeshChartingLib is one vertex per triangle CORNER with an
// identity index buffer: build_adjacency welds by exact position itself, so
// connectivity is recovered from the geometry and the caller does not have to
// pre-weld (and must not, since the weld that matters for output happens after
// UVs exist).
namespace {

struct ChartFrames {
    std::vector<int> chart_of_tri;
    int chart_count = 0;
    std::vector<float> tangent;    // 3 per chart
    std::vector<float> bitangent;  // 3 per chart
    std::vector<float> min_uv;     // 2 per chart, plane-space metres
};

bool compute_chart_frames(const std::vector<float>& positions,
                          const std::vector<uint32_t>& indices,
                          int tri_count,
                          float cone_deg,
                          ChartFrames& out,
                          std::vector<mesh_charting::ChartRect>& rects,
                          float& max_distortion) {
    const std::vector<mesh_charting::TriAdj> adj =
        mesh_charting::build_adjacency(positions.data(), indices.data(), tri_count);
    out.chart_of_tri = mesh_charting::segment_charts(
        positions.data(), indices.data(), tri_count, adj, cone_deg, out.chart_count);
    if (out.chart_count <= 0) return false;

    const std::vector<float> normals = mesh_charting::chart_average_normals(
        positions.data(), indices.data(), tri_count, out.chart_of_tri, out.chart_count);

    out.tangent.assign(static_cast<size_t>(out.chart_count) * 3u, 0.0f);
    out.bitangent.assign(static_cast<size_t>(out.chart_count) * 3u, 0.0f);
    out.min_uv.assign(static_cast<size_t>(out.chart_count) * 2u, 0.0f);
    rects.assign(static_cast<size_t>(out.chart_count), mesh_charting::ChartRect{});

    // Triangle lists per chart, in ascending triangle order — used for the
    // bounding box and for the distortion probe, and deterministic by
    // construction.
    std::vector<std::vector<int>> tris_of_chart(static_cast<size_t>(out.chart_count));
    for (int t = 0; t < tri_count; ++t) {
        const int c = out.chart_of_tri[static_cast<size_t>(t)];
        if (c >= 0 && c < out.chart_count) tris_of_chart[static_cast<size_t>(c)].push_back(t);
    }

    max_distortion = 0.0f;
    for (int c = 0; c < out.chart_count; ++c) {
        float T[3], B[3];
        mesh_charting::plane_basis(&normals[static_cast<size_t>(c) * 3u], T, B);
        std::memcpy(&out.tangent[static_cast<size_t>(c) * 3u], T, sizeof T);
        std::memcpy(&out.bitangent[static_cast<size_t>(c) * 3u], B, sizeof B);

        float lo_u = std::numeric_limits<float>::max();
        float lo_v = std::numeric_limits<float>::max();
        float hi_u = -std::numeric_limits<float>::max();
        float hi_v = -std::numeric_limits<float>::max();
        for (int t : tris_of_chart[static_cast<size_t>(c)]) {
            for (int k = 0; k < 3; ++k) {
                const float* p = &positions[static_cast<size_t>(indices[static_cast<size_t>(t) * 3u + k]) * 3u];
                const float u = dot3(p, T);
                const float v = dot3(p, B);
                lo_u = std::min(lo_u, u); hi_u = std::max(hi_u, u);
                lo_v = std::min(lo_v, v); hi_v = std::max(hi_v, v);
            }
        }
        if (lo_u > hi_u) { lo_u = hi_u = 0.0f; }
        if (lo_v > hi_v) { lo_v = hi_v = 0.0f; }

        out.min_uv[static_cast<size_t>(c) * 2u + 0u] = lo_u;
        out.min_uv[static_cast<size_t>(c) * 2u + 1u] = lo_v;
        rects[static_cast<size_t>(c)].minU = lo_u;
        rects[static_cast<size_t>(c)].minV = lo_v;
        rects[static_cast<size_t>(c)].w = std::max(hi_u - lo_u, kMinChartExtentMeters);
        rects[static_cast<size_t>(c)].h = std::max(hi_v - lo_v, kMinChartExtentMeters);

        const std::vector<int>& list = tris_of_chart[static_cast<size_t>(c)];
        if (!list.empty()) {
            const float d = mesh_charting::projection_distortion(
                positions.data(), indices.data(), tri_count, list.data(),
                static_cast<int>(list.size()), T, B);
            if (std::isfinite(d)) max_distortion = std::max(max_distortion, d);
        }
    }
    return true;
}

} // namespace

bool build_export_model(const std::vector<Tri>& tris,
                        const std::vector<TriEx>& triex,
                        const std::string& name,
                        uint64_t resolved_hash,
                        uint32_t lod,
                        uint32_t lod_count,
                        const ExportOptions& options,
                        ExportModel& out,
                        std::string& error) {
    error.clear();
    out = ExportModel{};
    out.name = sanitize_identifier(name, "part");
    out.resolved_hash = resolved_hash;
    out.lod = lod;
    out.lod_count = lod_count;

    if (tris.empty()) {
        error = "nothing to export: the selected LOD has no triangles";
        return false;
    }
    if (triex.size() != tris.size()) {
        error = "internal: TriEx array is not parallel to the triangle array";
        return false;
    }
    if (options.texture_size < 16u || options.texture_size > 8192u) {
        error = "texture size must be between 16 and 8192 texels";
        return false;
    }
    if (!(options.chart_cone_deg > 0.0f && options.chart_cone_deg < 90.0f)) {
        error = "chart cone angle must be greater than 0 and less than 90 degrees";
        return false;
    }
    if (options.gutter_texels * 4u >= options.texture_size) {
        error = "gutter is too large for the requested texture size";
        return false;
    }

    const int tri_count = static_cast<int>(tris.size());

    // ---- stage 2: chart + pack -------------------------------------------
    std::vector<TriEx> uv_triex = triex;   // copy: the caller's attributes are
                                           // reused verbatim except for the UVs
    if (options.generate_uvs) {
        std::vector<float> positions(static_cast<size_t>(tri_count) * 9u);
        std::vector<uint32_t> indices(static_cast<size_t>(tri_count) * 3u);
        for (int t = 0; t < tri_count; ++t) {
            const Tri& tri = tris[static_cast<size_t>(t)];
            const float3* v[3] = {&tri.vertex0, &tri.vertex1, &tri.vertex2};
            for (int k = 0; k < 3; ++k) {
                const size_t base = (static_cast<size_t>(t) * 3u + static_cast<size_t>(k)) * 3u;
                positions[base + 0u] = v[k]->x;
                positions[base + 1u] = v[k]->y;
                positions[base + 2u] = v[k]->z;
                indices[static_cast<size_t>(t) * 3u + static_cast<size_t>(k)] =
                    static_cast<uint32_t>(t) * 3u + static_cast<uint32_t>(k);
            }
        }

        ChartFrames frames;
        std::vector<mesh_charting::ChartRect> rects;
        float max_distortion = 0.0f;
        if (!compute_chart_frames(positions, indices, tri_count, options.chart_cone_deg,
                                  frames, rects, max_distortion)) {
            error = "chart segmentation produced no charts";
            return false;
        }

        const int atlas = static_cast<int>(options.texture_size);
        const int pad = static_cast<int>(options.gutter_texels);
        float scale = 0.0f;
        std::vector<mesh_charting::ChartPlacement> placements;
        if (!mesh_charting::pack_charts(rects, atlas, atlas, pad, scale, placements) ||
            placements.size() != rects.size() || !(scale > 0.0f)) {
            error = "could not pack " + std::to_string(frames.chart_count) +
                    " charts into a " + std::to_string(atlas) + "x" + std::to_string(atlas) +
                    " atlas; raise --texture-size or widen --chart-cone";
            return false;
        }

        const float inv_atlas = 1.0f / static_cast<float>(atlas);
        double covered_texels = 0.0;
        for (int t = 0; t < tri_count; ++t) {
            const int c = frames.chart_of_tri[static_cast<size_t>(t)];
            const mesh_charting::ChartPlacement& pl = placements[static_cast<size_t>(c)];
            const float* T = &frames.tangent[static_cast<size_t>(c) * 3u];
            const float* B = &frames.bitangent[static_cast<size_t>(c) * 3u];
            const float min_u = frames.min_uv[static_cast<size_t>(c) * 2u + 0u];
            const float min_v = frames.min_uv[static_cast<size_t>(c) * 2u + 1u];

            TriEx& ex = uv_triex[static_cast<size_t>(t)];
            const Tri& tri = tris[static_cast<size_t>(t)];
            const float3* verts[3] = {&tri.vertex0, &tri.vertex1, &tri.vertex2};
            float2* uvs[3] = {&ex.uv0, &ex.uv1, &ex.uv2};
            for (int k = 0; k < 3; ++k) {
                const float p[3] = {verts[k]->x, verts[k]->y, verts[k]->z};
                const float texel_u =
                    static_cast<float>(pl.ox + pad) + (dot3(p, T) - min_u) * scale;
                const float texel_v =
                    static_cast<float>(pl.oy + pad) + (dot3(p, B) - min_v) * scale;
                // v is stored UP (OBJ/glTF convention): texel row 0 is the top
                // of the atlas, so the OBJ v of a texel row r is 1 - r/H.
                uvs[k]->x = std::min(std::max(texel_u * inv_atlas, 0.0f), 1.0f);
                uvs[k]->y = 1.0f - std::min(std::max(texel_v * inv_atlas, 0.0f), 1.0f);
            }
        }
        for (const mesh_charting::ChartRect& r : rects) {
            covered_texels += static_cast<double>(std::ceil(r.w * scale)) *
                              static_cast<double>(std::ceil(r.h * scale));
        }

        out.charts.chart_count = static_cast<uint32_t>(frames.chart_count);
        out.charts.atlas_size = options.texture_size;
        out.charts.texels_per_meter = scale;
        out.charts.packing_fill =
            static_cast<float>(covered_texels / (static_cast<double>(atlas) * atlas));
        out.charts.max_distortion = max_distortion;
    } else {
        for (TriEx& ex : uv_triex) {
            ex.uv0 = float2(0.0f, 0.0f);
            ex.uv1 = float2(0.0f, 0.0f);
            ex.uv2 = float2(0.0f, 0.0f);
        }
    }

    // ---- stage 3: weld ----------------------------------------------------
    // build_indexed_part_geometry merges two corners only on an EXACT match of
    // every attribute, so a UV seam, a material boundary and a hard shading
    // edge each split the vertex — which is exactly the vertex set OBJ needs.
    // Signed zeros are normalised first; see canonicalize_zero above.
    std::vector<Tri> weld_tris = tris;
    for (Tri& tri : weld_tris) {
        float3* verts[3] = {&tri.vertex0, &tri.vertex1, &tri.vertex2};
        for (float3* v : verts) {
            canonicalize_zero(v->x);
            canonicalize_zero(v->y);
            canonicalize_zero(v->z);
        }
    }
    for (TriEx& ex : uv_triex) {
        float3* normals[3] = {&ex.N0, &ex.N1, &ex.N2};
        for (float3* n : normals) {
            canonicalize_zero(n->x);
            canonicalize_zero(n->y);
            canonicalize_zero(n->z);
        }
        float2* uvs[3] = {&ex.uv0, &ex.uv1, &ex.uv2};
        for (float2* uv : uvs) {
            canonicalize_zero(uv->x);
            canonicalize_zero(uv->y);
        }
    }
    const viewer::IndexedPartGeometry welded = viewer::build_indexed_part_geometry(
        weld_tris.data(), uv_triex.data(), tri_count, -1.0f);
    if (welded.vertex_count <= 0 || welded.indices.empty()) {
        error = "weld produced no geometry";
        return false;
    }

    ExportMesh& mesh = out.mesh;
    mesh.vertex_count = static_cast<uint32_t>(welded.vertex_count);
    mesh.positions = welded.vertices;
    mesh.normals = welded.normals;
    mesh.uvs = welded.surface_uvs;
    mesh.occlusion = welded.baked_ao;
    mesh.material_ids = welded.material_ids;
    mesh.tint.resize(static_cast<size_t>(mesh.vertex_count) * 4u, 0.0f);
    for (size_t v = 0; v < static_cast<size_t>(mesh.vertex_count); ++v) {
        for (int c = 0; c < 4; ++c) {
            const size_t k = v * 4u + static_cast<size_t>(c);
            mesh.tint[k] = (k < welded.colors.size())
                               ? static_cast<float>(welded.colors[k]) / 255.0f
                               : (c == 3 ? 0.0f : 1.0f);
        }
    }
    if (mesh.uvs.size() != static_cast<size_t>(mesh.vertex_count) * 2u)
        mesh.uvs.assign(static_cast<size_t>(mesh.vertex_count) * 2u, 0.0f);
    if (mesh.occlusion.size() != mesh.vertex_count)
        mesh.occlusion.assign(mesh.vertex_count, 1.0f);

    // ---- stage 4: group by material ---------------------------------------
    // Grouping is by the FIRST corner's material: build_indexed_part_geometry
    // writes the triangle's materialId onto all three of its corners, so the
    // three agree unless a corner was welded across a material boundary — which
    // it cannot be, since the material id is one of the attributes matched.
    const size_t tri_total = welded.indices.size() / 3u;
    std::vector<uint32_t> tri_material(tri_total, UINT32_MAX);
    std::vector<uint32_t> distinct;
    distinct.reserve(8);
    for (size_t t = 0; t < tri_total; ++t) {
        const uint32_t v0 = welded.indices[t * 3u];
        const uint32_t m = (v0 < welded.material_ids.size()) ? welded.material_ids[v0]
                                                             : UINT32_MAX;
        tri_material[t] = m;
        if (std::find(distinct.begin(), distinct.end(), m) == distinct.end())
            distinct.push_back(m);
    }
    std::sort(distinct.begin(), distinct.end());

    mesh.indices.clear();
    mesh.indices.reserve(welded.indices.size());
    mesh.submeshes.clear();
    mesh.submeshes.reserve(distinct.size());
    out.materials.clear();
    out.materials.reserve(distinct.size());
    for (uint32_t m : distinct) {
        ExportSubmesh sub;
        sub.material_id = m;
        sub.material_slot = static_cast<uint32_t>(out.materials.size());
        sub.first_index = static_cast<uint32_t>(mesh.indices.size());
        for (size_t t = 0; t < tri_total; ++t) {
            if (tri_material[t] != m) continue;
            mesh.indices.push_back(welded.indices[t * 3u + 0u]);
            mesh.indices.push_back(welded.indices[t * 3u + 1u]);
            mesh.indices.push_back(welded.indices[t * 3u + 2u]);
        }
        sub.index_count = static_cast<uint32_t>(mesh.indices.size()) - sub.first_index;
        if (sub.index_count == 0u) continue;
        mesh.submeshes.push_back(sub);

        ExportMaterial material;
        // UINT32_MAX is build_indexed_part_geometry's "no material" sentinel.
        // It is not a registry index, so describe it as the default surface
        // rather than probing the registry with a bogus id.
        if (m == UINT32_MAX) {
            material = ExportMaterial{};
            material.id = m;
            material.name = "mat_default";
        } else {
            describe_material(m, material);
        }
        out.materials.push_back(material);
    }

    if (mesh.submeshes.empty()) {
        error = "weld produced no triangles";
        return false;
    }
    return true;
}

} // namespace matter_export
