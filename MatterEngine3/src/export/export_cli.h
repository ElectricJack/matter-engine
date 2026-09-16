#pragma once

// MatterEngine3/src/export/export_cli.h
//
// The implementation of `matter export obj`, minus the argv parsing.
//
// WHY IT IS HERE AND NOT IN tools/matter_cli.cpp. Two callers need the whole
// pipeline — resolve a target, bake it, flatten it, export it — and they must
// exercise the SAME path: the CLI itself, and the golden-fixture test
// (MatterEngine3/tests/obj_export_golden_tests.cpp). A test that re-implemented
// the resolve/bake sequence would be testing its own copy. So the command lives
// here and tools/matter_cli.cpp is argv parsing and a report printer.
//
// THIS IS THE ONLY PART OF src/export THAT NEEDS THE SCRIPT HOST. Everything
// below it (mesh_export, texture_bake, obj_writer, part_export) takes baked
// artifacts, which is what lets the main export gate link without QuickJS.
//
// WHAT A TARGET CAN BE
//   module      a part module name resolved against the project's object tiers
//               (scene tier first, then project tier — LocalProviderConfig's
//               own search order). Baked if the cache misses.
//   scene       every root of a world/scene script, deduplicated by resolved
//               hash, with each root's placement recorded in the manifest
//               rather than baked into the geometry.
//   hash        a bundle already in the cache. Nothing is evaluated, nothing is
//               baked, and nothing is written into the cache.
//
// THE WORLD'S OWN `static exports` (the DSL binding). A scene script can
// declare what it wants exported; see matter/world_definition.h's
// WorldExportRequest and docs/export-obj.md. Those values are defaults —
// anything the caller passes in `overrides` wins — and they are only consulted
// for a scene target, because that is the only target that has a world.

#include "export/part_export.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace matter_export {

// Each field is "unset unless the caller said so". Resolution order is
// ExportSettings' compiled defaults, then the scene's `static exports`, then
// these.
struct ExportObjOverrides {
    std::optional<std::string> out_dir;
    std::optional<uint32_t> lod;
    std::optional<uint32_t> texture_size;
    std::optional<uint32_t> gutter_texels;
    std::optional<float> chart_cone_deg;
    std::optional<TextureFormat> texture_format;
    std::optional<bool> normal_space_flat;
    // Restrict a scene export to these root modules. Empty vector = every root.
    std::optional<std::vector<std::string>> modules;
};

enum class ExportTargetKind : uint32_t { Module = 0, Scene, Hash };

struct ExportObjRequest {
    std::string project_dir;
    // The scene/world whose object tier and cache namespace are used. For a
    // scene target this is the scene; for a module target it names the cache
    // bucket (<project>/.cache/<world_name>) and may be the module itself.
    std::string world_name;
    // MatterEngine3/shared-lib — the engine module prelude.
    std::string engine_shared_lib_dir;

    ExportTargetKind kind = ExportTargetKind::Module;
    std::string module;                      // Module targets
    std::string module_params_json = "{}";   // Module targets
    uint64_t resolved_hash = 0;              // Hash targets

    // Overrides <project>/.cache/<world_name>. Required in practice for a Hash
    // target that points somewhere else.
    std::string cache_root_override;

    ExportObjOverrides overrides;

    // Run part_flatten on each root before exporting, which is what gives a
    // composite part the merged, QEM-laddered mesh the renderer itself uses.
    // Defaults to true for Module and Scene targets (the CLI has just baked
    // into this cache, so completing the bake is expected) and is forced FALSE
    // for a Hash target, which must never write into a cache the caller only
    // asked to read.
    bool flatten = true;
};

struct ExportObjReport {
    std::string cache_root;
    ExportSettings settings;
    std::vector<PartExportSummary> parts;
    uint32_t baked = 0;        // parts this run had to bake
    uint32_t cache_hits = 0;   // parts already in the cache
    uint32_t flattened = 0;    // roots part_flatten ran on
    std::vector<std::string> warnings;
};

// Resolve, bake if needed, export, and write `<out_dir>/manifest.json`.
// Fails (false, `error` set) with a message meant to be printed straight to a
// user. Partial output is left on disk on failure, deliberately: a half-written
// export directory is easier to diagnose than a silently removed one.
bool run_export_obj(const ExportObjRequest& request, ExportObjReport& report,
                    std::string& error);

// Parse a 16-hex-digit resolved hash. False for anything else, including a
// shorter or longer run of hex digits — a truncated hash must not silently
// address a different part.
bool parse_resolved_hash(const std::string& text, uint64_t& out);

// What a BARE target name (no `scene:` prefix, not a hash) refers to in this
// project. A module wins over a scene of the same name, because a part is the
// thing `matter export obj <name>` is overwhelmingly asked for and the `scene:`
// prefix is always available to say otherwise. `Unknown` means neither exists,
// which the caller should report naming both places it looked.
enum class TargetProbe : uint32_t { Unknown = 0, Module, Scene };
TargetProbe probe_target(const std::string& project_dir, const std::string& name,
                         const std::string& engine_shared_lib_dir);

} // namespace matter_export
