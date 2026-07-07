
// Material Properties System
// Comprehensive material definition with PBR, emission, translucency, and surface properties

struct MaterialProperties
{
    vec3 albedo;
    float roughness;
    float metallic;
    float emission;
    float translucency;
    float ior;
    bool flatShading;
    int  groundTilesetSlot;  // Phase 4: -1 = untextured, 0..3 = viewer tileset slot.
                              // Fragment shaders branch on this to sample the Wang
                              // atlas instead of using the flat albedo/roughness/metallic.
};

// Packed material table, uploaded from the CPU registry. 12 floats per material
// (see MATERIAL_FLOATS_PER_DEF / MaterialRegistryPackForGPU):
//   [0..2] albedo, [3] roughness, [4] metallic, [5] emission, [6] pad,
//   [7] translucency, [8] ior, [9] flatShading, [10] mergeGroup, [11] groundTilesetSlot
// Was 64 — reduced to 32 to keep the `PARAM c[N]` block for compute shaders
// under NVIDIA's NVcp5.0 local-parameter cap on Windows. The CPU registry
// currently uses ~17 materials; 32 keeps 2x headroom. Increase carefully:
// the total uniform float count is MAX_MATERIALS * MATERIAL_FLOATS_PER_DEF
// (currently 384), and NVcp5.0 rejects programs whose local param count
// exceeds its (undocumented) profile cap around ~512-768 params.
#define MAX_MATERIALS 32
#define MATERIAL_FLOATS_PER_DEF 12
uniform float materialTable[MAX_MATERIALS * MATERIAL_FLOATS_PER_DEF];
uniform int materialCount;

// Material lookup table - data-driven via uniform array uploaded from CPU registry
MaterialProperties getMaterialProperties(int materialId)
{
    // Smooth-shading flag is now a table field; keep the legacy >=1M offset
    // working so existing callers that set it still smooth-shade.
    bool forceSmooth = false;
    int smooth_normals_offset = 1000000;
    if (materialId >= smooth_normals_offset) { materialId -= smooth_normals_offset; forceSmooth = true; }

    MaterialProperties mat;
    int id = materialId;
    if (id < 0 || id >= materialCount) {
        mat.albedo = vec3(0.6); mat.roughness = 0.1; mat.metallic = 0.8;
        mat.emission = 0.0; mat.translucency = 0.0; mat.ior = 1.0; mat.flatShading = true;
        mat.groundTilesetSlot = -1;
        return mat;
    }
    int b = id * MATERIAL_FLOATS_PER_DEF;
    mat.albedo = vec3(materialTable[b+0], materialTable[b+1], materialTable[b+2]);
    mat.roughness = materialTable[b+3];
    mat.metallic  = materialTable[b+4];
    mat.emission  = materialTable[b+5];
    mat.translucency = materialTable[b+7];
    mat.ior = materialTable[b+8];
    mat.flatShading = forceSmooth ? false : (materialTable[b+9] > 0.5);
    mat.groundTilesetSlot = int(materialTable[b+11]);
    return mat;
}

// Utility function to check if a material is emissive
bool isMaterialEmissive(int materialId)
{
    MaterialProperties mat = getMaterialProperties(materialId);
    return mat.emission > 0.0;
}

// Utility function to check if a material is translucent
bool isMaterialTranslucent(int materialId)
{
    MaterialProperties mat = getMaterialProperties(materialId);
    return mat.translucency > 0.0;
}

// Utility function to get emission color
vec3 getMaterialEmission(int materialId)
{
    MaterialProperties mat = getMaterialProperties(materialId);
    return mat.albedo * mat.emission;
}