#pragma once

// MatterEngine3/src/export/mesh_export.h
//
// THE FORMAT-AGNOSTIC HALF OF ASSET EXPORT. Everything here describes a baked
// Part as plain geometry + materials + texture images; nothing here knows what
// Wavefront OBJ, glTF or USD look like. obj_writer.{h,cpp} is the first (and
// today only) consumer, and a glTF writer would be a second one over the same
// ExportModel — that separation is the reason this file exists rather than an
// obj_writer that reaches into BLASManager itself.
//
// WHERE THE DATA COMES FROM. A committed Part artifact (part_asset_v2.h) hands
// back a BLASManager whose BLASEntry carries `triangles` (Tri) and `tri_extra`
// (TriEx: per-vertex normals/UV/AO, per-triangle materialId and tint) plus an
// ordered LOD ladder whose `blas_indices` are absolute indices into
// `blas.get_entries()` (see lod_bake.h's note on that — the value is stable
// only while nothing is released, which is true for a freshly loaded artifact).
// build_export_model walks one rung of that ladder and turns it into the
// structures below.
//
// WHAT IT DOES TO THE GEOMETRY, in order:
//   1. GATHER   — concatenate the rung's BLAS entries in blas_indices order.
//   2. CHART    — segment the triangle soup into planar charts and pack them
//                 into ONE square atlas of `texture_size` texels with
//                 libs/MeshChartingLib (the same build_adjacency ->
//                 segment_charts -> chart_average_normals -> plane_basis
//                 sequence lod_bake.cpp's build_chart_rung drives for the
//                 chart-space VT bake, but with the free-form `pack_charts`
//                 so the atlas is exactly the requested size instead of a
//                 page-aligned virtual one). The exporter ALWAYS builds its
//                 own UV set: a part's baked CHRT sidecar, when it has one at
//                 all, is sized and paged for virtual texturing, which is not
//                 what a single 2048² OBJ atlas wants.
//   3. WELD     — viewer::build_indexed_part_geometry, the engine's one
//                 deterministic weld (exact match on every attribute,
//                 first-encounter ordering), fed the chart UVs.
//   4. GROUP    — split the index buffer into one submesh per material id,
//                 ascending, so a writer can emit per-material groups.
//
// COORDINATE CONVENTION. Positions pass through UNCHANGED from part-local
// space: metres, Y-up, right-handed, triangle winding as the mesher emitted it
// (counter-clockwise when seen from outside — obj_export_tests.cpp asserts
// that against the shading normals). Nothing here rescales, re-origins or
// re-winds; a consumer that needs another convention converts at its own edge.
//
// DETERMINISM IS A CONTRACT, not a nicety: the same artifact and the same
// ExportOptions must produce byte-identical output so exports can be diffed
// and content-addressed. Every stage above is order-deterministic, and the
// texture bake (texture_bake.h) and the writers keep that property. Anything
// added here must too — no hash-map iteration order, no unstable sorts, no
// floating-point that depends on thread count.
//
// THREADING. Free functions over caller-owned data, no globals except the
// material registry, which is read (never written) through
// MaterialRegistryGet. Safe to call concurrently on different artifacts as
// long as nothing else is mutating the registry.

#include "blas_manager.hpp"      // BLASManager, BLASEntry (MatterSurfaceLib)
#include "material_registry.h"   // MaterialDef
#include "part_asset_v2.h"       // part_asset::LodLevel / LodLevels
#include "tri.h"                 // Tri, TriEx (SpatialQueryLib)

#include <cstdint>
#include <string>
#include <vector>

namespace matter_export {

// ---------------------------------------------------------------------------
// Texture maps
// ---------------------------------------------------------------------------

// The PBR channel set the exporter bakes. The order is the order maps are
// emitted in a manifest and in an MTL, so it is part of the byte-level
// contract; append only.
enum class MapKind : uint32_t {
    Albedo = 0,     // RGB8, sRGB-encoded (three.js: map, SRGBColorSpace)
    Normal,         // RGB8, tangent space, linear, +Z out (map_Bump / norm)
    Roughness,      // R8, linear (map_Pr)
    Metallic,       // R8, linear (map_Pm)
    Emissive,       // RGB8, sRGB-encoded (map_Ke)
    Occlusion,      // R8, linear; 255 = unoccluded (map_Ka)
    Count
};
inline constexpr uint32_t kMapCount = static_cast<uint32_t>(MapKind::Count);

// Stable, lowercase, filename-safe suffix for a map. Used for
// "<base>_<suffix>.png" and as the manifest key, so it is part of the output
// contract.
const char* map_suffix(MapKind kind);

// One baked image. `texels` is row-major, ROW 0 IS THE TOP of the image (PNG
// order), `channels` bytes per texel, `width * height * channels` bytes total.
// An image the bake skipped is left with width == height == 0.
struct ExportImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    std::vector<uint8_t> texels;
    // fnv1a64 over (width, height, channels, texels). THE STABLE TEXTURE HASH
    // the spec asks for: it is taken over the decoded payload, not over the
    // encoded PNG, so it is independent of the encoder and of any future
    // switch to KTX2. Zero for an empty image.
    uint64_t content_hash = 0;

    bool empty() const { return width == 0 || height == 0; }
};

// Recompute content_hash from the current dims + texels.
void finalize_image_hash(ExportImage& image);

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

// One material referenced by the exported mesh. `id` is the engine material
// registry index the TriEx carried; `name` is what a writer emits and what the
// mesh's submeshes are grouped by.
//
// The scalars are the authored MaterialDef values, kept here so a writer can
// emit them as flat material parameters even when it also emits texture maps.
// They are NOT redundant with the maps: a consumer that ignores textures still
// gets the right colour, and the maps are what a consumer uses when a part
// mixes several materials over one atlas.
struct ExportMaterial {
    uint32_t id = 0;
    std::string name;
    float albedo[3] = {1.0f, 1.0f, 1.0f};
    float emissive[3] = {0.0f, 0.0f, 0.0f};  // emissionColor * emission
    float roughness = 1.0f;
    float metallic = 0.0f;
    float opacity = 1.0f;
    float ior = 1.5f;
    float alpha_cutoff = 0.5f;
    bool alpha_tested = false;
    bool double_sided = false;
};

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

// The triangles of one material, as offsets into ExportMesh::indices. Half-open
// [first_index, first_index + index_count), always a multiple of 3. Submeshes
// are emitted in ascending material id and cover the index buffer exactly.
struct ExportSubmesh {
    uint32_t material_id = 0;
    uint32_t material_slot = 0;   // index into ExportModel::materials
    uint32_t first_index = 0;
    uint32_t index_count = 0;
};

// The welded vertex stream. Every per-vertex array is `vertex_count` long
// times its own stride; `indices` holds 3 corners per triangle.
struct ExportMesh {
    std::vector<float> positions;   // 3/vertex, part-local metres
    std::vector<float> normals;     // 3/vertex, unit length
    std::vector<float> uvs;         // 2/vertex, [0,1]; v is UP (OBJ/glTF order)
    std::vector<float> occlusion;   // 1/vertex, baked AO, 1 = unoccluded
    std::vector<float> tint;        // 4/vertex, RGBA; a = blend against albedo
    std::vector<uint32_t> material_ids;  // 1/vertex
    std::vector<uint32_t> indices;
    std::vector<ExportSubmesh> submeshes;
    uint32_t vertex_count = 0;

    uint32_t triangle_count() const {
        return static_cast<uint32_t>(indices.size() / 3u);
    }
};

// ---------------------------------------------------------------------------
// Charting
// ---------------------------------------------------------------------------

// What the chart pass decided, kept for the manifest and for diagnostics. The
// numbers here are the ones worth looking at when an export's texel density is
// disappointing: too many charts means the normal cone is too tight for the
// mesh, and a low `packing_fill` means the atlas is mostly gutter.
struct ChartStats {
    uint32_t chart_count = 0;
    uint32_t atlas_size = 0;        // texels per edge (square)
    float texels_per_meter = 0.0f;  // the packer's chosen scale
    float packing_fill = 0.0f;      // covered chart area / atlas area, 0-1
    float max_distortion = 0.0f;    // worst per-chart projection distortion
};

// ---------------------------------------------------------------------------
// Options and result
// ---------------------------------------------------------------------------

struct ExportOptions {
    // Atlas edge in texels. Must be >= 16 and <= 8192. The chart packer fills
    // exactly this square; the baked maps are this size.
    uint32_t texture_size = 2048;
    // Dilated texels reserved around every chart's content inside the atlas,
    // so bilinear sampling at a chart edge never reads a neighbouring chart.
    uint32_t gutter_texels = 4;
    // Normal-cone half-angle for chart segmentation, degrees. Must be < 90.
    float chart_cone_deg = 45.0f;
    // Skip the chart/UV pass entirely and emit no UVs. Only meaningful for a
    // mesh-only export (texture_format "none").
    bool generate_uvs = true;
};

// One Part, at one LOD, ready for a format writer.
struct ExportModel {
    std::string name;               // sanitized; used for file names and `o`
    uint64_t resolved_hash = 0;
    uint32_t lod = 0;
    uint32_t lod_count = 0;
    ExportMesh mesh;
    std::vector<ExportMaterial> materials;
    ChartStats charts;
    // Baked maps, indexed by MapKind. Filled by texture_bake.h, not by
    // build_export_model — an empty image means "not baked".
    ExportImage maps[kMapCount];
};

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

// Gather the triangles of one LOD rung out of a loaded artifact.
// `blas_indices` are absolute indices into blas.get_entries(); out-of-range
// entries are skipped (the artifact loaders already validate them, so this is
// belt-and-braces rather than an expected path). A BLAS entry whose tri_extra
// is missing or the wrong length contributes neutral attributes: face normal,
// material `default_material`, AO 1, no tint.
//
// Appends to the outputs, which stay parallel; returns the triangle count
// appended.
uint32_t gather_rung_triangles(const BLASManager& blas,
                               const std::vector<uint32_t>& blas_indices,
                               int default_material,
                               std::vector<Tri>& tris_out,
                               std::vector<TriEx>& triex_out);

// Build the export model from a triangle soup. `tris` and `triex` are parallel
// and must be the same length. Fails (false, `error` set) only for empty input
// or an unusable ExportOptions; a chart-packing failure is reported with an
// actionable message naming the atlas size to raise.
bool build_export_model(const std::vector<Tri>& tris,
                        const std::vector<TriEx>& triex,
                        const std::string& name,
                        uint64_t resolved_hash,
                        uint32_t lod,
                        uint32_t lod_count,
                        const ExportOptions& options,
                        ExportModel& out,
                        std::string& error);

// Read one material out of the engine registry. An id outside the live table
// yields a neutral mid-grey dielectric named "mat_<id>_missing" and returns
// false, so a part baked against materials this process never defined still
// exports rather than aborting. (That happens for real: a world's
// defineMaterial() entries are dynamic registry tail entries, not serialized
// into the artifact, so exporting a lone part whose author used a world
// material sees only the frozen builtins.)
bool describe_material(uint32_t material_id, ExportMaterial& out);

// fnv1a64 over a byte range, seeded as the rest of the engine seeds it. Shared
// with the writers so a manifest and an image agree on what a hash is.
uint64_t fnv1a64_bytes(const void* data, size_t len, uint64_t seed = 0xcbf29ce484222325ull);

} // namespace matter_export
