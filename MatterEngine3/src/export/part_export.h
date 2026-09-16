#pragma once

// MatterEngine3/src/export/part_export.h
//
// Artifact-in, files-out: the layer that turns a COMMITTED part bundle into an
// OBJ/MTL/texture set on disk, plus the manifest that describes a whole export
// directory.
//
// WHAT IT DELIBERATELY DOES NOT DO: bake. Nothing here starts a script host or
// a part graph, so the whole export path can be tested, and linked, without
// QuickJS. Turning a module name or a scene into resolved hashes is the CLI's
// job (MatterEngine3/tools/matter_cli.cpp); by the time it calls in here, the
// artifacts exist.
//
// SUBTREES. A part is rarely one mesh. A flat artifact inlines its children,
// but the flatten stage declares an INSTANCE BOUNDARY for a child subtree whose
// inlined size would blow the budget, and a part that was never flattened has a
// child-instance table instead. Both are followed here, transform-composed, so
// the OBJ a consumer gets is the whole part and not the parent's own geometry
// with the stones missing. Recursion is bounded (kMaxSubtreeDepth) and guarded
// against revisiting a hash on the current path, and every skipped reference is
// reported in the summary rather than dropped silently.
//
// LOD SELECTION. `lod` indexes the artifact's rung ladder, 0 = finest. A flat
// artifact's clusters each carry their own ladder, so rung N of the part means
// rung min(N, cluster.rungs - 1) of every cluster — a cluster with a shorter
// ladder contributes its coarsest rung rather than disappearing. Asking for a
// rung past the end of every ladder is an error, not a silent clamp, because a
// caller that asked for --lod 3 and silently got --lod 1 cannot tell.
//
// DETERMINISM. The whole chain below (gather order, chart pack, weld, texture
// rasterisation, text formatting) is deterministic, so exporting the same
// artifact twice writes byte-identical files and identical content hashes. The
// manifest is written with export_text.h's formatter for the same reason.

#include "export/export_image.h"
#include "export/mesh_export.h"
#include "export/obj_writer.h"
#include "export/texture_bake.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace matter_export {

// How deep child/instance references are followed before the exporter gives up
// and reports the truncation. Real part trees are a handful of levels; this is
// a guard against a cache that somehow contains a cycle, not a tuning dial.
inline constexpr uint32_t kMaxSubtreeDepth = 8;

struct ExportSettings {
    std::string out_dir;                 // must already exist
    uint32_t lod = 0;
    uint32_t texture_size = 2048;
    uint32_t gutter_texels = 4;
    float chart_cone_deg = 45.0f;
    TextureFormat texture_format = TextureFormat::Png;
    // See texture_bake.h's NORMAL SPACE note. Off = the glTF/three.js
    // convention.
    bool normal_space_flat = false;
};

// One part the exporter was asked to write.
struct PartExportRequest {
    // Cache root the bundle lives under; the exporter appends the
    // cache-relative path part_asset::cache_path_* returns.
    std::string cache_root;
    uint64_t resolved_hash = 0;
    std::string name;        // used for the `o` name and the file stem
    // Optional world placements of this part, row-major 4x4, recorded in the
    // manifest. NOT applied to the geometry: a part is exported in its own
    // local space so a consumer can instance it.
    std::vector<std::array<float, 16>> instances;
};

struct MaterialSummary {
    uint32_t id = 0;
    std::string name;
    float albedo[3] = {0.0f, 0.0f, 0.0f};
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float roughness = 0.0f;
    float metallic = 0.0f;
    uint32_t triangles = 0;
};

struct PartExportSummary {
    std::string name;
    uint64_t resolved_hash = 0;
    uint32_t lod = 0;
    uint32_t lod_count = 0;
    uint32_t vertex_count = 0;
    uint32_t triangle_count = 0;
    // Child/instance references that were followed and merged into the mesh.
    uint32_t inlined_subtree_refs = 0;
    // References that could NOT be followed (missing artifact, depth cap, a
    // hash already on the recursion path). Each one is also spelled out in
    // `warnings`.
    uint32_t skipped_subtree_refs = 0;
    std::vector<std::string> warnings;
    ChartStats charts;
    std::vector<MaterialSummary> materials;
    // Per-map content hashes, indexed by MapKind; 0 where nothing was baked.
    uint64_t map_hashes[kMapCount] = {0, 0, 0, 0, 0, 0};
    ObjWriteResult files;
    std::vector<std::array<float, 16>> instances;
};

// Load one artifact, build its export model, bake its maps and write the files.
// `settings.out_dir` must exist. Fails (false, `error` set) when the artifact
// cannot be loaded, the rung does not exist, or any stage below reports a
// problem; warnings that do not prevent an export land in
// `summary.warnings` instead.
bool export_part(const PartExportRequest& request, const ExportSettings& settings,
                 PartExportSummary& summary, std::string& error);

// Render the directory manifest. Deterministic and locale-independent.
std::string build_manifest_json(const std::vector<PartExportSummary>& parts,
                                const ExportSettings& settings);

// build_manifest_json + write to `<out_dir>/manifest.json`.
bool write_manifest(const std::vector<PartExportSummary>& parts,
                    const ExportSettings& settings, std::string& error);

// Create `path` and every missing parent. True when the directory exists
// afterwards.
bool ensure_directory(const std::string& path, std::string& error);

} // namespace matter_export
