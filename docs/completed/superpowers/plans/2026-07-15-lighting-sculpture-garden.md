# Lighting Sculpture Garden Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a deterministic, walkable 5 by 5 dusk sculpture garden that demonstrates current Vulkan lighting and serves as a visual red/green fixture for planned transmission and scattering.

**Architecture:** Extend the one authoritative material registry with twelve reusable named presets, then author one data-driven `LightingGarden` Part through the existing JS DSL. Verify it through a real cold ScriptHost/PartGraph bake and baked-triangle material checks before producing and launching the CUDA/Streamline Windows Viewer.

**Tech Stack:** C/C++17, QuickJS Part DSL, MatterSurfaceLib material/asset schema, Vulkan 1.3 hybrid RTX renderer, NVIDIA Streamline 2.12, CUDA 13.3, UCRT64/MinGW.

## Global Constraints

- The committed world is one open, walkable 5 by 5 matrix with exactly 25 unique cells at 5.0-unit spacing.
- Cubes, UV spheres, and marching-cubes isosurfaces are interleaved; material families are not isolated into separate courts.
- The garden uses only the production material registry, asset bake, G-buffer, RT, composite, DLSS, and display-transform paths; add no demo-only shader branch or runtime material override.
- Existing material IDs 0 through 17 retain their exact meaning and legacy 12-float OpenGL packing; the legacy-prefix FNV-1a byte hash remains `0x69c22a3502ba9490`.
- Add reusable material IDs 18 through 29 with the exact names and values from the approved design; assign merge groups 14 through 25 in ID order.
- `glassSmoke` uses legacy `translucency=0.85` as well as `transmission=0.85` so current nonopaque classification matches the established glass migration; every other new preset uses legacy translucency zero.
- Bump `MATERIAL_SCHEMA_VERSION` from 2 to 3 and require safe atomic rebake of incompatible cached artifacts.
- Authored glass, water, wax, leaf, and thin foliage keep their future transport fields but receive no fake Task 9/10 shader behavior.
- The manifest uses exactly `light sun -0.55 -0.35 -0.75 0.45 0.24 0.12` and `light sky 0.055 0.075 0.16`.
- Verification is deliberately bounded: focused contract tests, one fresh-cache garden bake, one enabled Windows build, and one validation-clean Viewer launch; no exhaustive screenshot or performance gate.
- Enabled Windows build uses `HAVE_CUDA=1`, `HAVE_STREAMLINE=1`, `STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.12.0`, and `STREAMLINE_DLL_DIR=/d/SDKs/streamline-sdk-v2.12.0/bin/x64/development`.

---

## File Structure

- `MatterSurfaceLib/include/material_registry.h` owns the serialized material schema version and public record contract.
- `MatterSurfaceLib/src/material_registry.c` remains the single authoritative table and packs both legacy and RTX records.
- `MatterEngine3/src/part_base.js.h` exposes stable authoring names; it contains no material values.
- `MatterSurfaceLib/tests/material_registry_tests.cpp` locks IDs, exact values, flags, RTX packing, and the unchanged legacy prefix.
- `MatterEngine3/examples/world_demo/schemas/LightingGarden.js` owns the 25-cell table and all garden/environment geometry.
- `MatterEngine3/examples/world_demo/WorldData/LightingGarden/world.manifest` owns the root and dusk lights.
- `MatterEngine3/tests/lighting_garden_tests.cpp` performs a real cold bake, loads the artifact, and finds the expected sculpture material above every cell.
- `MatterEngine3/tests/Makefile` adds only the focused `lighting_garden_tests`/`run-lighting-garden` target.

---

### Task 1: Add the Reusable Showcase Material Contract

**Files:**
- Modify: `MatterSurfaceLib/include/material_registry.h`
- Modify: `MatterSurfaceLib/src/material_registry.c`
- Modify: `MatterEngine3/src/part_base.js.h`
- Modify: `MatterSurfaceLib/tests/material_registry_tests.cpp`

**Interfaces:**
- Consumes: current IDs 0-17, `MaterialDef`, `MaterialGpuRecord`, `MaterialRegistryPackForGPU`, and `MaterialRegistryPackRtForGPU`.
- Produces: schema version 3, stable IDs 18-29, `MAT.plaster` through `MAT.foliageThin`, and `MAT.greenGlass` for the existing ID 6.

- [ ] **Step 1: Write failing registry identity, packing, and compatibility tests**

Add these helpers and assertions to `material_registry_tests.cpp` before changing production code:

```cpp
#include "../../MatterEngine3/src/part_base.js.h"
#include <cstdint>
#include <cstring>

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
```

After both GPU packs, assert the complete contract:

```cpp
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
```

- [ ] **Step 2: Run the focused test and verify RED**

Run from an MSYS2 UCRT64 shell:

```bash
make -C MatterSurfaceLib/tests run-reg
```

Expected: the binary builds, then fails on count 30, schema 3, new material
values/packing, and missing `MAT.*` names. The existing legacy-prefix hash
assertion passes before production edits.

- [ ] **Step 3: Bump the schema and add advanced preset initialization**

Change `MATERIAL_SCHEMA_VERSION` to `3` in `material_registry.h`.

In `material_registry.c`, retain the existing `MATERIAL_DEF` macro and every
existing ID 0-17 line byte-for-byte. Add merge groups:

```c
GROUP_PLASTER = 14, GROUP_CHARCOAL = 15, GROUP_CHROME = 16,
GROUP_GOLD_ROUGH = 17, GROUP_COPPER = 18, GROUP_CERAMIC = 19,
GROUP_LACQUER_RED = 20, GROUP_LIGHT_COOL = 21,
GROUP_LIGHT_WARM_LOW = 22, GROUP_GLASS_SMOKE = 23,
GROUP_WAX = 24, GROUP_FOLIAGE_THIN = 25
```

Add a second initializer macro that exposes coat fields without changing the
legacy macro:

```c
#define MATERIAL_DEF_ADVANCED(R,G,B,ROUGH,METAL,EMIT,TRANSLUCENT,IOR,FLAT,GROUP,MESHER,SLOT, \
    TRANSMIT,ER,EG,EB,AR,AG,AB,ADIST,THICK,SUBSURFACE,SR,SG,SB,SDIST,ANISO,COAT,COATROUGH,FLAGS) \
    {{R,G,B}, ROUGH, METAL, EMIT, TRANSLUCENT, IOR, FLAT, GROUP, MESHER, SLOT, \
     1.0f, TRANSMIT, {ER,EG,EB}, {AR,AG,AB}, ADIST, THICK, SUBSURFACE, {SR,SG,SB}, \
     SDIST, ANISO, COAT, COATROUGH, 1.0f, {1.0f,1.0f,1.0f}, 0.0f, 1.0f, FLAGS}
```

Append these exact definitions after ID 17:

```c
/* 18 PLASTER */ MATERIAL_DEF_ADVANCED(0.82f,0.78f,0.72f, 0.92f,0,0,0,1, 1,GROUP_PLASTER,0,-1, 0, 0,0,0, 0,0,0,0,0, 0, 0,0,0,0,0, 0,0, MATERIAL_SURFACE_NONE),
/* 19 CHARCOAL */ MATERIAL_DEF_ADVANCED(0.035f,0.04f,0.05f, 0.75f,0,0,0,1, 1,GROUP_CHARCOAL,0,-1, 0, 0,0,0, 0,0,0,0,0, 0, 0,0,0,0,0, 0,0, MATERIAL_SURFACE_NONE),
/* 20 CHROME */ MATERIAL_DEF_ADVANCED(0.62f,0.65f,0.70f, 0.03f,1,0,0,1, 0,GROUP_CHROME,0,-1, 0, 0,0,0, 0,0,0,0,0, 0, 0,0,0,0,0, 0,0, MATERIAL_SURFACE_NONE),
/* 21 GOLD ROUGH */ MATERIAL_DEF_ADVANCED(0.83f,0.57f,0.17f, 0.36f,1,0,0,1, 0,GROUP_GOLD_ROUGH,0,-1, 0, 0,0,0, 0,0,0,0,0, 0, 0,0,0,0,0, 0,0, MATERIAL_SURFACE_NONE),
/* 22 COPPER */ MATERIAL_DEF_ADVANCED(0.90f,0.32f,0.12f, 0.18f,1,0,0,1, 0,GROUP_COPPER,0,-1, 0, 0,0,0, 0,0,0,0,0, 0, 0,0,0,0,0, 0,0, MATERIAL_SURFACE_NONE),
/* 23 CERAMIC */ MATERIAL_DEF_ADVANCED(0.82f,0.86f,0.90f, 0.22f,0,0,0,1, 0,GROUP_CERAMIC,0,-1, 0, 0,0,0, 0,0,0,0,0, 0, 0,0,0,0,0, 1.0f,0.04f, MATERIAL_SURFACE_NONE),
/* 24 LACQUER RED */ MATERIAL_DEF_ADVANCED(0.45f,0.015f,0.02f, 0.38f,0,0,0,1, 0,GROUP_LACQUER_RED,0,-1, 0, 0,0,0, 0,0,0,0,0, 0, 0,0,0,0,0, 0.85f,0.08f, MATERIAL_SURFACE_NONE),
/* 25 LIGHT COOL */ MATERIAL_DEF_ADVANCED(0.35f,0.65f,1.0f, 1.0f,0,6.0f,0,1, 1,GROUP_LIGHT_COOL,0,-1, 0, 0.25f,0.55f,1.0f, 0,0,0,0,0, 0, 0,0,0,0,0, 0,0, MATERIAL_SURFACE_NONE),
/* 26 LIGHT WARM LOW */ MATERIAL_DEF_ADVANCED(1.0f,0.50f,0.15f, 1.0f,0,1.5f,0,1, 1,GROUP_LIGHT_WARM_LOW,0,-1, 0, 1.0f,0.35f,0.08f, 0,0,0,0,0, 0, 0,0,0,0,0, 0,0, MATERIAL_SURFACE_NONE),
/* 27 GLASS SMOKE */ MATERIAL_DEF_ADVANCED(0.16f,0.18f,0.22f, 0.04f,0,0,0.85f,1.48f, 0,GROUP_GLASS_SMOKE,0,-1, 0.85f, 0,0,0, 0.12f,0.16f,0.20f,1.5f,1.0f, 0, 0,0,0,0,0, 0,0, MATERIAL_VOLUME_BOUNDARY),
/* 28 WAX */ MATERIAL_DEF_ADVANCED(0.85f,0.42f,0.24f, 0.62f,0,0,0,1, 0,GROUP_WAX,0,-1, 0, 0,0,0, 0,0,0,0,0, 0.80f, 1.0f,0.35f,0.18f,0.35f,0.10f, 0,0, MATERIAL_SURFACE_NONE),
/* 29 FOLIAGE THIN */ MATERIAL_DEF_ADVANCED(0.12f,0.32f,0.08f, 0.75f,0,0,0,1, 0,GROUP_FOLIAGE_THIN,0,-1, 0, 0,0,0, 0,0,0,0,0, 0.85f, 0.25f,0.65f,0.12f,0.18f,0.35f, 0,0, MATERIAL_THIN_WALLED | MATERIAL_DOUBLE_SIDED),
```

- [ ] **Step 4: Expose stable DSL names**

Extend `globalThis.MAT` without renaming existing keys:

```js
greenGlass: 6,
plaster: 18, charcoal: 19, chrome: 20, goldRough: 21,
copper: 22, ceramic: 23, lacquerRed: 24,
lightCool: 25, lightWarmLow: 26, glassSmoke: 27,
wax: 28, foliageThin: 29,
```

- [ ] **Step 5: Verify GREEN and legacy cache rejection**

Run:

```bash
make -C MatterSurfaceLib/tests run-reg
make -C MatterEngine3/tests run-partv2
```

Expected: both print their pass banners. Registry output proves count 30,
schema 3, exact new RTX fields, and the unchanged legacy-prefix hash. Part
asset tests prove schema-2 artifacts reject before reading enlarged/current
material records and regenerate through the established compatibility path.

- [ ] **Step 6: Commit Task 1**

```bash
git add MatterSurfaceLib/include/material_registry.h \
  MatterSurfaceLib/src/material_registry.c \
  MatterEngine3/src/part_base.js.h \
  MatterSurfaceLib/tests/material_registry_tests.cpp
git commit -m "feat(materials): add sculpture garden presets"
```

---

### Task 2: Author and Cold-Bake the 25-Cell Lighting Garden

**Files:**
- Create: `MatterEngine3/examples/world_demo/schemas/LightingGarden.js`
- Create: `MatterEngine3/examples/world_demo/WorldData/LightingGarden/world.manifest`
- Create: `MatterEngine3/tests/lighting_garden_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: Task 1 stable `MAT.*` names/IDs, existing Part DSL mesh/voxel verbs, `PartGraph`, `HostBaker`, `part_asset::load_v2`, and `BLASEntry::tri_extra`.
- Produces: discoverable `LightingGarden`, deterministic `GARDEN_CELLS`, a real cold-bake regression, and a demo-ready Windows world.

- [ ] **Step 1: Write the failing real-bake contract test and Make target**

Create `lighting_garden_tests.cpp` using the portable `std::filesystem` pattern
below. Define the expected cells independently of the JS table:

```cpp
#include "part_graph.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "check.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace part_graph;

struct SandboxGuard {
    fs::path original;
    fs::path sandbox;
    ~SandboxGuard() {
        std::error_code ignored;
        fs::current_path(original, ignored);
        fs::remove_all(sandbox, ignored);
    }
};

struct ExpectedCell { int row, col, material; const char* kind; };
static constexpr std::array<ExpectedCell, 25> kCells{{
    {0,0,18,"cube"}, {0,1,3,"sphere"}, {0,2,5,"iso"},
    {0,3,6,"sphere"}, {0,4,19,"cube"},
    {1,0,22,"iso"}, {1,1,25,"cube"}, {1,2,17,"sphere"},
    {1,3,7,"water"}, {1,4,24,"iso"},
    {2,0,14,"cube"}, {2,1,23,"sphere"}, {2,2,5,"cube"},
    {2,3,20,"sphere"}, {2,4,15,"iso"},
    {3,0,27,"sphere"}, {3,1,21,"iso"}, {3,2,8,"cube"},
    {3,3,25,"sphere"}, {3,4,28,"iso"},
    {4,0,9,"iso"}, {4,1,8,"sphere"}, {4,2,26,"cube"},
    {4,3,29,"iso"}, {4,4,8,"sphere"},
}};
```

The test must:

```cpp
const fs::path original = fs::current_path();
const fs::path schemas = fs::absolute("../examples/world_demo/schemas");
const fs::path world_data = fs::absolute("../examples/world_demo/WorldData");
const fs::path shared_lib = fs::absolute("../shared-lib");
const fs::path sandbox = fs::temp_directory_path() / "me3_lighting_garden";
fs::remove_all(sandbox);
fs::create_directories(sandbox / "parts");
SandboxGuard cleanup{original, sandbox};
fs::current_path(sandbox);

script_host::ScriptHost host;
host.set_shared_lib_root(shared_lib.string());
FileModuleResolver resolver(host, schemas.string());
HostBaker baker(host, ".");
PartGraph graph(resolver, baker);

std::vector<ChildRequest> roots;
std::string error;
CHECK(PartGraph::read_manifest(world_data.string(), "LightingGarden", roots, error),
      "LightingGarden manifest parses");
CHECK(roots.size() == 1 && roots[0].module == "LightingGarden",
      "manifest has one LightingGarden root");
InstallResult install = graph.install(roots);
CHECK(install.ok, "cold LightingGarden install succeeds");
CHECK(install.baked.size() == 1 && install.root_hashes.size() == 1,
      "cold garden bake publishes one complete root artifact");

BLASManager blas;
TLASManager tlas(256);
std::vector<part_asset::ChildInstance> children;
part_asset::LodLevels lods;
CHECK(part_asset::load_v2(part_asset::cache_path_resolved(install.root_hashes[0]),
                          install.root_hashes[0], blas, tlas, children, lods),
      "garden root artifact round-trips");
CHECK(children.empty(), "garden matrix is one open root, not isolated child courts");

std::array<size_t, 25> hits{};
for (const auto& entry : blas.get_entries()) {
    for (size_t i = 0; i < entry->triangles.size(); ++i) {
        if (i >= entry->tri_extra.size()) continue;
        const int material = entry->tri_extra[i].materialId;
        const Tri& tri = entry->triangles[i];
        const float cx = (tri.vertex0.x + tri.vertex1.x + tri.vertex2.x) / 3.0f;
        const float cy = (tri.vertex0.y + tri.vertex1.y + tri.vertex2.y) / 3.0f;
        const float cz = (tri.vertex0.z + tri.vertex1.z + tri.vertex2.z) / 3.0f;
        if (cy <= 0.50f) continue;
        for (size_t cell = 0; cell < kCells.size(); ++cell) {
            const float x = (kCells[cell].col - 2) * 5.0f;
            const float z = (kCells[cell].row - 2) * 5.0f;
            if (material == kCells[cell].material &&
                std::fabs(cx - x) < 1.8f && std::fabs(cz - z) < 1.8f) {
                ++hits[cell];
            }
        }
    }
}
for (size_t i = 0; i < hits.size(); ++i)
    CHECK(hits[i] > 0, "each grid cell bakes geometry with its authored material");

std::ifstream schema_file(schemas / "LightingGarden.js");
std::ostringstream schema_text;
schema_text << schema_file.rdbuf();
const std::string source = schema_text.str();
auto occurrences = [&](const std::string& needle) {
    size_t count = 0, at = 0;
    while ((at = source.find(needle, at)) != std::string::npos) {
        ++count; at += needle.size();
    }
    return count;
};
CHECK(occurrences("kind: 'cube'") == 7, "garden declares seven cubes");
CHECK(occurrences("kind: 'sphere'") == 9, "garden declares nine spheres");
CHECK(occurrences("kind: 'iso'") == 8, "garden declares eight isosurfaces");
CHECK(occurrences("kind: 'water'") == 1, "garden declares one water capsule");
CHECK(source.find("Math.random") == std::string::npos &&
          source.find("Date.now") == std::string::npos,
      "garden schema is deterministic");

InstallResult warm = graph.install(roots);
CHECK(warm.ok && warm.baked.empty(), "second garden install is a pure cache hit");
```

Finish with the standard `ALL PASS`/failure count return. `SandboxGuard`
restores the working directory and removes the transient cache on every return.

In `MatterEngine3/tests/Makefile`, add:

```make
LIGHTING_GARDEN_TARGET = lighting_garden_tests
LIGHTING_GARDEN_CPP = lighting_garden_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))
LIGHTING_GARDEN_C = $(COMMON_MSL_C)
LIGHTING_GARDEN_OBJS = $(call obj_list,sh,$(LIGHTING_GARDEN_CPP)) \
                       $(call obj_list,mslc,$(LIGHTING_GARDEN_C)) $(qjsc_C_OBJS)

$(LIGHTING_GARDEN_TARGET): $(LIGHTING_GARDEN_OBJS)
	$(CC) $(LIGHTING_GARDEN_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-lighting-garden: $(LIGHTING_GARDEN_TARGET)
	./$(LIGHTING_GARDEN_TARGET)
```

Add the target to `.PHONY` and `clean` without changing unrelated targets.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
make -C MatterEngine3/tests run-lighting-garden
```

Expected: the test target builds, then fails because the `LightingGarden`
manifest/schema do not exist.

- [ ] **Step 3: Create the exact manifest**

Create `WorldData/LightingGarden/world.manifest`:

```text
# Walkable dusk material-interaction matrix and visual RT red/green fixture.
LightingGarden
light sun  -0.55 -0.35 -0.75  0.45 0.24 0.12
light sky   0.055 0.075 0.16
```

- [ ] **Step 4: Author the deterministic matrix and environment**

Create `LightingGarden.js` with this exact independent data table:

```js
const GARDEN_CELLS = [
  { row: 0, col: 0, kind: 'cube',   material: MAT.plaster },
  { row: 0, col: 1, kind: 'sphere', material: MAT.metal },
  { row: 0, col: 2, kind: 'iso',    material: MAT.light },
  { row: 0, col: 3, kind: 'sphere', material: MAT.greenGlass },
  { row: 0, col: 4, kind: 'cube',   material: MAT.charcoal },
  { row: 1, col: 0, kind: 'iso',    material: MAT.copper },
  { row: 1, col: 1, kind: 'cube',   material: MAT.lightCool },
  { row: 1, col: 2, kind: 'sphere', material: MAT.snow },
  { row: 1, col: 3, kind: 'water',  material: MAT.water },
  { row: 1, col: 4, kind: 'iso',    material: MAT.lacquerRed },
  { row: 2, col: 0, kind: 'cube',   material: MAT.bark },
  { row: 2, col: 1, kind: 'sphere', material: MAT.ceramic },
  { row: 2, col: 2, kind: 'cube',   material: MAT.light },
  { row: 2, col: 3, kind: 'sphere', material: MAT.chrome },
  { row: 2, col: 4, kind: 'iso',    material: MAT.leaf },
  { row: 3, col: 0, kind: 'sphere', material: MAT.glassSmoke },
  { row: 3, col: 1, kind: 'iso',    material: MAT.goldRough },
  { row: 3, col: 2, kind: 'cube',   material: MAT.stone,
    tint: [0.78, 0.10, 0.065, 1.0] },
  { row: 3, col: 3, kind: 'sphere', material: MAT.lightCool },
  { row: 3, col: 4, kind: 'iso',    material: MAT.wax },
  { row: 4, col: 0, kind: 'iso',    material: MAT.stoneDark },
  { row: 4, col: 1, kind: 'sphere', material: MAT.stone,
    tint: [0.12, 0.62, 0.20, 1.0] },
  { row: 4, col: 2, kind: 'cube',   material: MAT.lightWarmLow },
  { row: 4, col: 3, kind: 'iso',    material: MAT.foliageThin },
  { row: 4, col: 4, kind: 'sphere', material: MAT.stone,
    tint: [0.94, 0.94, 0.98, 1.0] },
];
```

Implement one `LightingGarden extends Part` with these exact geometry rules:

```js
const SPACING = 5.0;
const NEUTRAL_TINT = [1, 1, 1, 0];
const center = c => [(c.col - 2) * SPACING, (c.row - 2) * SPACING];

class LightingGarden extends Part {
  build(p) {
    // Ground slab, top at Y=0.
    this.fill(MAT.charcoal);
    this.box([0, -0.10, 0], [16.0, 0.10, 16.0]);

    // Northern water strip, outside the matrix walking lanes.
    this.fill(MAT.water);
    this.box([0, 0.06, -14.0], [12.0, 0.06, 0.75]);

    // Sparse silhouette backdrops behind alternating northern edge cells.
    this.fill(MAT.charcoal);
    for (const x of [-10, 0, 10])
      this.box([x, 2.30, -12.35], [1.70, 2.30, 0.12]);

    // Plinths and direct mesh sculptures.
    for (const cell of GARDEN_CELLS) {
      const [x, z] = center(cell);
      this.fill(MAT.plaster);
      this.tint(NEUTRAL_TINT[0], NEUTRAL_TINT[1], NEUTRAL_TINT[2], NEUTRAL_TINT[3]);
      this.box([x, 0.175, z], [1.60, 0.175, 1.60]);
      if (cell.kind === 'iso') continue;

      const tint = cell.tint || NEUTRAL_TINT;
      this.fill(cell.material);
      this.tint(tint[0], tint[1], tint[2], tint[3]);
      this.pushMatrix();
      this.translate(x, 1.50, z);
      if (cell.kind === 'cube') {
        this.rotateY(Math.PI / 8);
        this.box([0, 0, 0], [1.15, 1.15, 1.15]);
      } else if (cell.kind === 'sphere') {
        this.sphere([0, 0.20, 0], 1.35);
      } else {
        this.capsule([-0.9, 0.15, 0], [0.9, 0.15, 0], 0.75);
      }
      this.popMatrix();
    }

    // One separated marching-cubes field keeps organic cells comparable while
    // remaining far enough apart that their SDFs never interact.
    this.beginVoxels(0.10);
    this.smoothing(0.35);
    let isoIndex = 0;
    for (const cell of GARDEN_CELLS) {
      if (cell.kind !== 'iso') continue;
      const [x, z] = center(cell);
      const tint = cell.tint || NEUTRAL_TINT;
      this.fill(cell.material);
      this.tint(tint[0], tint[1], tint[2], tint[3]);
      this.sphere([x - 0.45, 1.50, z], 1.15);
      this.sphere([x + 0.45, 1.75, z], 0.90);
      if ((isoIndex & 1) !== 0) {
        this.sphere([x, 1.85, z + 0.80], 0.55);
        this.difference();
      }
      ++isoIndex;
    }
    this.endVoxels();
    this.tint(1, 1, 1, 0);
  }
}
```

- [ ] **Step 5: Verify cold bake, exact cells, geometry families, and warm cache**

Run:

```bash
make -C MatterEngine3/tests run-lighting-garden
```

Expected: `ALL PASS`. The output reports one cold root artifact, nonzero baked
triangles for each of the 25 expected material/coordinate pairs, 7 cubes,
9 spheres, 8 isosurfaces, one water capsule, and a pure cache hit on the second
install.

- [ ] **Step 6: Build the enabled Windows Viewer and run one bounded garden smoke**

From an MSYS2 UCRT64 shell with `/ucrt64/bin:/usr/bin` at the front of `PATH`:

```bash
make -C MatterViewer windows -j2 \
  HAVE_CUDA=1 HAVE_STREAMLINE=1 \
  STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.12.0 \
  STREAMLINE_DLL_DIR=/d/SDKs/streamline-sdk-v2.12.0/bin/x64/development
```

Then run one bounded, self-exiting PowerShell smoke with the validation layer,
a fresh cache, and a screenshot used only as proof of presentation:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"
$env:VK_LAYER_PATH = 'C:\msys64\ucrt64\bin'
$env:MATTER_WORLD = 'LightingGarden'
$env:MATTER_CACHE_ROOT = 'D:\Shared With Desktop\AI\matter-engine-cpp\.tmp\lighting-garden-demo-cache'
$env:MATTER_SCREENSHOT = 'D:\Shared With Desktop\AI\matter-engine-cpp\.tmp\lighting-garden-demo.png'
$env:MATTER_HIDE_UI = '1'
$log = 'D:\Shared With Desktop\AI\matter-engine-cpp\.tmp\lighting-garden-demo.log'
$process = Start-Process -FilePath 'D:\Shared With Desktop\AI\matter-engine-cpp\MatterViewer\viewer.exe' `
  -WorkingDirectory 'D:\Shared With Desktop\AI\matter-engine-cpp\MatterViewer' `
  -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru
if (-not $process.WaitForExit(120000)) {
    $process.Kill()
    throw 'LightingGarden viewer smoke exceeded 120 seconds'
}
if ($process.ExitCode -ne 0) { throw "viewer exited $($process.ExitCode)" }
$joined = (Get-Content -Raw $log) + "`n" + (Get-Content -Raw "$log.err")
foreach ($forbidden in @('FATAL:', 'bake error', 'Vulkan validation errors:')) {
    if ($joined.Contains($forbidden)) { throw "garden smoke reported $forbidden" }
}
foreach ($required in @('bake finished (0 errors)',
                        'selected world LightingGarden hash',
                        'screenshot written to')) {
    if (-not $joined.Contains($required)) { throw "garden smoke missing $required" }
}
if (-not (Test-Path $env:MATTER_SCREENSHOT)) {
    throw 'garden smoke did not write its presentation screenshot'
}
```

Do not inspect screenshot pixels. The Viewer exits itself after the requested
capture, producing a graceful validation summary. The controller launches the
final sign-off instance without `MATTER_SCREENSHOT` after review.

- [ ] **Step 7: Commit Task 2**

```bash
git add MatterEngine3/examples/world_demo/schemas/LightingGarden.js \
  MatterEngine3/examples/world_demo/WorldData/LightingGarden/world.manifest \
  MatterEngine3/tests/lighting_garden_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat(demo): add lighting sculpture garden"
```

---

## Final Acceptance Gate

- [ ] Generate a review package from pre-plan base `9669259` through Task 2 and request a fresh whole-feature review.
- [ ] Fix every Critical and Important finding and re-review until approved.
- [ ] Re-run `run-reg`, `run-partv2`, `run-lighting-garden`, and the enabled Windows build only for fixes that touch their covered files.
- [ ] Launch `MatterViewer/viewer.exe` with `MATTER_WORLD=LightingGarden`, the configured Vulkan validation layer, and the fresh validated garden cache.
- [ ] Leave the approved Viewer running for user inspection.
