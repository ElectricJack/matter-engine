// MatterEngine3/src/export/export_cli.cpp — see export_cli.h for what a target
// can be and where `static exports` fits.

#include "export/export_cli.h"

#include "export/export_text.h"
#include "matter/log.h"
#include "matter/world_definition.h"
#include "part_flatten.h"
#include "part_graph.h"
#include "provider/local_provider.h"   // LocalProviderConfig::for_project
#include "script/world_definition_loader.h"
#include "script_host.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace matter_export {
namespace {

std::string join_path(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + leaf;
    return dir + "/" + leaf;
}

// Resolve an authored/CLI output directory against the PROJECT, not the process
// cwd: a scene's `static exports.out` is written by someone thinking in project
// paths, and a CLI run from anywhere must mean the same thing.
std::string resolve_out_dir(const std::string& project_dir, const std::string& out_dir) {
    std::filesystem::path path(out_dir);
    if (path.is_absolute()) return path.string();
    std::error_code code;
    std::filesystem::path base = std::filesystem::absolute(
        std::filesystem::path(project_dir), code);
    if (code) base = std::filesystem::path(project_dir);
    return (base / path).string();
}

bool texture_format_from_name(const std::string& name, TextureFormat& out) {
    return parse_texture_format(name, out);
}

} // namespace

bool parse_resolved_hash(const std::string& text, uint64_t& out) {
    if (text.size() != 16u) return false;
    uint64_t value = 0;
    for (char c : text) {
        uint64_t digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<uint64_t>(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') digit = static_cast<uint64_t>(c - 'A') + 10u;
        else return false;
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

bool run_export_obj(const ExportObjRequest& request, ExportObjReport& report,
                    std::string& error) {
    error.clear();
    report = ExportObjReport{};

    if (request.project_dir.empty()) {
        error = "no project directory: pass --project";
        return false;
    }
    if (request.world_name.empty()) {
        error = "no world/scene name: pass --world (it also names the cache bucket)";
        return false;
    }

    const viewer::LocalProviderConfig layout = viewer::LocalProviderConfig::for_project(
        request.project_dir, request.world_name, request.engine_shared_lib_dir);
    report.cache_root = request.cache_root_override.empty() ? layout.cache_root
                                                            : request.cache_root_override;

    // ---- settings: compiled defaults, then the scene's own declaration, then
    // whatever the caller overrode ------------------------------------------
    ExportSettings settings;
    std::vector<std::string> module_filter;
    matter::WorldDefinition world;
    const bool is_scene = request.kind == ExportTargetKind::Scene;

    if (is_scene) {
        matter::WorldLoadDesc load_desc;
        load_desc.world_path = layout.world_path;
        load_desc.objects_dir = layout.objects_dir;
        load_desc.project_shared_lib_dir = layout.project_shared_lib_dir;
        load_desc.engine_shared_lib_dir = layout.engine_shared_lib_dir;
        matter::WorldLoadError load_error;
        if (!matter::load_world_definition(load_desc, world, load_error)) {
            error = "could not load scene '" + request.world_name + "' (" +
                    layout.world_path + "): " + load_error.message;
            if (!load_error.property_path.empty())
                error += " [" + load_error.property_path + "]";
            return false;
        }
        for (const matter::WorldExportRequest& declared : world.exports) {
            if (declared.format != "obj") continue;
            settings.out_dir = declared.out_dir;
            settings.lod = declared.lod;
            settings.texture_size = declared.texture_size;
            settings.chart_cone_deg = declared.chart_cone_deg;
            settings.normal_space_flat = declared.normal_space == "flat";
            TextureFormat declared_format = TextureFormat::Png;
            if (texture_format_from_name(declared.texture_format, declared_format))
                settings.texture_format = declared_format;
            module_filter = declared.modules;
            break;
        }
    }

    const ExportObjOverrides& overrides = request.overrides;
    if (overrides.out_dir) settings.out_dir = *overrides.out_dir;
    if (overrides.lod) settings.lod = *overrides.lod;
    if (overrides.texture_size) settings.texture_size = *overrides.texture_size;
    if (overrides.gutter_texels) settings.gutter_texels = *overrides.gutter_texels;
    if (overrides.chart_cone_deg) settings.chart_cone_deg = *overrides.chart_cone_deg;
    if (overrides.texture_format) settings.texture_format = *overrides.texture_format;
    if (overrides.normal_space_flat) settings.normal_space_flat = *overrides.normal_space_flat;
    if (overrides.modules) module_filter = *overrides.modules;

    if (settings.out_dir.empty()) {
        error = is_scene
                    ? "no output directory: pass --out, or declare `static exports` "
                      "on the scene (docs/export-obj.md)"
                    : "no output directory: pass --out";
        return false;
    }
    settings.out_dir = resolve_out_dir(request.project_dir, settings.out_dir);

    if (!ensure_directory(settings.out_dir, error)) return false;
    report.settings = settings;

    // ---- resolve the target to (name, hash, placements) --------------------
    struct Target {
        std::string name;
        uint64_t hash = 0;
        std::vector<std::array<float, 16>> instances;
    };
    std::vector<Target> targets;

    if (request.kind == ExportTargetKind::Hash) {
        if (request.resolved_hash == 0u) {
            error = "hash target needs a 16-hex-digit resolved hash";
            return false;
        }
        Target target;
        target.hash = request.resolved_hash;
        target.name = "part_" + format_hex64(request.resolved_hash);
        targets.push_back(std::move(target));
    } else {
        // Both the module and the scene target bake, so both need a cache root
        // that exists.
        if (!ensure_directory(join_path(report.cache_root, "parts"), error)) return false;

        script_host::ScriptHost host;
        host.set_shared_lib_roots(layout.shared_lib_roots());
        part_graph::FileModuleResolver resolver(host, layout.object_roots());
        part_graph::HostBaker baker(host, report.cache_root);
        part_graph::PartGraph graph(resolver, baker);

        std::vector<part_graph::ChildRequest> roots;
        std::vector<std::array<float, 16>> placements;
        std::vector<std::string> names;

        if (request.kind == ExportTargetKind::Module) {
            if (request.module.empty()) {
                error = "module target needs a module name";
                return false;
            }
            if (layout.resolve_object_path(request.module).empty()) {
                error = "no module '" + request.module + "' under " +
                        layout.objects_dir +
                        (layout.scene_objects_dir.empty()
                             ? std::string()
                             : " or " + layout.scene_objects_dir);
                return false;
            }
            part_graph::ChildRequest root;
            root.module = request.module;
            root.params = part_graph::params_from_json(request.module_params_json);
            roots.push_back(std::move(root));
            names.push_back(request.module);
            placements.push_back(std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0,
                                                       0, 0, 1, 0, 0, 0, 0, 1});
        } else {
            for (const matter::WorldRoot& world_root : world.roots) {
                if (world_root.tileset) {
                    report.warnings.push_back(
                        "skipped tileset root '" + world_root.module +
                        "': a ground tileset is a baked atlas, not an exportable Part");
                    continue;
                }
                if (!module_filter.empty() &&
                    std::find(module_filter.begin(), module_filter.end(),
                              world_root.module) == module_filter.end())
                    continue;
                part_graph::ChildRequest root;
                root.module = world_root.module;
                root.params = part_graph::params_from_json(world_root.params_json);
                roots.push_back(std::move(root));
                names.push_back(world_root.module);
                std::array<float, 16> transform{};
                for (int i = 0; i < 16; ++i) transform[i] = world_root.transform.m[i];
                placements.push_back(transform);
            }
            if (roots.empty()) {
                error = module_filter.empty()
                            ? "scene '" + request.world_name + "' declares no exportable roots"
                            : "no root of scene '" + request.world_name +
                                  "' matches the requested modules";
                return false;
            }
        }

        const part_graph::InstallResult installed = graph.install(roots);
        if (!installed.ok) {
            error = "bake failed: " + installed.error;
            return false;
        }
        report.baked = static_cast<uint32_t>(installed.baked.size());
        report.cache_hits = static_cast<uint32_t>(installed.hits);
        for (const part_graph::FailedPart& failed : installed.failed)
            report.warnings.push_back("part '" + failed.module + "' failed to bake: " +
                                      failed.error);

        // One Target per distinct resolved hash: two roots of the same module
        // with the same params are one exported mesh with two placements.
        for (size_t i = 0; i < installed.root_hashes.size() && i < names.size(); ++i) {
            const uint64_t hash = installed.root_hashes[i];
            if (hash == 0u) {
                report.warnings.push_back("root '" + names[i] +
                                          "' did not resolve and was not exported");
                continue;
            }
            Target* existing = nullptr;
            for (Target& candidate : targets)
                if (candidate.hash == hash) { existing = &candidate; break; }
            if (existing) {
                existing->instances.push_back(placements[i]);
                continue;
            }
            Target target;
            target.hash = hash;
            target.name = names[i];
            target.instances.push_back(placements[i]);
            targets.push_back(std::move(target));
        }
        if (targets.empty()) {
            error = "nothing resolved to export";
            return false;
        }

        // Flatten, so `--lod N` addresses the merged QEM ladder the renderer
        // itself draws rather than the compositional part's own rungs. Never
        // for a Hash target: that cache is the caller's, read-only.
        if (request.flatten) {
            for (const Target& target : targets) {
                const part_flatten::FlattenResult flattened =
                    part_flatten::flatten_part(report.cache_root, target.hash);
                if (!flattened.ok) {
                    report.warnings.push_back(
                        "flatten of '" + target.name + "' failed (" + flattened.error +
                        "); exporting the compositional part instead");
                    continue;
                }
                ++report.flattened;
            }
        }
    }

    // ---- export ------------------------------------------------------------
    for (const Target& target : targets) {
        PartExportRequest part_request;
        part_request.cache_root = report.cache_root;
        part_request.resolved_hash = target.hash;
        part_request.name = target.name;
        part_request.instances = target.instances;

        PartExportSummary summary;
        if (!export_part(part_request, settings, summary, error)) {
            error = "export of '" + target.name + "' failed: " + error;
            return false;
        }
        report.parts.push_back(std::move(summary));
    }

    if (!write_manifest(report.parts, settings, error)) return false;
    return true;
}

} // namespace matter_export
