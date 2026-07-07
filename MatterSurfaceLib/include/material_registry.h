#ifndef MATERIAL_REGISTRY_H
#define MATERIAL_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

// A single material definition. This is the ONE place materials are defined;
// both the CPU (meshing decisions) and the GPU (shading) consume this table.
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
    int   groundTilesetSlot; // Phase 4: -1 = untextured, 0..3 = viewer tileset slot to sample.
                             // For static registry entries this stays -1; the viewer runtime
                             // sets a live override via MaterialRegistrySetGroundTilesetSlot()
                             // after loading a world tileset atlas.
} MaterialDef;

// Number of defined materials.
int MaterialRegistryCount(void);

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
#define MATERIAL_FLOATS_PER_DEF 12
void MaterialRegistryPackForGPU(float* out);

// Runtime override: bind material `materialId` to viewer tileset slot `slot`.
// Pass slot < 0 to clear. Values persist for the life of the process and are
// read by MaterialRegistryPackForGPU() (slot [11]). Used by the viewer to bind
// material 16 (DIRT) to the ForestFloor atlas after LocalProvider::connect().
// materialId out-of-range is a no-op (fail-closed defensive default).
void MaterialRegistrySetGroundTilesetSlot(int materialId, int slot);

#ifdef __cplusplus
}
#endif

#endif // MATERIAL_REGISTRY_H
