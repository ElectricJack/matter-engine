#include "material_registry.h"
#include "../../MatterEngine3/src/render/vk_gi_contract.h"
#include <cassert>
#include <cstdio>
#include <cmath>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

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
    CHECK(MaterialRegistrySchemaVersion() == 2u, "material schema version is 2");
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
