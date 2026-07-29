#ifndef MATERIAL_REGISTRY_H
#define MATERIAL_REGISTRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A single material definition. This is the ONE place materials are defined;
// both the CPU (meshing decisions) and the GPU (shading) consume this table.
typedef enum MaterialSurfaceFlags {
    MATERIAL_SURFACE_NONE    = 0u,
    MATERIAL_THIN_WALLED     = 1u << 0,
    MATERIAL_DOUBLE_SIDED    = 1u << 1,
    MATERIAL_ALPHA_TESTED    = 1u << 2,
    MATERIAL_VOLUME_BOUNDARY = 1u << 3
} MaterialSurfaceFlags;

enum { MATERIAL_SCHEMA_VERSION = 4 };

typedef struct {
    float albedo[3];      // base color
    float roughness;      // 0 = mirror, 1 = rough
    float metallic;       // 0 = dielectric, 1 = metal
    float emission;       // emission strength
    float translucency;   // 0 = opaque, >0 = translucent (gates carving)
    float ior;            // index of refraction
    int   flatShading;    // 0 = smooth normals, 1 = flat
    int   mergeGroup;     // particles whose materials share a mergeGroup blend together
    int   meshingAlgorithm; // 0 = marching cubes (default), 1 = oriented cubes; selects the mesher
    int   groundTilesetSlot; // Phase 4: -1 = untextured, else the viewer tileset slot to sample,
                             // in [0, MATERIAL_MAX_DETAIL_SLOTS).
                             // Schema v4 (spec "Material schema"): this is now the "detail slot"
                             // (field name kept for source compatibility). For static registry
                             // entries this stays -1; the viewer runtime sets a live override via
                             // MaterialRegistrySetGroundTilesetSlot() after loading a world tileset
                             // atlas.
    int   groundMacroSlot;  // Schema v4: -1 = no macro layer, else the viewer tileset slot (in
                             // [0, MATERIAL_MAX_DETAIL_SLOTS)) sampled
                             // as the coarse macro/frequency-split layer (Phase 3). Overridden at
                             // runtime via MaterialRegistrySetGroundMacroSlot(). Vulkan-only: the
                             // GL path (MaterialRegistryPackForGPU, 12-float table) never reads
                             // this field; it flows solely through MaterialGpuRecord.flags_misc[1]
                             // (see MaterialRegistryPackRtForGPU and vk_gi_contract.h).
    float opacity;
    float transmission;
    float emissionColor[3];
    float absorptionColor[3];
    float absorptionDistance;
    float thickness;
    float subsurface;
    float scatteringColor[3];
    float scatteringDistance;
    float anisotropy;
    float clearcoat;
    float clearcoatRoughness;
    float specularStrength;
    float specularTint[3];
    float alphaCutoff;
    float shadowOpacity;
    uint32_t surfaceFlags;
} MaterialDef;

typedef struct MaterialGpuRecord {
    float base_roughness[4];
    float metal_opacity_spec_coat[4];
    float specular_tint_coat_roughness[4];
    float emission_strength[4];
    float transmission[4];
    float absorption_pad[4];
    float scattering[4];
    float scattering_shape[4];
    uint32_t flags_misc[4];
} MaterialGpuRecord;

// ---------------------------------------------------------------------------
// Registry capacity (static builtins + per-world dynamic extension)
// ---------------------------------------------------------------------------
// MATERIAL_MAX_TOTAL bounds every per-material side array in this module
// (g_slot_overrides / g_macro_overrides were already sized 64). Dynamic
// entries occupy indices [MaterialRegistryStaticCount(), MATERIAL_MAX_TOTAL).
enum { MATERIAL_MAX_TOTAL = 64 };
// Longest dynamic material name retained (including the terminator).
enum { MATERIAL_NAME_MAX = 64 };
// Number of viewer detail/macro tileset slots the packed slot byte accepts.
//
// MUST equal tileset::kMaxTilesetSlots (MatterEngine3/src/tileset_gtex.h),
// which is the single source of truth for the slot count and carries the full
// list of everything derived from it. This file is C and cannot include that
// C++ header, so the two are paired by comment — change both together. (The
// renderer asserts the pairing at compile time: see the static_assert next to
// kMaxTilesetSlots' use in vk_scene_renderer.cpp.)
//
// Raised 4 -> 8 by WP-B alongside the descriptor array (24 -> 48 =
// slot*6+channel) and the BC-compressed tileset slices that pay for it.
enum { MATERIAL_MAX_DETAIL_SLOTS = 8 };

// Number of defined materials: static builtins plus any dynamic entries added
// by MaterialRegistryDefineDynamic() for the currently loaded world. Every GPU
// pack path below iterates this count, so dynamic entries ride the same tables.
int MaterialRegistryCount(void);

// Number of frozen builtin materials (30). Never changes at runtime. Part
// artifacts serialize exactly this many MaterialDefs, so per-world dynamic
// entries never invalidate a baked `.part` (see part_asset_v2.cpp).
int MaterialRegistryStaticCount(void);

// Number of dynamic entries currently defined (0 for worlds that never call
// defineMaterial).
int MaterialRegistryDynamicCount(void);

// ---------------------------------------------------------------------------
// Dynamic (per-world) registry extension — chart-VT spec Phase 3 / contract C3
// ---------------------------------------------------------------------------
// Appends `*def` to the registry under `name` and returns its index
// (>= MaterialRegistryStaticCount()). The returned handle is a plain material
// id, usable anywhere a builtin id is.
//
// Idempotent by name: re-defining an existing name with a byte-identical
// MaterialDef returns the same index (so a shared-lib module that defines its
// material at import time is safe to import twice). Re-defining an existing
// name with a DIFFERENT definition is an error.
//
// Negative returns (see MATERIAL_DEFINE_ERR_*):
//   -1 invalid argument (null def/name, empty or over-long name)
//   -2 capacity exhausted (MATERIAL_MAX_TOTAL reached)
//   -3 name already defined with a different MaterialDef
enum {
    MATERIAL_DEFINE_ERR_INVALID  = -1,
    MATERIAL_DEFINE_ERR_FULL     = -2,
    MATERIAL_DEFINE_ERR_CONFLICT = -3
};
int MaterialRegistryDefineDynamic(const MaterialDef* def, const char* name);

// Drops every dynamic entry and clears the runtime slot/macro overrides that
// belonged to them. Called on world (re)connect so a second world never
// inherits the first world's material extension. Builtin entries and their
// overrides are untouched.
void MaterialRegistryResetDynamic(void);

// Index of the dynamic entry named `name`, or -1 when no dynamic entry carries
// that name. Builtin materials are unnamed and never match.
int MaterialRegistryFindByName(const char* name);

// Name of a dynamic entry, or NULL for builtin/out-of-range ids.
const char* MaterialRegistryNameOf(int materialId);

// Fills `out` with the authoring defaults used by the JS `defineMaterial`
// binding for fields the author did not specify: neutral dielectric, opaque,
// smooth-shaded, no detail slot. mergeGroup is left at -1; the caller assigns
// a stable per-name group so distinct dynamic materials never merge.
void MaterialRegistryDefaultDynamicDef(MaterialDef* out);

// Version of MaterialDef's serialized authoring schema.
uint32_t MaterialRegistrySchemaVersion(void);

// Returns the definition for materialId. Out-of-range ids return a default
// gray opaque material (never NULL).
const MaterialDef* MaterialRegistryGet(int materialId);

// Merge group for a material id (the SDF grouping key).
int MaterialMergeGroup(int materialId);

// Meshing algorithm for a material id (MeshAlgorithm enum value; 0 = marching cubes).
int MaterialMeshingAlgorithm(int materialId);

// Non-zero when the material is translucent (translucency > 0). Drives the
// cross-group carve decision in Phase 3.
int MaterialIsTransparent(int materialId);

// Fills out[MaterialRegistryCount() * MATERIAL_FLOATS_PER_DEF] with the table
// packed for GPU upload (see MATERIAL_FLOATS_PER_DEF). Used by the renderer.
// Dynamic entries are packed after the builtins with identical semantics.
#define MATERIAL_FLOATS_PER_DEF 12
void MaterialRegistryPackForGPU(float* out);

// Pure packing helper for MaterialGpuRecord.flags_misc[1] (schema v4, spec
// "Material schema" / Vulkan tileset). detailSlot/macroSlot are the already-
// resolved slots (-1 = none, override-or-static already applied by the
// caller); this function has no global state so tests can assert the
// encoding directly: MaterialPackDetailMacroSlots(0, 2) == (1u | (3u << 8)),
// MaterialPackDetailMacroSlots(-1, -1) == 0u. Mirrors exactly what
// MaterialRegistryPackRtForGPU packs; decoded by shaders_vk/tileset_common.glsl's
// tileset_detail_slot()/tileset_macro_slot() (see render/vk_gi_contract.h).
static inline uint32_t MaterialPackDetailMacroSlots(int detailSlot, int macroSlot) {
    return ((uint32_t)(detailSlot + 1) & 0xFFu) |
           (((uint32_t)(macroSlot + 1) & 0xFFu) << 8);
}

// Packs the registry into the Vulkan ray-tracing material layout.
void MaterialRegistryPackRtForGPU(MaterialGpuRecord* out);

// Runtime override: bind material `materialId` to viewer tileset slot `slot`.
// Pass slot < 0 to clear. Values persist for the life of the process and are
// read by MaterialRegistryPackForGPU() (slot [11]). Used by the viewer to bind
// material 16 (DIRT) to the ForestFloor atlas after LocalProvider::connect().
// Silently no-op on materialId out of range OR slot outside
// [-1, MATERIAL_MAX_DETAIL_SLOTS-1].
void MaterialRegistrySetGroundTilesetSlot(int materialId, int slot);

// Runtime override: bind material `materialId` to viewer macro tileset slot
// `slot` (Phase 3 frequency-split ground layer). Pass slot < 0 to clear.
// Mirrors MaterialRegistrySetGroundTilesetSlot's semantics and validation but
// is independent: a material can carry a detail slot, a macro slot, both, or
// neither. Values persist for the life of the process. Consumed only by
// MaterialRegistryPackRtForGPU (Vulkan MaterialGpuRecord.flags_misc[1]); the
// GL path's MaterialRegistryPackForGPU never reads it.
// Silently no-op on materialId out of range OR slot outside
// [-1, MATERIAL_MAX_DETAIL_SLOTS-1].
void MaterialRegistrySetGroundMacroSlot(int materialId, int slot);

#ifdef __cplusplus
}
#endif

#endif // MATERIAL_REGISTRY_H
