// MatterEngine3/tools/matter_cli.cpp — the `matter` headless command.
//
//   matter export obj <part-id|scene> --out <dir> [--lod N] [--texture-size N]
//
// This file is argv parsing and a report printer. The command itself is
// MatterEngine3/src/export/export_cli.h's run_export_obj, so that the
// golden-fixture test drives exactly the code path a user does rather than a
// re-implementation of it.
//
// OUTPUT DISCIPLINE (CLAUDE.md, "Logging"): the report on stdout is deliberate
// machine-readable program output and stays on printf. Diagnostics go through
// MATTER_LOGE, which tees to stderr, so a caller can parse stdout without
// filtering warnings out of it.
//
// EXIT CODES: 0 success, 1 a usage error or a failed export, 2 nothing to do
// (`--help`). Anything non-zero has already printed a reason to stderr.

#include "export/export_cli.h"
#include "export/export_text.h"
#include "matter/log.h"
#ifdef MATTER_ENABLE_GI_CLI
#include "../cli/matter_bake_cli.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

const char* const kUsage =
    "matter — MatterEngine3 headless tools\n"
    "\n"
    "Usage:\n"
    "  matter export obj <part-id|scene> --out <dir> [options]\n"
    "  matter bake gi <scene> --out <dir> [options]\n"
    "\n"
    "Target forms:\n"
    "  <Module>            a part module in the project's object tiers; baked on a\n"
    "                      cache miss. --world names the cache bucket and defaults\n"
    "                      to the module name.\n"
    "  <Scene>             a bare name that is not a module but is a scene exports\n"
    "                      that scene; use scene:<Name> when both exist.\n"
    "  scene:<Name>        every root of scenes/<Name>/<Name>.js (or worlds/<Name>.js),\n"
    "                      one OBJ per distinct part, placements in manifest.json.\n"
    "  <16 hex digits>     a bundle already in the cache. Nothing is evaluated,\n"
    "                      baked, flattened or written into the cache.\n"
    "\n"
    "Options:\n"
    "  --out <dir>             output directory; created if missing. Relative paths\n"
    "                          resolve against --project, not the current directory.\n"
    "  --project <dir>         project root (default: projects/world_demo)\n"
    "  --world <name>          scene / cache bucket (default: the target)\n"
    "  --engine-shared-lib <d> engine module prelude (default: MatterEngine3/shared-lib)\n"
    "  --cache-root <dir>      override <project>/.cache/<world>\n"
    "  --lod N                 rung index into the LOD ladder, 0 = finest (default 0)\n"
    "  --texture-size N        atlas edge in texels, 16..8192 (default 2048)\n"
    "  --texture-format F      png | ktx2 | none (default png; ktx2 is reserved)\n"
    "  --gutter N              dilated texels around each chart (default 4)\n"
    "  --chart-cone D          chart normal-cone half-angle in degrees (default 45)\n"
    "  --normal-space S        smooth | flat (default smooth; see docs/export-obj.md)\n"
    "  --modules a,b,c         restrict a scene export to these root modules\n"
    "  --params <json>         params override for a module target (default {})\n"
    "  --no-flatten            export the compositional part instead of running\n"
    "                          part_flatten first (always off for a hash target)\n"
    "  -h, --help              this text\n"
    "\n"
    "A scene may declare its own defaults with `static exports` — see\n"
    "docs/export-obj.md. Any option above overrides them.\n";

bool parse_uint(const std::string& text, uint32_t& out) {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10u + static_cast<uint64_t>(c - '0');
        if (value > 0xFFFFFFFFull) return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

bool parse_float(const std::string& text, float& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return false;
    out = static_cast<float>(value);
    return true;
}

std::vector<std::string> split_commas(const std::string& text) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const size_t end = (comma == std::string::npos) ? text.size() : comma;
        if (end > start) parts.push_back(text.substr(start, end - start));
        if (comma == std::string::npos) break;
        start = comma + 1u;
    }
    return parts;
}

// The repository root, found by walking up from the executable's directory
// looking for MatterEngine3/shared-lib. Falls back to the current directory,
// which is what a developer running from the repo root gets anyway.
std::string guess_repo_root() {
    std::error_code code;
    std::filesystem::path here = std::filesystem::current_path(code);
    if (code) return ".";
    for (int depth = 0; depth < 8; ++depth) {
        if (std::filesystem::is_directory(here / "MatterEngine3" / "shared-lib", code))
            return here.string();
        if (!here.has_parent_path() || here.parent_path() == here) break;
        here = here.parent_path();
    }
    return ".";
}

int export_obj_command(int argc, char** argv) {
    if (argc < 1) {
        MATTER_LOGE("export", "%s", "export obj needs a target; try --help");
        return 1;
    }

    matter_export::ExportObjRequest request;
    matter_export::ExportObjOverrides& overrides = request.overrides;
    std::string target;
    bool flatten_requested = true;

    for (int i = 0; i < argc; ++i) {
        const std::string argument = argv[i];
        auto next = [&](const char* name, std::string& out) -> bool {
            if (i + 1 >= argc) {
                MATTER_LOGE("export", "%s needs a value", name);
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string value;

        if (argument == "-h" || argument == "--help") {
            std::printf("%s", kUsage);
            return 2;
        }
        if (argument == "--out") {
            if (!next("--out", value)) return 1;
            overrides.out_dir = value;
        } else if (argument == "--project") {
            if (!next("--project", value)) return 1;
            request.project_dir = value;
        } else if (argument == "--world") {
            if (!next("--world", value)) return 1;
            request.world_name = value;
        } else if (argument == "--engine-shared-lib") {
            if (!next("--engine-shared-lib", value)) return 1;
            request.engine_shared_lib_dir = value;
        } else if (argument == "--cache-root") {
            if (!next("--cache-root", value)) return 1;
            request.cache_root_override = value;
        } else if (argument == "--lod") {
            uint32_t parsed = 0;
            if (!next("--lod", value) || !parse_uint(value, parsed)) {
                MATTER_LOGE("export", "--lod must be a non-negative integer, got '%s'",
                            value.c_str());
                return 1;
            }
            overrides.lod = parsed;
        } else if (argument == "--texture-size") {
            uint32_t parsed = 0;
            if (!next("--texture-size", value) || !parse_uint(value, parsed)) {
                MATTER_LOGE("export", "--texture-size must be an integer, got '%s'",
                            value.c_str());
                return 1;
            }
            overrides.texture_size = parsed;
        } else if (argument == "--gutter") {
            uint32_t parsed = 0;
            if (!next("--gutter", value) || !parse_uint(value, parsed)) {
                MATTER_LOGE("export", "--gutter must be an integer, got '%s'", value.c_str());
                return 1;
            }
            overrides.gutter_texels = parsed;
        } else if (argument == "--chart-cone") {
            float parsed = 0.0f;
            if (!next("--chart-cone", value) || !parse_float(value, parsed)) {
                MATTER_LOGE("export", "--chart-cone must be a number, got '%s'", value.c_str());
                return 1;
            }
            overrides.chart_cone_deg = parsed;
        } else if (argument == "--texture-format") {
            matter_export::TextureFormat parsed = matter_export::TextureFormat::Png;
            if (!next("--texture-format", value) ||
                !matter_export::parse_texture_format(value, parsed)) {
                MATTER_LOGE("export",
                            "--texture-format must be png, ktx2 or none, got '%s'",
                            value.c_str());
                return 1;
            }
            overrides.texture_format = parsed;
        } else if (argument == "--normal-space") {
            if (!next("--normal-space", value)) return 1;
            if (value != "smooth" && value != "flat") {
                MATTER_LOGE("export", "--normal-space must be smooth or flat, got '%s'",
                            value.c_str());
                return 1;
            }
            overrides.normal_space_flat = (value == "flat");
        } else if (argument == "--modules") {
            if (!next("--modules", value)) return 1;
            overrides.modules = split_commas(value);
        } else if (argument == "--params") {
            if (!next("--params", value)) return 1;
            request.module_params_json = value;
        } else if (argument == "--no-flatten") {
            flatten_requested = false;
        } else if (!argument.empty() && argument[0] == '-') {
            MATTER_LOGE("export", "unknown option '%s'; try --help", argument.c_str());
            return 1;
        } else if (target.empty()) {
            target = argument;
        } else {
            MATTER_LOGE("export", "unexpected extra target '%s'", argument.c_str());
            return 1;
        }
    }

    if (target.empty()) {
        MATTER_LOGE("export", "%s", "export obj needs a target; try --help");
        return 1;
    }

    const std::string repo_root = guess_repo_root();
    if (request.project_dir.empty())
        request.project_dir = (std::filesystem::path(repo_root) / "projects" / "world_demo").string();
    if (request.engine_shared_lib_dir.empty())
        request.engine_shared_lib_dir =
            (std::filesystem::path(repo_root) / "MatterEngine3" / "shared-lib").string();

    // Target form. A bare 16-hex-digit token is a resolved hash; `scene:` says
    // explicitly which of the two a name means; anything else is probed against
    // the project, with a module beating a scene of the same name. The prefix
    // stays available precisely because that tie-break is a choice — someone who
    // wants the scene should be able to say so rather than rename a part.
    if (target.rfind("scene:", 0) == 0) {
        request.kind = matter_export::ExportTargetKind::Scene;
        const std::string name = target.substr(6);
        if (name.empty()) {
            MATTER_LOGE("export", "%s", "scene: needs a scene name");
            return 1;
        }
        if (request.world_name.empty()) request.world_name = name;
    } else if (matter_export::parse_resolved_hash(target, request.resolved_hash)) {
        request.kind = matter_export::ExportTargetKind::Hash;
        if (request.world_name.empty()) request.world_name = "parts";
    } else {
        const matter_export::TargetProbe probe = matter_export::probe_target(
            request.project_dir, target, request.engine_shared_lib_dir);
        if (probe == matter_export::TargetProbe::Unknown) {
            MATTER_LOGE("export",
                        "'%s' is neither a part module nor a scene under %s "
                        "(looked in objects/, scenes/%s/objects/, scenes/%s/%s.js and "
                        "worlds/%s.js); a 16-hex-digit resolved hash also works",
                        target.c_str(), request.project_dir.c_str(), target.c_str(),
                        target.c_str(), target.c_str(), target.c_str());
            return 1;
        }
        if (probe == matter_export::TargetProbe::Scene) {
            request.kind = matter_export::ExportTargetKind::Scene;
        } else {
            request.kind = matter_export::ExportTargetKind::Module;
            request.module = target;
        }
        if (request.world_name.empty()) request.world_name = target;
    }

    // A hash target names a cache the caller only asked us to read.
    request.flatten =
        flatten_requested && request.kind != matter_export::ExportTargetKind::Hash;

    matter_export::ExportObjReport report;
    std::string error;
    if (!matter_export::run_export_obj(request, report, error)) {
        MATTER_LOGE("export", "%s", error.c_str());
        return 1;
    }

    for (const std::string& warning : report.warnings)
        MATTER_LOGW("export", "%s", warning.c_str());

    std::printf("==== obj export ====\n");
    std::printf("cache_root %s\n", report.cache_root.c_str());
    std::printf("out_dir %s\n", report.settings.out_dir.c_str());
    std::printf("lod %u texture_size %u baked %u cache_hits %u flattened %u\n",
                report.settings.lod, report.settings.texture_size, report.baked,
                report.cache_hits, report.flattened);
    for (const matter_export::PartExportSummary& part : report.parts) {
        std::printf("part %s hash %s lod %u/%u verts %u tris %u charts %u materials %zu\n",
                    part.name.c_str(),
                    matter_export::format_hex64(part.resolved_hash).c_str(), part.lod,
                    part.lod_count, part.vertex_count, part.triangle_count,
                    part.charts.chart_count, part.materials.size());
        for (const matter_export::WrittenFile& file : part.files.files)
            std::printf("  file %s %llu\n", file.name.c_str(),
                        static_cast<unsigned long long>(file.bytes));
        for (const std::string& warning : part.warnings)
            MATTER_LOGW("export", "%s: %s", part.name.c_str(), warning.c_str());
    }
    std::printf("manifest %s\n",
                (std::filesystem::path(report.settings.out_dir) / "manifest.json")
                    .string()
                    .c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("%s", kUsage);
        return 2;
    }
    const std::string group = argv[1];
    if (group == "-h" || group == "--help") {
        std::printf("%s", kUsage);
        return 2;
    }
    if (group == "bake") {
#ifdef MATTER_ENABLE_GI_CLI
        if (argc >= 3 && std::strcmp(argv[2], "gi") == 0)
            return matter_bake_gi_cli_main(argc - 3, argv + 3);
        MATTER_LOGE("matter", "%s", "bake needs a format; today only 'gi'");
#else
        MATTER_LOGE("matter", "%s", "this rollback build supports export obj only");
#endif
        return 1;
    }
    if (group != "export") {
        MATTER_LOGE("matter", "unknown command '%s'; try --help", group.c_str());
        return 1;
    }
    if (argc < 3) {
        MATTER_LOGE("matter", "%s", "export needs a format; today only 'obj'");
        return 1;
    }
    const std::string format = argv[2];
    if (format != "obj") {
        MATTER_LOGE("matter",
                    "unknown export format '%s'; today only 'obj' "
                    "(glTF is a deliberate non-goal, see docs/export-obj.md)",
                    format.c_str());
        return 1;
    }
    return export_obj_command(argc - 3, argv + 3);
}
