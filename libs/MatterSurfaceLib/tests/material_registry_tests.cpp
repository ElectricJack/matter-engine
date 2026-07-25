#include "material_registry.h"
#include "../../MatterEngine3/src/render/vk_gi_contract.h"
#include "../../MatterEngine3/src/part_base.js.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <initializer_list>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static bool nearf(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

static uint64_t fnv1a64(const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

int main() {
    // Glass (id 4 in the ported table) is translucent; steel-like metal (id 3) is not.
    CHECK(MaterialIsTransparent(4) != 0, "material 4 (glass) should be transparent");
    CHECK(MaterialIsTransparent(3) == 0, "material 3 (gold/metal) should be opaque");

    // Out-of-range id returns a usable default, never crashes.
    const MaterialDef* def = MaterialRegistryGet(99999);
    CHECK(def != nullptr, "out-of-range id must return non-NULL default");

    // Two stone shades (ids 8 and 9) share a merge group.
    CHECK(MaterialMergeGroup(8) == MaterialMergeGroup(9),
          "stone_light(8) and stone_dark(9) must share a merge group");
    // Glass and metal do not.
    CHECK(MaterialMergeGroup(4) != MaterialMergeGroup(3),
          "glass(4) and metal(3) must be different merge groups");

    // GPU packing produces the right count of floats and round-trips translucency.
    int n = MaterialRegistryCount();
    CHECK(n >= 10, "expected at least 10 materials");
    float buf[64 * MATERIAL_FLOATS_PER_DEF];
    assert(n <= 64);
    MaterialRegistryPackForGPU(buf);
    // translucency is the 8th float (index 7) in each packed record (see MaterialRegistryPackForGPU).
    CHECK(fabsf(buf[4 * MATERIAL_FLOATS_PER_DEF + 7] - MaterialRegistryGet(4)->translucency) < 1e-6f,
          "packed translucency for material 4 must match the table");

    MaterialGpuRecord records[64]{};
    MaterialRegistryPackRtForGPU(records);
    CHECK(sizeof(MaterialGpuRecord) == 144, "RTX material record is 9x vec4");
    CHECK(offsetof(MaterialGpuRecord, base_roughness) == 0,
          "RTX base/roughness vec4 begins at byte 0");
    CHECK(offsetof(MaterialGpuRecord, metal_opacity_spec_coat) == 16,
          "RTX metal/opacity/specular/coat vec4 begins at byte 16");
    CHECK(offsetof(MaterialGpuRecord, specular_tint_coat_roughness) == 32,
          "RTX specular tint/coat roughness vec4 begins at byte 32");
    CHECK(offsetof(MaterialGpuRecord, emission_strength) == 48,
          "RTX emission/strength vec4 begins at byte 48");
    CHECK(offsetof(MaterialGpuRecord, transmission) == 64,
          "RTX transmission vec4 begins at byte 64");
    CHECK(offsetof(MaterialGpuRecord, absorption_pad) == 80,
          "RTX absorption vec4 begins at byte 80");
    CHECK(offsetof(MaterialGpuRecord, scattering) == 96,
          "RTX scattering vec4 begins at byte 96");
    CHECK(offsetof(MaterialGpuRecord, scattering_shape) == 112,
          "RTX scattering shape vec4 begins at byte 112");
    CHECK(offsetof(MaterialGpuRecord, flags_misc) == 128,
          "RTX flags uvec4 begins at byte 128");
    CHECK(MaterialRegistryCount() == 30, "garden registry has stable count 30");
    CHECK(MaterialRegistrySchemaVersion() == 3u, "material schema version is 3");
    CHECK(fnv1a64(buf, 18u * MATERIAL_FLOATS_PER_DEF * sizeof(float)) ==
              0x69c22a3502ba9490ull,
          "legacy IDs 0-17 keep byte-identical 12-float packing");

    struct ExpectedBase { int id; float r,g,b,rough,metal; };
    const ExpectedBase expected[] = {
        {18,0.82f,0.78f,0.72f,0.92f,0.0f},
        {19,0.035f,0.04f,0.05f,0.75f,0.0f},
        {20,0.62f,0.65f,0.70f,0.03f,1.0f},
        {21,0.83f,0.57f,0.17f,0.36f,1.0f},
        {22,0.90f,0.32f,0.12f,0.18f,1.0f},
        {23,0.82f,0.86f,0.90f,0.22f,0.0f},
        {24,0.45f,0.015f,0.02f,0.38f,0.0f},
        {25,0.35f,0.65f,1.00f,1.00f,0.0f},
        {26,1.00f,0.50f,0.15f,1.00f,0.0f},
        {27,0.16f,0.18f,0.22f,0.04f,0.0f},
        {28,0.85f,0.42f,0.24f,0.62f,0.0f},
        {29,0.12f,0.32f,0.08f,0.75f,0.0f},
    };
    for (const auto& e : expected) {
        const MaterialDef* m = MaterialRegistryGet(e.id);
        CHECK(nearf(m->albedo[0], e.r) && nearf(m->albedo[1], e.g) &&
                  nearf(m->albedo[2], e.b), "garden material base color");
        CHECK(nearf(m->roughness, e.rough), "garden material roughness");
        CHECK(nearf(m->metallic, e.metal), "garden material metallic");
        CHECK(MaterialMergeGroup(e.id) == e.id - 4,
              "garden material stable merge group 14-25");
    }

    CHECK(nearf(records[23].metal_opacity_spec_coat[3], 1.0f) &&
              nearf(records[23].specular_tint_coat_roughness[3], 0.04f),
          "ceramic packs clearcoat lobe");
    CHECK(nearf(records[24].metal_opacity_spec_coat[3], 0.85f) &&
              nearf(records[24].specular_tint_coat_roughness[3], 0.08f),
          "lacquer packs rough clearcoat lobe");
    CHECK(nearf(records[25].emission_strength[0], 0.25f) &&
              nearf(records[25].emission_strength[1], 0.55f) &&
              nearf(records[25].emission_strength[2], 1.0f) &&
              nearf(records[25].emission_strength[3], 6.0f),
          "cool light packs colored emission");
    CHECK(nearf(records[26].emission_strength[3], 1.5f),
          "low warm light packs authored strength");
    CHECK(nearf(records[27].transmission[0], 0.85f) &&
              nearf(records[27].transmission[1], 1.48f) &&
              nearf(records[27].transmission[2], 1.0f) &&
              nearf(records[27].transmission[3], 1.5f) &&
              (records[27].flags_misc[0] & MATERIAL_VOLUME_BOUNDARY) != 0,
          "smoke glass packs future volume transport");
    CHECK(MaterialIsTransparent(27) != 0,
          "smoke glass participates in current nonopaque classification");
    CHECK(nearf(records[28].scattering[3], 0.80f) &&
              nearf(records[28].scattering_shape[0], 0.35f),
          "wax packs future scattering");
    CHECK(nearf(records[29].scattering[3], 0.85f) &&
              (records[29].flags_misc[0] &
               (MATERIAL_THIN_WALLED | MATERIAL_DOUBLE_SIDED)) ==
                  (MATERIAL_THIN_WALLED | MATERIAL_DOUBLE_SIDED),
          "thin foliage packs future thin scattering");
    CHECK(MaterialIsTransparent(28) == 0 && MaterialIsTransparent(29) == 0,
          "scattering materials do not become carve volumes");

    for (const char* mapping : {
             "greenGlass: 6", "plaster: 18", "charcoal: 19", "chrome: 20",
             "goldRough: 21", "copper: 22", "ceramic: 23", "lacquerRed: 24",
             "lightCool: 25", "lightWarmLow: 26", "glassSmoke: 27",
             "wax: 28", "foliageThin: 29"}) {
        CHECK(std::strstr(kPartBaseJS, mapping) != nullptr,
              "Part DSL exposes stable garden material name");
    }
    CHECK(records[1].specular_tint_coat_roughness[0] > 0.99f &&
          records[1].specular_tint_coat_roughness[1] > 0.99f &&
          records[1].specular_tint_coat_roughness[2] > 0.99f,
          "ordinary dielectric uses neutral white specular tint");
    CHECK(fabsf(records[0].emission_strength[0] - records[0].base_roughness[0]) < 1e-6f &&
          fabsf(records[0].emission_strength[1] - records[0].base_roughness[1]) < 1e-6f &&
          fabsf(records[0].emission_strength[2] - records[0].base_roughness[2]) < 1e-6f &&
          fabsf(records[0].emission_strength[3] - 0.1f) < 1e-6f,
          "legacy emissive material preserves albedo-colored RTX radiance inputs");
    CHECK(records[4].transmission[0] > 0.0f, "glass opts into transmission");
    CHECK((records[4].flags_misc[0] & MATERIAL_VOLUME_BOUNDARY) != 0,
          "glass is a closed volume");
    CHECK(records[7].transmission[1] > 1.32f &&
          records[7].transmission[1] < 1.34f, "water IOR is preserved");
    CHECK(records[15].scattering[3] > 0.0f &&
          (records[15].flags_misc[0] & MATERIAL_THIN_WALLED) != 0,
          "leaf opts into thin scattering");
    CHECK(MaterialIsTransparent(15) == 0,
          "subsurface leaf does not become a meshing carve volume");

    // Meshing algorithm defaults to 0 (marching cubes) for existing materials.
    CHECK(MaterialMeshingAlgorithm(0) == 0, "material 0 should default to marching cubes (0)");
    CHECK(MaterialMeshingAlgorithm(3) == 0, "material 3 should default to marching cubes (0)");
    // Out-of-range id returns the default material's algorithm (0), never crashes.
    CHECK(MaterialMeshingAlgorithm(99999) == 0, "out-of-range id must return default algorithm 0");
    // Sand (new material id 13) selects the oriented-cube algorithm (1).
    CHECK(MaterialMeshingAlgorithm(13) == 1, "sand(13) should select oriented cubes (1)");
    CHECK(MaterialRegistryCount() >= 14, "expected at least 14 materials after adding sand");

    if (failures == 0) printf("All material_registry tests passed\n");
    return failures == 0 ? 0 : 1;
}
