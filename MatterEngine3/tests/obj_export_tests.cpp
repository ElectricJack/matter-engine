// MatterEngine3/tests/obj_export_tests.cpp
//
// The gate for MatterEngine3/src/export: OBJ+MTL+PBR-texture export of a baked
// Part, read back with a parser that shares no code with the writer
// (tests/obj_parser.h).
//
//   make -C MatterEngine3/tests run-obj-export
//
// WHAT IS COVERED, and why each one is here rather than assumed:
//   * deterministic number formatting — the whole "byte-identical exports"
//     claim rests on export_text.h's formatter, so it is checked directly,
//     including the tie-rounding and negative-zero cases printf gets wrong.
//   * round trip — counts, index ranges, UV range, unit normals, and that
//     every material and every map_* the MTL names actually exists on disk.
//     A dangling map reference is the classic broken export, and it is exactly
//     what a writer refactor breaks.
//   * winding — the exported triangles must be counter-clockwise seen from
//     outside, which the OBJ header claims. Asserted against the shading
//     normals, so a flip anywhere in gather/weld/write is caught.
//   * texture content — a texel inside a known chart must carry the material's
//     sRGB albedo and the vertex AO. Without this the maps could be uniformly
//     background and every structural check would still pass.
//   * determinism — two exports of the same model, byte-compared, files and
//     texture hashes both.
//   * artifact path — a real part_asset_v2 bundle written to a scratch cache
//     and exported through export_part(), including a parent whose child table
//     must be merged in with its transform, and an out-of-range --lod.
//
// NOT covered here: baking a part from JavaScript. That needs the script host,
// and lives in obj_export_golden_tests.cpp so this gate stays cheap.

#include "check.h"
#include "obj_parser.h"
#include "test_sandbox.h"

#include "export/export_text.h"
#include "export/mesh_export.h"
#include "export/obj_writer.h"
#include "export/part_export.h"
#include "export/texture_bake.h"

#include "blas_manager.hpp"
#include "part_asset_v2.h"
#include "tlas_manager.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

using matter_export::ExportModel;
using matter_export::ExportOptions;
using matter_export::MapKind;
using matter_export::TextureFormat;

constexpr uint32_t kTestTextureSize = 64;

// ---------------------------------------------------------------------------
// Fixture geometry
// ---------------------------------------------------------------------------

struct Soup {
    std::vector<Tri> tris;
    std::vector<TriEx> triex;
};

void push_triangle(Soup& soup, const float a[3], const float b[3], const float c[3],
                   int material, float ao, const float tint[4]) {
    Tri tri{};
    tri.vertex0 = make_float3(a[0], a[1], a[2]);
    tri.vertex1 = make_float3(b[0], b[1], b[2]);
    tri.vertex2 = make_float3(c[0], c[1], c[2]);
    soup.tris.push_back(tri);

    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                  e1[0] * e2[1] - e1[1] * e2[0]};
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0.0f) { n[0] /= len; n[1] /= len; n[2] /= len; }

    TriEx ex{};
    ex.N0 = ex.N1 = ex.N2 = make_float3(n[0], n[1], n[2]);
    ex.uv0 = ex.uv1 = ex.uv2 = float2(0.0f, 0.0f);
    ex.materialId = material;
    ex.tint = float4(tint[0], tint[1], tint[2], tint[3]);
    ex.ao0 = ex.ao1 = ex.ao2 = ao;
    soup.triex.push_back(ex);
}

// Axis-aligned unit cube centred on the origin, counter-clockwise when seen
// from outside, with one material per face. `face_materials` is indexed in the
// quad order below: -Y, +Y, -Z, +Z, +X, -X.
Soup make_faceted_cube(float half, const int face_materials[6]);

// Axis-aligned unit cube centred on the origin, counter-clockwise when seen
// from outside. The +Y face takes `top_material` so the export has two
// materials and therefore two submeshes and two `usemtl` groups.
Soup make_cube(float half, int side_material, int top_material, float top_ao) {
    Soup soup;
    const float p[8][3] = {
        {-half, -half, -half}, {half, -half, -half}, {half, -half, half}, {-half, -half, half},
        {-half,  half, -half}, {half,  half, -half}, {half,  half, half}, {-half,  half, half},
    };
    const float no_tint[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    const float top_tint[4] = {1.0f, 0.0f, 0.0f, 1.0f};   // fully red-tinted top

    // Each quad is wound counter-clockwise SEEN FROM OUTSIDE, which
    // test_round_trip re-derives and checks against the cube centre — so the
    // winding assertion there is a real check of the exporter, not a tautology
    // over normals this fixture computed from the same winding.
    struct Quad { int a, b, c, d; int material; float ao; const float* tint; };
    const Quad quads[6] = {
        {0, 1, 2, 3, side_material, 1.0f, no_tint},   // -Y (bottom)
        {7, 6, 5, 4, top_material, top_ao, top_tint}, // +Y (top)
        {1, 0, 4, 5, side_material, 1.0f, no_tint},   // -Z
        {3, 2, 6, 7, side_material, 1.0f, no_tint},   // +Z
        {2, 1, 5, 6, side_material, 1.0f, no_tint},   // +X
        {0, 3, 7, 4, side_material, 1.0f, no_tint},   // -X
    };
    for (const Quad& q : quads) {
        push_triangle(soup, p[q.a], p[q.b], p[q.c], q.material, q.ao, q.tint);
        push_triangle(soup, p[q.a], p[q.c], p[q.d], q.material, q.ao, q.tint);
    }
    return soup;
}

Soup make_faceted_cube(float half, const int face_materials[6]) {
    Soup soup;
    const float p[8][3] = {
        {-half, -half, -half}, {half, -half, -half}, {half, -half, half}, {-half, -half, half},
        {-half,  half, -half}, {half,  half, -half}, {half,  half, half}, {-half,  half, half},
    };
    const float no_tint[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    const int quads[6][4] = {
        {0, 1, 2, 3}, {7, 6, 5, 4}, {1, 0, 4, 5},
        {3, 2, 6, 7}, {2, 1, 5, 6}, {0, 3, 7, 4},
    };
    for (int q = 0; q < 6; ++q) {
        push_triangle(soup, p[quads[q][0]], p[quads[q][1]], p[quads[q][2]],
                      face_materials[q], 1.0f, no_tint);
        push_triangle(soup, p[quads[q][0]], p[quads[q][2]], p[quads[q][3]],
                      face_materials[q], 1.0f, no_tint);
    }
    return soup;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string read_or_empty(const std::string& path) {
    std::string text;
    if (!obj_parse::read_text_file(path, text)) return std::string();
    return text;
}

bool files_identical(const std::string& a, const std::string& b) {
    std::string left, right;
    if (!obj_parse::read_text_file(a, left)) return false;
    if (!obj_parse::read_text_file(b, right)) return false;
    return left == right;
}

bool export_fixture(const Soup& soup, const std::string& dir, const std::string& name,
                    uint32_t texture_size, matter_export::ExportModel& model,
                    matter_export::ObjWriteResult& written, std::string& error) {
    ExportOptions options;
    options.texture_size = texture_size;
    options.gutter_texels = 2;
    if (!matter_export::build_export_model(soup.tris, soup.triex, name, 0xabcdef0123456789ull,
                                           0, 1, options, model, error))
        return false;

    matter_export::TextureBakeOptions bake;
    bake.size = texture_size;
    bake.dilate_texels = 2;
    if (!matter_export::bake_maps(model, bake, error)) return false;

    matter_export::ObjWriteOptions write_options;
    write_options.directory = dir;
    write_options.base_name = name;
    write_options.texture_format = TextureFormat::Png;
    return matter_export::write_obj(model, write_options, written, error);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_number_formatting() {
    using matter_export::format_fixed;
    CHECK(format_fixed(0.0, 6) == "0", "format: zero");
    CHECK(format_fixed(-0.0, 6) == "0", "format: negative zero normalises");
    CHECK(format_fixed(1.0, 6) == "1", "format: trailing zeros trimmed");
    CHECK(format_fixed(-1.5, 6) == "-1.5", "format: negative");
    CHECK(format_fixed(0.0000005, 6) == "0.000001", "format: half rounds away from zero");
    CHECK(format_fixed(-0.0000005, 6) == "-0.000001", "format: negative half rounds away");
    CHECK(format_fixed(0.1234564, 6) == "0.123456", "format: rounds down");
    CHECK(format_fixed(0.25, 1) == "0.3", "format: one decimal, half up");
    CHECK(format_fixed(std::numeric_limits<double>::infinity(), 6) == "0",
          "format: infinity is not written as text");
    CHECK(format_fixed(12.0, 0) == "12", "format: zero decimals");
    CHECK(format_fixed(0.0009, 6) == "0.0009", "format: leading fraction zeros kept");
    CHECK(matter_export::format_hex64(0x0123456789abcdefull) == "0123456789abcdef",
          "format: 16-digit hex");
    CHECK(matter_export::format_hex64(0) == "0000000000000000", "format: hex zero padded");
}

void test_round_trip(const std::string& root) {
    const std::string dir = root + "/roundtrip";
    std::string error;
    CHECK(matter_export::ensure_directory(dir, error), "roundtrip: output dir created");

    const Soup soup = make_cube(0.5f, 3, 5, 0.25f);
    ExportModel model;
    matter_export::ObjWriteResult written;
    if (!export_fixture(soup, dir, "cube", kTestTextureSize, model, written, error)) {
        printf("FAIL: roundtrip export: %s\n", error.c_str());
        ++g_failures;
        return;
    }

    CHECK(model.mesh.triangle_count() == 12u, "roundtrip: 12 triangles survive the weld");
    CHECK(model.mesh.submeshes.size() == 2u, "roundtrip: two material groups");
    CHECK(model.materials.size() == 2u, "roundtrip: two materials described");
    CHECK(model.charts.chart_count >= 6u,
          "roundtrip: a cube segments into at least one chart per face");

    const obj_parse::ObjFile obj = obj_parse::parse_obj(read_or_empty(dir + "/cube.obj"));
    if (!obj.ok()) {
        printf("FAIL: roundtrip parse: %s\n", obj.error.c_str());
        ++g_failures;
        return;
    }
    CHECK(obj.position_count() == model.mesh.vertex_count, "roundtrip: vertex count matches");
    CHECK(obj.normal_count() == model.mesh.vertex_count, "roundtrip: normal count matches");
    CHECK(obj.uv_count() == model.mesh.vertex_count, "roundtrip: uv count matches");
    CHECK(obj.faces.size() == model.mesh.triangle_count(), "roundtrip: face count matches");
    CHECK(obj.mtllib == "cube.mtl", "roundtrip: mtllib names the sibling MTL");
    CHECK(obj.objects.size() == 1u && obj.objects[0] == "cube", "roundtrip: one named object");
    CHECK(obj.materials_used.size() == 2u, "roundtrip: two usemtl groups in the OBJ");

    bool uv_in_range = true;
    for (float uv : obj.uvs) uv_in_range = uv_in_range && uv >= 0.0f && uv <= 1.0f;
    CHECK(uv_in_range, "roundtrip: every UV is inside [0,1]");

    bool normals_unit = true;
    for (size_t v = 0; v < obj.normal_count(); ++v) {
        const float* n = &obj.normals[v * 3u];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        normals_unit = normals_unit && std::fabs(len - 1.0f) < 1e-3f;
    }
    CHECK(normals_unit, "roundtrip: every exported normal is unit length");

    // Winding: the OBJ header claims counter-clockwise front faces. The cube is
    // centred on the origin, so "outward" is checkable independently of the
    // shading normals — and the shading normals must agree with it too.
    bool winding_ok = true;
    bool normals_outward = true;
    for (const obj_parse::Face& face : obj.faces) {
        const float* a = &obj.positions[static_cast<size_t>(face.corner[0].position) * 3u];
        const float* b = &obj.positions[static_cast<size_t>(face.corner[1].position) * 3u];
        const float* c = &obj.positions[static_cast<size_t>(face.corner[2].position) * 3u];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float g[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0]};
        const float centroid[3] = {(a[0] + b[0] + c[0]) / 3.0f, (a[1] + b[1] + c[1]) / 3.0f,
                                   (a[2] + b[2] + c[2]) / 3.0f};
        winding_ok = winding_ok &&
                     (g[0] * centroid[0] + g[1] * centroid[1] + g[2] * centroid[2] > 0.0f);
        const float* n = &obj.normals[static_cast<size_t>(face.corner[0].normal) * 3u];
        normals_outward = normals_outward && (g[0] * n[0] + g[1] * n[1] + g[2] * n[2] > 0.0f);
    }
    CHECK(winding_ok, "roundtrip: faces are counter-clockwise seen from outside");
    CHECK(normals_outward, "roundtrip: shading normals agree with the face winding");

    // With signed zeros canonicalised, a unit cube welds to exactly four
    // vertices per face: one per corner per face, split only by the UV seam
    // and the hard normal between faces.
    CHECK(model.mesh.vertex_count == 24u,
          "roundtrip: the cube welds to 24 vertices (no signed-zero splits)");

    const obj_parse::MtlFile mtl = obj_parse::parse_mtl(read_or_empty(dir + "/cube.mtl"));
    if (!mtl.ok()) {
        printf("FAIL: roundtrip mtl parse: %s\n", mtl.error.c_str());
        ++g_failures;
        return;
    }
    CHECK(mtl.materials.size() == 2u, "roundtrip: two materials in the MTL");

    bool all_resolved = true;
    bool all_keys = true;
    for (const std::string& used : obj.materials_used) {
        const obj_parse::MtlMaterial* material = mtl.find(used);
        if (!material) { all_resolved = false; continue; }
        const char* required[] = {"Kd", "Ks", "Ke", "Ns", "Ni", "d", "illum", "Pr", "Pm"};
        for (const char* key : required) all_keys = all_keys && material->has(key);
        const char* map_keys[] = {"map_Kd", "map_Ka", "norm", "map_Bump",
                                  "map_Pr", "map_Pm", "map_Ke"};
        for (const char* key : map_keys) {
            const std::string path = obj_parse::map_path(*material, key);
            if (path.empty()) { all_keys = false; continue; }
            std::error_code code;
            if (!std::filesystem::exists(std::filesystem::path(dir) / path, code))
                all_resolved = false;
        }
    }
    CHECK(all_resolved, "roundtrip: every usemtl resolves and every map file exists");
    CHECK(all_keys, "roundtrip: every material carries the classic and PBR keys");

    // Kd must be white while map_Kd is present, or a textured material is
    // darkened twice by every consumer that multiplies the two.
    const obj_parse::MtlMaterial* first = mtl.find(obj.materials_used[0]);
    CHECK(first && first->find("Kd") && *first->find("Kd") == "1 1 1",
          "roundtrip: Kd is white while map_Kd carries the colour");
    // The top face uses the registry's LIGHT material, so this fixture exercises
    // the emissive branch: the map exists, so Ke hands the colour to it.
    CHECK(first && first->find("Ke") && *first->find("Ke") == "1 1 1",
          "roundtrip: Ke is white while map_Ke carries the emission");

    // Six maps, written and non-empty.
    uint32_t map_files = 0;
    for (const matter_export::WrittenFile& file : written.files)
        if (file.name.size() > 4u && file.name.compare(file.name.size() - 4u, 4u, ".png") == 0)
            map_files += (file.bytes > 0u) ? 1u : 0u;
    CHECK(map_files == matter_export::kMapCount, "roundtrip: all six maps written non-empty");
}

// Sample the baked albedo/AO at a texel the top face definitely owns, and check
// it carries what the top face was authored with.
void test_texture_content() {
    const Soup soup = make_cube(0.5f, 3, 5, 0.25f);
    ExportModel model;
    std::string error;
    ExportOptions options;
    options.texture_size = 128;
    options.gutter_texels = 2;
    if (!matter_export::build_export_model(soup.tris, soup.triex, "cube", 1, 0, 1, options,
                                           model, error)) {
        printf("FAIL: texture content build: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    matter_export::TextureBakeOptions bake;
    bake.size = 128;
    bake.dilate_texels = 2;
    if (!matter_export::bake_maps(model, bake, error)) {
        printf("FAIL: texture content bake: %s\n", error.c_str());
        ++g_failures;
        return;
    }

    const matter_export::ExportImage& albedo =
        model.maps[static_cast<uint32_t>(MapKind::Albedo)];
    const matter_export::ExportImage& ao =
        model.maps[static_cast<uint32_t>(MapKind::Occlusion)];
    CHECK(albedo.width == 128u && albedo.channels == 3u, "textures: albedo is 128x128 RGB");
    CHECK(ao.width == 128u && ao.channels == 1u, "textures: AO is 128x128 grey");

    // Find a triangle of the tinted top face and sample the texel at its
    // centroid. The top face is fully red-tinted (tint.a == 1), so the albedo
    // must be sRGB(1,0,0) regardless of the material's own colour, and AO must
    // be the authored 0.25.
    bool found = false;
    for (const matter_export::ExportSubmesh& sub : model.mesh.submeshes) {
        if (sub.material_id != 5u) continue;
        const uint32_t i0 = model.mesh.indices[sub.first_index + 0u];
        const uint32_t i1 = model.mesh.indices[sub.first_index + 1u];
        const uint32_t i2 = model.mesh.indices[sub.first_index + 2u];
        const float u = (model.mesh.uvs[i0 * 2u] + model.mesh.uvs[i1 * 2u] +
                         model.mesh.uvs[i2 * 2u]) / 3.0f;
        const float v = (model.mesh.uvs[i0 * 2u + 1u] + model.mesh.uvs[i1 * 2u + 1u] +
                         model.mesh.uvs[i2 * 2u + 1u]) / 3.0f;
        const uint32_t x = static_cast<uint32_t>(u * 128.0f);
        const uint32_t y = static_cast<uint32_t>((1.0f - v) * 128.0f);
        if (x >= 128u || y >= 128u) continue;
        const size_t texel = static_cast<size_t>(y) * 128u + x;
        const uint8_t expect_r = matter_export::encode_srgb_u8(1.0f);
        const uint8_t expect_g = matter_export::encode_srgb_u8(0.0f);
        CHECK(albedo.texels[texel * 3u + 0u] == expect_r,
              "textures: tinted top face albedo red channel");
        CHECK(albedo.texels[texel * 3u + 1u] == expect_g,
              "textures: tinted top face albedo green channel");
        CHECK(ao.texels[texel] >= 60u && ao.texels[texel] <= 68u,
              "textures: top face AO is the authored 0.25");
        found = true;
        break;
    }
    CHECK(found, "textures: found a top-face texel to sample");

    // A never-covered texel keeps the channel's neutral background rather than
    // black, which is what makes an under-filled atlas safe to sample.
    const matter_export::ExportImage& roughness =
        model.maps[static_cast<uint32_t>(MapKind::Roughness)];
    CHECK(roughness.content_hash != 0u, "textures: roughness map hashed");
    CHECK(model.maps[static_cast<uint32_t>(MapKind::Normal)].content_hash != 0u,
          "textures: normal map hashed");
}

// Six faces, six materials, six charts: sample the baked albedo at every
// triangle's UV centroid and demand it carry that triangle's own material
// colour. This is the assertion that ties the writer's UVs, the rasteriser's
// texel addressing and the chart packer's placements together — if any two of
// them disagreed, or if two charts overlapped in the atlas, some face would
// read a neighbour's colour and every structural check would still pass.
void test_uv_to_texel_agreement() {
    const int face_materials[6] = {1, 2, 8, 9, 16, 19};
    const Soup soup = make_faceted_cube(0.5f, face_materials);

    ExportModel model;
    std::string error;
    ExportOptions options;
    options.texture_size = 256;
    options.gutter_texels = 4;
    if (!matter_export::build_export_model(soup.tris, soup.triex, "faceted", 1, 0, 1,
                                           options, model, error)) {
        printf("FAIL: uv agreement build: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    matter_export::TextureBakeOptions bake;
    bake.size = 256;
    bake.dilate_texels = 4;
    if (!matter_export::bake_maps(model, bake, error)) {
        printf("FAIL: uv agreement bake: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    CHECK(model.materials.size() == 6u, "uv agreement: six distinct materials");
    CHECK(model.charts.chart_count == 6u,
          "uv agreement: six planar faces segment into six charts");

    const matter_export::ExportImage& albedo =
        model.maps[static_cast<uint32_t>(MapKind::Albedo)];
    uint32_t checked = 0;
    bool all_match = true;
    for (const matter_export::ExportSubmesh& sub : model.mesh.submeshes) {
        const matter_export::ExportMaterial& material = model.materials[sub.material_slot];
        const uint8_t want[3] = {matter_export::encode_srgb_u8(material.albedo[0]),
                                 matter_export::encode_srgb_u8(material.albedo[1]),
                                 matter_export::encode_srgb_u8(material.albedo[2])};
        for (uint32_t i = 0; i < sub.index_count; i += 3u) {
            float u = 0.0f, v = 0.0f;
            for (uint32_t k = 0; k < 3u; ++k) {
                const uint32_t vertex = model.mesh.indices[sub.first_index + i + k];
                u += model.mesh.uvs[static_cast<size_t>(vertex) * 2u + 0u];
                v += model.mesh.uvs[static_cast<size_t>(vertex) * 2u + 1u];
            }
            u /= 3.0f;
            v /= 3.0f;
            const uint32_t x = std::min<uint32_t>(255u, static_cast<uint32_t>(u * 256.0f));
            const uint32_t y =
                std::min<uint32_t>(255u, static_cast<uint32_t>((1.0f - v) * 256.0f));
            const size_t texel = static_cast<size_t>(y) * 256u + x;
            for (int c = 0; c < 3; ++c)
                all_match = all_match && albedo.texels[texel * 3u + static_cast<size_t>(c)] ==
                                             want[static_cast<size_t>(c)];
            ++checked;
        }
    }
    CHECK(checked == 12u, "uv agreement: every triangle sampled");
    CHECK(all_match,
          "uv agreement: each triangle's UV centroid reads its own material colour");
}

void test_determinism(const std::string& root) {
    const std::string dir_a = root + "/det_a";
    const std::string dir_b = root + "/det_b";
    std::string error;
    CHECK(matter_export::ensure_directory(dir_a, error), "determinism: dir a");
    CHECK(matter_export::ensure_directory(dir_b, error), "determinism: dir b");

    const Soup soup = make_cube(0.5f, 3, 5, 0.25f);
    ExportModel model_a, model_b;
    matter_export::ObjWriteResult written_a, written_b;
    const bool ok_a = export_fixture(soup, dir_a, "cube", kTestTextureSize, model_a, written_a, error);
    const bool ok_b = export_fixture(soup, dir_b, "cube", kTestTextureSize, model_b, written_b, error);
    CHECK(ok_a && ok_b, "determinism: both exports succeeded");
    if (!ok_a || !ok_b) return;

    CHECK(written_a.files.size() == written_b.files.size(), "determinism: same file set");
    bool same_bytes = true;
    bool same_hashes = true;
    for (size_t i = 0; i < written_a.files.size() && i < written_b.files.size(); ++i) {
        same_hashes = same_hashes &&
                      written_a.files[i].content_hash == written_b.files[i].content_hash &&
                      written_a.files[i].name == written_b.files[i].name;
        same_bytes = same_bytes && files_identical(dir_a + "/" + written_a.files[i].name,
                                                   dir_b + "/" + written_b.files[i].name);
    }
    CHECK(same_hashes, "determinism: recorded file hashes match");
    CHECK(same_bytes, "determinism: every written file is byte-identical");

    bool same_map_hashes = true;
    for (uint32_t m = 0; m < matter_export::kMapCount; ++m)
        same_map_hashes = same_map_hashes &&
                          model_a.maps[m].content_hash == model_b.maps[m].content_hash &&
                          model_a.maps[m].content_hash != 0u;
    CHECK(same_map_hashes, "determinism: texture content hashes are stable");
}

// ---------------------------------------------------------------------------
// The artifact path: write real bundles, then export through export_part().
// ---------------------------------------------------------------------------

uint64_t save_leaf_artifact(const std::string& cache_root, uint64_t hash, const Soup& soup,
                            uint32_t rungs) {
    BLASManager blas;
    TLASManager tlas;
    part_asset::LodLevels lods;
    for (uint32_t r = 0; r < rungs; ++r) {
        std::vector<Tri> tris = soup.tris;
        std::vector<TriEx> triex = soup.triex;
        // Coarser rungs drop the tail of the triangle list. Crude, but it is
        // the artifact SHAPE the exporter cares about, not the decimation.
        const size_t keep = std::max<size_t>(2u, tris.size() >> r);
        tris.resize(keep);
        triex.resize(keep);
        const BLASHandle handle = blas.register_triangles(tris.data(), static_cast<int>(keep),
                                                          triex.data(), true);
        part_asset::LodLevel level;
        level.screen_size_threshold = 1.0f / static_cast<float>(r + 1u);
        // blas_indices are ABSOLUTE indices into get_entries(); registration
        // order is that index while nothing is released.
        for (uint32_t i = 0; i < blas.get_entries().size(); ++i)
            if (blas.get_entries()[i]->handle == handle) level.blas_indices.push_back(i);
        lods.push_back(level);
    }
    const std::string path = cache_root + "/" + part_asset::cache_path_resolved(hash);
    if (!part_asset::save_v2(path, blas, tlas, nullptr, 0, lods, hash)) return 0;
    return hash;
}

void test_artifact_export(const std::string& root) {
    const std::string cache = root + "/cache";
    const std::string dir = root + "/artifact";
    std::string error;
    CHECK(matter_export::ensure_directory(cache + "/parts", error), "artifact: cache dir");
    CHECK(matter_export::ensure_directory(dir, error), "artifact: out dir");

    // Materials 3 (metal) and 8 (stone) both have zero emission, which is what
    // the emissive-map assertions below are about. Material 5 is the registry's
    // LIGHT (emission 5.0) and drives the opposite branch further down.
    const Soup soup = make_cube(0.5f, 3, 8, 1.0f);
    const uint64_t leaf_hash = 0x1111222233334444ull;
    CHECK(save_leaf_artifact(cache, leaf_hash, soup, 2) == leaf_hash,
          "artifact: leaf bundle written");

    matter_export::ExportSettings settings;
    settings.out_dir = dir;
    settings.lod = 0;
    settings.texture_size = kTestTextureSize;
    settings.gutter_texels = 2;
    settings.texture_format = TextureFormat::Png;

    matter_export::PartExportRequest request;
    request.cache_root = cache;
    request.resolved_hash = leaf_hash;
    request.name = "LeafCube";

    matter_export::PartExportSummary summary;
    if (!matter_export::export_part(request, settings, summary, error)) {
        printf("FAIL: artifact export: %s\n", error.c_str());
        ++g_failures;
    } else {
        CHECK(summary.triangle_count == 12u, "artifact: LOD 0 carries all 12 triangles");
        CHECK(summary.lod_count == 2u, "artifact: two rungs reported");
        CHECK(summary.skipped_subtree_refs == 0u, "artifact: nothing skipped");
        CHECK(!summary.files.obj_name.empty(), "artifact: obj written");
        const obj_parse::ObjFile obj =
            obj_parse::parse_obj(read_or_empty(dir + "/LeafCube.obj"));
        CHECK(obj.ok() && obj.faces.size() == 12u, "artifact: exported OBJ parses with 12 faces");

        // Nothing in this fixture emits, so export_part must skip the emissive
        // bake AND leave Ke at the real value. A white Ke with no map_Ke would
        // render the whole part as a self-lit white box on any consumer that
        // does not read map_Ke.
        const obj_parse::MtlFile mtl =
            obj_parse::parse_mtl(read_or_empty(dir + "/LeafCube.mtl"));
        bool emissive_safe = mtl.ok() && !mtl.materials.empty();
        bool emissive_map_written = false;
        for (const obj_parse::MtlMaterial& material : mtl.materials) {
            const std::string* ke = material.find("Ke");
            if (material.has("map_Ke")) emissive_map_written = true;
            else emissive_safe = emissive_safe && ke && *ke != "1 1 1";
        }
        CHECK(!emissive_map_written,
              "artifact: no emissive map is written for a part that does not emit");
        CHECK(emissive_safe, "artifact: Ke is not white when no map_Ke is written");
    }

    // Rung 1 of this fixture keeps half the triangles.
    settings.lod = 1;
    matter_export::PartExportSummary coarse;
    if (!matter_export::export_part(request, settings, coarse, error)) {
        printf("FAIL: artifact coarse export: %s\n", error.c_str());
        ++g_failures;
    } else {
        CHECK(coarse.triangle_count == 6u, "artifact: LOD 1 carries the coarse rung");
    }

    // A rung past the end is an error, not a silent clamp.
    settings.lod = 7;
    matter_export::PartExportSummary missing;
    const bool rejected = !matter_export::export_part(request, settings, missing, error);
    CHECK(rejected, "artifact: an out-of-range --lod is rejected");
    CHECK(error.find("does not exist") != std::string::npos,
          "artifact: the rejection says which rung is missing");

    // A parent whose child table must be merged in with its transform.
    const uint64_t parent_hash = 0x5555666677778888ull;
    {
        BLASManager blas;
        TLASManager tlas;
        Soup parent_soup = make_cube(0.25f, 3, 3, 1.0f);
        const BLASHandle handle =
            blas.register_triangles(parent_soup.tris.data(),
                                    static_cast<int>(parent_soup.tris.size()),
                                    parent_soup.triex.data(), true);
        part_asset::LodLevel level;
        level.screen_size_threshold = 1.0f;
        for (uint32_t i = 0; i < blas.get_entries().size(); ++i)
            if (blas.get_entries()[i]->handle == handle) level.blas_indices.push_back(i);
        part_asset::LodLevels lods{level};

        part_asset::ChildInstance child{};
        child.child_resolved_hash = leaf_hash;
        const float transform[16] = {1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(child.transform, transform, sizeof transform);

        const std::string path = cache + "/" + part_asset::cache_path_resolved(parent_hash);
        CHECK(part_asset::save_v2(path, blas, tlas, &child, 1, lods, parent_hash),
              "artifact: parent bundle written");
    }

    settings.lod = 0;
    matter_export::PartExportRequest parent_request;
    parent_request.cache_root = cache;
    parent_request.resolved_hash = parent_hash;
    parent_request.name = "ParentCube";
    matter_export::PartExportSummary parent_summary;
    if (!matter_export::export_part(parent_request, settings, parent_summary, error)) {
        printf("FAIL: artifact parent export: %s\n", error.c_str());
        ++g_failures;
    } else {
        CHECK(parent_summary.triangle_count == 24u,
              "artifact: the child subtree is merged into the parent's OBJ");
        CHECK(parent_summary.inlined_subtree_refs == 1u, "artifact: one child inlined");
        const obj_parse::ObjFile obj =
            obj_parse::parse_obj(read_or_empty(dir + "/ParentCube.obj"));
        float max_x = -1e9f;
        for (size_t v = 0; v < obj.position_count(); ++v)
            max_x = std::max(max_x, obj.positions[v * 3u]);
        CHECK(max_x > 9.0f, "artifact: the child's transform was applied");
    }

    // A reference to an artifact that is not in the cache must be reported, not
    // silently dropped.
    const uint64_t dangling_parent = 0x9999aaaabbbbccccull;
    {
        BLASManager blas;
        TLASManager tlas;
        Soup parent_soup = make_cube(0.25f, 3, 3, 1.0f);
        const BLASHandle handle =
            blas.register_triangles(parent_soup.tris.data(),
                                    static_cast<int>(parent_soup.tris.size()),
                                    parent_soup.triex.data(), true);
        part_asset::LodLevel level;
        for (uint32_t i = 0; i < blas.get_entries().size(); ++i)
            if (blas.get_entries()[i]->handle == handle) level.blas_indices.push_back(i);
        part_asset::LodLevels lods{level};
        part_asset::ChildInstance child{};
        child.child_resolved_hash = 0xdeadbeefdeadbeefull;
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(child.transform, identity, sizeof identity);
        CHECK(part_asset::save_v2(cache + "/" + part_asset::cache_path_resolved(dangling_parent),
                                  blas, tlas, &child, 1, lods, dangling_parent),
              "artifact: dangling-parent bundle written");
    }
    matter_export::PartExportRequest dangling_request;
    dangling_request.cache_root = cache;
    dangling_request.resolved_hash = dangling_parent;
    dangling_request.name = "DanglingParent";
    matter_export::PartExportSummary dangling_summary;
    if (matter_export::export_part(dangling_request, settings, dangling_summary, error)) {
        CHECK(dangling_summary.skipped_subtree_refs == 1u,
              "artifact: the missing child is counted as skipped");
        CHECK(!dangling_summary.warnings.empty(),
              "artifact: the missing child produces a warning");
    } else {
        printf("FAIL: artifact dangling export: %s\n", error.c_str());
        ++g_failures;
    }

    // The opposite branch through export_part: a part whose material emits DOES
    // get an emissive map.
    const uint64_t emissive_hash = 0x2222333344445555ull;
    CHECK(save_leaf_artifact(cache, emissive_hash, make_cube(0.5f, 5, 5, 1.0f), 1) ==
              emissive_hash,
          "artifact: emissive bundle written");
    matter_export::PartExportRequest emissive_request;
    emissive_request.cache_root = cache;
    emissive_request.resolved_hash = emissive_hash;
    emissive_request.name = "EmissiveCube";
    matter_export::PartExportSummary emissive_summary;
    if (matter_export::export_part(emissive_request, settings, emissive_summary, error)) {
        const obj_parse::MtlFile mtl =
            obj_parse::parse_mtl(read_or_empty(dir + "/EmissiveCube.mtl"));
        CHECK(mtl.ok() && !mtl.materials.empty() && mtl.materials[0].has("map_Ke"),
              "artifact: an emissive part does get map_Ke");
        CHECK(emissive_summary.map_hashes[static_cast<uint32_t>(MapKind::Emissive)] != 0u,
              "artifact: the emissive map is hashed in the summary");
    } else {
        printf("FAIL: artifact emissive export: %s\n", error.c_str());
        ++g_failures;
    }

    // The manifest describes what was written and is itself deterministic.
    std::vector<matter_export::PartExportSummary> parts{parent_summary};
    const std::string manifest_a = matter_export::build_manifest_json(parts, settings);
    const std::string manifest_b = matter_export::build_manifest_json(parts, settings);
    CHECK(manifest_a == manifest_b, "manifest: deterministic");
    CHECK(manifest_a.find("\"format\": \"matter-obj-export\"") != std::string::npos,
          "manifest: carries its format tag");
    CHECK(manifest_a.find("\"up_axis\": \"Y\"") != std::string::npos,
          "manifest: records the coordinate convention");
    CHECK(matter_export::write_manifest(parts, settings, error), "manifest: written");
    CHECK(!read_or_empty(dir + "/manifest.json").empty(), "manifest: exists on disk");
}

void test_texture_format_reservation(const std::string& root) {
    const std::string dir = root + "/formats";
    std::string error;
    CHECK(matter_export::ensure_directory(dir, error), "formats: dir");

    TextureFormat format = TextureFormat::None;
    CHECK(matter_export::parse_texture_format("png", format) && format == TextureFormat::Png,
          "formats: png parses");
    CHECK(matter_export::parse_texture_format("ktx2", format) && format == TextureFormat::Ktx2,
          "formats: ktx2 parses");
    CHECK(matter_export::parse_texture_format("none", format) && format == TextureFormat::None,
          "formats: none parses");
    CHECK(!matter_export::parse_texture_format("jpeg", format), "formats: unknown rejected");

    const Soup soup = make_cube(0.5f, 3, 3, 1.0f);
    ExportModel model;
    ExportOptions options;
    options.texture_size = kTestTextureSize;
    options.gutter_texels = 2;
    CHECK(matter_export::build_export_model(soup.tris, soup.triex, "cube", 2, 0, 1, options,
                                            model, error),
          "formats: model built");
    matter_export::TextureBakeOptions bake;
    bake.size = kTestTextureSize;
    bake.dilate_texels = 2;
    CHECK(matter_export::bake_maps(model, bake, error), "formats: maps baked");

    matter_export::ObjWriteOptions ktx;
    ktx.directory = dir;
    ktx.base_name = "ktx";
    ktx.texture_format = TextureFormat::Ktx2;
    matter_export::ObjWriteResult written;
    const bool ktx_rejected = !matter_export::write_obj(model, ktx, written, error);
    CHECK(ktx_rejected, "formats: ktx2 is reserved, not silently a PNG");
    CHECK(error.find("not implemented") != std::string::npos,
          "formats: the ktx2 refusal says so plainly");

    matter_export::ObjWriteOptions none;
    none.directory = dir;
    none.base_name = "geometry_only";
    none.texture_format = TextureFormat::None;
    matter_export::ObjWriteResult none_written;
    CHECK(matter_export::write_obj(model, none, none_written, error),
          "formats: none writes geometry only");
    CHECK(none_written.files.size() == 2u, "formats: none writes exactly obj + mtl");
    const obj_parse::MtlFile mtl =
        obj_parse::parse_mtl(read_or_empty(dir + "/geometry_only.mtl"));
    CHECK(mtl.ok() && !mtl.materials.empty(), "formats: texture-less MTL parses");
    if (mtl.ok() && !mtl.materials.empty()) {
        CHECK(!mtl.materials[0].has("map_Kd"), "formats: no map_Kd without textures");
        const std::string* kd = mtl.materials[0].find("Kd");
        CHECK(kd && *kd != "1 1 1",
              "formats: Kd carries the real colour when no map_Kd is written");
    }
}

void test_input_validation() {
    ExportModel model;
    std::string error;
    const Soup soup = make_cube(0.5f, 3, 3, 1.0f);

    ExportOptions tiny;
    tiny.texture_size = 8;
    CHECK(!matter_export::build_export_model(soup.tris, soup.triex, "cube", 1, 0, 1, tiny,
                                             model, error),
          "validation: a sub-16 texture size is rejected");

    ExportOptions cone;
    cone.texture_size = kTestTextureSize;
    cone.chart_cone_deg = 95.0f;
    CHECK(!matter_export::build_export_model(soup.tris, soup.triex, "cube", 1, 0, 1, cone,
                                             model, error),
          "validation: a >=90 degree chart cone is rejected");

    ExportOptions fine;
    fine.texture_size = kTestTextureSize;
    const std::vector<Tri> no_tris;
    const std::vector<TriEx> no_triex;
    CHECK(!matter_export::build_export_model(no_tris, no_triex, "cube", 1, 0, 1, fine,
                                             model, error),
          "validation: an empty mesh is rejected");
}

} // namespace

int main() {
    const std::string root = make_sandbox("sandbox/obj_export", {});

    test_number_formatting();
    test_round_trip(root);
    test_texture_content();
    test_uv_to_texel_agreement();
    test_determinism(root);
    test_artifact_export(root);
    test_texture_format_reservation(root);
    test_input_validation();

    if (g_failures == 0) printf("All obj export tests passed\n");
    return check_summary();
}
