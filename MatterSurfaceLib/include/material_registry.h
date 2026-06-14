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
} MaterialDef;

// Number of defined materials.
int MaterialRegistryCount(void);

// Returns the definition for materialId. Out-of-range ids return a default
// gray opaque material (never NULL).
const MaterialDef* MaterialRegistryGet(int materialId);

// Merge group for a material id (the SDF grouping key).
int MaterialMergeGroup(int materialId);

// Non-zero when the material is translucent (translucency > 0). Drives the
// cross-group carve decision in Phase 3.
int MaterialIsTransparent(int materialId);

// Fills out[MaterialRegistryCount() * MATERIAL_FLOATS_PER_DEF] with the table
// packed for GPU upload (see MATERIAL_FLOATS_PER_DEF). Used by the renderer.
#define MATERIAL_FLOATS_PER_DEF 12
void MaterialRegistryPackForGPU(float* out);

#ifdef __cplusplus
}
#endif

#endif // MATERIAL_REGISTRY_H
