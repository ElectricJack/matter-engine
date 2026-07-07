#include "material_registry.h"
#include <stddef.h>

// Merge groups: each distinct material type is a group. Shades of one type
// share a group. Values are arbitrary but must be stable and unique per type.
enum {
    GROUP_RED = 0, GROUP_BLUE = 1, GROUP_GROUND = 2, GROUP_METAL = 3,
    GROUP_GLASS = 4, GROUP_LIGHT = 5, GROUP_GREENGLASS = 6, GROUP_WATER = 7,
    GROUP_STONE = 8, GROUP_SAND = 9,
    GROUP_BARK = 10, GROUP_LEAF = 11, GROUP_DIRT = 12
};

static const MaterialDef g_materials[] = {
    /* 0 */ {{0.8f,0.2f,0.2f}, 0.2f,  0.6f, 0.1f, 0.0f, 1.0f,  1, GROUP_RED, 0, -1},
    /* 1 */ {{0.2f,0.3f,0.8f}, 0.7f,  0.1f, 0.0f, 0.0f, 1.0f,  0, GROUP_BLUE, 0, -1},
    /* 2 */ {{0.3f,0.7f,0.3f}, 0.9f,  0.0f, 0.0f, 0.0f, 1.0f,  1, GROUP_GROUND, 0, -1},
    /* 3 */ {{0.8f,0.7f,0.3f}, 0.05f, 1.0f, 0.0f, 0.0f, 1.0f,  0, GROUP_METAL, 0, -1},
    /* 4 */ {{0.9f,0.9f,0.9f}, 0.01f, 0.15f,0.0f, 0.5f, 1.5f,  0, GROUP_GLASS, 1, -1},
    /* 5 */ {{1.0f,0.9f,0.7f}, 1.0f,  0.0f, 5.0f, 0.0f, 1.0f,  1, GROUP_LIGHT, 0, -1},
    /* 6 */ {{0.2f,0.9f,0.3f}, 0.005f,0.15f,0.0f, 0.5f, 1.52f, 0, GROUP_GREENGLASS, 0, -1},
    /* 7 */ {{0.2f,0.4f,0.8f}, 0.0f,  0.1f, 0.0f, 1.0f, 1.33f, 0, GROUP_WATER, 0, -1},
    /* 8 */ {{0.55f,0.52f,0.5f},0.85f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0, -1},
    /* 9 */ {{0.32f,0.30f,0.29f},0.9f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0, -1},
    /* 10 */ {{0.50f,0.48f,0.46f},0.55f,0.30f,0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0, -1},
    /* 11 */ {{0.55f,0.53f,0.50f},0.35f,0.65f,0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0, -1},
    /* 12 */ {{0.62f,0.59f,0.54f},0.22f,0.90f,0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0, -1},
    /* 13 */ {{0.76f,0.70f,0.50f},0.95f,0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_SAND, 1, -1},
    /* 14 BARK */ {{0.36f,0.25f,0.16f}, 0.90f, 0.0f, 0.0f, 0.0f, 1.0f, 0, GROUP_BARK, 0, -1},
    /* 15 LEAF */ {{0.22f,0.45f,0.18f}, 0.80f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_LEAF, 0, -1},
    /* 16 DIRT */ {{0.40f,0.28f,0.18f}, 1.00f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_DIRT, 0, -1},
};

static const MaterialDef g_default =
    {{0.6f,0.6f,0.6f}, 0.1f, 0.8f, 0.0f, 0.0f, 1.0f, 1, -1, 0, -1};

static const int g_count = (int)(sizeof(g_materials) / sizeof(g_materials[0]));

// Runtime tileset-slot overrides (parallel to g_materials). Kept as file-scope
// static so MaterialRegistryPackForGPU() can read it without changing the const
// table. -1 = no override; the value in g_materials[i].groundTilesetSlot wins.
#define ME_MAX_SLOT_OVERRIDES 64
static int g_slot_overrides[ME_MAX_SLOT_OVERRIDES] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
};

int MaterialRegistryCount(void) { return g_count; }

const MaterialDef* MaterialRegistryGet(int materialId) {
    if (materialId < 0 || materialId >= g_count) return &g_default;
    return &g_materials[materialId];
}

int MaterialMergeGroup(int materialId) {
    return MaterialRegistryGet(materialId)->mergeGroup;
}

int MaterialMeshingAlgorithm(int materialId) {
    return MaterialRegistryGet(materialId)->meshingAlgorithm;
}

int MaterialIsTransparent(int materialId) {
    return MaterialRegistryGet(materialId)->translucency > 0.0f ? 1 : 0;
}

void MaterialRegistrySetGroundTilesetSlot(int materialId, int slot) {
    if (materialId < 0 || materialId >= ME_MAX_SLOT_OVERRIDES) return;
    g_slot_overrides[materialId] = slot;
}

void MaterialRegistryPackForGPU(float* out) {
    // Pack as three vec4s (std140-friendly):
    //   [albedo.xyz, roughness]
    //   [metallic, emission, pad, translucency]
    //   [ior, flatShading, mergeGroup, groundTilesetSlot]
    // Slot [11] (previously pad) now carries groundTilesetSlot. If the runtime
    // set an override via MaterialRegistrySetGroundTilesetSlot(), it wins over
    // the static table value (-1 by default in every registry entry).
    for (int i = 0; i < g_count; ++i) {
        const MaterialDef* m = &g_materials[i];
        float* r = out + (size_t)i * MATERIAL_FLOATS_PER_DEF;
        r[0]=m->albedo[0]; r[1]=m->albedo[1]; r[2]=m->albedo[2];
        r[3]=m->roughness; r[4]=m->metallic; r[5]=m->emission;
        r[6]=0.0f; /* pad */ r[7]=m->translucency; r[8]=m->ior;
        r[9]=(float)m->flatShading; r[10]=(float)m->mergeGroup;
        int slot = (i < ME_MAX_SLOT_OVERRIDES && g_slot_overrides[i] >= 0)
                       ? g_slot_overrides[i]
                       : m->groundTilesetSlot;
        r[11]=(float)slot;
    }
}
