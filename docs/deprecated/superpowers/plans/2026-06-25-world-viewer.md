# World Viewer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a GL viewer under `MatterEngine3/viewer/` that connects to a world source, reconciles a persistent content-addressed part cache (build-once / instant warm reload), composes the world into the MSL raytracer with selectable sector-LOD, and overlays a Dear ImGui debug HUD.

**Architecture:** Six units. `world_source` defines the `WorldProvider` lifecycle (connect → reconcile → fetch_parts → poll_deltas) plus the live `WorldState`; `LocalProvider` drives the SP-3 install path over a durable cache dir. `part_library`'s `PartStore` owns one `BLASManager`, loads `.part` blobs, and regenerates LOD levels via `lod_bake` (since `.part` stores only LOD0). `world_composer` resolves active instances per frame through a swappable `SectorResolver` (`PassThrough` vs `SectorLod`) into a `TLASManager`. `renderer` binds BLAS/TLAS to the MSL raytrace shader and draws a fullscreen pass. `ui` is the Dear ImGui overlay. `main` wires them together.

**Tech Stack:** C++17, MSL `BLASManager`/`TLASManager` + `raytrace_tlas_blas_processed.fs` (read-only reuse), SP-1 `part_asset_v2`, SP-2 `ScriptHost`/QuickJS, SP-3 `PartGraph`, SP-4 `world_flatten`/`sector_grid`/`lod_select`/`lod_bake`, raylib (GLFW desktop), Dear ImGui (GLFW + OpenGL3 backends).

---

## File Structure

All new code lives under `MatterEngine3/viewer/`. MSL and SP-1…SP-7 sources are consumed read-only.

| File | Responsibility |
| --- | --- |
| `MatterEngine3/viewer/world_source.h` | `WorldManifestEntry`, `WorldManifest`, `WorldDelta`, `WorldState` (+ apply-delta), `WorldProvider` interface. |
| `MatterEngine3/viewer/world_state.cpp` | `WorldState` apply-delta implementation (GL-free). |
| `MatterEngine3/viewer/local_provider.h` / `.cpp` | `LocalProvider : WorldProvider` over a persistent cache dir using the SP-3 install path. |
| `MatterEngine3/viewer/part_store.h` / `.cpp` | `PartStore`: owned `BLASManager`, `.part` load + `lod_bake` LOD regen, `has`/`get_or_load`, `part_lod_table()`. |
| `MatterEngine3/viewer/sector_resolver.h` | `ResolvedInstance`, `SectorResolver` interface. |
| `MatterEngine3/viewer/resolvers.cpp` | `PassThroughResolver` + `SectorLodResolver` implementations. |
| `MatterEngine3/viewer/world_composer.h` / `.cpp` | `WorldComposer`: resolve → record into `TLASManager` → build. |
| `MatterEngine3/viewer/renderer.h` / `.cpp` | `Renderer`: raytrace shader, camera + uniforms, fullscreen draw. |
| `MatterEngine3/viewer/ui.h` / `.cpp` | `Ui` + `ViewerStats`: Dear ImGui setup/begin/end/shutdown + `draw_debug_panel`. |
| `MatterEngine3/viewer/main.cpp` | Window, wire-up, connect sequence, frame loop. |
| `MatterEngine3/viewer/Makefile` | GL build target (reuses the `example_world` link recipe + imgui). |
| `MatterEngine3/tests/viewer_logic_tests.cpp` | Headless tests for `WorldState`, `SectorResolver`, `PartStore`, `LocalProvider` cache behavior. |

The GL-free units (`world_state`, `part_store`, `sector_resolver`/`resolvers`, `world_composer`, `local_provider`) are tested headlessly via a `run-viewer-logic` target. `renderer`, `ui`, and `main` need a GL context and are verified manually.

---

### Task 1: WorldState types and delta application (GL-free)

**Files:**
- Create: `MatterEngine3/viewer/world_source.h`
- Create: `MatterEngine3/viewer/world_state.cpp`
- Create: `MatterEngine3/tests/viewer_logic_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile` (add `run-viewer-logic` target)

- [ ] **Step 1: Write the failing test**

Create `MatterEngine3/tests/viewer_logic_tests.cpp`:

```cpp
// Headless tests for the GL-free viewer units (WorldState, resolvers, PartStore,
// LocalProvider cache behavior). Run via `make run-viewer-logic`.
#include "../viewer/world_source.h"

#include <cstdio>
#include <cstdint>
#include <string>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else        { printf("ok:   %s\n", (msg)); } } while (0)

static viewer::WorldManifestEntry mk_entry(uint32_t id, uint64_t hash, float x) {
    viewer::WorldManifestEntry e{};
    e.instance_id = id;
    e.part_hash   = hash;
    for (int i = 0; i < 16; ++i) e.transform[i] = 0.0f;
    e.transform[0] = e.transform[5] = e.transform[10] = e.transform[15] = 1.0f;
    e.transform[3] = x;   // translate-x
    return e;
}

static void test_world_state_delta() {
    viewer::WorldState state;
    viewer::WorldManifest m;
    m.world_root_hash = 1;
    m.instances.push_back(mk_entry(10, 0xAAAA, 1.0f));
    m.instances.push_back(mk_entry(11, 0xBBBB, 2.0f));
    state.reset(m);
    CHECK(state.entries().size() == 2, "reset loads manifest entries");

    viewer::WorldDelta d;
    d.added.push_back(mk_entry(12, 0xCCCC, 3.0f));   // new
    d.added.push_back(mk_entry(10, 0xAAAA, 9.0f));   // move existing id 10
    d.removed.push_back(11);                          // drop id 11
    state.apply(d);

    CHECK(state.entries().size() == 2, "delta: add one, remove one -> net 2");
    const viewer::WorldManifestEntry* moved = state.find(10);
    CHECK(moved && moved->transform[3] == 9.0f, "delta: id 10 transform updated in place");
    CHECK(state.find(11) == nullptr, "delta: id 11 removed");
    CHECK(state.find(12) != nullptr, "delta: id 12 added");
}

int main() {
    test_world_state_delta();
    printf("\n%s\n", g_failures == 0 ? "viewer-logic OK" : "viewer-logic FAILED");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run the test to verify it fails to build**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: FAIL — `world_source.h` not found / `run-viewer-logic` target missing.

- [ ] **Step 3: Write `world_source.h`**

Create `MatterEngine3/viewer/world_source.h`:

```cpp
#ifndef VIEWER_WORLD_SOURCE_H
#define VIEWER_WORLD_SOURCE_H

#include <cstdint>
#include <string>
#include <vector>

namespace viewer {

class PartStore;   // fwd; defined in part_store.h

// One placed instance in the authoritative world. Transform is row-major
// float[16] to match part_asset::ChildInstance and TLAS DrawInstance.
struct WorldManifestEntry {
    uint32_t instance_id = 0;
    uint64_t part_hash   = 0;   // resolved hash of the placed part
    float    transform[16] = {0};
};

struct WorldManifest {
    uint64_t world_root_hash = 0;
    std::vector<WorldManifestEntry> instances;
};

struct WorldDelta {
    std::vector<WorldManifestEntry> added;    // new or moved (replace by instance_id)
    std::vector<uint32_t>           removed;  // instance_ids to drop
};

// Live, mutable world: the manifest snapshot plus incremental deltas.
class WorldState {
public:
    void reset(const WorldManifest& m);          // replace all entries
    void apply(const WorldDelta& d);              // add/move/remove by instance_id
    const std::vector<WorldManifestEntry>& entries() const { return entries_; }
    const WorldManifestEntry* find(uint32_t instance_id) const;

private:
    std::vector<WorldManifestEntry> entries_;
};

// Source of world + part data. Same interface for LocalProvider (in-process)
// and a future NetworkProvider. See world_source.h docs / the design spec.
class WorldProvider {
public:
    virtual ~WorldProvider() = default;
    virtual bool connect(WorldManifest& out, std::string& err) = 0;
    virtual std::vector<uint64_t>
        reconcile(const WorldManifest& manifest, const PartStore& store) = 0;
    virtual bool fetch_parts(const std::vector<uint64_t>& want,
                             PartStore& store, std::string& err) = 0;
    virtual bool poll_deltas(WorldDelta& out) = 0;
};

} // namespace viewer

#endif // VIEWER_WORLD_SOURCE_H
```

- [ ] **Step 4: Write `world_state.cpp`**

Create `MatterEngine3/viewer/world_state.cpp`:

```cpp
#include "world_source.h"

#include <cstring>

namespace viewer {

void WorldState::reset(const WorldManifest& m) {
    entries_ = m.instances;
}

const WorldManifestEntry* WorldState::find(uint32_t instance_id) const {
    for (const auto& e : entries_)
        if (e.instance_id == instance_id) return &e;
    return nullptr;
}

void WorldState::apply(const WorldDelta& d) {
    // Removals first so a same-frame re-add of an id is honored.
    for (uint32_t id : d.removed) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].instance_id == id) {
                entries_.erase(entries_.begin() + i);
                break;
            }
        }
    }
    // Adds: replace existing id in place (a "move"), else append.
    for (const auto& add : d.added) {
        bool replaced = false;
        for (auto& e : entries_) {
            if (e.instance_id == add.instance_id) { e = add; replaced = true; break; }
        }
        if (!replaced) entries_.push_back(add);
    }
}

} // namespace viewer
```

- [ ] **Step 5: Add the `run-viewer-logic` target to the tests Makefile**

In `MatterEngine3/tests/Makefile`, the `example_world` recipe (`EXAMPLE_TARGET`, lines 269-294) already defines the exact pipeline source set we need. Reuse its `$(EXAMPLE_CPP)` (drop `example_world.cpp`), `$(EXAMPLE_C)`, `$(QJS_C)` and the gcc object pattern. Add this near the other `run-*` targets, and add `run-viewer-logic` to the `.PHONY` line (57) and `viewer_logic_tests` to the `clean` rule (82):

```makefile
# --- Viewer GL-free logic suite (WorldState, resolvers, PartStore, LocalProvider).
# Reuses the example_world pipeline source set (SP-2 host + QuickJS + SP-4 compose
# + MSL backend) and adds the viewer's GL-free units. Same dir depth as tests/, so
# every ../ / ../../ path resolves identically. NOT a GL build (no renderer/ui/main).
VIEWER_LOGIC_TARGET = viewer_logic_tests
# EXAMPLE_CPP without its driver (example_world.cpp), plus the viewer's logic units.
VIEWER_PIPELINE_CPP = ../src/part_graph.cpp \
              ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_bindings.cpp \
              ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/script_rng_binding.cpp \
              ../src/part_asset_v2.cpp ../src/lod_bake.cpp ../src/world_flatten.cpp \
              ../src/sector_grid.cpp ../src/lod_select.cpp \
              ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
              ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
              ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
              ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
              ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
              ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
              ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp
VIEWER_LOGIC_CPP = viewer_logic_tests.cpp \
              ../viewer/world_state.cpp ../viewer/part_store.cpp ../viewer/resolvers.cpp \
              ../viewer/world_composer.cpp ../viewer/local_provider.cpp \
              $(VIEWER_PIPELINE_CPP)

$(VIEWER_LOGIC_TARGET): $(VIEWER_LOGIC_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(VIEWER_LOGIC_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(VIEWER_LOGIC_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) -I../viewer $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-viewer-logic: $(VIEWER_LOGIC_TARGET)
	./$(VIEWER_LOGIC_TARGET)
```

NOTE: `VIEWER_LOGIC_CPP` references viewer units created in later tasks (`part_store.cpp`, `resolvers.cpp`, `world_composer.cpp`, `local_provider.cpp`). When running THIS task standalone, the linker will fail on those missing files — temporarily trim `VIEWER_LOGIC_CPP` to `viewer_logic_tests.cpp ../viewer/world_state.cpp $(VIEWER_PIPELINE_CPP)` and restore the full list at Task 4 (after `local_provider.cpp` exists). The Task 1 test only uses `WorldState`, so the trimmed link is sufficient here.

- [ ] **Step 6: Run the test to verify it passes**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: PASS — all `ok:` lines, `viewer-logic OK`, exit 0.

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/viewer/world_source.h MatterEngine3/viewer/world_state.cpp \
        MatterEngine3/tests/viewer_logic_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat(viewer): WorldState + WorldProvider interface with delta apply"
```

---

### Task 2: SectorResolver seam (PassThrough + SectorLod)

**Files:**
- Create: `MatterEngine3/viewer/sector_resolver.h`
- Create: `MatterEngine3/viewer/resolvers.cpp`
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp` (add resolver tests)

- [ ] **Step 1: Write the failing test**

Add to `MatterEngine3/tests/viewer_logic_tests.cpp` (above `main`, and call from `main`):

```cpp
#include "../viewer/sector_resolver.h"
#include "lod_select.h"   // PartLodTable, PartLod

static void test_resolvers() {
    const uint64_t kPart = 0xF00DULL;
    // Two instances of one part, far apart on the x axis.
    viewer::WorldState state;
    viewer::WorldManifest m; m.world_root_hash = 1;
    m.instances.push_back(mk_entry(1, kPart, 0.0f));
    m.instances.push_back(mk_entry(2, kPart, 200.0f));
    state.reset(m);

    // LOD table: one part, bound radius 1.0, three thresholds (coarser = larger).
    lod_select::PartLodTable lods;
    lods[kPart] = lod_select::PartLod{ 1.0f, { 0.50f, 0.20f, 0.05f } };

    // PassThrough: everything active at LOD 0, ignores camera.
    viewer::PassThroughResolver pass;
    auto a = pass.resolve(state, lods, make_float3(0,0,0));
    CHECK(a.size() == 2, "passthrough activates all instances");
    CHECK(a[0].lod_level == 0 && a[1].lod_level == 0, "passthrough uses LOD 0");

    // SectorLod with a large activation radius so BOTH instances stay active and
    // the test exercises LOD selection (not culling): near camera keeps the near
    // instance fine, far camera coarsens.
    viewer::SectorLodResolver sec(16.0f /*pitch*/, 1000.0f /*active radius*/);
    auto near = sec.resolve(state, lods, make_float3(0,4,-4));
    bool near_present = false; int near_lod = -1;
    for (auto& r : near) if (r.transform[3] == 0.0f) { near_present = true; near_lod = r.lod_level; }
    CHECK(near_present, "sectorlod keeps the near instance active");

    // Far camera: both instances are distant -> coarser-or-equal LOD than near view.
    auto far_view = sec.resolve(state, lods, make_float3(0, 4000, -4000));
    int far_max_lod = 0;
    for (auto& r : far_view) far_max_lod = (r.lod_level > far_max_lod) ? r.lod_level : far_max_lod;
    CHECK(far_max_lod >= near_lod, "sectorlod picks coarser-or-equal LOD from far camera");
}
```

In `main`, add `test_resolvers();` after `test_world_state_delta();`. (`far` is a reserved-ish identifier on some toolchains, so this uses `far_view`.)

- [ ] **Step 2: Run the test to verify it fails**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: FAIL — `sector_resolver.h` not found.

- [ ] **Step 3: Write `sector_resolver.h`**

Create `MatterEngine3/viewer/sector_resolver.h`:

```cpp
#ifndef VIEWER_SECTOR_RESOLVER_H
#define VIEWER_SECTOR_RESOLVER_H

#include "world_source.h"
#include "lod_select.h"        // lod_select::PartLodTable; also brings in float3/make_float3
#include "sector_grid.h"       // sector_grid::SectorGrid, bin_instances (transitively precomp.h)

#include <cstdint>
#include <vector>

namespace viewer {

// An instance the composer should record this frame, with its chosen LOD level.
struct ResolvedInstance {
    uint64_t part_hash  = 0;
    int      lod_level  = 0;          // index into the part's LOD levels
    float    transform[16] = {0};     // row-major world placement
};

// Strategy: "given the camera, which instances render and at what LOD?"
class SectorResolver {
public:
    virtual ~SectorResolver() = default;
    virtual std::vector<ResolvedInstance>
        resolve(const WorldState& state,
                const lod_select::PartLodTable& lods,
                const float3& cam_pos) = 0;
    virtual const char* name() const = 0;
};

// Baseline: all instances active at LOD 0, no culling. Correctness reference.
class PassThroughResolver : public SectorResolver {
public:
    std::vector<ResolvedInstance>
        resolve(const WorldState&, const lod_select::PartLodTable&, const float3&) override;
    const char* name() const override { return "PassThrough"; }
};

// Bins instances into sectors, picks per-sector LOD via lod_select, and
// activates sectors within `active_radius_` of the camera.
class SectorLodResolver : public SectorResolver {
public:
    SectorLodResolver(float pitch, float active_radius)
        : pitch_(pitch), active_radius_(active_radius) {}
    std::vector<ResolvedInstance>
        resolve(const WorldState&, const lod_select::PartLodTable&, const float3&) override;
    const char* name() const override { return "SectorLod"; }
    void set_active_radius(float r) { active_radius_ = r; }

private:
    float pitch_;
    float active_radius_;
};

} // namespace viewer

#endif // VIEWER_SECTOR_RESOLVER_H
```

NOTE: confirm the `float3` / `make_float3` declaration the SP-4 headers already use (the same one `lod_select.h` / `sector_grid.h` consume — `example_world.cpp` calls `make_float3`). Include whatever header provides it (likely `raymath.h` or an MSL vector header); match `example_world.cpp`'s include set rather than guessing. If `float3` comes from an MSL header, include that instead of `raymath.h`.

- [ ] **Step 4: Write `resolvers.cpp`**

Create `MatterEngine3/viewer/resolvers.cpp`:

```cpp
#include "sector_resolver.h"

#include "world_flatten.h"     // world_flatten::FlatInstance
#include <cmath>
#include <cstring>

namespace viewer {

static ResolvedInstance to_resolved(const WorldManifestEntry& e, int lod) {
    ResolvedInstance r;
    r.part_hash = e.part_hash;
    r.lod_level = lod;
    std::memcpy(r.transform, e.transform, sizeof(r.transform));
    return r;
}

std::vector<ResolvedInstance>
PassThroughResolver::resolve(const WorldState& state,
                             const lod_select::PartLodTable&, const float3&) {
    std::vector<ResolvedInstance> out;
    out.reserve(state.entries().size());
    for (const auto& e : state.entries())
        out.push_back(to_resolved(e, 0));
    return out;
}

std::vector<ResolvedInstance>
SectorLodResolver::resolve(const WorldState& state,
                           const lod_select::PartLodTable& lods,
                           const float3& cam_pos) {
    // 1. Build FlatInstances so we can reuse SP-4 binning + lod-select verbatim.
    std::vector<world_flatten::FlatInstance> flat;
    flat.reserve(state.entries().size());
    for (const auto& e : state.entries()) {
        world_flatten::FlatInstance fi;
        fi.resolved_hash = e.part_hash;
        std::memcpy(fi.world.cell, e.transform, sizeof(fi.world.cell));  // mat4::cell[16]
        flat.push_back(fi);
    }

    // 2. Bin into sectors and choose per-sector LOD for this camera.
    sector_grid::SectorGrid grid(pitch_);
    sector_grid::Sectors sectors = sector_grid::bin_instances(flat, grid);
    auto chosen = lod_select::select_sector_lods(sectors, lods, cam_pos);

    // 3. Emit instances only for sectors within the activation sphere.
    std::vector<ResolvedInstance> out;
    for (const auto& sk : sectors) {
        const sector_grid::SectorCoord& c = sk.first;
        float sx = (c.x + 0.5f) * pitch_;
        float sy = (c.y + 0.5f) * pitch_;
        float sz = (c.z + 0.5f) * pitch_;
        float dx = sx - cam_pos.x, dy = sy - cam_pos.y, dz = sz - cam_pos.z;
        if (std::sqrt(dx*dx + dy*dy + dz*dz) > active_radius_) continue;

        const auto& lod_for_part = chosen[c];   // map<part_hash,int>
        for (const auto& inst : sk.second) {
            int lod = 0;
            auto it = lod_for_part.find(inst.resolved_hash);
            if (it != lod_for_part.end()) lod = it->second;
            WorldManifestEntry tmp;
            tmp.part_hash = inst.resolved_hash;
            std::memcpy(tmp.transform, inst.world.cell, sizeof(tmp.transform));  // mat4::cell[16]
            out.push_back(to_resolved(tmp, lod));
        }
    }
    return out;
}

} // namespace viewer
```

NOTE: `FlatInstance.world` is a `mat4` whose storage member is `float cell[16]` (row-major, identity-initialized) — verified in `MatterSurfaceLib/include/bvh.h:133`, NOT `.m`. `float3` (members `x,y,z`, also `cell[3]`) and `make_float3` come from `MatterSurfaceLib/include/precomp.h` (pulled in transitively via `blas_manager.hpp`/`bvh.h`); include whichever the sibling headers already expose — `example_world.cpp` gets `make_float3` for free through its includes. Do NOT add `raymath.h` for this; remove the `#include "raymath.h"` from `sector_resolver.h` if `precomp.h`/`bvh.h` already supplies `float3`.

The far-camera test asserts coarser-or-equal LOD; if `active_radius_` deactivates the far sector entirely, the far instance is dropped (also acceptable — adjust the test's `far` assertion to "far view drops or coarsens distant instances" if needed, but prefer a radius large enough that both are active so LOD differences are exercised). Set the test's `SectorLodResolver` active radius to `1000.0f` so both instances stay active and the test exercises LOD selection, not just culling.

- [ ] **Step 5: Run the test to verify it passes**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: PASS — resolver checks `ok:`.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/viewer/sector_resolver.h MatterEngine3/viewer/resolvers.cpp \
        MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat(viewer): SectorResolver seam with PassThrough and SectorLod"
```

---

### Task 3: PartStore (load + LOD regen + content-addressed dedup)

**Files:**
- Create: `MatterEngine3/viewer/part_store.h`
- Create: `MatterEngine3/viewer/part_store.cpp`
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp` (add PartStore test — exercised indirectly via Task 4's LocalProvider test; here only construction/`has` on missing hash)

- [ ] **Step 1: Write the failing test**

Add to `viewer_logic_tests.cpp` and call from `main`:

```cpp
#include "../viewer/part_store.h"

static void test_part_store_missing() {
    viewer::PartStore store("/tmp/me3_viewer_test_cache_empty");
    CHECK(!store.has(0xDEADBEEFULL), "fresh store reports unknown hash as absent");
    CHECK(store.loaded_count() == 0, "fresh store has nothing loaded");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: FAIL — `part_store.h` not found.

- [ ] **Step 3: Write `part_store.h`**

Create `MatterEngine3/viewer/part_store.h`:

```cpp
#ifndef VIEWER_PART_STORE_H
#define VIEWER_PART_STORE_H

#include "blas_manager.hpp"     // MSL BLASManager / BLASHandle
#include "lod_select.h"         // lod_select::PartLodTable

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace viewer {

// A part loaded into the shared BLASManager: one BLAS handle per LOD level
// (regenerated via lod_bake, since .part stores only LOD0), plus the LOD
// metadata the SectorResolver needs.
struct LoadedPart {
    std::vector<BLASHandle> lod_blas;       // lod_blas[i] -> BLAS for LOD level i
    float                   bound_radius = 0.0f;
    std::vector<float>      thresholds;      // per-LOD screen-size thresholds
};

// Owns one BLASManager shared across all loaded parts. Content-addressed and
// durable: a .part baked on a prior run is found on disk under cache_root/parts/.
class PartStore {
public:
    explicit PartStore(std::string cache_root);

    // True if the part is loaded in memory OR a .part exists on disk. Drives reconcile.
    bool has(uint64_t part_hash) const;

    // Load (memoized) a part: load_v2 -> lod_bake LODs -> register in the shared
    // BLASManager. Returns nullptr on failure (logged once per hash). idempotent.
    const LoadedPart* get_or_load(uint64_t part_hash);

    BLASManager& blas() { return blas_; }
    const std::string& cache_root() const { return cache_root_; }
    size_t loaded_count() const { return loaded_.size(); }

    // LOD table for the SectorResolver: radius + thresholds per loaded part.
    lod_select::PartLodTable part_lod_table() const;

private:
    std::string disk_path(uint64_t part_hash) const;   // cache_root_ + "/parts/<hash>.part"

    std::string                       cache_root_;
    BLASManager                       blas_;
    std::map<uint64_t, LoadedPart>    loaded_;
    std::map<uint64_t, bool>          load_failed_;     // suppress repeat logging
};

} // namespace viewer

#endif // VIEWER_PART_STORE_H
```

- [ ] **Step 4: Write `part_store.cpp`**

Create `MatterEngine3/viewer/part_store.cpp`:

```cpp
#include "part_store.h"

#include "part_asset_v2.h"     // load_v2, cache_path_resolved, ChildInstance, LodLevels
#include "lod_bake.h"          // lod_bake::bake_lods, BakeTargets
#include "tlas_manager.hpp"    // TLASManager (load_v2 signature needs one)

#include <cmath>
#include <cstdio>
#include <sys/stat.h>

namespace viewer {

PartStore::PartStore(std::string cache_root) : cache_root_(std::move(cache_root)) {}

std::string PartStore::disk_path(uint64_t part_hash) const {
    // cache_path_resolved returns the RELATIVE "parts/<hash>.part"; prefix cache_root_.
    return cache_root_ + "/" + part_asset::cache_path_resolved(part_hash);
}

bool PartStore::has(uint64_t part_hash) const {
    if (loaded_.count(part_hash)) return true;
    struct stat st;
    return ::stat(disk_path(part_hash).c_str(), &st) == 0;
}

const LoadedPart* PartStore::get_or_load(uint64_t part_hash) {
    auto cached = loaded_.find(part_hash);
    if (cached != loaded_.end()) return &cached->second;
    if (load_failed_.count(part_hash)) return nullptr;

    const std::string path = disk_path(part_hash);

    // load_v2 registers the full-resolution geometry into a SCRATCH BLASManager;
    // we then re-bake LODs into the shared store BLASManager.
    BLASManager scratch;
    TLASManager scratch_tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods_in;   // .part stores LOD0 only (empty levels)
    if (!part_asset::load_v2(path, part_hash, scratch, scratch_tlas, children, lods_in)) {
        printf("PartStore: load_v2 failed for %016llx (%s)\n",
               (unsigned long long)part_hash, path.c_str());
        load_failed_[part_hash] = true;
        return nullptr;
    }

    // Gather full-res triangles for lod_bake.
    std::vector<Tri> tris;
    for (const auto& e : scratch.get_entries())
        tris.insert(tris.end(), e->triangles.begin(), e->triangles.end());

    // Bound radius = half AABB diagonal (drives projected-size LOD math).
    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
    auto acc = [&](const float3& v){
        mn[0]=std::fmin(mn[0],v.x); mx[0]=std::fmax(mx[0],v.x);
        mn[1]=std::fmin(mn[1],v.y); mx[1]=std::fmax(mx[1],v.y);
        mn[2]=std::fmin(mn[2],v.z); mx[2]=std::fmax(mx[2],v.z);
    };
    for (const auto& t : tris) { acc(t.vertex0); acc(t.vertex1); acc(t.vertex2); }
    float radius = 0.0f;
    if (!tris.empty()) {
        float dx=mx[0]-mn[0], dy=mx[1]-mn[1], dz=mx[2]-mn[2];
        radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
    }

    // Re-bake LODs into the SHARED store BLASManager. Map each level's local
    // blas index to a global handle: get_entries()[before + local].
    LoadedPart lp;
    lp.bound_radius = radius;
    const size_t before = blas_.get_entries().size();
    BLASManager& shared = blas_;
    // bake_lods registers into the BLASManager passed to it.
    lod_bake::LodLevels lods = lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, shared);
    for (const auto& L : lods) {
        lp.thresholds.push_back(L.screen_size_threshold);
        size_t local = L.blas_indices[0];
        // The handle for entry index (before + local). BLASHandle is the entry's handle.
        lp.lod_blas.push_back(shared.get_entries()[before + local]->handle);
    }
    if (lp.lod_blas.empty()) {
        // No geometry (empty part) -> register an empty BLAS so lookups don't crash.
        printf("PartStore: part %016llx produced no LOD geometry\n",
               (unsigned long long)part_hash);
    }

    auto ins = loaded_.emplace(part_hash, std::move(lp));
    return &ins.first->second;
}

lod_select::PartLodTable PartStore::part_lod_table() const {
    lod_select::PartLodTable table;
    for (const auto& kv : loaded_)
        table[kv.first] = lod_select::PartLod{ kv.second.bound_radius, kv.second.thresholds };
    return table;
}

} // namespace viewer
```

NOTE on `bake_lods` index mapping: confirm against `example_world.cpp` (lines 162-170) which calls `lod_bake::bake_lods(tris, BakeTargets{}, lod_blas)` then reads `lod_blas.get_entries()[L.blas_indices[0]]->triangles.size()`. There `lod_blas` is fresh so `before == 0`. In `PartStore`, the shared BLASManager accumulates across parts, so the `before` offset is required. Verify `BLASEntry` exposes `->handle` (the summary says `BLASEntry has handle`); if the handle is obtained differently (e.g. `register_*` return value), capture handles from the entries created in the `before..after` range instead.

- [ ] **Step 5: Run the test to verify it passes**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: PASS — `fresh store reports unknown hash as absent`, `fresh store has nothing loaded`.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/viewer/part_store.h MatterEngine3/viewer/part_store.cpp \
        MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat(viewer): PartStore with LOD regen over a shared BLASManager"
```

---

### Task 4: LocalProvider over a persistent cache (two-run cache test)

**Files:**
- Create: `MatterEngine3/viewer/local_provider.h`
- Create: `MatterEngine3/viewer/local_provider.cpp`
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp` (add the two-run cache test)

- [ ] **Step 1: Write the failing test**

Add to `viewer_logic_tests.cpp` and call from `main`. This is the headline behavior — cold run bakes & wants everything, warm run bakes nothing & wants nothing:

```cpp
#include "../viewer/local_provider.h"

static void test_local_provider_cache() {
    const std::string cache = "/tmp/me3_viewer_cache_test";
    system(("rm -rf " + cache).c_str());

    // Resolve committed example assets relative to MatterEngine3/tests (cwd).
    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir    = "../examples/world_demo/schemas";
    cfg.world_data_dir = "../examples/world_demo/WorldData";
    cfg.world_name     = "Demo";
    cfg.shared_lib_dir = "../shared-lib";
    cfg.cache_root     = cache;

    // --- Cold run: bake everything, want everything. ---
    {
        viewer::LocalProvider prov(cfg);
        viewer::WorldManifest m; std::string err;
        CHECK(prov.connect(m, err), "cold connect succeeds");
        CHECK(!m.instances.empty(), "cold connect yields placed instances");
        CHECK(prov.baked_count() > 0, "cold connect bakes parts (cache miss)");

        viewer::PartStore store(cache);
        auto want = prov.reconcile(m, store);
        CHECK(!want.empty(), "cold reconcile wants the missing parts");
        CHECK(prov.fetch_parts(want, store, err), "cold fetch_parts loads wanted parts");
        CHECK(store.loaded_count() == want.size(), "cold fetch loads every wanted hash");
    }

    // --- Warm run: same cache, nothing changed -> bake nothing, want nothing. ---
    {
        viewer::LocalProvider prov(cfg);
        viewer::WorldManifest m; std::string err;
        CHECK(prov.connect(m, err), "warm connect succeeds");
        CHECK(prov.baked_count() == 0, "warm connect bakes nothing (all cache hits)");

        viewer::PartStore store(cache);
        auto want = prov.reconcile(m, store);
        CHECK(want.empty(), "warm reconcile wants nothing (instant reload)");
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: FAIL — `local_provider.h` not found.

- [ ] **Step 3: Write `local_provider.h`**

Create `MatterEngine3/viewer/local_provider.h`:

```cpp
#ifndef VIEWER_LOCAL_PROVIDER_H
#define VIEWER_LOCAL_PROVIDER_H

#include "world_source.h"
#include "part_store.h"

#include <cstdint>
#include <map>
#include <string>

namespace viewer {

struct LocalProviderConfig {
    std::string schemas_dir;      // ../examples/world_demo/schemas
    std::string world_data_dir;   // ../examples/world_demo/WorldData
    std::string world_name;       // "Demo"
    std::string shared_lib_dir;   // ../shared-lib
    std::string cache_root;       // persistent parts/ cache (NOT a /tmp throwaway)
};

// Drives the SP-3 install path over a persistent content-addressed cache and
// scatters the example world (terrain/trees/grass) into a WorldManifest. Same
// interface as a future NetworkProvider.
class LocalProvider : public WorldProvider {
public:
    explicit LocalProvider(LocalProviderConfig cfg);

    bool connect(WorldManifest& out, std::string& err) override;
    std::vector<uint64_t> reconcile(const WorldManifest& manifest,
                                    const PartStore& store) override;
    bool fetch_parts(const std::vector<uint64_t>& want,
                     PartStore& store, std::string& err) override;
    bool poll_deltas(WorldDelta& out) override;   // LocalProvider: always false (static world)

    int baked_count() const { return baked_count_; }
    int hit_count()   const { return hit_count_; }

private:
    LocalProviderConfig cfg_;
    int baked_count_ = 0;
    int hit_count_   = 0;
};

} // namespace viewer

#endif // VIEWER_LOCAL_PROVIDER_H
```

- [ ] **Step 4: Write `local_provider.cpp`**

Create `MatterEngine3/viewer/local_provider.cpp`. This mirrors `example_world.cpp` but uses a PERSISTENT cache dir and does NOT chdir/`rm -rf`. HostBaker writes to `cache_root/parts/`:

```cpp
#include "local_provider.h"

#include "part_graph.h"        // -DMATTER_HAVE_SCRIPT_HOST pulls in script_host.h
#include "part_asset_v2.h"     // cache_path_resolved

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

using namespace part_graph;

namespace viewer {

// Deterministic splitmix64 (matches example_world's scatter exactly).
namespace {
struct Rng64 {
    uint64_t s;
    explicit Rng64(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s += 0x9e3779b97f4a7c15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    float range(float a, float b) {
        return a + (float)((next() >> 11) * (1.0 / 9007199254740992.0)) * (b - a);
    }
};
std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}
void set_translate(float m[16], float x, float y, float z) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[3] = x; m[7] = y; m[11] = z;
}
} // namespace

LocalProvider::LocalProvider(LocalProviderConfig cfg) : cfg_(std::move(cfg)) {}

bool LocalProvider::connect(WorldManifest& out, std::string& err) {
    baked_count_ = 0;
    hit_count_   = 0;

    // Ensure the persistent cache dir exists (HostBaker writes cache_root/parts/<hash>.part).
    ::mkdir(cfg_.cache_root.c_str(), 0755);
    std::string parts_dir = cfg_.cache_root + "/parts";
    ::mkdir(parts_dir.c_str(), 0755);

    // SP-2/SP-3/SP-7 wiring. parts_dir_ is the PARENT of parts/ (== cache_root).
    script_host::ScriptHost host;
    host.set_shared_lib_root(cfg_.shared_lib_dir);
    FileModuleResolver resolver(host, cfg_.schemas_dir);
    HostBaker baker(host, cfg_.cache_root);
    PartGraph graph(resolver, baker);

    std::vector<ChildRequest> roots;
    if (!PartGraph::read_manifest(cfg_.world_data_dir, cfg_.world_name, roots, err))
        return false;

    InstallResult ir = graph.install(roots);
    if (!ir.ok) { err = ir.error; return false; }
    baked_count_ = (int)ir.baked.size();
    hit_count_   = ir.hits;

    // Resolve each module's content hash (host is the hash authority).
    std::map<std::string, uint64_t> hash_of;
    for (auto& r : roots) {
        std::string src = read_file(cfg_.schemas_dir + "/" + r.module + ".js");
        if (src.empty()) { err = "missing schema " + r.module; return false; }
        uint64_t h = host.resolve_hash(src, "{}");
        if (h == 0) { err = "resolve_hash failed for " + r.module; return false; }
        hash_of[r.module] = h;
    }

    // Scatter the example world (identical layout to example_world.cpp).
    out.world_root_hash = 1;
    out.instances.clear();
    uint32_t next_id = 1;
    auto place = [&](uint64_t h, float x, float y, float z) {
        WorldManifestEntry e;
        e.instance_id = next_id++;
        e.part_hash   = h;
        set_translate(e.transform, x, y, z);
        out.instances.push_back(e);
    };

    const int kTileGrid = 3; const float kTile = 8.0f;
    const float kSpan = kTileGrid * kTile;
    for (int i = 0; i < kTileGrid; ++i)
        for (int j = 0; j < kTileGrid; ++j)
            place(hash_of["Terrain"], i * kTile, 0.0f, j * kTile);

    Rng64 rng(0xC0FFEEu);
    const int kTrees = 24, kGrass = 120;
    for (int n = 0; n < kTrees; ++n)
        place(hash_of["Tree"],  rng.range(0, kSpan), 1.0f, rng.range(0, kSpan));
    for (int n = 0; n < kGrass; ++n)
        place(hash_of["Grass"], rng.range(0, kSpan), 0.6f, rng.range(0, kSpan));

    return true;
}

std::vector<uint64_t>
LocalProvider::reconcile(const WorldManifest& manifest, const PartStore& store) {
    std::vector<uint64_t> want;
    std::map<uint64_t, bool> seen;
    for (const auto& e : manifest.instances) {
        if (seen.count(e.part_hash)) continue;
        seen[e.part_hash] = true;
        if (!store.has(e.part_hash)) want.push_back(e.part_hash);
    }
    return want;
}

bool LocalProvider::fetch_parts(const std::vector<uint64_t>& want,
                                PartStore& store, std::string& err) {
    // LocalProvider already wrote the .part blobs to the shared cache during
    // connect()'s install; "fetching" is just loading them into the store.
    for (uint64_t h : want) {
        if (!store.get_or_load(h)) { err = "load failed for part"; return false; }
    }
    return true;
}

bool LocalProvider::poll_deltas(WorldDelta&) { return false; }  // static world

} // namespace viewer
```

NOTE: confirm `ChildRequest` has a `.module` member (example_world iterates `roots` and reads `r.module`). Confirm `host.resolve_hash(src, "{}")` signature matches example_world (it does — line 121). Confirm `set_shared_lib_root` and `FileModuleResolver(host, schemas)` / `HostBaker(host, parts_dir)` signatures match part_graph.h (verified). The test's `store.loaded_count() == want.size()` holds only if every part has geometry; the example parts do.

- [ ] **Step 5: Run the test to verify it passes**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: PASS — cold-run checks then warm-run `warm connect bakes nothing`, `warm reconcile wants nothing`.

- [ ] **Step 6: Add `run-viewer-logic` to build-all.sh headless sweep**

In `build-all.sh`, add `viewer_logic_tests` to the MatterSurfaceLib-style loop? No — it's a MatterEngine3 test. Add it alongside the MatterEngine3 `run-*` targets (~line 146). Change:

```bash
    for tgt in run-partv2 run-script run-graph run-graph-integration run-trivar run-shlib run-comp run-dev run-example run-viewer-logic; do
```

- [ ] **Step 7: Verify the headless sweep includes the viewer logic suite**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: PASS (exit 0). (Full `./build-all.sh test` optional — slow.)

- [ ] **Step 8: Commit**

```bash
git add MatterEngine3/viewer/local_provider.h MatterEngine3/viewer/local_provider.cpp \
        MatterEngine3/tests/viewer_logic_tests.cpp build-all.sh
git commit -m "feat(viewer): LocalProvider with persistent cache and build-once reload"
```

---

### Task 5: WorldComposer (resolve → TLAS)

**Files:**
- Create: `MatterEngine3/viewer/world_composer.h`
- Create: `MatterEngine3/viewer/world_composer.cpp`
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp` (add a compose-count test — GL-free; TLAS build is CPU-side)

- [ ] **Step 1: Write the failing test**

Add to `viewer_logic_tests.cpp` and call from `main`. WorldComposer is GL-free for the record/build step (no shader binding). Assert active-instance count differs between resolvers:

```cpp
#include "../viewer/world_composer.h"

static void test_composer_counts() {
    const std::string cache = "/tmp/me3_viewer_cache_test";  // reuse Task 4's warm cache
    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir = "../examples/world_demo/schemas";
    cfg.world_data_dir = "../examples/world_demo/WorldData";
    cfg.world_name = "Demo";
    cfg.shared_lib_dir = "../shared-lib";
    cfg.cache_root = cache;

    viewer::LocalProvider prov(cfg);
    viewer::WorldManifest m; std::string err;
    if (!prov.connect(m, err)) { CHECK(false, "composer test: connect"); return; }
    viewer::PartStore store(cache);
    auto want = prov.reconcile(m, store);
    prov.fetch_parts(want, store, err);

    viewer::WorldState state; state.reset(m);

    viewer::WorldComposer composer(store, m.instances.size() + 16);
    auto lods = store.part_lod_table();

    viewer::PassThroughResolver pass;
    int active_all = composer.compose(state, pass, lods, make_float3(0,0,0));
    CHECK(active_all == (int)m.instances.size(), "passthrough composes every instance");

    // Far camera with a small activation radius -> fewer active instances.
    viewer::SectorLodResolver sec(16.0f, 32.0f);
    int active_far = composer.compose(state, sec, lods, make_float3(1000,1000,1000));
    CHECK(active_far < active_all, "sectorlod from far/small-radius composes fewer");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: FAIL — `world_composer.h` not found.

- [ ] **Step 3: Write `world_composer.h`**

Create `MatterEngine3/viewer/world_composer.h`:

```cpp
#ifndef VIEWER_WORLD_COMPOSER_H
#define VIEWER_WORLD_COMPOSER_H

#include "world_source.h"
#include "part_store.h"
#include "sector_resolver.h"
#include "tlas_manager.hpp"     // MSL TLASManager

namespace viewer {

// Per frame: resolve active instances, record each into the TLAS at its LOD's
// BLAS handle, and build. Returns the count of recorded instances.
class WorldComposer {
public:
    WorldComposer(PartStore& store, size_t tlas_capacity)
        : store_(store), tlas_(tlas_capacity) {}

    int compose(const WorldState& state,
                SectorResolver& resolver,
                const lod_select::PartLodTable& lods,
                const float3& cam_pos);

    TLASManager& tlas() { return tlas_; }

private:
    PartStore&  store_;
    TLASManager tlas_;
};

} // namespace viewer

#endif // VIEWER_WORLD_COMPOSER_H
```

- [ ] **Step 4: Write `world_composer.cpp`**

Create `MatterEngine3/viewer/world_composer.cpp` (mirrors the MSL compose pattern at `part_asset.cpp:198-219`):

```cpp
#include "world_composer.h"

#include <cstring>

namespace viewer {

int WorldComposer::compose(const WorldState& state,
                           SectorResolver& resolver,
                           const lod_select::PartLodTable& lods,
                           const float3& cam_pos) {
    auto resolved = resolver.resolve(state, lods, cam_pos);

    std::vector<DrawInstance> insts;
    insts.reserve(resolved.size());
    for (const auto& r : resolved) {
        const LoadedPart* lp = store_.get_or_load(r.part_hash);
        if (!lp || lp->lod_blas.empty()) continue;
        int lod = r.lod_level;
        if (lod < 0) lod = 0;
        if (lod >= (int)lp->lod_blas.size()) lod = (int)lp->lod_blas.size() - 1;

        DrawInstance di;
        di.blas_handle = lp->lod_blas[lod];
        di.material_id = 0;          // example parts carry their own materials in BLAS; default slot
        di.is_imposter = false;
        std::memcpy(di.transform.m, r.transform, sizeof(di.transform.m));
        insts.push_back(di);
    }

    tlas_.clear();
    tlas_.draw_batch(insts);
    tlas_.build(store_.blas());
    return (int)insts.size();
}

} // namespace viewer
```

NOTE: confirm `DrawInstance` field names (`blas_handle`, `transform.m`, `material_id`, `is_imposter`) and `draw_batch`/`build`/`clear` against `tlas_manager.hpp` (verified in prior reads). If `material_id` should come from the part rather than a fixed `0`, the BLAS entries already carry per-triangle materials in the MSL path; the instance-level `material_id` is a tint/override slot — `0` matches example_world's implicit default. Confirm `make_float3` is in scope for the test (same header as Task 2).

- [ ] **Step 5: Run the test to verify it passes**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: PASS — `passthrough composes every instance`, `sectorlod from far/small-radius composes fewer`.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/viewer/world_composer.h MatterEngine3/viewer/world_composer.cpp \
        MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat(viewer): WorldComposer records resolved instances into the TLAS"
```

---

### Task 6: Renderer (GL — manual verify)

**Files:**
- Create: `MatterEngine3/viewer/renderer.h`
- Create: `MatterEngine3/viewer/renderer.cpp`

This unit needs a GL context; it is verified by building and running `main` (Task 8), not by a headless test. Mirror MSL `main.cpp`'s render path exactly.

- [ ] **Step 1: Write `renderer.h`**

Create `MatterEngine3/viewer/renderer.h`:

```cpp
#ifndef VIEWER_RENDERER_H
#define VIEWER_RENDERER_H

#include "raylib.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <string>

namespace viewer {

// Owns the raytrace shader + camera and draws a fullscreen traced frame.
// Mirrors MatterSurfaceLib/main.cpp's render path.
class Renderer {
public:
    bool init(const std::string& shader_fs_path, std::string& err);   // after InitWindow
    void shutdown();

    Camera3D& camera() { return camera_; }
    void update_camera_free();                  // UpdateCamera(CAMERA_FREE)

    // Bind BLAS/TLAS + camera + material uniforms and draw the fullscreen pass.
    void draw(BLASManager& blas, TLASManager& tlas);

private:
    void upload_material_table();

    Shader   shader_{};
    Camera3D camera_{};
    int      loc_cam_pos_ = -1, loc_cam_target_ = -1, loc_cam_up_ = -1;
    int      loc_cam_fovy_ = -1, loc_screen_size_ = -1;
    int      loc_material_table_ = -1, loc_material_count_ = -1;
    bool     ready_ = false;
};

} // namespace viewer

#endif // VIEWER_RENDERER_H
```

- [ ] **Step 2: Write `renderer.cpp`**

Create `MatterEngine3/viewer/renderer.cpp` (uniform names and call order copied from MSL `main.cpp`):

```cpp
#include "renderer.h"

#include "material_registry.h"   // MaterialRegistryPackForGPU/Count, MATERIAL_FLOATS_PER_DEF

#include <cstdio>

namespace viewer {

bool Renderer::init(const std::string& shader_fs_path, std::string& err) {
    shader_ = LoadShader(nullptr, shader_fs_path.c_str());
    if (shader_.id == 0) { err = "failed to load shader: " + shader_fs_path; return false; }

    loc_cam_pos_       = GetShaderLocation(shader_, "cameraPos");
    loc_cam_target_    = GetShaderLocation(shader_, "cameraTarget");
    loc_cam_up_        = GetShaderLocation(shader_, "cameraUp");
    loc_cam_fovy_      = GetShaderLocation(shader_, "cameraFovy");
    loc_screen_size_   = GetShaderLocation(shader_, "screenSize");
    loc_material_table_ = GetShaderLocation(shader_, "materialTable");
    loc_material_count_ = GetShaderLocation(shader_, "materialCount");

    camera_.position   = (Vector3){ 12.0f, 10.0f, -12.0f };
    camera_.target     = (Vector3){ 12.0f, 1.0f, 12.0f };
    camera_.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera_.fovy       = 60.0f;
    camera_.projection = CAMERA_PERSPECTIVE;

    ready_ = true;
    return true;
}

void Renderer::shutdown() {
    if (ready_) UnloadShader(shader_);
    ready_ = false;
}

void Renderer::update_camera_free() { UpdateCamera(&camera_, CAMERA_FREE); }

void Renderer::upload_material_table() {
    float table[64 * MATERIAL_FLOATS_PER_DEF] = {0};
    MaterialRegistryPackForGPU(table);
    int count = MaterialRegistryCount();
    SetShaderValueV(shader_, loc_material_table_, table, SHADER_UNIFORM_FLOAT,
                    count * MATERIAL_FLOATS_PER_DEF);
    SetShaderValue(shader_, loc_material_count_, &count, SHADER_UNIFORM_INT);
}

void Renderer::draw(BLASManager& blas, TLASManager& tlas) {
    Vector3 cp = camera_.position, ct = camera_.target, cu = camera_.up;
    float fovy = camera_.fovy;
    float screen[2] = { (float)GetScreenWidth(), (float)GetScreenHeight() };

    SetShaderValue(shader_, loc_cam_pos_,    &cp,   SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_cam_target_, &ct,   SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_cam_up_,     &cu,   SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_cam_fovy_,   &fovy, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, loc_screen_size_, screen, SHADER_UNIFORM_VEC2);
    upload_material_table();

    blas.ensure_gpu_textures_ready();
    blas.bind_to_shader(shader_);
    tlas.bind_to_shader(shader_, blas);

    BeginShaderMode(shader_);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
    EndShaderMode();
}

} // namespace viewer
```

NOTE: confirm `LoadShader(nullptr, fs)` (vertex=nullptr uses raylib's default VS) matches how MSL loads the raytrace shader — MSL may pass an explicit vertex path. Match MSL `main.cpp`'s exact `LoadShader` call. Confirm `blas.bind_to_shader(Shader)` / `tlas.bind_to_shader(Shader, BLASManager&)` and `ensure_gpu_textures_ready()` names against the headers (verified). Confirm whether the fullscreen draw uses `DrawRectangle` (it does in MSL) and whether MSL flips Y / uses a render texture; replicate MSL precisely.

- [ ] **Step 3: Defer verification to Task 8**

This unit compiles only inside the GL build (Task 8). No commit yet OR commit the source now and verify at Task 8. Commit now so the unit is checkpointed:

```bash
git add MatterEngine3/viewer/renderer.h MatterEngine3/viewer/renderer.cpp
git commit -m "feat(viewer): raytrace Renderer mirroring the MSL render path"
```

---

### Task 7: Dear ImGui debug overlay (GL — manual verify)

**Files:**
- Create: `MatterEngine3/viewer/ui.h`
- Create: `MatterEngine3/viewer/ui.cpp`

- [ ] **Step 1: Write `ui.h`**

Create `MatterEngine3/viewer/ui.h`:

```cpp
#ifndef VIEWER_UI_H
#define VIEWER_UI_H

#include <cstdint>

namespace viewer {

// Read-only stats the HUD displays each frame; the resolver selector is the one
// field the panel writes back. Everything else is filled by main/composer/provider.
struct ViewerStats {
    float    fps = 0.0f;
    float    frame_ms = 0.0f;
    float    cam_pos[3] = {0,0,0};
    int      instances_total = 0;
    int      instances_active = 0;
    int      occupied_sectors = 0;
    bool     connected = false;
    int      parts_baked = 0;       // last connect: cache misses
    int      cache_hits = 0;        // last connect: cache hits
    int      last_want_count = 0;   // last reconcile want-list size
    // Writable: 0 = PassThrough, 1 = SectorLod. Panel sets this; main swaps resolver.
    int      resolver_choice = 0;
    bool     reload_requested = false;   // panel sets; main clears after handling
};

class Ui {
public:
    void setup();        // after InitWindow
    void shutdown();
    void begin_frame();
    void end_frame();
    void draw_debug_panel(ViewerStats& stats);
};

} // namespace viewer

#endif // VIEWER_UI_H
```

- [ ] **Step 2: Write `ui.cpp`**

Create `MatterEngine3/viewer/ui.cpp` (Dear ImGui GLFW + OpenGL3, copied from MSL's integration):

```cpp
#include "ui.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace viewer {

void Ui::setup() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(glfwGetCurrentContext(), true);
    ImGui_ImplOpenGL3_Init("#version 330");
}

void Ui::shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void Ui::begin_frame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Ui::end_frame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Ui::draw_debug_panel(ViewerStats& s) {
    ImGui::Begin("Viewer Debug");

    ImGui::Text("FPS: %.1f  (%.2f ms)", s.fps, s.frame_ms);
    ImGui::Text("Camera: %.1f, %.1f, %.1f", s.cam_pos[0], s.cam_pos[1], s.cam_pos[2]);
    ImGui::Separator();

    ImGui::Text("Instances: %d active / %d total", s.instances_active, s.instances_total);
    ImGui::Text("Occupied sectors: %d", s.occupied_sectors);
    ImGui::Separator();

    ImGui::Text("Provider: %s", s.connected ? "connected" : "disconnected");
    ImGui::Text("Last connect: %d baked, %d cache hits", s.parts_baked, s.cache_hits);
    ImGui::Text("Last reconcile want: %d", s.last_want_count);
    ImGui::Separator();

    const char* resolvers[] = { "PassThrough", "SectorLod" };
    ImGui::Combo("Resolver", &s.resolver_choice, resolvers, 2);
    if (ImGui::Button("Reload world")) s.reload_requested = true;

    ImGui::End();
}

} // namespace viewer
```

- [ ] **Step 3: Defer verification to Task 8; commit the source**

```bash
git add MatterEngine3/viewer/ui.h MatterEngine3/viewer/ui.cpp
git commit -m "feat(viewer): Dear ImGui debug HUD with resolver toggle and reload"
```

---

### Task 8: main wire-up + Makefile (GL build — manual verify)

**Files:**
- Create: `MatterEngine3/viewer/main.cpp`
- Create: `MatterEngine3/viewer/Makefile`

- [ ] **Step 1: Write `main.cpp`**

Create `MatterEngine3/viewer/main.cpp`:

```cpp
// MatterEngine3 world viewer: connect to a WorldProvider, reconcile a persistent
// part cache, compose with a selectable SectorResolver, raytrace, ImGui HUD.
#include "raylib.h"

#include "local_provider.h"
#include "part_store.h"
#include "world_composer.h"
#include "sector_resolver.h"
#include "renderer.h"
#include "ui.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace viewer;

int main() {
    const int W = 1280, H = 720;
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(W, H, "MatterEngine3 World Viewer");
    SetTargetFPS(60);

    Ui ui; ui.setup();

    Renderer renderer;
    std::string err;
    if (!renderer.init("shaders/raytrace_tlas_blas_processed.fs", err)) {
        printf("FATAL: %s\n", err.c_str());
        return 1;
    }

    LocalProviderConfig cfg;
    cfg.schemas_dir    = "../examples/world_demo/schemas";
    cfg.world_data_dir = "../examples/world_demo/WorldData";
    cfg.world_name     = "Demo";
    cfg.shared_lib_dir = "../shared-lib";
    cfg.cache_root     = "cache";   // persistent: viewer/cache/parts/<hash>.part

    auto provider = std::make_unique<LocalProvider>(cfg);

    // --- Connect sequence (reusable for the reload button). ---
    ViewerStats stats{};
    WorldManifest manifest;
    WorldState state;
    std::unique_ptr<PartStore> store;
    std::unique_ptr<WorldComposer> composer;
    lod_select::PartLodTable lods;

    auto connect_sequence = [&]() -> bool {
        if (!provider->connect(manifest, err)) { printf("connect: %s\n", err.c_str()); return false; }
        store = std::make_unique<PartStore>(cfg.cache_root);
        auto want = provider->reconcile(manifest, *store);
        if (!provider->fetch_parts(want, *store, err)) { printf("fetch: %s\n", err.c_str()); return false; }
        state.reset(manifest);
        composer = std::make_unique<WorldComposer>(*store, manifest.instances.size() + 16);
        lods = store->part_lod_table();
        stats.connected        = true;
        stats.parts_baked      = provider->baked_count();
        stats.cache_hits       = provider->hit_count();
        stats.last_want_count  = (int)want.size();
        stats.instances_total  = (int)manifest.instances.size();
        return true;
    };
    if (!connect_sequence()) return 1;

    PassThroughResolver pass;
    SectorLodResolver   sec(16.0f, 64.0f);
    bool camera_capture = false;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_TAB)) {
            camera_capture = !camera_capture;
            if (camera_capture) DisableCursor(); else EnableCursor();
        }
        if (camera_capture) renderer.update_camera_free();

        Vector3 cp = renderer.camera().position;
        float3 cam = make_float3(cp.x, cp.y, cp.z);

        SectorResolver& resolver =
            (stats.resolver_choice == 1) ? (SectorResolver&)sec : (SectorResolver&)pass;
        int active = composer->compose(state, resolver, lods, cam);

        stats.fps = (float)GetFPS();
        stats.frame_ms = GetFrameTime() * 1000.0f;
        stats.cam_pos[0] = cp.x; stats.cam_pos[1] = cp.y; stats.cam_pos[2] = cp.z;
        stats.instances_active = active;

        BeginDrawing();
            ClearBackground(BLACK);
            renderer.draw(store->blas(), composer->tlas());
            ui.begin_frame();
            ui.draw_debug_panel(stats);
            ui.end_frame();
        EndDrawing();

        WorldDelta d;
        if (provider->poll_deltas(d)) state.apply(d);

        if (stats.reload_requested) {
            stats.reload_requested = false;
            connect_sequence();
        }
    }

    ui.shutdown();
    renderer.shutdown();
    CloseWindow();
    return 0;
}
```

NOTE: `occupied_sectors` is left at 0 unless wired; optionally compute it via `sector_grid::bin_instances` for the HUD. Leave at 0 in the minimal viewer (the resolver owns binning internally). Confirm `make_float3` include matches Task 2.

- [ ] **Step 2: Write the Makefile**

Create `MatterEngine3/viewer/Makefile`. The viewer dir is the same depth as `tests/`, so every `../` / `../../` path from the `example_world` recipe resolves identically here. This is a complete, concrete Makefile (the pipeline list is copied verbatim from `EXAMPLE_CPP`, lines 270-281 of the tests Makefile, minus the `example_world.cpp` driver):

```makefile
# MatterEngine3 World Viewer (GL build). Reuses the example_world pipeline source
# set (SP-2 host + QuickJS + SP-4 compose + MSL backend) plus raylib GL and Dear
# ImGui. NOT part of the headless build-all test sweep (needs a GL context).

CC = g++
CFLAGS = -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable \
         -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33

RAYLIB_PATH = ../../Libraries/raylib/src
IMGUI_PATH  = ../../Libraries/imgui
QJS_DIR     = ../../Libraries/quickjs-ng
QJS_INC     = -I$(QJS_DIR)

INCLUDE_PATHS = -I../include -I../../MatterSurfaceLib/include -I$(RAYLIB_PATH) \
                -I../viewer -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends \
                -I$(RAYLIB_PATH)/external/glfw/include

LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
LDLIBS  = ../../Libraries/raylib/build/linux/libraylib.a

# Viewer units (GL-free logic + GL renderer/ui/main).
VIEWER_SRC = main.cpp renderer.cpp ui.cpp \
             world_state.cpp part_store.cpp resolvers.cpp \
             world_composer.cpp local_provider.cpp

# ME3 pipeline + MSL backend C++ sources (verbatim from tests/Makefile EXAMPLE_CPP,
# minus example_world.cpp). Same dir depth, so the ../ paths are unchanged.
PIPELINE_CPP = ../src/part_graph.cpp \
              ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_bindings.cpp \
              ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/script_rng_binding.cpp \
              ../src/part_asset_v2.cpp ../src/lod_bake.cpp ../src/world_flatten.cpp \
              ../src/sector_grid.cpp ../src/lod_select.cpp \
              ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
              ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
              ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
              ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
              ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
              ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
              ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp

# C sources (gcc-compiled to keep extern "C" symbols unmangled) + QuickJS C.
PIPELINE_C = ../../MatterSurfaceLib/src/surface.c ../../MatterSurfaceLib/src/open_particle_surface.c \
             ../../MatterSurfaceLib/src/spatial_hash.c ../../MatterSurfaceLib/src/object_allocator.c \
             ../../MatterSurfaceLib/src/material_registry.c
PIPELINE_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o material_registry.o
QJS_C   = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c $(QJS_DIR)/libunicode.c \
          $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c
QJS_OBJ = quickjs.o libregexp.o libunicode.o cutils.o xsum.o

IMGUI_SRC = $(IMGUI_PATH)/imgui.cpp $(IMGUI_PATH)/imgui_draw.cpp \
            $(IMGUI_PATH)/imgui_tables.cpp $(IMGUI_PATH)/imgui_widgets.cpp \
            $(IMGUI_PATH)/backends/imgui_impl_glfw.cpp \
            $(IMGUI_PATH)/backends/imgui_impl_opengl3.cpp

viewer: shaders $(VIEWER_SRC) $(PIPELINE_CPP) $(PIPELINE_C) $(QJS_C) $(IMGUI_SRC)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(PIPELINE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(VIEWER_SRC) $(PIPELINE_CPP) $(IMGUI_SRC) $(QJS_OBJ) $(PIPELINE_C_OBJ) \
	  -o viewer $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(PIPELINE_C_OBJ)

# Shader path resolves relative to the binary; symlink MSL's shaders/ dir.
shaders:
	@[ -e shaders ] || ln -s ../../MatterSurfaceLib/shaders shaders

clean:
	rm -f viewer shaders

.PHONY: viewer clean
```

NOTE: confirm `../examples/world_demo` exists relative to `viewer/` (it is `MatterEngine3/examples/world_demo`, a sibling of `viewer/`). The example_world driver runs from `tests/` with the same `../examples/...` paths, so this resolves. Confirm the imgui sources exist at `../../Libraries/imgui/{imgui,imgui_draw,imgui_tables,imgui_widgets}.cpp` and `backends/imgui_impl_{glfw,opengl3}.cpp` (the same paths MSL's Makefile uses — check `MatterSurfaceLib/Makefile` if unsure). If MSL also links `imgui_demo.cpp`, it is optional; omit unless bring-up needs it.

- [ ] **Step 3: Build the viewer**

Run: `make -C MatterEngine3/viewer viewer`
Expected: links to `MatterEngine3/viewer/viewer`, no errors. If unresolved symbols appear, diff `PIPELINE_CPP`/`PIPELINE_C`/`QJS_C` against the current `EXAMPLE_CPP`/`EXAMPLE_C`/`QJS_C` in `MatterEngine3/tests/Makefile` — the source set must stay in sync.

- [ ] **Step 4: Manual GL verification**

The harness reaps backgrounded GUI children, so ask the user to launch it. Tell the user to run, from the repo:

```
! cd MatterEngine3/viewer && ./viewer
```

Verify in the window: the world draws (terrain tiles + scattered trees/grass), free-fly camera works after pressing Tab, the ImGui "Viewer Debug" panel shows FPS/camera/instance counts, the Resolver combo switches PassThrough↔SectorLod (active-instance count changes), and "Reload world" re-runs connect with `0 baked` (warm cache).

- [ ] **Step 5: Verify warm-cache instant reload**

After a first run created `MatterEngine3/viewer/cache/parts/*.part`, relaunch. Confirm startup logs no rebake (HUD shows `0 baked`, want `0`) — the build-once / instant-reload behavior.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/viewer/main.cpp MatterEngine3/viewer/Makefile
git commit -m "feat(viewer): main loop wiring and GL build target"
```

---

## Self-Review Notes (for the implementer)

- **`.part` stores no LODs** (`script_host.cpp:807` bakes empty `LodLevels`). PartStore MUST regenerate via `lod_bake::bake_lods` (Task 3) — do not expect LOD levels from `load_v2`'s `lods_out`.
- **Shared BLASManager index offset:** when mapping `LodLevel.blas_indices[0]` to a global handle, add the entry count captured *before* the bake (`before + local`). This differs from `example_world.cpp`, which uses a fresh per-part BLASManager (`before == 0`).
- **Persistent vs throwaway cache:** unlike `example_world.cpp` (which `rm -rf`s `/tmp`), `LocalProvider` and `PartStore` MUST use a durable dir and never delete it, or the warm-reload test (Task 4) fails.
- **`make_float3`/`float3` include:** match `example_world.cpp`'s include set across all files that use them. Resolve this once in Task 2 and reuse.
- **Makefile source lists:** Tasks 1 and 8 both depend on copying the exact `example_world` pipeline source + QuickJS object lists from `MatterEngine3/tests/Makefile`. Read that recipe before writing either Makefile.
- **Type consistency:** `WorldManifestEntry.transform`, `ResolvedInstance.transform`, and `DrawInstance.transform.m` are all row-major `float[16]`; bridged by `memcpy`. `PartLod{bound_radius, thresholds}` and `PartLodTable = map<uint64_t,PartLod>` match `lod_select.h`.
