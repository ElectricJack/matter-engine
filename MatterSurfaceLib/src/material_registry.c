#include "material_registry.h"
#include <stddef.h>

// Merge groups: each distinct material type is a group. Shades of one type
// share a group. Values are arbitrary but must be stable and unique per type.
enum {
    GROUP_RED = 0, GROUP_BLUE = 1, GROUP_GROUND = 2, GROUP_METAL = 3,
    GROUP_GLASS = 4, GROUP_LIGHT = 5, GROUP_GREENGLASS = 6, GROUP_WATER = 7,
    GROUP_STONE = 8
};

static const MaterialDef g_materials[] = {
    /* 0 */ {{0.8f,0.2f,0.2f}, 0.2f,  0.6f, 0.1f, 0.0f, 1.0f,  1, GROUP_RED},
    /* 1 */ {{0.2f,0.3f,0.8f}, 0.7f,  0.1f, 0.0f, 0.0f, 1.0f,  0, GROUP_BLUE},
    /* 2 */ {{0.3f,0.7f,0.3f}, 0.9f,  0.0f, 0.0f, 0.0f, 1.0f,  1, GROUP_GROUND},
    /* 3 */ {{0.8f,0.7f,0.3f}, 0.05f, 1.0f, 0.0f, 0.0f, 1.0f,  0, GROUP_METAL},
    /* 4 */ {{0.9f,0.9f,0.9f}, 0.01f, 0.15f,0.0f, 0.5f, 1.5f,  0, GROUP_GLASS},
    /* 5 */ {{1.0f,0.9f,0.7f}, 1.0f,  0.0f, 5.0f, 0.0f, 1.0f,  1, GROUP_LIGHT},
    /* 6 */ {{0.2f,0.9f,0.3f}, 0.005f,0.15f,0.0f, 0.5f, 1.52f, 0, GROUP_GREENGLASS},
    /* 7 */ {{0.2f,0.4f,0.8f}, 0.0f,  0.1f, 0.0f, 1.0f, 1.33f, 0, GROUP_WATER},
    /* 8 */ {{0.55f,0.52f,0.5f},0.85f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_STONE}, // stone_light
    /* 9 */ {{0.32f,0.30f,0.29f},0.9f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_STONE}, // stone_dark
};

static const MaterialDef g_default =
    {{0.6f,0.6f,0.6f}, 0.1f, 0.8f, 0.0f, 0.0f, 1.0f, 1, -1};

static const int g_count = (int)(sizeof(g_materials) / sizeof(g_materials[0]));

int MaterialRegistryCount(void) { return g_count; }

const MaterialDef* MaterialRegistryGet(int materialId) {
    if (materialId < 0 || materialId >= g_count) return &g_default;
    return &g_materials[materialId];
}

int MaterialMergeGroup(int materialId) {
    return MaterialRegistryGet(materialId)->mergeGroup;
}

int MaterialIsTransparent(int materialId) {
    return MaterialRegistryGet(materialId)->translucency > 0.0f ? 1 : 0;
}

void MaterialRegistryPackForGPU(float* out) {
    for (int i = 0; i < g_count; ++i) {
        const MaterialDef* m = &g_materials[i];
        float* r = out + (size_t)i * MATERIAL_FLOATS_PER_DEF;
        r[0]=m->albedo[0]; r[1]=m->albedo[1]; r[2]=m->albedo[2];
        r[3]=m->roughness; r[4]=m->metallic; r[5]=m->emission;
        r[6]=0.0f; /* pad */ r[7]=m->translucency; r[8]=m->ior;
        r[9]=(float)m->flatShading; r[10]=(float)m->mergeGroup; r[11]=0.0f; /* pad */
    }
}
