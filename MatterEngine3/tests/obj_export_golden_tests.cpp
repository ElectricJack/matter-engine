// MatterEngine3/tests/obj_export_golden_tests.cpp
//
// End-to-end gate for `matter export obj`: bake three real world_demo parts
// through MatterEngine3/src/export/export_cli.h — the same entry point the CLI
// calls — and compare the result against the fixtures in
// tests/fixtures/export/.
//
//   make -C MatterEngine3/tests run-obj-export-golden
//   MATTER_EXPORT_GOLDEN_UPDATE=1 make -C MatterEngine3/tests run-obj-export-golden
//
// THE PARTS. CastleStone and CastlePavingSlab are the two castle parts the
// export spec named; AlpineFlower is the organic one — thin, two-material,
// forty-odd charts, which is the shape that stresses the chart packer hardest.
//
// WHAT IS PINNED, AND WHAT IS DELIBERATELY NOT
//   * The DIGEST (<part>.golden) pins the resolved hash, the counts, the chart
//     table's shape, the material list and every texture's content hash. It is
//     compared exactly.
//   * The OBJ and MTL are pinned as TEXT for the two parts small enough to
//     check in, but compared through tests/obj_parser.h with a numeric
//     tolerance rather than byte-for-byte. Byte-identical output is a real
//     contract — it is what makes an export diffable and cacheable — but it is
//     a contract WITHIN a build: two different CPU architectures can round the
//     last digit of a chart's UV differently, and a gate that failed on that
//     would be reporting the platform, not a regression. Byte identity is
//     tested where it is actually meaningful, in run-obj-export's determinism
//     case (same binary, twice).
//   * CastleStone's geometry is NOT checked in. Its LOD 0 is ~3400 triangles,
//     which is a 600 KB text fixture that would rewrite wholesale on any
//     mesher change; its digest and its MTL are pinned instead. The spec asked
//     for size limits on these fixtures and this is where that bites.
//
// STALE FIXTURE vs REGRESSION. The digest leads with the part's resolved hash.
// If that differs, the BAKE changed (a schema edit, a version-vector bump) and
// the fixture simply needs regenerating; if it matches and anything else
// differs, the input was identical and the exporter changed. The failure
// message says which, because those two need completely different responses.

#include "check.h"
#include "obj_parser.h"
#include "test_sandbox.h"

#include "export/export_cli.h"
#include "export/export_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Fixture size caps. These are the "size limits" the export spec asks for:
// a golden that grows past them has stopped being a fixture and become a
// data dump, and the right response is to pin a digest instead of the text.
constexpr uint64_t kMaxGoldenObjBytes = 64u * 1024u;
constexpr uint64_t kMaxGoldenMtlBytes = 8u * 1024u;
constexpr uint64_t kMaxExportedTextureBytes = 512u * 1024u;

constexpr uint32_t kGoldenTextureSize = 256;

struct GoldenPart {
    const char* module;
    bool pin_geometry;   // false = digest + MTL only (see the header note)
};

const GoldenPart kGoldenParts[] = {
    {"CastleStone", false},
    {"CastlePavingSlab", true},
    {"AlpineFlower", true},
};

bool update_mode() {
    const char* value = std::getenv("MATTER_EXPORT_GOLDEN_UPDATE");
    return value && value[0] != '\0' && std::string(value) != "0";
}

std::string read_file(const fs::path& path) {
    std::string text;
    if (!obj_parse::read_text_file(path.string(), text)) return std::string();
    return text;
}

bool write_file(const fs::path& path, const std::string& text) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (!file) return false;
    const size_t written = text.empty() ? 0u : std::fwrite(text.data(), 1, text.size(), file);
    const bool ok = (written == text.size());
    return (std::fclose(file) == 0) && ok;
}

// ---------------------------------------------------------------------------
// Digest
// ---------------------------------------------------------------------------

std::string build_digest(const matter_export::PartExportSummary& part,
                         const matter_export::ExportSettings& settings) {
    std::string out;
    out += "module " + part.name + "\n";
    out += "resolved_hash " + matter_export::format_hex64(part.resolved_hash) + "\n";
    out += "lod " + std::to_string(part.lod) + "\n";
    out += "lod_count " + std::to_string(part.lod_count) + "\n";
    out += "texture_size " + std::to_string(settings.texture_size) + "\n";
    out += "vertices " + std::to_string(part.vertex_count) + "\n";
    out += "triangles " + std::to_string(part.triangle_count) + "\n";
    out += "charts " + std::to_string(part.charts.chart_count) + "\n";
    out += "atlas " + std::to_string(part.charts.atlas_size) + "\n";
    out += "inlined_subtree_refs " + std::to_string(part.inlined_subtree_refs) + "\n";
    out += "skipped_subtree_refs " + std::to_string(part.skipped_subtree_refs) + "\n";
    for (const matter_export::MaterialSummary& material : part.materials) {
        out += "material " + material.name + " " +
               (material.id == UINT32_MAX ? std::string("none")
                                          : std::to_string(material.id)) +
               " " + std::to_string(material.triangles) + "\n";
    }
    for (uint32_t m = 0; m < matter_export::kMapCount; ++m) {
        if (part.map_hashes[m] == 0u) continue;
        out += std::string("map ") +
               matter_export::map_suffix(static_cast<matter_export::MapKind>(m)) + " " +
               matter_export::format_hex64(part.map_hashes[m]) + "\n";
    }
    return out;
}

// A digest mismatch reported usefully: the first differing line, plus which of
// the two causes it is.
void compare_digest(const std::string& module, const std::string& expected,
                    const std::string& actual) {
    if (expected == actual) return;

    auto lines = [](const std::string& text) {
        std::vector<std::string> out;
        size_t cursor = 0;
        while (cursor < text.size()) {
            const size_t newline = text.find('\n', cursor);
            const size_t end = (newline == std::string::npos) ? text.size() : newline;
            out.push_back(text.substr(cursor, end - cursor));
            if (newline == std::string::npos) break;
            cursor = newline + 1u;
        }
        return out;
    };
    const std::vector<std::string> want = lines(expected);
    const std::vector<std::string> got = lines(actual);

    const bool hash_changed =
        !want.empty() && !got.empty() && want[0].rfind("module", 0) == 0 &&
        want.size() > 1u && got.size() > 1u && want[1] != got[1];

    printf("FAIL: golden %s: digest differs\n", module.c_str());
    if (hash_changed) {
        printf("  the part's RESOLVED HASH changed (%s -> %s): the bake moved, not the\n"
               "  exporter. Regenerate with MATTER_EXPORT_GOLDEN_UPDATE=1 and review the\n"
               "  new digest.\n",
               want[1].c_str(), got[1].c_str());
    } else {
        printf("  the resolved hash is unchanged, so the INPUT was identical and the\n"
               "  EXPORTER's output moved. Treat this as a regression until proven\n"
               "  otherwise.\n");
    }
    for (size_t i = 0; i < std::max(want.size(), got.size()); ++i) {
        const std::string a = i < want.size() ? want[i] : std::string("<missing>");
        const std::string b = i < got.size() ? got[i] : std::string("<missing>");
        if (a != b) printf("  line %zu: want '%s' got '%s'\n", i + 1u, a.c_str(), b.c_str());
    }
    ++g_failures;
}

// ---------------------------------------------------------------------------
// OBJ / MTL comparison
// ---------------------------------------------------------------------------

bool close_enough(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

void compare_obj(const std::string& module, const std::string& expected_text,
                 const std::string& actual_text) {
    const obj_parse::ObjFile want = obj_parse::parse_obj(expected_text);
    const obj_parse::ObjFile got = obj_parse::parse_obj(actual_text);
    if (!want.ok()) {
        printf("FAIL: golden %s: fixture OBJ does not parse: %s\n", module.c_str(),
               want.error.c_str());
        ++g_failures;
        return;
    }
    if (!got.ok()) {
        printf("FAIL: golden %s: exported OBJ does not parse: %s\n", module.c_str(),
               got.error.c_str());
        ++g_failures;
        return;
    }

    CHECK(want.position_count() == got.position_count(),
          (module + ": golden vertex count").c_str());
    CHECK(want.normal_count() == got.normal_count(),
          (module + ": golden normal count").c_str());
    CHECK(want.uv_count() == got.uv_count(), (module + ": golden uv count").c_str());
    CHECK(want.faces.size() == got.faces.size(), (module + ": golden face count").c_str());
    CHECK(want.materials_used == got.materials_used,
          (module + ": golden usemtl groups").c_str());
    CHECK(want.mtllib == got.mtllib, (module + ": golden mtllib").c_str());
    if (want.position_count() != got.position_count() ||
        want.faces.size() != got.faces.size())
        return;

    bool positions_match = true;
    for (size_t i = 0; i < want.positions.size(); ++i)
        positions_match = positions_match && close_enough(want.positions[i], got.positions[i], 1e-5f);
    CHECK(positions_match, (module + ": golden vertex positions").c_str());

    bool normals_match = true;
    for (size_t i = 0; i < want.normals.size() && i < got.normals.size(); ++i)
        normals_match = normals_match && close_enough(want.normals[i], got.normals[i], 1e-4f);
    CHECK(normals_match, (module + ": golden vertex normals").c_str());

    bool uvs_match = true;
    for (size_t i = 0; i < want.uvs.size() && i < got.uvs.size(); ++i)
        uvs_match = uvs_match && close_enough(want.uvs[i], got.uvs[i], 1e-5f);
    CHECK(uvs_match, (module + ": golden uvs").c_str());

    bool faces_match = true;
    for (size_t f = 0; f < want.faces.size(); ++f) {
        for (int k = 0; k < 3; ++k) {
            faces_match = faces_match &&
                          want.faces[f].corner[k].position == got.faces[f].corner[k].position &&
                          want.faces[f].corner[k].uv == got.faces[f].corner[k].uv &&
                          want.faces[f].corner[k].normal == got.faces[f].corner[k].normal;
        }
        faces_match = faces_match && want.faces[f].material == got.faces[f].material;
    }
    CHECK(faces_match, (module + ": golden face indices and material groups").c_str());
}

void compare_mtl(const std::string& module, const std::string& expected_text,
                 const std::string& actual_text) {
    const obj_parse::MtlFile want = obj_parse::parse_mtl(expected_text);
    const obj_parse::MtlFile got = obj_parse::parse_mtl(actual_text);
    if (!want.ok() || !got.ok()) {
        printf("FAIL: golden %s: MTL does not parse (%s / %s)\n", module.c_str(),
               want.error.c_str(), got.error.c_str());
        ++g_failures;
        return;
    }
    CHECK(want.materials.size() == got.materials.size(),
          (module + ": golden material count").c_str());
    if (want.materials.size() != got.materials.size()) return;

    bool entries_match = true;
    for (size_t m = 0; m < want.materials.size(); ++m) {
        const obj_parse::MtlMaterial& a = want.materials[m];
        const obj_parse::MtlMaterial& b = got.materials[m];
        if (a.name != b.name || a.entries.size() != b.entries.size()) {
            entries_match = false;
            continue;
        }
        for (size_t e = 0; e < a.entries.size(); ++e) {
            if (a.entries[e].first != b.entries[e].first) { entries_match = false; break; }
            // Numeric values compare with a tolerance for the same reason the
            // OBJ's do; map paths and the rest compare exactly.
            const std::string& key = a.entries[e].first;
            const bool numeric = key == "Kd" || key == "Ks" || key == "Ke" || key == "Ns" ||
                                 key == "Ni" || key == "d" || key == "Pr" || key == "Pm";
            if (!numeric) {
                if (a.entries[e].second != b.entries[e].second) entries_match = false;
                continue;
            }
            const char* pa = a.entries[e].second.c_str();
            const char* pb = b.entries[e].second.c_str();
            char* end_a = nullptr;
            char* end_b = nullptr;
            while (*pa && *pb) {
                const float va = std::strtof(pa, &end_a);
                const float vb = std::strtof(pb, &end_b);
                if (end_a == pa || end_b == pb) break;
                if (!close_enough(va, vb, 1e-4f)) { entries_match = false; break; }
                pa = end_a;
                pb = end_b;
                while (*pa == ' ') ++pa;
                while (*pb == ' ') ++pb;
            }
        }
    }
    CHECK(entries_match, (module + ": golden MTL keys and values").c_str());
}

// ---------------------------------------------------------------------------

std::string repo_root() {
    // The suites run from MatterEngine3/tests.
    std::error_code code;
    const fs::path root = fs::absolute(fs::path("..") / "..", code);
    return code ? std::string("../..") : root.lexically_normal().string();
}

void run_part(const GoldenPart& part, const std::string& sandbox, const fs::path& fixtures) {
    const std::string module = part.module;
    const fs::path out_dir = fs::path(sandbox) / module;

    matter_export::ExportObjRequest request;
    request.project_dir = (fs::path(repo_root()) / "projects" / "world_demo").string();
    request.world_name = module;
    request.engine_shared_lib_dir =
        (fs::path(repo_root()) / "MatterEngine3" / "shared-lib").string();
    request.kind = matter_export::ExportTargetKind::Module;
    request.module = module;
    // Hermetic: never bake into the project's shared cache, so a developer's
    // working cache can neither hide a bug nor be polluted by the gate.
    request.cache_root_override = (fs::path(sandbox) / "cache" / module).string();
    request.overrides.out_dir = out_dir.string();
    request.overrides.texture_size = kGoldenTextureSize;
    request.overrides.lod = 0u;

    matter_export::ExportObjReport report;
    std::string error;
    if (!matter_export::run_export_obj(request, report, error)) {
        printf("FAIL: golden %s: export failed: %s\n", module.c_str(), error.c_str());
        ++g_failures;
        return;
    }
    if (report.parts.size() != 1u) {
        printf("FAIL: golden %s: expected exactly one exported part, got %zu\n",
               module.c_str(), report.parts.size());
        ++g_failures;
        return;
    }
    const matter_export::PartExportSummary& summary = report.parts.front();

    // Structural checks that hold regardless of what the bake produced. These
    // are the ones worth having even on the day the fixtures go stale.
    CHECK(summary.triangle_count > 0u, (module + ": exported some geometry").c_str());
    CHECK(summary.charts.chart_count > 0u, (module + ": produced a chart set").c_str());
    CHECK(summary.skipped_subtree_refs == 0u,
          (module + ": every child reference resolved").c_str());
    bool textures_within_cap = true;
    for (const matter_export::WrittenFile& file : summary.files.files) {
        if (file.name.size() > 4u &&
            file.name.compare(file.name.size() - 4u, 4u, ".png") == 0)
            textures_within_cap = textures_within_cap && file.bytes <= kMaxExportedTextureBytes;
    }
    CHECK(textures_within_cap, (module + ": every texture is within the size cap").c_str());

    const std::string obj_text = read_file(out_dir / (module + ".obj"));
    const std::string mtl_text = read_file(out_dir / (module + ".mtl"));
    CHECK(!obj_text.empty(), (module + ": an OBJ was written").c_str());
    CHECK(!mtl_text.empty(), (module + ": an MTL was written").c_str());
    CHECK(mtl_text.size() <= kMaxGoldenMtlBytes, (module + ": MTL is within the size cap").c_str());
    if (part.pin_geometry)
        CHECK(obj_text.size() <= kMaxGoldenObjBytes,
              (module + ": OBJ is small enough to pin as a fixture").c_str());

    const std::string digest = build_digest(summary, report.settings);
    const fs::path digest_path = fixtures / (module + ".golden");
    const fs::path obj_path = fixtures / (module + ".obj");
    const fs::path mtl_path = fixtures / (module + ".mtl");

    if (update_mode()) {
        std::error_code code;
        fs::create_directories(fixtures, code);
        CHECK(write_file(digest_path, digest), (module + ": digest fixture written").c_str());
        CHECK(write_file(mtl_path, mtl_text), (module + ": MTL fixture written").c_str());
        if (part.pin_geometry)
            CHECK(write_file(obj_path, obj_text), (module + ": OBJ fixture written").c_str());
        printf("  updated fixtures for %s\n", module.c_str());
        return;
    }

    const std::string expected_digest = read_file(digest_path);
    if (expected_digest.empty()) {
        printf("FAIL: golden %s: no fixture at %s (regenerate with "
               "MATTER_EXPORT_GOLDEN_UPDATE=1)\n",
               module.c_str(), digest_path.string().c_str());
        ++g_failures;
        return;
    }
    compare_digest(module, expected_digest, digest);
    compare_mtl(module, read_file(mtl_path), mtl_text);
    if (part.pin_geometry) compare_obj(module, read_file(obj_path), obj_text);
}


// ---------------------------------------------------------------------------
// The DSL binding, end to end.
//
// world_definition_tests.cpp already covers `static exports` extraction and
// every rejection path. What is NOT covered there is the half that matters to
// a user: that the exporter actually READS the declaration and treats it as
// defaults an explicit flag can still beat. That needs a real scene, a real
// bake and a real export, so it lives here — on a throwaway project written by
// the test, so it depends on nothing in projects/world_demo.
// ---------------------------------------------------------------------------

void write_text_file(const fs::path& path, const std::string& text) {
    std::error_code code;
    fs::create_directories(path.parent_path(), code);
    if (!write_file(path, text)) {
        printf("FAIL: could not write fixture %s\n", path.string().c_str());
        ++g_failures;
    }
}

void test_scene_export_reads_world_exports(const std::string& sandbox) {
    const fs::path project = fs::path(sandbox) / "dsl_project";
    write_text_file(project / "objects" / "ExportFixtureBox.js",
                    "class ExportFixtureBox extends Part {\n"
                    "  static noImpostor = true;\n"
                    "  build(p) {\n"
                    "    this.fill(MAT.stone);\n"
                    "    this.box([0, 0, 0], [1, 1, 1]);\n"
                    "  }\n"
                    "}\n");
    write_text_file(project / "scenes" / "ExportFixtureScene" / "ExportFixtureScene.js",
                    "class ExportFixtureScene extends World {\n"
                    "  static roots = [{\n"
                    "    module: 'ExportFixtureBox',\n"
                    "    transform: [1, 0, 0, 3, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],\n"
                    "  }];\n"
                    "  static exports = {\n"
                    "    out: 'declared-export',\n"
                    "    textureSize: 128,\n"
                    "    normalSpace: 'flat',\n"
                    "  };\n"
                    "}\n");

    matter_export::ExportObjRequest request;
    request.project_dir = project.string();
    request.world_name = "ExportFixtureScene";
    request.engine_shared_lib_dir =
        (fs::path(repo_root()) / "MatterEngine3" / "shared-lib").string();
    request.kind = matter_export::ExportTargetKind::Scene;
    request.cache_root_override = (fs::path(sandbox) / "cache" / "dsl").string();
    // Deliberately NO overrides: everything must come from `static exports`.

    matter_export::ExportObjReport report;
    std::string error;
    if (!matter_export::run_export_obj(request, report, error)) {
        printf("FAIL: scene export from `static exports` failed: %s\n", error.c_str());
        ++g_failures;
        return;
    }

    CHECK(report.settings.texture_size == 128u,
          "dsl: textureSize comes from the scene's static exports");
    CHECK(report.settings.normal_space_flat,
          "dsl: normalSpace comes from the scene's static exports");
    CHECK(report.settings.texture_format == matter_export::TextureFormat::Png,
          "dsl: an undeclared textureFormat keeps the documented default");
    // `out` is authored relative to the project, not the process cwd.
    CHECK(report.settings.out_dir == (project / "declared-export").string(),
          "dsl: out resolves against the project directory");
    CHECK(report.parts.size() == 1u, "dsl: the scene's one root was exported");
    if (report.parts.empty()) return;

    const matter_export::PartExportSummary& part = report.parts.front();
    CHECK(part.name == "ExportFixtureBox", "dsl: the part is named after its module");
    CHECK(part.triangle_count > 0u, "dsl: the exported part has geometry");
    CHECK(part.charts.atlas_size == 128u, "dsl: the atlas was packed at the declared size");
    CHECK(part.instances.size() == 1u, "dsl: the root's placement is recorded");
    // The placement is recorded, NOT applied: the mesh stays in part-local
    // space so a consumer can instance it.
    if (part.instances.size() == 1u)
        CHECK(std::fabs(part.instances[0][3] - 3.0f) < 1e-6f,
              "dsl: the manifest carries the root transform");
    const std::string obj_text = read_file(fs::path(report.settings.out_dir) /
                                           "ExportFixtureBox.obj");
    const obj_parse::ObjFile obj = obj_parse::parse_obj(obj_text);
    CHECK(obj.ok() && !obj.faces.empty(), "dsl: the exported OBJ parses");
    bool local_space = true;
    for (size_t v = 0; v < obj.position_count(); ++v)
        local_space = local_space && obj.positions[v * 3u] < 2.5f;
    CHECK(local_space, "dsl: geometry is exported in part-local space, untransformed");

    // An explicit override beats the declaration — that is the whole point of
    // calling them defaults.
    matter_export::ExportObjRequest override_request = request;
    override_request.overrides.texture_size = 64u;
    override_request.overrides.out_dir =
        (fs::path(sandbox) / "dsl_override").string();
    matter_export::ExportObjReport override_report;
    if (!matter_export::run_export_obj(override_request, override_report, error)) {
        printf("FAIL: scene export with overrides failed: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    CHECK(override_report.settings.texture_size == 64u,
          "dsl: an explicit texture size overrides the declared one");
    CHECK(override_report.settings.out_dir == (fs::path(sandbox) / "dsl_override").string(),
          "dsl: an explicit out directory overrides the declared one");
    CHECK(override_report.settings.normal_space_flat,
          "dsl: an undeclared override leaves the scene's value in place");
}

} // namespace

int main() {
    const std::string sandbox = make_sandbox("sandbox/obj_export_golden", {"cache"});
    const fs::path fixtures = fs::path("fixtures") / "export";

    for (const GoldenPart& part : kGoldenParts) {
        printf("-- %s\n", part.module);
        run_part(part, sandbox, fixtures);
    }

    printf("-- World.exports (DSL binding)\n");
    test_scene_export_reads_world_exports(sandbox);

    if (g_failures == 0) printf("All obj export golden tests passed\n");
    return check_summary();
}
