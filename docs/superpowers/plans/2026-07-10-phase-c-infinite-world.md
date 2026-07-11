# Phase C Infinite Voxel World Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the closed 51×51 Meadow world with an infinite, camera-streamed voxel world: a native SDF field graph declared by the world DSL, `WorldSector(tx,tz,rung)` parts baked on demand with transient artifacts, and a sector streamer that keeps a detail ladder resident around the camera.

**Architecture:** Four layers per the spec (`docs/superpowers/specs/2026-07-10-phase-c-infinite-world-design.md`): world-definition DSL (declares field graph + biome table, enumerates nothing) → native field runtime (`terrain_field`) → WorldSector parts (native `terrainVolume` surface-nets extraction + deterministic scatter) → sector streamer (desired-set diffing, rung swaps, eviction). Sector artifacts are transient (tmpfs scratch dir, deleted on evict); scatter assets stay disk-cached.

**Tech Stack:** C++17 (MatterEngine3 kernel), QuickJS-ng DSL, raylib explorer, existing Phase B/C machinery (demand bake, publish pipeline, PartStore/GpuCuller release, RefineController patterns).

## Global Constraints

- `GALLIUM_DRIVER=d3d12` for every GPU/viewer/sweep run (WSLg llvmpipe fallback FATALs the gate).
- MatterSurfaceLib is **read-only** (genuine-bug exception only, surfaced as a scope decision). Terrain extraction lives in MatterEngine3.
- ExplorerDemo consumes only `MatterEngine3/include/matter/` headers.
- One fresh JSRuntime per bake is retained (hermetic bakes). Amortization comes from the fold cache only.
- World shape: infinite XZ, bounded Y **−64…+192**; sector size **16.0** units; rung voxel sizes **2.0 / 1.0 / 0.5 / 0.25** (rung 0–3); streamer rings **rung 3 ≤ 48, rung 2 ≤ 120, rung 1 ≤ 300, rung 0 ≤ 800**, hysteresis **16.0**.
- Scripted viewer/explorer runs must self-terminate (no windows left open).
- Use "part" terminology, never "brick".
- After any engine/header change that the Windows viewer links: clean-rebuild Windows objects and run `make windows`.
- Test scope per task: only genuinely-covering suites; full sweep is the final gate only.
- Old-world parity is NOT required: the new field runtime does not need to reproduce `terrain_noise.js` heights; the Meadow 51×51 world retires.

## File Structure

| File | Responsibility |
|---|---|
| `MatterEngine3/src/script_host.h/.cpp` (modify) | Fold cache; `eval_world`; `BakeOptions.world` (WorldBinding) plumb-through |
| `MatterEngine3/src/terrain_field.h/.cpp` (create) | Field program parse/eval, noise primitives, biome/material queries, canonical hash |
| `MatterEngine3/src/terrain_mesher.h/.cpp` (create) | Surface-nets extraction over a sampled slab + skirts, triangle emission |
| `MatterEngine3/src/world_base.js.h` (create) | Embedded JS: `World` base class + field-graph builder DSL (`noise2`, `blend`, …) |
| `MatterEngine3/src/dsl_state.h`, `src/dsl_bindings.cpp` (modify) | `terrainVolume` verb; `heightAt/slopeAt/biomeAt/moistureAt` query verbs; `field` pointer |
| `MatterEngine3/src/sector_streamer.h/.cpp` (create) | Desired-set/diff/hysteresis/eviction logic (pure CPU) |
| `MatterEngine3/src/part_graph.h/.cpp` (modify) | HostBaker transient routing (scratch parts_dir per module) |
| `MatterEngine3/src/provider/local_provider.h/.cpp` (modify) | Transient module set + scratch dir; sector request path; world-kind manifest |
| `MatterEngine3/src/render/part_store.h/.cpp` (modify) | Scratch-dir lookup for transient artifacts |
| `MatterEngine3/src/matter_engine.cpp` (modify) | World-kind load path, streamer wiring, water-level accessor, regenerate |
| `MatterEngine3/include/matter/world_session.h`, `include/matter/events.h` (modify) | `sea_level()`, `FrameStats.resident_sectors`, event reuse |
| `MatterEngine3/shared-lib/scatter_grid.js` (create) | Deterministic jittered-grid spaced-scatter candidates |
| `MatterEngine3/examples/world_demo/schemas/WorldSector.js` (create) | Sector part: terrainVolume + scatter |
| `MatterEngine3/examples/world_demo/schemas/MeadowWorld.js` (create) | World definition: field graph + biome table |
| `MatterEngine3/examples/world_demo/WorldData/MeadowWorld/world.manifest` (create) | `MeadowWorld world` |
| `ExplorerDemo/main.cpp`, `ExplorerDemo/hud.*` (modify) | Water plane, world name, HUD resident count, flight smoke |
| `MatterEngine3/tests/*` (create/modify) | New suites: fold_cache, transient, terrain_field, terrain_mesher, sector_bake, sector_streamer, world_stream (gpu); retire closed-world count suites |

Verification note for implementers: the dossier of exact signatures used below was compiled on 2026-07-10; if a signature disagrees with the code, the code wins — adapt and note it in your report.

---

### Task 1: Fold cache in ScriptHost

**Files:**
- Modify: `MatterEngine3/src/script_host.h` (class members, ~line 47–132)
- Modify: `MatterEngine3/src/script_host.cpp` (fold call sites: `bake_source` ~966, `merge_params_canonical` ~319, `eval_requires` ~407, `eval_lod_budgets` ~548)
- Create: `MatterEngine3/tests/fold_cache_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: `module_resolver::fold_sources(part_source, shared_lib_root, FoldResult&, err)` (`module_resolver.h:30–51`).
- Produces: `bool ScriptHost::fold_sources_cached(const std::string& source, module_resolver::FoldResult& out, std::string& err)`; counters `uint64_t fold_cache_hits() const` / `fold_cache_misses() const`; `void clear_fold_cache()`. Later tasks (eval_world) reuse `fold_sources_cached`.

**Ownership check (do this first):** confirm the `ScriptHost` instance used by `HostBaker` is long-lived across bakes (one per provider/worker), not constructed per bake. If it is per-bake, hoist it to a member of its owner so the cache survives between bakes, and say so in your report.

- [ ] **Step 1: Write the failing test**

```cpp
// MatterEngine3/tests/fold_cache_tests.cpp
#include "check.h"
#include "../src/script_host.h"
#include <string>

static const char* kTinyPart = R"JS(
import { rng } from 'shared-lib/rng';
class Tiny extends Part {
  static params = { seed: 1 };
  build(p) { const r = rng(p.seed); this.pushMatrix(); this.popMatrix(); }
}
)JS";

int main() {
    ScriptHost host;
    host.set_shared_lib_root("../shared-lib");

    CHECK(host.fold_cache_misses() == 0 && host.fold_cache_hits() == 0,
          "counters start at zero");

    uint64_t h1 = host.resolve_hash(kTinyPart, "{}");
    CHECK(host.fold_cache_misses() == 1, "first fold is a miss");

    uint64_t h2 = host.resolve_hash(kTinyPart, "{}");
    CHECK(h1 == h2, "hash stable across cached fold");
    CHECK(host.fold_cache_hits() >= 1, "second fold hits the cache");
    CHECK(host.fold_cache_misses() == 1, "no second miss for same source");

    std::string other = std::string(kTinyPart) + "\n// different\n";
    host.resolve_hash(other, "{}");
    CHECK(host.fold_cache_misses() == 2, "different source is a new miss");

    host.clear_fold_cache();
    host.resolve_hash(kTinyPart, "{}");
    CHECK(host.fold_cache_misses() == 3, "clear_fold_cache invalidates");

    return check_summary();
}
```

- [ ] **Step 2: Add the Makefile target** (follow the `MEADOW_TARGET` pattern at `tests/Makefile:408–412`: define `FOLDCACHE_TARGET = fold_cache_tests`, `FOLDCACHE_CPP = fold_cache_tests.cpp` plus the same pipeline sources the script-host suites link, add to the `sh` flavor's `_CPP_SRCS` sort list, add a link rule and a `run-foldcache` target). Run: `make -C MatterEngine3/tests run-foldcache` — Expected: FAIL (methods don't exist / compile error).

- [ ] **Step 3: Implement the cache**

In `script_host.h` (private members + public accessors):

```cpp
#include "module_resolver.h"
#include <mutex>
#include <unordered_map>

public:
    bool fold_sources_cached(const std::string& source,
                             module_resolver::FoldResult& out,
                             std::string& err);
    uint64_t fold_cache_hits() const   { return fold_hits_; }
    uint64_t fold_cache_misses() const { return fold_misses_; }
    void clear_fold_cache();

private:
    std::mutex fold_mu_;
    std::unordered_map<uint64_t, module_resolver::FoldResult> fold_cache_;
    uint64_t fold_hits_ = 0, fold_misses_ = 0;
```

In `script_host.cpp`:

```cpp
static uint64_t fold_key_fnv1a64(const std::string& a, const std::string& b) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::string& s) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        h ^= 0xff; h *= 1099511628211ull;   // separator
    };
    mix(a); mix(b);
    return h;
}

bool ScriptHost::fold_sources_cached(const std::string& source,
                                     module_resolver::FoldResult& out,
                                     std::string& err) {
    if (shared_lib_root_.empty()) { out = module_resolver::FoldResult{}; return true; }
    const uint64_t key = fold_key_fnv1a64(source, shared_lib_root_);
    {
        std::lock_guard<std::mutex> lk(fold_mu_);
        auto it = fold_cache_.find(key);
        if (it != fold_cache_.end()) { ++fold_hits_; out = it->second; return true; }
    }
    module_resolver::FoldResult fresh;
    if (!module_resolver::fold_sources(source, shared_lib_root_, fresh, err))
        return false;
    std::lock_guard<std::mutex> lk(fold_mu_);
    ++fold_misses_;
    out = fold_cache_.emplace(key, std::move(fresh)).first->second;
    return true;
}

void ScriptHost::clear_fold_cache() {
    std::lock_guard<std::mutex> lk(fold_mu_);
    fold_cache_.clear();
}
```

Replace all four fold call sites with the cached helper, preserving each site's error handling, e.g. in `bake_source` (~line 966):

```cpp
module_resolver::FoldResult fold;
{
    std::string ferr;
    if (!fold_sources_cached(source, fold, ferr)) {
        r.error.ok = false;
        r.error.message = "module resolution failed: " + ferr;
        return r;
    }
}
```

Also call `clear_fold_cache()` inside `set_shared_lib_root` (root change invalidates), and find the engine `reload()` path (live-edit re-reads schemas/shared-lib from disk) — it must call `clear_fold_cache()` on the provider's host so edited shared-lib files are re-read.

- [ ] **Step 4: Run tests**

Run: `make -C MatterEngine3/tests run-foldcache` — Expected: ALL PASS.
Covering regressions: `make -C MatterEngine3/tests run-script run-partv2 run-graph` — Expected: pass (hash parity: fold content unchanged, only re-reads eliminated).

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/script_host.h MatterEngine3/src/script_host.cpp MatterEngine3/tests/fold_cache_tests.cpp MatterEngine3/tests/Makefile
git commit -m "perf(script-host): cache fold_sources by (source, shared-lib root) — kills per-bake shared-lib disk re-reads"
```

---

### Task 2: Transient artifact routing (tmpfs scratch dir)

**Files:**
- Modify: `MatterEngine3/src/part_graph.h` (HostBaker), `MatterEngine3/src/part_graph.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.h/.cpp`
- Modify: `MatterEngine3/src/render/part_store.h/.cpp`
- Create: `MatterEngine3/tests/transient_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: `HostBaker::cached(uint64_t)` / `HostBaker::bake(...)` (`part_graph.h:214` area); `BakeOptions.parts_dir` (`script_host.h:10–14`); `PartStore::release(uint64_t)` (`part_store.h:100`); `save_v2`/`load_v2` (`part_asset_v2.h:84–88`).
- Produces:
  - `HostBaker::set_transient(const std::set<std::string>* modules, std::string scratch_dir)` — bakes of listed modules write artifacts under `scratch_dir` instead of the parts cache; `cached()` checks both locations (scratch first).
  - `LocalProvider::set_transient_modules(std::set<std::string> modules)` — creates the scratch dir (`/tmp/matter_transient/<pid>/`), forwards to its baker, forwards the dir to the PartStore.
  - `LocalProvider::release_transient(uint64_t hash)` — unlinks the scratch artifact file (both `.part` and `.flat.part` if present). Safe no-op if absent.
  - `PartStore::set_scratch_dir(std::string dir)` — `get_or_load` probes scratch first, then the cache dir.

Implementation notes:
- The artifact filename scheme must match `cache_path_resolved(hash)`'s basename so lookups are a straight dirname swap; reuse the same path-building helper with a different root.
- Scratch dir is per-process (`/tmp/matter_transient/` + pid) and created with `mkdir -p` semantics at `set_transient_modules` time; stale dirs from dead processes are not this task's problem.
- `ensure_part_flattened` writes `.flat.part` next to the artifact — for transient modules that lands in scratch too (same dirname swap in whatever helper builds the flat path).

- [ ] **Step 1: Write the failing test**

```cpp
// MatterEngine3/tests/transient_tests.cpp
#include "check.h"
#include <cstdio>
#include <string>
#include <sys/stat.h>
// include the provider/graph headers the demand_bake_tests suite uses
#include "../src/provider/local_provider.h"

static bool file_exists(const std::string& p) {
    struct stat st{}; return ::stat(p.c_str(), &st) == 0;
}

int main() {
    // Arrange a provider exactly the way demand_bake_tests.cpp does
    // (same schemas dir, same cache sandbox under /tmp), then:
    //   provider.set_transient_modules({"Rock"});
    //
    // 1. install/bake a Rock variant (any params).
    // 2. CHECK(artifact file exists under the scratch dir, not under cache/parts).
    // 3. CHECK(PartStore::get_or_load(hash) succeeds with scratch lookup).
    // 4. provider.release_transient(hash);
    //    CHECK(scratch file gone).
    // 5. bake a NON-transient module (e.g. Pebble); CHECK its artifact IS in
    //    cache/parts (disk caching unaffected for normal modules).
    //
    // Mirror the fixture code from demand_bake_tests.cpp verbatim for setup —
    // it already constructs LocalProvider + module resolver against
    // examples/world_demo/schemas with a /tmp cache sandbox.
    return check_summary();
}
```

(The fixture body is deliberately copied from `demand_bake_tests.cpp` — same provider construction, same sandbox reset; the new assertions are the five numbered checks above, written as real code against that fixture.)

- [ ] **Step 2: Add `run-transient` Makefile target** (same pattern as Task 1, linking the provider/graph sources that `demand_bake_tests` links). Run: `make -C MatterEngine3/tests run-transient` — Expected: FAIL (APIs don't exist).

- [ ] **Step 3: Implement**

`part_graph.h` — HostBaker gains:

```cpp
    void set_transient(const std::set<std::string>* modules, std::string scratch_dir) {
        transient_modules_ = modules;
        transient_dir_ = std::move(scratch_dir);
    }
private:
    const std::set<std::string>* transient_modules_ = nullptr;
    std::string transient_dir_;
    bool is_transient(const std::string& module) const {
        return transient_modules_ && transient_modules_->count(module) != 0;
    }
```

`part_graph.cpp`:
- In `HostBaker::bake(...)` (which receives the module name via `BakeInputs`): when `is_transient(inputs.module)`, set `opts.parts_dir = transient_dir_` before calling `ScriptHost::bake_source`.
- In `HostBaker::cached(hash)`: probe the scratch path first (`transient_dir_` non-empty), then the normal `cache_path_resolved(hash)`. A hit in either returns true.

`local_provider.h/.cpp`:

```cpp
    void set_transient_modules(std::set<std::string> modules);   // mkdir scratch, wire baker + store
    void release_transient(uint64_t hash);                       // unlink scratch .part/.flat.part
    const std::string& transient_dir() const { return transient_dir_; }
```

`part_store.h/.cpp`:

```cpp
    void set_scratch_dir(std::string dir) { scratch_dir_ = std::move(dir); }
    // in get_or_load(hash): if scratch_dir_ set and scratch path exists, load from it;
    // else fall through to the existing cache path.
```

- [ ] **Step 4: Run tests**

Run: `make -C MatterEngine3/tests run-transient` — Expected: ALL PASS.
Covering regressions: `make -C MatterEngine3/tests run-graph run-dev` (demand-bake + provider suites) — Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/part_graph.h MatterEngine3/src/part_graph.cpp MatterEngine3/src/provider/local_provider.h MatterEngine3/src/provider/local_provider.cpp MatterEngine3/src/render/part_store.h MatterEngine3/src/render/part_store.cpp MatterEngine3/tests/transient_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat(provider): transient artifact routing — per-module tmpfs scratch dir, release_transient unlink, scratch-aware PartStore"
```

---

### Task 3: `terrain_field` — native field program, noise, biome/material queries

**Files:**
- Create: `MatterEngine3/src/terrain_field.h`
- Create: `MatterEngine3/src/terrain_field.cpp`
- Create: `MatterEngine3/tests/terrain_field_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`, `MatterEngine3/Makefile` (add terrain_field.cpp to the lib sources)

**Interfaces:**
- Consumes: nothing engine-side (pure module; no JS, no GL).
- Produces (used by Tasks 4, 5, 7, 9):

```cpp
namespace terrain_field {

// Canonical text program, one op per line. Args are either `r<N>` register
// refs or float literals. Registers are the op indices (op i defines r<i>).
// Directive lines (order-independent, after ops):
//   height r<N>       moisture r<N>     relief r<N>
//   seaLevel <f>      biome <mountReliefThresh> <rockyMoistThresh>
struct FieldProgram {
    static bool parse(const std::string& text, FieldProgram& out, std::string& err);
    uint64_t hash() const;              // fnv1a64 over the canonical text
    const std::string& text() const;
};

class FieldRuntime {
public:
    explicit FieldRuntime(FieldProgram p);
    float height_at(float x, float z) const;
    float density_at(float x, float y, float z) const;   // v1: height_at(x,z) - y
    float slope_at(float x, float z) const;               // |grad h|, central diff eps=0.5
    float moisture_at(float x, float z) const;            // 0..1
    float relief_at(float x, float z) const;              // 0..1
    enum Biome { Ocean = 0, Meadow = 1, Foothills = 2, Mountains = 3 };
    Biome biome_at(float x, float z) const;
    enum Material { MatGrass = 0, MatDirt = 1, MatRock = 2, MatSnow = 3 };
    Material material_at(float x, float z) const;         // slope/height/biome rules
    float sea_level() const;
    uint64_t hash() const;                                 // == program hash
};

} // namespace terrain_field
```

**Op set (v1):** `const v` · `noise2 seed freq oct gain lac` · `ridge2 seed freq oct gain lac` · `warp2 rSrc seed freq strength` · `add a b` · `mul a b` · `min a b` · `max a b` · `clamp a lo hi` · `blend a b t` · `smoothstep e0 e1 x` (e0/e1 float literals, x reg or literal).

- [ ] **Step 1: Write the failing tests**

```cpp
// MatterEngine3/tests/terrain_field_tests.cpp
#include "check.h"
#include "../src/terrain_field.h"
#include <cmath>
#include <string>

using namespace terrain_field;

static FieldRuntime make(const std::string& text) {
    FieldProgram p; std::string err;
    if (!FieldProgram::parse(text, p, err)) { printf("parse err: %s\n", err.c_str()); }
    return FieldRuntime(std::move(p));
}

int main() {
    // --- constant program: height 5 everywhere -----------------------------
    {
        FieldRuntime f = make(
            "const 5\nconst 0.5\nconst 0.5\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(std::fabs(f.height_at(3, 7) - 5.0f) < 1e-5f, "const height");
        CHECK(std::fabs(f.density_at(3, 5, 7)) < 1e-5f, "density zero at surface");
        CHECK(f.density_at(3, 0, 7) > 0 && f.density_at(3, 10, 7) < 0,
              "density sign: solid below, air above");
        CHECK(f.slope_at(3, 7) < 1e-4f, "flat slope");
        CHECK(f.biome_at(3, 7) == FieldRuntime::Meadow, "mid moisture/relief = meadow");
    }
    // --- biome classification via crafted control fields --------------------
    {
        FieldRuntime f = make("const 5\nconst 0.5\nconst 0.9\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(f.biome_at(0, 0) == FieldRuntime::Mountains, "high relief = mountains");
    }
    {
        FieldRuntime f = make("const 5\nconst 0.1\nconst 0.2\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(f.biome_at(0, 0) == FieldRuntime::Foothills, "low moisture = foothills");
    }
    {
        FieldRuntime f = make("const -3\nconst 0.5\nconst 0.2\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(f.biome_at(0, 0) == FieldRuntime::Ocean, "below sea level = ocean");
    }
    // --- noise: deterministic, seed-sensitive, continuous, bounded ---------
    {
        const char* prog =
            "noise2 1234 0.01 4 0.5 2.0\nconst 0.5\nconst 0.5\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n";
        FieldRuntime a = make(prog), b = make(prog);
        CHECK(a.height_at(10, 20) == b.height_at(10, 20), "noise deterministic");
        FieldRuntime c = make(
            "noise2 9999 0.01 4 0.5 2.0\nconst 0.5\nconst 0.5\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(a.height_at(10, 20) != c.height_at(10, 20), "seed changes noise");
        float h0 = a.height_at(10, 20), h1 = a.height_at(10.01f, 20);
        CHECK(std::fabs(h1 - h0) < 0.05f, "noise continuous");
        bool bounded = true;
        for (int i = 0; i < 1000; ++i) {
            float v = a.height_at(i * 3.7f, i * -1.9f);
            if (v < -1.5f || v > 1.5f) bounded = false;
        }
        CHECK(bounded, "fbm output roughly in [-1,1]");
    }
    // --- hash: stable for same text, differs for different text ------------
    {
        FieldProgram p1, p2, p3; std::string err;
        FieldProgram::parse("const 1\nheight r0\nconst 0.5\nmoisture r2\nconst 0.5\nrelief r4\nseaLevel 0\nbiome 0.65 0.35\n", p1, err);
        FieldProgram::parse("const 1\nheight r0\nconst 0.5\nmoisture r2\nconst 0.5\nrelief r4\nseaLevel 0\nbiome 0.65 0.35\n", p2, err);
        FieldProgram::parse("const 2\nheight r0\nconst 0.5\nmoisture r2\nconst 0.5\nrelief r4\nseaLevel 0\nbiome 0.65 0.35\n", p3, err);
        CHECK(p1.hash() == p2.hash(), "hash stable");
        CHECK(p1.hash() != p3.hash(), "hash differs on text change");
    }
    // --- materials ----------------------------------------------------------
    {
        // steep ridge program: mul big amplitude → slope high somewhere
        FieldRuntime f = make(
            "ridge2 7 0.02 4 0.5 2.0\nconst 110\nmul r0 r1\nconst 0.5\nconst 0.9\n"
            "height r2\nmoisture r3\nrelief r4\nseaLevel 0\nbiome 0.65 0.35\n");
        // find a steep sample; material must be rock there
        bool found_rock = false;
        for (int i = 0; i < 4000 && !found_rock; ++i) {
            float x = i * 1.3f, z = i * -2.1f;
            if (f.slope_at(x, z) > 1.2f)
                found_rock = f.material_at(x, z) == FieldRuntime::MatRock;
        }
        CHECK(found_rock, "steep slope classifies as rock");
    }
    // --- parse errors fail loudly ------------------------------------------
    {
        FieldProgram p; std::string err;
        CHECK(!FieldProgram::parse("bogusop 1 2\nheight r0\n", p, err), "unknown op rejected");
        CHECK(!err.empty(), "error message set");
        CHECK(!FieldProgram::parse("const 1\n", p, err), "missing height directive rejected");
    }
    return check_summary();
}
```

- [ ] **Step 2: Add `run-terrainfield` target** (pure-CPU suite: links only `terrain_field.cpp` + the test). Run: `make -C MatterEngine3/tests run-terrainfield` — Expected: FAIL (files missing).

- [ ] **Step 3: Implement `terrain_field.h/.cpp`**

Noise core (fresh definitions — no `terrain_noise.js` parity required):

```cpp
namespace {
inline uint32_t hash2i(int32_t ix, int32_t iz, uint32_t seed) {
    uint32_t h = (uint32_t)ix * 374761393u + (uint32_t)iz * 668265263u
               + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float rand01(int32_t ix, int32_t iz, uint32_t seed) {
    return (float)(hash2i(ix, iz, seed) & 0xffffff) / (float)0x1000000;
}
inline float smooth5(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }

float value_noise(float x, float z, uint32_t seed) {
    int32_t ix = (int32_t)std::floor(x), iz = (int32_t)std::floor(z);
    float fx = x - ix, fz = z - iz;
    float a = rand01(ix, iz, seed),     b = rand01(ix + 1, iz, seed);
    float c = rand01(ix, iz + 1, seed), d = rand01(ix + 1, iz + 1, seed);
    float u = smooth5(fx), v = smooth5(fz);
    return (a + (b - a) * u) * (1 - v) + (c + (d - c) * u) * v;   // 0..1
}

float fbm2(float x, float z, uint32_t seed, int oct, float gain, float lac,
           float freq, bool ridged) {
    float amp = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int i = 0; i < oct; ++i) {
        float n = value_noise(x * freq, z * freq, seed + (uint32_t)i * 131u);
        n = n * 2.0f - 1.0f;                       // -1..1
        if (ridged) n = 1.0f - std::fabs(n) * 2.0f; // ridge: peaks at lattice
        sum += n * amp; norm += amp;
        amp *= gain; freq *= lac;
    }
    return sum / norm;                              // ~-1..1
}
} // namespace
```

Program storage and evaluator:

```cpp
struct Op {
    enum Kind { Const, Noise2, Ridge2, Warp2, Add, Mul, Min, Max, Clamp,
                Blend, Smoothstep } kind;
    int a = -1, b = -1, c = -1;      // register operands (or -1)
    float f0 = 0, f1 = 0, f2 = 0, f3 = 0;  // literals: value/freq/gain/lac/edges
    uint32_t seed = 0; int oct = 0;
};

float FieldRuntime::eval_reg(int target, float x, float z) const;
// Implementation: fixed-size stack buffer float regs[kMaxOps=64]; walk ops
// 0..target in order (programs are tiny DAGs; evaluating all ops up to the
// highest needed register is fine at these sizes). Warp2 evaluates its source
// register at displaced coordinates:
//   dx = value_noise(x*freq, z*freq, seed)      * 2-1) * strength
//   dz = value_noise(x*freq, z*freq, seed^0x9e37) * 2-1) * strength
//   value = eval of rSrc at (x+dx, z+dz)  — implemented by re-running the
//   prefix 0..rSrc with shifted coords into a scratch reg file.
```

Parsing: split lines, tokenize on spaces; `r<N>` → register operand, else float literal materialized as an implicit `Const` op appended before the referencing op (registers renumber accordingly — simplest is a two-pass: pass 1 collects lines, pass 2 emits). Validate: register refs must be < current op index (no forward refs), all of `height/moisture/relief/seaLevel/biome` directives present, register count ≤ 64. On any violation: `err` set, return false.

Biome/material rules:

```cpp
FieldRuntime::Biome FieldRuntime::biome_at(float x, float z) const {
    if (height_at(x, z) < sea_level_) return Ocean;
    if (relief_at(x, z) >= mount_relief_thresh_) return Mountains;
    if (moisture_at(x, z) < rocky_moist_thresh_) return Foothills;
    return Meadow;
}
FieldRuntime::Material FieldRuntime::material_at(float x, float z) const {
    if (slope_at(x, z) > 1.0f) return MatRock;
    Biome b = biome_at(x, z);
    if (b == Mountains) return height_at(x, z) > 100.0f ? MatSnow : MatRock;
    if (b == Foothills) return MatDirt;
    if (b == Ocean)     return MatDirt;   // sea floor
    return MatGrass;
}
```

- [ ] **Step 4: Run tests**

Run: `make -C MatterEngine3/tests run-terrainfield` — Expected: ALL PASS.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/terrain_field.h MatterEngine3/src/terrain_field.cpp MatterEngine3/tests/terrain_field_tests.cpp MatterEngine3/tests/Makefile MatterEngine3/Makefile
git commit -m "feat(terrain): native field program — parse/eval/hash, fbm+ridge+warp noise, biome and material queries"
```

---

### Task 4: World entry point — builder DSL + `eval_world` + manifest kind

**Files:**
- Create: `MatterEngine3/src/world_base.js.h` (embedded JS, same convention as `part_base.js.h`)
- Modify: `MatterEngine3/src/script_host.h/.cpp` (add `eval_world`)
- Modify: `MatterEngine3/src/provider/local_provider.h/.cpp` (manifest kind `world`)
- Create: `MatterEngine3/tests/eval_world_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: `fold_sources_cached` (Task 1); `FieldProgram::parse` (Task 3); the existing manifest parser that reads `<Module> <kind>` lines (kinds today: `expand`, `tileset`).
- Produces:

```cpp
// script_host.h
struct WorldEvalResult {
    bool ok = false;
    std::string message;        // error when !ok
    std::string field_program;  // canonical text for terrain_field::FieldProgram::parse
    std::string biomes_json;    // canonical JSON of the biomes() table (JSON.stringify)
    float sector_size = 16.0f;
    float y_min = -64.0f, y_max = 192.0f;
};
WorldEvalResult ScriptHost::eval_world(const std::string& source,
                                       const std::string& params_json);
```

- Manifest: a line `MeadowWorld world` marks the world-kind root; `LocalProvider` exposes `const std::string& world_module() const` (empty if the manifest has no world-kind entry). Task 9 consumes it.

**Design notes (binding contract for the implementer):**
- `eval_world` mirrors `eval_requires`' structure (`script_host.cpp:407`): fold → fresh runtime/context → eval `kWorldBaseJS` + folded sources + world class → call `field(mergedParams)` → read the accumulated op lines → call `biomes()` → `JS_JSONStringify` → teardown.
- The builder JS accumulates program lines in a module-global `__world_ops` array; `FieldNode` wraps a register index. `field()` returns `{ density, moisture, relief, seaLevel }` where density/moisture/relief are FieldNodes; the host appends the directive lines (`height r<N>` etc. — in v1 `heightToDensity` is the identity marker, so `density.r` IS the height register).
- Threshold directives come from optional statics: `static biomeThresholds = { mountRelief: 0.65, rockyMoisture: 0.35 }` (defaults if absent).
- World constants from `static world = { sectorSize: 16, yMin: -64, yMax: 192 }` (defaults if absent).

- [ ] **Step 1: Write the failing test**

```cpp
// MatterEngine3/tests/eval_world_tests.cpp
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include <string>

static const char* kWorld = R"JS(
class TestWorld extends World {
  static params = { worldSeed: 42 };
  static world  = { sectorSize: 16, yMin: -64, yMax: 192 };
  field(p) {
    const relief   = noise2(p.worldSeed ^ 1, 1/900, 3);
    const plains   = noise2(p.worldSeed ^ 3, 1/160, 4).mul(8);
    const mounts   = ridge2(p.worldSeed ^ 4, 1/340, 5).mul(110);
    const height   = blend(plains, mounts, relief.smoothstep(0.45, 0.75)).add(-6);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 3);
    return { density: heightToDensity(height), moisture, relief, seaLevel: 0.0 };
  }
  biomes() {
    return { meadow: { grass: 156, pebbles: 16, rocks: 2, trees: true },
             foothills: { grass: 39, rocks: 2 },
             mountains: { rocks: 1 }, ocean: {} };
  }
}
)JS";

int main() {
    ScriptHost host;
    WorldEvalResult r = host.eval_world(kWorld, "{}");
    CHECK(r.ok, r.message.c_str());
    CHECK(!r.field_program.empty(), "program emitted");
    CHECK(r.biomes_json.find("meadow") != std::string::npos, "biomes json present");
    CHECK(r.sector_size == 16.0f && r.y_min == -64.0f && r.y_max == 192.0f,
          "world constants read");

    terrain_field::FieldProgram prog; std::string err;
    CHECK(terrain_field::FieldProgram::parse(r.field_program, prog, err),
          err.c_str());
    terrain_field::FieldRuntime f(std::move(prog));
    float h = f.height_at(100, 100);
    CHECK(h > -130.0f && h < 130.0f, "height in plausible range");

    // Determinism + seed sensitivity
    WorldEvalResult r2 = host.eval_world(kWorld, "{}");
    CHECK(r2.field_program == r.field_program, "program deterministic");
    WorldEvalResult r3 = host.eval_world(kWorld, "{\"worldSeed\":7}");
    CHECK(r3.field_program != r.field_program, "seed changes program");

    // Error path: field() throwing must fail loudly
    WorldEvalResult bad = host.eval_world(
        "class B extends World { field(p) { throw new Error('boom'); } }", "{}");
    CHECK(!bad.ok && bad.message.find("boom") != std::string::npos,
          "field() error surfaces");
    return check_summary();
}
```

- [ ] **Step 2: Add `run-evalworld` target** (links script-host pipeline + terrain_field). Run — Expected: FAIL.

- [ ] **Step 3: Implement `world_base.js.h`** (complete embedded JS):

```js
// embedded as kWorldBaseJS, evaluated before the world-def source
let __world_ops = [];
function __emit(line) { __world_ops.push(line); return __world_ops.length - 1; }
function __reg(v) {                      // FieldNode or number -> register index
  if (v instanceof FieldNode) return v.r;
  return __emit('const ' + (+v));
}
class FieldNode {
  constructor(r) { this.r = r; }
  add(o)  { return new FieldNode(__emit('add r' + this.r + ' r' + __reg(o))); }
  mul(o)  { return new FieldNode(__emit('mul r' + this.r + ' r' + __reg(o))); }
  min(o)  { return new FieldNode(__emit('min r' + this.r + ' r' + __reg(o))); }
  max(o)  { return new FieldNode(__emit('max r' + this.r + ' r' + __reg(o))); }
  clamp(lo, hi) { return new FieldNode(__emit('clamp r' + this.r + ' ' + (+lo) + ' ' + (+hi))); }
  smoothstep(e0, e1) { return new FieldNode(__emit('smoothstep ' + (+e0) + ' ' + (+e1) + ' r' + this.r)); }
}
function noise2(seed, freq, octaves = 3, gain = 0.5, lacunarity = 2.0) {
  return new FieldNode(__emit('noise2 ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                              (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity)));
}
function ridge2(seed, freq, octaves = 3, gain = 0.5, lacunarity = 2.0) {
  return new FieldNode(__emit('ridge2 ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                              (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity)));
}
function warp2(src, seed, freq, strength) {
  return new FieldNode(__emit('warp2 r' + __reg(src) + ' ' + (seed >>> 0) + ' ' +
                              (+freq) + ' ' + (+strength)));
}
function blend(a, b, t) {
  return new FieldNode(__emit('blend r' + __reg(a) + ' r' + __reg(b) + ' r' + __reg(t)));
}
function heightToDensity(h) { return h; }   // v1 marker: density == height field
class World {}
```

- [ ] **Step 4: Implement `ScriptHost::eval_world`** following the `eval_requires` skeleton: fold via `fold_sources_cached`, fresh runtime, eval `kWorldBaseJS`, folded shared-lib, world class source; find the class name (reuse `find_part_class_name` — it matches `class X extends Y`; verify it accepts `extends World`, extend the regex if it hard-codes `Part`); merge params via the existing canonical merge; instantiate; call `field(p)`; on exception → `ok=false` with the JS error text. Read back `__world_ops` (JS array of strings), append directives:

```
height r<density.r>
moisture r<moisture.r>
relief r<relief.r>
seaLevel <field-result seaLevel>
biome <mountRelief> <rockyMoisture>
```

Call `biomes()` if present → `JS_JSONStringify` → `biomes_json`. Read `static world` constants. Free everything; return.

- [ ] **Step 5: Manifest kind** — in the `world.manifest` parser in `local_provider.cpp`, accept kind token `world`; store the module name in `world_module_`; do NOT add it to the expand/tileset install lists (the world module never installs as a graph root). Add accessor. Existing manifests without a `world` line behave exactly as today.

- [ ] **Step 6: Run tests**

Run: `make -C MatterEngine3/tests run-evalworld` — Expected: ALL PASS.
Covering regression: `make -C MatterEngine3/tests run-script run-dev` — Expected: pass.

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/src/world_base.js.h MatterEngine3/src/script_host.h MatterEngine3/src/script_host.cpp MatterEngine3/src/provider/local_provider.h MatterEngine3/src/provider/local_provider.cpp MatterEngine3/tests/eval_world_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat(dsl): World entry point — field-graph builder JS, ScriptHost::eval_world, 'world' manifest kind"
```

---

### Task 5: `terrain_mesher` (native surface nets) + `terrainVolume` verb

**Files:**
- Create: `MatterEngine3/src/terrain_mesher.h`
- Create: `MatterEngine3/src/terrain_mesher.cpp`
- Modify: `MatterEngine3/src/dsl_state.h` (add `WorldBinding` + field pointer)
- Modify: `MatterEngine3/src/script_host.h` (`BakeOptions` gains `dsl::WorldBinding world`)
- Modify: `MatterEngine3/src/script_host.cpp` (thread `opts.world` into `DslState` before `build()`)
- Modify: `MatterEngine3/src/part_graph.cpp` (`HostBaker::set_world(const dsl::WorldBinding*)` — copied into each bake's `BakeOptions`)
- Modify: `MatterEngine3/src/dsl_bindings.cpp` (register `js_terrainVolume`)
- Modify: `MatterEngine3/src/part_base.js.h` (expose `terrainVolume` on the Part prototype, same pattern as `vertex`)
- Create: `MatterEngine3/tests/terrain_mesher_tests.cpp` (pure mesher)
- Create: `MatterEngine3/tests/terrain_verb_tests.cpp` (verb through `bake_source`)
- Modify: `MatterEngine3/tests/Makefile` (`run-terrainmesh`, `run-terrainverb`)

**Interfaces:**
- Consumes: `terrain_field::FieldRuntime` (Task 3); `DslState::fill/beginShape/vertex/endShape` (`dsl_state.h:126,183-185`); `BakeOptions` (`script_host.h`).
- Produces:

```cpp
// terrain_mesher.h
#pragma once
#include "terrain_field.h"
#include <cstdint>
#include <string>
#include <vector>

namespace terrain_mesher {

// One material bucket: flat triangle list. x/z are sector-LOCAL
// (world minus sector origin); y is world-absolute.
struct MaterialBucket {
    uint32_t material = 0;         // terrain_field::FieldRuntime::Material value
    std::vector<float> positions;  // 9 floats per triangle
    std::vector<float> normals;    // 9 floats per triangle (gradient normals)
};

struct SectorMesh {
    std::vector<MaterialBucket> buckets;
    size_t triangle_count() const;
};

// Naive surface nets over the sector slab.
//   voxel = 2.0f / (1 << rung)     (rung 0..3 -> 2.0 / 1.0 / 0.5 / 0.25)
// Returns false + err only on degenerate config (rung outside 0..3,
// sector_size <= 0, y_min >= y_max).
bool mesh_sector(const terrain_field::FieldRuntime& field,
                 int64_t tx, int64_t tz, int rung,
                 float sector_size, float y_min, float y_max,
                 SectorMesh& out, std::string& err);

} // namespace terrain_mesher
```

```cpp
// dsl_state.h (namespace dsl)
namespace terrain_field { class FieldRuntime; }
struct WorldBinding {
    const terrain_field::FieldRuntime* field = nullptr;  // null => no world bound
    float sector_size = 16.0f;
    float y_min = -64.0f, y_max = 192.0f;
};
// DslState gains:
//   void set_world(const WorldBinding& w) { world_ = w; }
//   const WorldBinding& world() const { return world_; }
```

- `BakeOptions` gains `dsl::WorldBinding world;` — NOT hashed (the field is addressed by `fieldHash` in sector params, Task 7).
- JS verb: `this.terrainVolume(tx, tz, rung, [matGrass, matDirt, matRock, matSnow])` — the script passes engine material ids (the `MAT` table), so native code never hard-codes the material registry.

**Design notes (binding contract):**
- **Normals policy:** `SectorMesh` carries gradient normals (they validate extraction quality in tests and lock triangle orientation), but the verb emits positions only through the existing `fill → beginShape(0) → vertex → endShape` path — the downstream mesh pipeline computes shading normals exactly as it does for `Terrain.js` today. No new normal plumbing.
- **Sampling with a ring:** the density lattice extends ONE sample ring beyond the sector footprint on each side, so border cells sample identical world positions in both neighbors → same-rung seams are exactly watertight. Faces are emitted only when the generating edge's sample sits in local `[0, sector_size)` (half-open), so neighbors partition space with no duplicate geometry.
- **Slab bounds:** scan `height_at` over the footprint (+ring) at voxel spacing; slab = `[max(y_min, hMin − 2·voxel), min(y_max, hMax + 2·voxel)]`. Cost scales with surface area, not volume.
- **Skirts (cross-rung seams):** along each of the 4 borders, emit a vertical curtain per border segment: top edge at `height_at + 0.5·voxel`, bottom at `height_at − 2.0` (one coarsest voxel), outward horizontal normals, material from `material_at` at the segment midpoint.

- [ ] **Step 1: Write the failing mesher tests**

```cpp
// MatterEngine3/tests/terrain_mesher_tests.cpp
#include "check.h"
#include "../src/terrain_field.h"
#include "../src/terrain_mesher.h"
#include <cmath>
#include <cstring>

using namespace terrain_field;
using namespace terrain_mesher;

static FieldRuntime make(const char* text) {
    FieldProgram p; std::string err;
    if (!FieldProgram::parse(text, p, err)) printf("parse err: %s\n", err.c_str());
    return FieldRuntime(std::move(p));
}
static const char* kFlat5 =
    "const 5\nconst 0.5\nconst 0.5\n"
    "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n";
static const char* kNoise =
    "noise2 1234 0.02 4 0.5 2.0\nconst 20\nmul r0 r1\nconst 0.5\nconst 0.5\n"
    "height r2\nmoisture r3\nrelief r4\nseaLevel -100\nbiome 0.65 0.35\n";

// Count tris whose (stored) normal satisfies pred; also range-check surface y.
template <typename P>
static size_t count_tris(const SectorMesh& m, P pred) {
    size_t n = 0;
    for (const auto& b : m.buckets)
        for (size_t t = 0; t * 9 < b.normals.size(); ++t)
            if (pred(b.normals[t*9+0], b.normals[t*9+1], b.normals[t*9+2])) ++n;
    return n;
}

int main() {
    // --- flat field, rung 0: counts, height, orientation -------------------
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m; std::string err;
        CHECK(mesh_sector(f, 0, 0, 0, 16.0f, -64.0f, 192.0f, m, err), err.c_str());
        size_t up    = count_tris(m, [](float, float ny, float){ return ny >  0.9f; });
        size_t skirt = count_tris(m, [](float, float ny, float){ return std::fabs(ny) < 0.3f; });
        CHECK(up == 128, "flat rung0: 8x8 cells -> 128 surface tris");
        CHECK(skirt == 64, "flat rung0: 4 sides x 8 segs x 2 = 64 skirt tris");
        bool y_ok = true, xz_ok = true;
        for (const auto& b : m.buckets)
            for (size_t t = 0; t * 9 < b.positions.size(); ++t) {
                float ny = b.normals[t*9+1];
                for (int v = 0; v < 3; ++v) {
                    float x = b.positions[t*9+v*3+0], y = b.positions[t*9+v*3+1],
                          z = b.positions[t*9+v*3+2];
                    if (ny > 0.9f && std::fabs(y - 5.0f) > 1e-3f) y_ok = false;
                    if (x < -2.1f || x > 18.1f || z < -2.1f || z > 18.1f) xz_ok = false;
                }
            }
        CHECK(y_ok, "surface verts at y=5");
        CHECK(xz_ok, "verts in sector-local range");
    }
    // --- rung scaling: rung1 = 4x surface tris of rung0 --------------------
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m0, m1; std::string err;
        CHECK(mesh_sector(f, 0, 0, 0, 16.0f, -64, 192, m0, err), "rung0");
        CHECK(mesh_sector(f, 0, 0, 1, 16.0f, -64, 192, m1, err), "rung1");
        size_t up0 = count_tris(m0, [](float,float ny,float){ return ny > 0.9f; });
        size_t up1 = count_tris(m1, [](float,float ny,float){ return ny > 0.9f; });
        CHECK(up1 == 4 * up0, "rung1 surface = 4x rung0");
    }
    // --- determinism --------------------------------------------------------
    {
        FieldRuntime f = make(kNoise);
        SectorMesh a, b; std::string err;
        CHECK(mesh_sector(f, 3, -2, 2, 16.0f, -64, 192, a, err), "a");
        CHECK(mesh_sector(f, 3, -2, 2, 16.0f, -64, 192, b, err), "b");
        CHECK(a.buckets.size() == b.buckets.size(), "same bucket count");
        bool same = true;
        for (size_t i = 0; i < a.buckets.size(); ++i)
            if (a.buckets[i].positions != b.buckets[i].positions) same = false;
        CHECK(same, "byte-identical positions");
    }
    // --- same-rung seam: sector (0,0) x=16 border == sector (1,0) x=0 ------
    {
        FieldRuntime f = make(kNoise);
        SectorMesh a, b; std::string err;
        CHECK(mesh_sector(f, 0, 0, 1, 16.0f, -64, 192, a, err), "a");
        CHECK(mesh_sector(f, 1, 0, 1, 16.0f, -64, 192, b, err), "b");
        // Every surface vert of A with local x in [15,16] must appear in B at
        // local x-16 (world-identical) within 1e-4.
        size_t checked = 0, matched = 0;
        for (const auto& ba : a.buckets)
            for (size_t v = 0; v * 3 < ba.positions.size(); ++v) {
                float ax = ba.positions[v*3], ay = ba.positions[v*3+1], az = ba.positions[v*3+2];
                if (ax < 15.0f || ax > 16.0f) continue;
                ++checked;
                for (const auto& bb : b.buckets)
                    for (size_t w = 0; w * 3 < bb.positions.size(); ++w) {
                        float dx = (bb.positions[w*3] + 16.0f) - ax;
                        float dy = bb.positions[w*3+1] - ay;
                        float dz = bb.positions[w*3+2] - az;
                        if (dx*dx + dy*dy + dz*dz < 1e-8f) { ++matched; goto next; }
                    }
                next:;
            }
        CHECK(checked > 0, "border verts exist");
        CHECK(matched == checked, "all border verts match neighbor exactly");
    }
    // --- degenerate config fails loudly -------------------------------------
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m; std::string err;
        CHECK(!mesh_sector(f, 0, 0, 4, 16.0f, -64, 192, m, err), "rung 4 rejected");
        CHECK(!mesh_sector(f, 0, 0, 0, 16.0f, 10, -10, m, err), "bad slab rejected");
    }
    return check_summary();
}
```

- [ ] **Step 2: Add `run-terrainmesh` target** (compiles terrain_field + terrain_mesher + test; no JS/GL link). Run — Expected: FAIL (no terrain_mesher.cpp).

- [ ] **Step 3: Implement `terrain_mesher.cpp`**

```cpp
#include "terrain_mesher.h"
#include <cmath>
#include <unordered_map>

namespace terrain_mesher {

size_t SectorMesh::triangle_count() const {
    size_t n = 0;
    for (const auto& b : buckets) n += b.positions.size() / 9;
    return n;
}

namespace {

struct V3 { float x, y, z; };
struct CellVert { V3 p; V3 n; };

MaterialBucket& bucket_for(SectorMesh& m, uint32_t mat) {
    for (auto& b : m.buckets) if (b.material == mat) return b;
    m.buckets.push_back(MaterialBucket{mat, {}, {}});
    return m.buckets.back();
}

void push_tri(MaterialBucket& b, const CellVert& a, const CellVert& c, const CellVert& d) {
    const CellVert* vs[3] = {&a, &c, &d};
    for (const CellVert* v : vs) {
        b.positions.push_back(v->p.x); b.positions.push_back(v->p.y); b.positions.push_back(v->p.z);
        b.normals.push_back(v->n.x);   b.normals.push_back(v->n.y);   b.normals.push_back(v->n.z);
    }
}

// 12 cell edges as corner-offset pairs (i0,j0,k0, i1,j1,k1).
const int kEdges[12][6] = {
    {0,0,0,1,0,0},{0,1,0,1,1,0},{0,0,1,1,0,1},{0,1,1,1,1,1},
    {0,0,0,0,1,0},{1,0,0,1,1,0},{0,0,1,0,1,1},{1,0,1,1,1,1},
    {0,0,0,0,0,1},{1,0,0,1,0,1},{0,1,0,0,1,1},{1,1,0,1,1,1},
};

} // namespace

bool mesh_sector(const terrain_field::FieldRuntime& field,
                 int64_t tx, int64_t tz, int rung,
                 float sector_size, float y_min, float y_max,
                 SectorMesh& out, std::string& err) {
    if (rung < 0 || rung > 3) { err = "terrain_mesher: rung out of 0..3"; return false; }
    if (sector_size <= 0.0f || y_min >= y_max) { err = "terrain_mesher: bad slab config"; return false; }

    const float voxel = 2.0f / float(1 << rung);
    const int   n     = int(std::lround(sector_size / voxel));  // cells per side in x/z
    const double ox = double(tx) * sector_size, oz = double(tz) * sector_size;

    // Slab bounds from a footprint height scan (+1 ring).
    float hMin = y_max, hMax = y_min;
    for (int k = -1; k <= n + 1; ++k)
        for (int i = -1; i <= n + 1; ++i) {
            float h = field.height_at(float(ox + i * voxel), float(oz + k * voxel));
            if (h < hMin) hMin = h;
            if (h > hMax) hMax = h;
        }
    const float y0 = std::max(y_min, hMin - 2.0f * voxel);
    const float y1 = std::min(y_max, hMax + 2.0f * voxel);
    const int   ny = std::max(1, int(std::ceil((y1 - y0) / voxel)));

    // Density lattice (n+3)x(ny+1)x(n+3): x/z sample i is local (i-1)*voxel,
    // i.e. one ring outside the footprint on each side.
    const int sx = n + 3, sy = ny + 1, szn = n + 3;
    std::vector<float> d(size_t(sx) * sy * szn);
    auto at = [&](int i, int j, int k) -> float& { return d[(size_t(k)*sy + j)*sx + i]; };
    for (int k = 0; k < szn; ++k)
        for (int j = 0; j < sy; ++j)
            for (int i = 0; i < sx; ++i)
                at(i, j, k) = field.density_at(float(ox + (i-1)*voxel),
                                               y0 + j * voxel,
                                               float(oz + (k-1)*voxel));

    // One vertex per mixed-sign cell: centroid of edge crossings; gradient normal.
    std::unordered_map<int64_t, CellVert> verts;
    auto key = [&](int ci, int cj, int ck) { return (int64_t(ck)*sy + cj)*sx + ci; };
    auto get_vert = [&](int ci, int cj, int ck) -> const CellVert* {
        if (ci < 0 || cj < 0 || ck < 0 || ci >= sx-1 || cj >= sy-1 || ck >= szn-1) return nullptr;
        auto it = verts.find(key(ci, cj, ck));
        if (it != verts.end()) return &it->second;
        float px = 0, py = 0, pz = 0; int cnt = 0;
        for (const int* e : kEdges) {
            float a = at(ci+e[0], cj+e[1], ck+e[2]);
            float b = at(ci+e[3], cj+e[4], ck+e[5]);
            if ((a > 0) == (b > 0)) continue;
            float t = a / (a - b);
            px += (ci+e[0]) + t * (e[3]-e[0]);
            py += (cj+e[1]) + t * (e[4]-e[1]);
            pz += (ck+e[2]) + t * (e[5]-e[2]);
            ++cnt;
        }
        if (!cnt) return nullptr;
        CellVert cv;
        cv.p = { (px/cnt - 1.0f) * voxel, y0 + (py/cnt) * voxel, (pz/cnt - 1.0f) * voxel };
        const float e2 = voxel;
        const float wx = float(ox + cv.p.x), wy = cv.p.y, wz = float(oz + cv.p.z);
        float gx = field.density_at(wx+e2, wy, wz) - field.density_at(wx-e2, wy, wz);
        float gy = field.density_at(wx, wy+e2, wz) - field.density_at(wx, wy-e2, wz);
        float gz = field.density_at(wx, wy, wz+e2) - field.density_at(wx, wy, wz-e2);
        float len = std::sqrt(gx*gx + gy*gy + gz*gz);
        cv.n = len > 1e-12f ? V3{-gx/len, -gy/len, -gz/len} : V3{0, 1, 0};
        return &(verts[key(ci, cj, ck)] = cv);
    };

    // Faces: one quad per interior lattice edge with a sign change, joining the
    // 4 cells sharing that edge. Ownership: emit only when the edge's base
    // sample lies in local [0, sector_size) on both x and z.
    auto emit_quad = [&](const CellVert* v00, const CellVert* v10,
                         const CellVert* v11, const CellVert* v01,
                         bool flip, float wxc, float wzc) {
        if (!v00 || !v10 || !v11 || !v01) return;
        MaterialBucket& b = bucket_for(out, uint32_t(field.material_at(wxc, wzc)));
        if (flip) std::swap(v10, v01);
        push_tri(b, *v00, *v10, *v11);
        push_tri(b, *v00, *v11, *v01);
    };
    auto owned = [&](int i, int k) {   // sample-index ownership test
        float lx = (i-1) * voxel, lz = (k-1) * voxel;
        return lx >= 0.0f && lx < sector_size && lz >= 0.0f && lz < sector_size;
    };
    for (int k = 0; k < szn; ++k)
        for (int j = 0; j < sy; ++j)
            for (int i = 0; i < sx; ++i) {
                float a = at(i, j, k);
                float wxs = float(ox + (i-1)*voxel), wzs = float(oz + (k-1)*voxel);
                // +y edge (the common terrain surface case)
                if (j + 1 < sy && owned(i, k)) {
                    float b = at(i, j+1, k);
                    if ((a > 0) != (b > 0))
                        emit_quad(get_vert(i-1, j, k-1), get_vert(i, j, k-1),
                                  get_vert(i, j, k),     get_vert(i-1, j, k),
                                  /*flip=*/a <= 0, wxs, wzs);
                }
                // +x edge
                if (i + 1 < sx && owned(i, k)) {
                    float b = at(i+1, j, k);
                    if ((a > 0) != (b > 0))
                        emit_quad(get_vert(i, j-1, k-1), get_vert(i, j, k-1),
                                  get_vert(i, j, k),     get_vert(i, j-1, k),
                                  /*flip=*/a > 0, wxs + 0.5f*voxel, wzs);
                }
                // +z edge
                if (k + 1 < szn && owned(i, k)) {
                    float b = at(i, j, k+1);
                    if ((a > 0) != (b > 0))
                        emit_quad(get_vert(i-1, j-1, k), get_vert(i, j-1, k),
                                  get_vert(i, j, k),     get_vert(i-1, j, k),
                                  /*flip=*/a <= 0, wxs, wzs + 0.5f*voxel);
                }
            }

    // Skirts: vertical curtains along the 4 borders.
    auto skirt_seg = [&](float lx0, float lz0, float lx1, float lz1, float nx, float nz) {
        float wx0 = float(ox + lx0), wz0 = float(oz + lz0);
        float wx1 = float(ox + lx1), wz1 = float(oz + lz1);
        float h0 = field.height_at(wx0, wz0) + 0.5f * voxel;
        float h1 = field.height_at(wx1, wz1) + 0.5f * voxel;
        CellVert t0{{lx0, h0, lz0}, {nx, 0, nz}}, b0{{lx0, h0 - 2.0f, lz0}, {nx, 0, nz}};
        CellVert t1{{lx1, h1, lz1}, {nx, 0, nz}}, b1{{lx1, h1 - 2.0f, lz1}, {nx, 0, nz}};
        MaterialBucket& b = bucket_for(out,
            uint32_t(field.material_at(0.5f*(wx0+wx1), 0.5f*(wz0+wz1))));
        push_tri(b, t0, b0, b1);
        push_tri(b, t0, b1, t1);
    };
    for (int i = 0; i < n; ++i) {
        float a = i * voxel, c = (i+1) * voxel, S = sector_size;
        skirt_seg(a, 0, c, 0, 0, -1);   // -z border
        skirt_seg(a, S, c, S, 0,  1);   // +z border
        skirt_seg(0, a, 0, c, -1, 0);   // -x border
        skirt_seg(S, a, S, c,  1, 0);   // +x border
    }
    return true;
}

} // namespace terrain_mesher
```

**Winding note:** the test locks orientation (flat field ⇒ stored normals up AND the emitted winding must be front-facing for the pipeline; verify visually in Task 10). If the `flip` conventions above produce backfaces, invert the affected `flip` expression — the seam/count/normal tests are winding-independent, so fix winding without touching them.

- [ ] **Step 4: Run mesher tests** — `make -C MatterEngine3/tests run-terrainmesh` — Expected: ALL PASS.

- [ ] **Step 5: Write the failing verb test**

```cpp
// MatterEngine3/tests/terrain_verb_tests.cpp
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/part_asset_v2.h"
#include <cstdio>
#include <cstdlib>

using namespace script_host;

static const char* kSector = R"JS(
class S extends Part {
  static params = { tx: 0, tz: 0, rung: 0 };
  build(p) {
    this.terrainVolume(p.tx, p.tz, p.rung, [MAT.grass, MAT.dirt, MAT.rock, MAT.snow]);
  }
}
)JS";

int main() {
    terrain_field::FieldProgram prog; std::string err;
    CHECK(terrain_field::FieldProgram::parse(
        "const 5\nconst 0.5\nconst 0.5\n"
        "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n",
        prog, err), err.c_str());
    terrain_field::FieldRuntime field(std::move(prog));

    system("rm -rf /tmp/terrain_verb_parts && mkdir -p /tmp/terrain_verb_parts");
    ScriptHost host;

    // No world bound -> loud error
    {
        BakeOptions opts; opts.parts_dir = "/tmp/terrain_verb_parts";
        BakeResult r = host.bake_source(kSector, "{}", opts);
        CHECK(!r.error.ok, "terrainVolume without world binding must fail");
        CHECK(r.error.message.find("terrainVolume") != std::string::npos, "names the verb");
    }
    // Bound -> bakes; artifact holds the flat-field triangle count (128+64)
    {
        BakeOptions opts; opts.parts_dir = "/tmp/terrain_verb_parts";
        opts.world.field = &field;   // sector_size / y bounds = defaults
        BakeResult r = host.bake_source(kSector, "{}", opts);
        CHECK(r.error.ok, r.error.message.c_str());
        // Load the artifact and count triangles (use the same load_v2 accessors
        // demand_bake_tests uses to inspect baked geometry).
        // Expected: 192 triangles total (128 surface + 64 skirt), >1 material.
    }
    return check_summary();
}
```

(Fill the artifact-inspection block from whatever accessor `part_asset_v2.h:84-88` exposes — the same pattern existing bake tests use to assert geometry presence. The binding assertions are: bake ok, triangle count == 192, at least 1 material bucket.)

- [ ] **Step 6: Implement the verb + plumbing**

1. `dsl_state.h`: add `WorldBinding` struct (code above), member `WorldBinding world_;`, setter/getter.
2. `script_host.h`: `BakeOptions` gains `dsl::WorldBinding world;`.
3. `script_host.cpp` (bake path, where `DslState` is prepared for `build()`): `st.set_world(opts.world);`.
4. `part_graph.cpp`: `HostBaker::set_world(const dsl::WorldBinding& w)` stores a copy; every `BakeOptions` it constructs gets `opts.world = world_;`.
5. `dsl_bindings.cpp`: `js_terrainVolume(tx, tz, rung, matArray)`:
   - `DslState* st = state_from(ctx)`; if `!st->world().field` → `st->set_error("terrainVolume: no world field bound (part baked outside a world)")` and return.
   - Read 4 ints from `matArray` → `mats[4]`.
   - `terrain_mesher::mesh_sector(*st->world().field, tx, tz, rung, st->world().sector_size, st->world().y_min, st->world().y_max, mesh, err)`; on false → `set_error(err)`.
   - For each bucket: `st->fill(mats[bucket.material]); st->beginShape(0 /*SHAPE.triangles*/);` then `st->vertex(x,y,z)` per position triple; `st->endShape();`. (Batch natively — never round-trip through JS.)
6. `part_base.js.h`: add `terrainVolume(tx, tz, rung, mats) { __terrainVolume(tx, tz, rung, mats); }` to the Part prototype, following exactly how `vertex`/`beginShape` are exposed.

- [ ] **Step 7: Run** — `make -C MatterEngine3/tests run-terrainmesh run-terrainverb` — Expected: ALL PASS.
Covering regression: `make -C MatterEngine3/tests run-script run-partv2` — Expected: pass (BakeOptions default `world` is inert).

- [ ] **Step 8: Commit**

```bash
git add MatterEngine3/src/terrain_mesher.h MatterEngine3/src/terrain_mesher.cpp \
        MatterEngine3/src/dsl_state.h MatterEngine3/src/script_host.h MatterEngine3/src/script_host.cpp \
        MatterEngine3/src/part_graph.cpp MatterEngine3/src/dsl_bindings.cpp MatterEngine3/src/part_base.js.h \
        MatterEngine3/tests/terrain_mesher_tests.cpp MatterEngine3/tests/terrain_verb_tests.cpp \
        MatterEngine3/tests/Makefile
git commit -m "feat(terrain): native surface-nets sector mesher + terrainVolume DSL verb"
```

---

### Task 6: `scatter_grid.js` — deterministic cross-sector spaced scatter

(Ordered before WorldSector.js because the sector schema imports it.)

**Files:**
- Create: `MatterEngine3/shared-lib/scatter_grid.js`
- Modify: `MatterEngine3/tests/shared_lib_tests.cpp` (add scatter_grid section, existing `run-shlib` target)

**Interfaces:**
- Consumes: nothing (pure JS module, hash-based — no rng import, no state).
- Produces (used by WorldSector.js in Task 7):

```js
// One candidate per virtual grid cell of size minDist. Survival uses
// neighbor-priority rejection: exact min-dist guarantee, order-independent,
// identical from any sector that can see the cell.
export function cellCandidate(seed, kind, cellX, cellZ, minDist)
//   -> { x, z, rot, u, v, pri }   (always returns; survival is separate)
//      x/z: world position (jittered inside the cell)
//      rot: uniform [0, 2*pi);  u, v: uniform [0,1) extra picks;  pri: priority hash
export function survives(seed, kind, cellX, cellZ, minDist)  // -> bool
export function candidatesInRect(seed, kind, minDist, x0, z0, w, h)
//   -> [ {x, z, rot, u, v} ]  surviving candidates whose position lies in
//      [x0, x0+w) x [z0, z0+h)  (position ownership: each candidate is
//      returned by exactly one covering rect in a partition)
```

**Algorithm (binding contract):**
- Cell of candidate = `(floor(x / minDist), floor(z / minDist))` in world space.
- Per-cell hash: 32-bit integer mix of `(seed, kind, cellX, cellZ)` — use the same style of avalanche mixing as `terrain_field`'s `hash2i` constants (`374761393, 668265263, 2246822519, 1274126177`), implemented with `Math.imul` and `>>> 0`. Successive derived values (jx, jz, rot, u, v, pri) come from re-mixing the base hash with distinct odd constants — write a tiny `mix(h, c)` helper, no RNG object.
- Position: `x = (cellX + 0.25 + 0.5*jx) * minDist` (jitter confined to the middle half of the cell keeps worst-case neighbor distance sane), same for z.
- Survival: candidate C survives iff **no** candidate in the 8 neighboring cells has `dist < minDist` AND (`pri` higher than C's, tie-broken by `(cellX, cellZ)` lexicographic). Cells two apart are ≥ minDist by construction, so checking 8 neighbors is exhaustive.
- `candidatesInRect` enumerates every cell overlapping the rect **plus one cell margin** is NOT needed (ownership is by candidate position, and a candidate's position is inside its own cell) — enumerate exactly the cells intersecting the rect, keep survivors with position inside the rect.

- [ ] **Step 1: Write the failing tests** — add a `scatter_grid` section to `shared_lib_tests.cpp` following the existing per-module pattern in that file (evaluate the module in the test's JS context, run assertions in JS, surface pass/fail through the existing harness). The assertions, in JS:

```js
import { cellCandidate, survives, candidatesInRect } from 'shared-lib/scatter_grid';

const MD = 24.0, SEED = 20260710, KIND = 1;

// (a) min-dist property over a 10x10-cell region
const all = candidatesInRect(SEED, KIND, MD, 0, 0, 10 * MD, 10 * MD);
if (all.length < 5) throw new Error('suspiciously few survivors: ' + all.length);
for (let i = 0; i < all.length; ++i)
  for (let j = i + 1; j < all.length; ++j) {
    const dx = all[i].x - all[j].x, dz = all[i].z - all[j].z;
    if (dx*dx + dz*dz < MD*MD - 1e-6)
      throw new Error('min-dist violated: ' + Math.sqrt(dx*dx + dz*dz));
  }

// (b) partition property: 3x3 sector rects tile the big rect exactly
const S = 80.0;  // 3x3 of 80 = one 240 rect
let union = [];
for (let a = 0; a < 3; ++a)
  for (let b = 0; b < 3; ++b)
    union = union.concat(candidatesInRect(SEED, KIND, MD, a*S, b*S, S, S));
const whole = candidatesInRect(SEED, KIND, MD, 0, 0, 3*S, 3*S);
if (union.length !== whole.length)
  throw new Error('partition mismatch: ' + union.length + ' vs ' + whole.length);
const keyOf = c => c.x.toFixed(4) + ',' + c.z.toFixed(4);
const set = new Set(union.map(keyOf));
for (const c of whole)
  if (!set.has(keyOf(c))) throw new Error('candidate missing from union');

// (c) determinism + seed sensitivity
const again = candidatesInRect(SEED, KIND, MD, 0, 0, 240, 240);
if (JSON.stringify(again) !== JSON.stringify(whole)) throw new Error('nondeterministic');
const other = candidatesInRect(SEED + 1, KIND, MD, 0, 0, 240, 240);
if (JSON.stringify(other) === JSON.stringify(whole)) throw new Error('seed-insensitive');

// (d) kind independence: different kind -> different layout
const k2 = candidatesInRect(SEED, KIND + 1, MD, 0, 0, 240, 240);
if (JSON.stringify(k2) === JSON.stringify(whole)) throw new Error('kind-insensitive');

// (e) negative-coordinate cells work (infinite world!)
const neg = candidatesInRect(SEED, KIND, MD, -400, -400, 240, 240);
if (neg.length < 5) throw new Error('negative-region survivors: ' + neg.length);
```

- [ ] **Step 2: Run** — `make -C MatterEngine3/tests run-shlib` — Expected: FAIL (module missing).

- [ ] **Step 3: Implement `shared-lib/scatter_grid.js`**

```js
// Deterministic spaced scatter on a virtual world grid. One candidate per
// minDist-sized cell; neighbor-priority rejection gives an exact min-dist
// guarantee that is order-independent and identical from any sector.

function mix(h, c) {
  h = Math.imul(h ^ (h >>> 15), c | 1);
  h ^= h + Math.imul(h ^ (h >>> 7), h | 61);
  return (h ^ (h >>> 14)) >>> 0;
}
function baseHash(seed, kind, cx, cz) {
  let h = (seed | 0) ^ Math.imul(kind | 0, 374761393);
  h = mix(h ^ Math.imul(cx | 0, 668265263), 2246822519);
  h = mix(h ^ Math.imul(cz | 0, 1274126177), 374761393);
  return h >>> 0;
}
const unit = h => h / 4294967296;   // [0,1)

export function cellCandidate(seed, kind, cellX, cellZ, minDist) {
  const h = baseHash(seed, kind, cellX, cellZ);
  const jx = unit(mix(h, 0x9E3779B1)), jz = unit(mix(h, 0x85EBCA77));
  return {
    x: (cellX + 0.25 + 0.5 * jx) * minDist,
    z: (cellZ + 0.25 + 0.5 * jz) * minDist,
    rot: unit(mix(h, 0xC2B2AE3D)) * Math.PI * 2,
    u: unit(mix(h, 0x27D4EB2F)),
    v: unit(mix(h, 0x165667B1)),
    pri: h,
  };
}

export function survives(seed, kind, cellX, cellZ, minDist) {
  const c = cellCandidate(seed, kind, cellX, cellZ, minDist);
  for (let dz = -1; dz <= 1; ++dz)
    for (let dx = -1; dx <= 1; ++dx) {
      if (dx === 0 && dz === 0) continue;
      const nx = cellX + dx, nz = cellZ + dz;
      const o = cellCandidate(seed, kind, nx, nz, minDist);
      const ddx = c.x - o.x, ddz = c.z - o.z;
      if (ddx * ddx + ddz * ddz >= minDist * minDist) continue;
      // Conflict: higher priority wins; ties broken by cell coords.
      if (o.pri > c.pri) return false;
      if (o.pri === c.pri && (nz < cellZ || (nz === cellZ && nx < cellX))) return false;
    }
  return true;
}

export function candidatesInRect(seed, kind, minDist, x0, z0, w, h) {
  const out = [];
  const c0 = Math.floor(x0 / minDist), c1 = Math.floor((x0 + w) / minDist);
  const r0 = Math.floor(z0 / minDist), r1 = Math.floor((z0 + h) / minDist);
  for (let cz = r0; cz <= r1; ++cz)
    for (let cx = c0; cx <= c1; ++cx) {
      const c = cellCandidate(seed, kind, cx, cz, minDist);
      if (c.x < x0 || c.x >= x0 + w || c.z < z0 || c.z >= z0 + h) continue;
      if (!survives(seed, kind, cx, cz, minDist)) continue;
      out.push({ x: c.x, z: c.z, rot: c.rot, u: c.u, v: c.v });
    }
  return out;
}
```

- [ ] **Step 4: Run** — `make -C MatterEngine3/tests run-shlib` — Expected: ALL PASS.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/shared-lib/scatter_grid.js MatterEngine3/tests/shared_lib_tests.cpp
git commit -m "feat(shared-lib): scatter_grid — deterministic cross-sector spaced scatter"
```

---

### Task 7: `WorldSector.js` + native world query verbs

**Files:**
- Create: `MatterEngine3/examples/world_demo/schemas/WorldSector.js`
- Modify: `MatterEngine3/src/dsl_bindings.cpp` (query verbs `heightAt/slopeAt/biomeAt/moistureAt`)
- Modify: `MatterEngine3/src/part_base.js.h` (expose the 4 verbs on the Part prototype)
- Create: `MatterEngine3/tests/sector_bake_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile` (`run-sectorbake`)

**Interfaces:**
- Consumes: `terrainVolume` verb + `WorldBinding` (Task 5); `scatter_grid.js` (Task 6); `bake_source`/`eval_requires`; existing schemas `Rock/Pebble/Grass/Tree` (unchanged).
- Produces: the `WorldSector` module with `static params = { tx, tz, rung, worldSeed, fieldHash: '', biomes: '' }` and a FIXED asset-variant `static requires` (independent of tx/tz — installed once, Task 9 relies on this); native verbs (world coordinates, all error loudly when no world is bound):
  - `this.heightAt(x, z) -> float`
  - `this.slopeAt(x, z) -> float`
  - `this.biomeAt(x, z) -> 'ocean' | 'meadow' | 'foothills' | 'mountains'`
  - `this.moistureAt(x, z) -> float`

**Design notes (binding contract):**
- Sector geometry is LOCAL in x/z (world − sector origin) with world-absolute y; Task 9 places instances with `translate(tx*16, 0, tz*16)`. All scatter `translate` calls therefore use `wx - ox` / `wz - oz`.
- `fieldHash` and `biomes` (canonical JSON string) travel as **params**, so the part hash changes whenever the world definition changes — the FieldRuntime pointer itself rides the non-hashed `BakeOptions.world`.
- Rung gating: `rung < 2` → terrain only. Rungs 2 and 3 place the SAME deterministic scatter (identical placements → no pop on rung swap).
- Dense kinds (rocks/pebbles/grass) use a per-sector-seeded `rng` — deterministic per (worldSeed, tx, tz). Spaced kinds (trees kind 1, boulders kind 2) use `scatter_grid` — deterministic across sector borders.
- Counts come from the biomes table (per-sector integers, e.g. meadow `{ grass: 156, pebbles: 16, rocks: 2, trees: true }` ≈ the old Meadow densities per 16×16 tile).

- [ ] **Step 1: Write `WorldSector.js`**

```js
import { rng } from 'shared-lib/rng';
import { candidatesInRect } from 'shared-lib/scatter_grid';

// One streamed column of the infinite world. Terrain comes from the native
// world field (terrainVolume); scatter reads the biomes table passed down
// from the world definition. Geometry is sector-local in x/z, world y.

const SECTOR = 16.0;
const ROCK_VARIANTS = 8, PEBBLE_VARIANTS = 6, GRASS_VARIANTS = 5;
const BOULDER_SIZES = [2.5, 4.0], BOULDER_SEEDS = 4;
const TREE_MIN_DIST = 24.0, BOULDER_MIN_DIST = 70.0;
const GRASS_SLOPE_MAX = 0.5;

function assetVariants() {
  const req = [];
  for (let s = 0; s < ROCK_VARIANTS; ++s) req.push({ module: 'Rock', params: { seed: s } });
  for (const sz of BOULDER_SIZES)
    for (let s = 0; s < BOULDER_SEEDS; ++s)
      req.push({ module: 'Rock', params: { seed: s, size: sz } });
  for (let s = 0; s < PEBBLE_VARIANTS; ++s) req.push({ module: 'Pebble', params: { seed: s } });
  for (let s = 0; s < GRASS_VARIANTS; ++s) req.push({ module: 'Grass', params: { seed: s } });
  req.push({ module: 'Tree' });
  return req;
}

class WorldSector extends Part {
  static params = { tx: 0, tz: 0, rung: 0, worldSeed: 0, fieldHash: '', biomes: '' };
  // FIXED variant list — independent of tx/tz so the whole asset set installs
  // once at world load and every sector bake hits the same child hashes.
  static requires = assetVariants();

  build(p) {
    this.terrainVolume(p.tx, p.tz, p.rung, [MAT.grass, MAT.dirt, MAT.rock, MAT.snow]);
    if (p.rung < 2) return;

    const table = p.biomes ? JSON.parse(p.biomes) : {};
    const ox = p.tx * SECTOR, oz = p.tz * SECTOR;
    const counts = table[this.biomeAt(ox + SECTOR / 2, oz + SECTOR / 2)] || {};
    const r = rng((p.worldSeed ^ Math.imul(p.tx | 0, 73856093)
                               ^ Math.imul(p.tz | 0, 19349663)) >>> 0);

    const put = (module, params, wx, wz, s, sinkY) => {
      this.pushMatrix();
      this.translate(wx - ox, this.heightAt(wx, wz) - sinkY, wz - oz);
      this.rotateY(r.range(0, Math.PI * 2));
      this.scale(s, s, s);
      this.placeChild(module, params);
      this.popMatrix();
    };
    const inSector = () => [ox + r.range(0, SECTOR), oz + r.range(0, SECTOR)];

    // ---- dense kinds: per-sector rng, counts from the biome table ----------
    for (let i = 0, n = counts.rocks | 0; i < n; ++i) {
      const [wx, wz] = inSector();
      if (this.biomeAt(wx, wz) === 'ocean') continue;
      const s = r.range(0.6, 1.8);
      put('Rock', { seed: r.int(ROCK_VARIANTS) }, wx, wz, s, 0.15 * s);
    }
    for (let i = 0, n = counts.pebbles | 0; i < n; ++i) {
      const [wx, wz] = inSector();
      if (this.biomeAt(wx, wz) === 'ocean') continue;
      put('Pebble', { seed: r.int(PEBBLE_VARIANTS) }, wx, wz, r.range(0.5, 1.5), 0.02);
    }
    for (let i = 0, n = counts.grass | 0; i < n; ++i) {
      const [wx, wz] = inSector();
      if (this.biomeAt(wx, wz) === 'ocean') continue;
      if (this.slopeAt(wx, wz) > GRASS_SLOPE_MAX && r.random() < 0.7) continue;
      put('Grass', { seed: r.int(GRASS_VARIANTS) }, wx, wz, r.range(0.8, 1.3), 0.02);
    }

    // ---- spaced kinds: cross-sector deterministic ---------------------------
    if (counts.trees) {
      for (const c of candidatesInRect(p.worldSeed, 1, TREE_MIN_DIST, ox, oz, SECTOR, SECTOR)) {
        if (this.biomeAt(c.x, c.z) === 'ocean') continue;
        if (this.slopeAt(c.x, c.z) > 0.8) continue;
        this.pushMatrix();
        this.translate(c.x - ox, this.heightAt(c.x, c.z), c.z - oz);
        this.rotateY(c.rot);
        this.placeChild('Tree');
        this.popMatrix();
      }
    }
    // Landmark boulders: every land biome, sparse.
    for (const c of candidatesInRect(p.worldSeed, 2, BOULDER_MIN_DIST, ox, oz, SECTOR, SECTOR)) {
      if (this.biomeAt(c.x, c.z) === 'ocean') continue;
      const sz = BOULDER_SIZES[(c.u * BOULDER_SIZES.length) | 0];
      const s = 0.8 + 0.4 * c.v;
      this.pushMatrix();
      this.translate(c.x - ox, this.heightAt(c.x, c.z) - 0.15 * sz * s, c.z - oz);
      this.rotateY(c.rot);
      this.scale(s, s, s);
      this.placeChild('Rock', { seed: (c.u * 16 | 0) % BOULDER_SEEDS, size: sz });
      this.popMatrix();
    }
  }
}
```

- [ ] **Step 2: Write the failing test**

```cpp
// MatterEngine3/tests/sector_bake_tests.cpp
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace script_host;

static std::string slurp(const char* path) {
    std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

int main() {
    // Mountain-ish noise field so slopeAt/biomeAt exercise real variation.
    terrain_field::FieldProgram prog; std::string err;
    CHECK(terrain_field::FieldProgram::parse(
        "noise2 42 0.005 4 0.5 2.0\nconst 60\nmul r0 r1\nconst 0.6\nconst 0.3\n"
        "height r2\nmoisture r3\nrelief r4\nseaLevel -80\nbiome 0.65 0.35\n",
        prog, err), err.c_str());
    terrain_field::FieldRuntime field(std::move(prog));

    std::string src = slurp("../examples/world_demo/schemas/WorldSector.js");
    CHECK(!src.empty(), "WorldSector.js readable");

    ScriptHost host;
    host.set_shared_lib_root("../shared-lib");  // tests run from MatterEngine3/tests — match how existing shared-lib-importing tests set this

    // requires: fixed asset list, independent of tx/tz
    {
        auto req_a = host.eval_requires(src, R"({"tx":0,"tz":0,"rung":3})");
        auto req_b = host.eval_requires(src, R"({"tx":900,"tz":-77,"rung":0})");
        CHECK(!req_a.empty(), "requires non-empty");
        CHECK(req_a.size() == req_b.size(), "requires independent of tx/tz");
        // 8 rocks + 8 boulders + 6 pebbles + 5 grass + 1 tree = 28
        CHECK(req_a.size() == 28, "full variant list");
    }

    system("rm -rf /tmp/sector_bake_parts && mkdir -p /tmp/sector_bake_parts");
    auto bake = [&](const char* params) {
        BakeOptions opts;
        opts.parts_dir = "/tmp/sector_bake_parts";
        opts.world.field = &field;
        return host.bake_source(src, params, opts);
    };
    // rung 0 bakes terrain-only (no placeChild -> bakes even without child hashes)
    const char* p00 = R"({"tx":0,"tz":0,"rung":0,"worldSeed":42,"fieldHash":"abc","biomes":""})";
    BakeResult r0 = bake(p00);
    CHECK(r0.error.ok, r0.error.message.c_str());

    // determinism: same params -> same hash; different tx -> different hash
    BakeResult r0b = bake(p00);
    CHECK(r0b.error.ok && r0b.resolved_hash == r0.resolved_hash, "deterministic hash");
    BakeResult r1 = bake(
        R"({"tx":1,"tz":0,"rung":0,"worldSeed":42,"fieldHash":"abc","biomes":""})");
    CHECK(r1.error.ok && r1.resolved_hash != r0.resolved_hash, "tx changes hash");

    // fieldHash participates in the hash (world edits invalidate sectors)
    BakeResult rf = bake(
        R"({"tx":0,"tz":0,"rung":0,"worldSeed":42,"fieldHash":"xyz","biomes":""})");
    CHECK(rf.error.ok && rf.resolved_hash != r0.resolved_hash, "fieldHash changes hash");

    return check_summary();
}
```

Note: rung≥2 bakes (scatter with `placeChild`) need child hashes installed the way `demand_bake_tests` does it — if that fixture is easy to reuse, add one rung-3 bake asserting `error.ok`; if it requires the full provider, leave rung≥2 coverage to Task 9's integration test and say so in your report.

- [ ] **Step 3: Add `run-sectorbake` target; run** — Expected: FAIL (verbs missing).

- [ ] **Step 4: Implement the query verbs** in `dsl_bindings.cpp` + `part_base.js.h`:
- `js_heightAt(x, z)` → `st->world().field->height_at(x, z)`; null field → `set_error("heightAt: no world field bound")` (same for all four).
- `js_slopeAt(x, z)` → `slope_at`.
- `js_moistureAt(x, z)` → `moisture_at`.
- `js_biomeAt(x, z)` → `biome_at` mapped to JS strings: `Ocean→'ocean'`, `Meadow→'meadow'`, `Foothills→'foothills'`, `Mountains→'mountains'`.
- Expose all four on the Part prototype in `part_base.js.h`.

- [ ] **Step 5: Run** — `make -C MatterEngine3/tests run-sectorbake` — Expected: ALL PASS.
Covering regression: `make -C MatterEngine3/tests run-shlib run-terrainverb` — Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/WorldSector.js \
        MatterEngine3/src/dsl_bindings.cpp MatterEngine3/src/part_base.js.h \
        MatterEngine3/tests/sector_bake_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat(world): WorldSector part — terrainVolume + biome-table scatter + world query verbs"
```

---

### Task 8: `SectorStreamer` — desired-set diff, rung swaps, eviction

**Files:**
- Create: `MatterEngine3/src/sector_streamer.h`
- Create: `MatterEngine3/src/sector_streamer.cpp`
- Create: `MatterEngine3/tests/sector_streamer_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile` (`run-sectorstream`)

**Interfaces:**
- Consumes: nothing (pure CPU, no JS/GL — mirrors `refine_controller`'s testability).
- Produces (Task 9 drives it from the worker loop):

```cpp
// sector_streamer.h
#pragma once
#include <cstdint>
#include <vector>

namespace matter_stream {

struct Ring { float radius; int rung; };

struct Config {
    float sector_size = 16.0f;
    // Innermost first. A sector's desired rung = the rung of the first ring
    // whose radius covers the camera->sector-centre distance; beyond the last
    // ring the sector is not desired.
    std::vector<Ring> rings { {48.0f, 3}, {120.0f, 2}, {300.0f, 1}, {800.0f, 0} };
    float hysteresis = 16.0f;        // extra distance before demote/evict
    int   max_inflight = 8;
    int   fail_cooldown_updates = 64;
};

struct SectorRequest { int64_t tx, tz; int rung; };
struct Eviction      { int64_t tx, tz; int rung; };

class SectorStreamer {
public:
    explicit SectorStreamer(Config cfg);

    // Recompute the desired set for this camera position (call once per tick).
    void update(float cam_x, float cam_z);

    // Next bake to launch: holes (nothing resident) before upgrades, nearest
    // first within each class. Returns false when nothing is needed or
    // max_inflight is reached. Marks the sector in-flight.
    bool next_request(SectorRequest& out);

    // Bake finished. Returns true if the streamer accepted it as resident
    // (the caller publishes). Returns false if it is no longer desired
    // (camera moved on / clear() happened) — the caller must discard the
    // artifact WITHOUT publishing. On an accepted upgrade, the previously
    // resident rung is queued as an eviction (publish-then-evict: no hole).
    bool on_published(int64_t tx, int64_t tz, int rung);

    // Bake failed: drop from inflight, cool down before re-requesting.
    void on_failed(int64_t tx, int64_t tz, int rung);

    // Drain sectors to unpublish + release (each was previously accepted).
    std::vector<Eviction> take_evictions();

    // Reroll: every resident sector moves to the eviction queue; inflight
    // bookkeeping resets (their on_published will return false).
    void clear();

    size_t resident_count() const;
    size_t inflight_count() const;
};

} // namespace matter_stream
```

**Semantics (binding contract):**
- Distance = camera to sector centre `((tx+0.5)*S, (tz+0.5)*S)` in the XZ plane.
- `update()` scans the square of sectors covering the outermost ring radius around the camera; every scanned sector gets `desired_rung` or "not desired".
- Hysteresis: a RESIDENT sector is only demoted/evicted when its distance exceeds (its current ring radius + hysteresis). Promotion has no hysteresis.
- Demotion (e.g. rung 3 resident, now in the rung-2 band) is an upgrade-style swap: request the lower rung, publish, then evict the old — never evict first.
- One in-flight request per sector max; `max_inflight` total.
- `on_failed` sets a cooldown of `fail_cooldown_updates` `update()` calls for that (sector, rung); it stays resident at its old rung (if any) meanwhile.
- All state keyed by `(tx, tz)` in a hash map (`std::unordered_map` with a 64-bit key like `(uint64_t(uint32_t(tx)) << 32) | uint32_t(tz)` — document the cast; sectors beyond ±2^31 are out of scope).

- [ ] **Step 1: Write the failing tests**

```cpp
// MatterEngine3/tests/sector_streamer_tests.cpp
#include "check.h"
#include "../src/sector_streamer.h"
#include <cmath>
#include <cstdio>

using namespace matter_stream;

// Service every outstanding request (simulate instant bakes). Optionally
// record each request via cb(q).
template <typename F>
static void service_all(SectorStreamer& s, F cb) {
    SectorRequest q;
    while (s.next_request(q)) { cb(q); s.on_published(q.tx, q.tz, q.rung); }
}
static void service_all(SectorStreamer& s) { service_all(s, [](const SectorRequest&){}); }

// Settle: update+service until quiescent; RETURNS all evictions drained on
// the way (callers assert on them — do not discard silently).
static std::vector<Eviction> settle(SectorStreamer& s, float x, float z) {
    std::vector<Eviction> evs;
    for (int i = 0; i < 10000; ++i) {           // ~8 publishes per update
        s.update(x, z);
        SectorRequest q;
        bool any = false;
        while (s.next_request(q)) { any = true; s.on_published(q.tx, q.tz, q.rung); }
        auto e = s.take_evictions();
        evs.insert(evs.end(), e.begin(), e.end());
        if (!any) break;
    }
    return evs;
}

int main() {
    Config cfg;   // defaults: rings 48/120/300/800, hysteresis 16, inflight 8

    // --- desired rung by distance -------------------------------------------
    {
        SectorStreamer s(cfg);
        // Fill everything, verifying known probe sectors were requested at the
        // right final rung (last request seen per probe wins).
        int rung_00 = -1, rung_40 = -1, rung_10_0 = -1, rung_30_0 = -1;
        bool saw_60_0 = false;
        for (int i = 0; i < 10000; ++i) {
            s.update(8.0f, 8.0f);        // camera at centre of sector (0,0)
            bool any = false;
            service_all(s, [&](const SectorRequest& q) {
                any = true;
                if (q.tx == 0  && q.tz == 0) rung_00   = q.rung;
                if (q.tx == 4  && q.tz == 0) rung_40   = q.rung;   // centre dist ~64
                if (q.tx == 10 && q.tz == 0) rung_10_0 = q.rung;   // ~160
                if (q.tx == 30 && q.tz == 0) rung_30_0 = q.rung;   // ~480
                if (q.tx == 60 && q.tz == 0) saw_60_0  = true;     // ~960: never
            });
            s.take_evictions();
            if (!any) break;
        }
        CHECK(rung_00 == 3,   "sector under camera -> rung 3");
        CHECK(rung_40 == 2,   "dist ~64 -> rung 2");
        CHECK(rung_10_0 == 1, "dist ~160 -> rung 1");
        CHECK(rung_30_0 == 0, "dist ~480 -> rung 0");
        CHECK(!saw_60_0,      "beyond outer ring: never requested");
    }
    // --- inflight cap + holes-before-upgrades --------------------------------
    {
        SectorStreamer s(cfg);
        s.update(8.0f, 8.0f);
        SectorRequest q;
        int got = 0;
        while (s.next_request(q)) ++got;
        CHECK(got == 8, "max_inflight caps outstanding requests");
        CHECK(s.inflight_count() == 8, "inflight_count tracks");
    }
    // --- rung swap: publish new THEN evict old --------------------------------
    {
        SectorStreamer s(cfg);
        settle(s, 8.0f, 8.0f);
        // Move camera so sector (0,0) drops from rung 3 into the rung-2 band.
        auto ev = settle(s, 8.0f + 100.0f, 8.0f);   // (0,0) now at dist ~100
        bool evicted_00_r3 = false;
        for (auto& e : ev) if (e.tx == 0 && e.tz == 0 && e.rung == 3) evicted_00_r3 = true;
        CHECK(evicted_00_r3, "old rung evicted after demotion swap");
    }
    // --- hysteresis: small camera moves don't churn ---------------------------
    {
        SectorStreamer s(cfg);
        settle(s, 8.0f, 8.0f);
        s.update(8.0f + 8.0f, 8.0f);      // move less than hysteresis
        auto ev = s.take_evictions();
        CHECK(ev.empty(), "no eviction within hysteresis");
    }
    // --- late publish rejected ------------------------------------------------
    {
        SectorStreamer s(cfg);
        s.update(8.0f, 8.0f);
        SectorRequest q;
        CHECK(s.next_request(q), "got a request");
        s.update(8.0f + 5000.0f, 8.0f);   // camera long gone
        CHECK(!s.on_published(q.tx, q.tz, q.rung), "stale publish rejected");
    }
    // --- fail cooldown ---------------------------------------------------------
    {
        Config c2 = cfg; c2.fail_cooldown_updates = 10; c2.max_inflight = 1;
        SectorStreamer s(c2);
        s.update(8.0f, 8.0f);
        SectorRequest q;
        CHECK(s.next_request(q), "first request");
        int64_t fx = q.tx, fz = q.tz; int fr = q.rung;
        s.on_failed(fx, fz, fr);
        bool re_requested_early = false;
        for (int i = 0; i < 9; ++i) {
            s.update(8.0f, 8.0f);
            if (s.next_request(q)) {
                if (q.tx == fx && q.tz == fz && q.rung == fr) re_requested_early = true;
                s.on_published(q.tx, q.tz, q.rung);   // keep the queue moving
                s.take_evictions();
            }
        }
        CHECK(!re_requested_early, "failed sector cools down");
        bool re_requested_later = false;
        for (int i = 0; i < 5000 && !re_requested_later; ++i) {
            s.update(8.0f, 8.0f);
            if (s.next_request(q)) {
                if (q.tx == fx && q.tz == fz && q.rung == fr) re_requested_later = true;
                else { s.on_published(q.tx, q.tz, q.rung); s.take_evictions(); }
            }
        }
        CHECK(re_requested_later, "failed sector retried after cooldown");
    }
    // --- clear(): everything evicts, stale publishes rejected -----------------
    {
        SectorStreamer s(cfg);
        settle(s, 8.0f, 8.0f);
        s.take_evictions();
        size_t res = s.resident_count();
        CHECK(res > 0, "resident before clear");
        s.clear();
        CHECK(s.resident_count() == 0, "clear empties residency");
        CHECK(s.take_evictions().size() == res, "clear evicts everything");
    }
    // --- long flight: bounded residency, no monotonic growth ------------------
    {
        SectorStreamer s(cfg);
        size_t peak = 0;
        for (int step = 0; step < 500; ++step) {
            float x = 8.0f + step * 10.0f;   // 5,000 units of flight
            s.update(x, 8.0f);
            service_all(s);
            s.take_evictions();
            peak = std::max(peak, s.resident_count());
        }
        // Outer ring disc: pi*800^2 / 256 ~ 7,854 sectors. Allow slack for
        // hysteresis + square scan, reject unbounded growth.
        CHECK(peak < 9500, "resident bounded during long flight");
        settle(s, 8.0f + 500 * 10.0f, 8.0f);
        s.take_evictions();
        size_t at_end = s.resident_count();
        CHECK(at_end < 9500, "no leak after flight");
        printf("  long flight: peak=%zu end=%zu\n", peak, at_end);
    }
    return check_summary();
}
```

- [ ] **Step 2: Add `run-sectorstream` target** (pure CPU link, follow `run-refinectl`'s pattern). Run — Expected: FAIL.

- [ ] **Step 3: Implement `sector_streamer.cpp`.** Suggested internal state:

```cpp
struct SectorState {
    int resident_rung = -1;   // -1 = nothing resident
    int inflight_rung = -1;   // -1 = no request outstanding
    int desired_rung  = -1;   // recomputed each update(); -1 = not desired
    float dist = 0.0f;        // camera distance at last update
    int cooldown = 0;         // updates remaining before re-request allowed
};
std::unordered_map<uint64_t, SectorState> sectors_;
std::vector<Eviction> evictions_;
int inflight_ = 0;
```

`update()`: decrement cooldowns; scan the sector square covering the outer radius; set `desired_rung` per ring table; for resident sectors outside (ring radius + hysteresis) of their CURRENT rung's ring, if `desired_rung == -1` move to evictions and erase, else leave for the swap path. Erase non-resident, non-inflight entries that fall out of range (prevents map growth — the long-flight test enforces this).
`next_request()`: two passes over `sectors_` (holes first, then upgrades/demotions where `desired != resident`), pick nearest by `dist`, skip cooldown > 0 or inflight, respect `max_inflight`.
`on_published()`: if the entry is missing or `desired_rung != rung` → return false (and clear its inflight mark if set). Otherwise: if previously resident at another rung, push that as an eviction; set `resident_rung = rung`, clear inflight, return true.
`clear()`: push every resident as an eviction; wipe the map and inflight count (subsequent `on_published` finds no entry → returns false).

- [ ] **Step 4: Run** — `make -C MatterEngine3/tests run-sectorstream` — Expected: ALL PASS. If the long-flight test is slow (full-map scans per update), bound `update()`'s scan by only touching sectors in the camera square plus currently-tracked entries — the test doubles as the perf gate (must finish in seconds).

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/sector_streamer.h MatterEngine3/src/sector_streamer.cpp \
        MatterEngine3/tests/sector_streamer_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat(stream): SectorStreamer — ring-based desired set, rung swaps, hysteresis, eviction"
```

---

### Task 9: Engine wiring — world-kind load path, streaming worker loop, publish/evict

The integration task: the kernel learns to open a `world`-kind manifest, bind the field, install the sector asset set once, and drive `SectorStreamer` from the worker loop (replacing the Phase C refine loop for world-kind sessions). Closed-world (`expand`/`tileset`) sessions keep their existing path untouched.

**Files:**
- Modify: `MatterEngine3/src/matter_engine.cpp` (facade: load path, worker loop, publish/evict, new accessors)
- Modify: `MatterEngine3/src/provider/local_provider.h/.cpp` (world load: `eval_world`, field construction, asset install, sector bake entry)
- Modify: `MatterEngine3/include/matter/world_session.h` (public additions below)
- Modify: `MatterEngine3/include/matter/events.h` (comment-only: document reuse for sectors)
- Create: `MatterEngine3/tests/world_stream_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile` (`run-worldstream`, api-tests style — GPU, run with `GALLIUM_DRIVER=d3d12`)

**Interfaces:**
- Consumes: `WorldEvalResult`/`eval_world` + `world_module()` (Task 4); `terrain_field::FieldRuntime` (Task 3); `WorldBinding` + `HostBaker::set_world` (Task 5); `WorldSector.js` (Task 7); `SectorStreamer` (Task 8); transient routing (`set_transient_modules`/`release_transient`/`PartStore::set_scratch_dir`, Task 2); existing `ensure_part_baked`/`ensure_part_flattened` (`local_provider.h:93-100`), `PartStore::get_or_load` + `release` (`part_store.h:100`), `GpuCuller::release_part` (`gpu_culler.h:51`), `state.apply(WorldDelta)`, `set_bake_focus` (`world_session.h:81`).
- Produces (public API additions):

```cpp
// world_session.h
struct FrameStats {
    // ... existing fields ...
    uint32_t resident_sectors = 0;   // world-kind sessions only; 0 otherwise
};
class WorldSession {
    // ... existing ...
    // World-kind sessions: the sea level from the world definition.
    // Returns false for closed-world sessions (no water plane).
    bool sea_level(float& out) const;
};
```

**Binding contract:**

1. **Load path** (where the manifest is parsed and roots installed): if `world_module()` is non-empty →
   - `eval_world(world_source, root_params_override_json)` (the same override `regenerate()` stores — so a reroll re-evals the field with the new `worldSeed`). Failure → `BakeError` (fail-closed, render no-ops — same semantics as today).
   - `FieldProgram::parse(r.field_program)` → construct `FieldRuntime` (owned by the provider; lives as long as the session generation). `fieldHash` = 16-hex-digit string of `FieldRuntime::hash()`.
   - `HostBaker::set_world({&field, r.sector_size, r.y_min, r.y_max})`.
   - `set_transient_modules({"WorldSector"})` (Task 2 — sector artifacts go to scratch, never the parts cache).
   - **Asset install:** `eval_requires(WorldSector source, defaults)` → the fixed 28-variant list → bake/flatten each via the existing `ensure_part_baked`/`ensure_part_flattened` install path (these DO go to the parts cache and are the bulk of a cold start; warm relaunch hits cache). Compute and retain the child-hash map used for every sector bake's `BakeInputs.child_hashes`.
   - Emit `BakeStarted` when the world load begins.
2. **Worker loop** (world-kind replaces the refine step):
   - Each pass: read the latest bake-focus position (already plumbed via `set_bake_focus`; the explorer calls it every frame) → `streamer.update(focus.x, focus.z)`.
   - Drain up to `max_inflight` via `next_request` → for each, build `BakeInputs` for `WorldSector` with params `{tx, tz, rung, worldSeed, fieldHash, biomes: biomes_json}` and the retained child-hash map → bake (transient scratch artifact) → flatten.
   - On bake success → hand to the GL-side publish queue. On the GL thread (`pump_gpu_jobs`): if `streamer.on_published(tx,tz,rung)` → `get_or_load(hash)` + `state.apply(WorldDelta{added: [{instance_id, part_hash, transform = translate(tx*S, 0, tz*S), module: "WorldSector"}]})`; else discard: `release_transient(hash)` without publishing.
   - On bake failure → `streamer.on_failed(...)` + emit `BakeError` (non-fatal for the session: other sectors continue — this differs from closed-world fail-closed; the world stays up, the failed sector cools down and retries).
   - Then drain `take_evictions()` → for each: `state.apply(WorldDelta{removed: [instance_id]})`, `PartStore::release(hash)`, `GpuCuller::release_part(hash)`, `release_transient(hash)`. (Track `(tx,tz,rung) → {hash, instance_id}` in a provider-side map added at publish, erased at evict.)
   - **`BakeFinished`** fires once per world load: when every sector desired at the FIRST `update()` after install has been published at least at rung 0 (i.e. the initial ring is hole-free). Later rung swaps emit `RefineTileDone` events exactly like the old refine loop (reuse the event unchanged: `done` = current resident count, `total` = current desired count).
3. **Instance ids:** allocate from the existing instance-id source used by the publish pipeline; never reuse a live id.
4. **`regenerate(seed)`**: existing supersession semantics; on the new generation, `streamer.clear()` runs first, all evictions are processed (releases + transient unlink), THEN the world re-evals (step 1) with the new seed. Sector params carry the new `worldSeed` and new `fieldHash` → all-new part hashes; asset variants (seed-free params) hit cache.
5. **`sea_level(float&)`**: returns the `seaLevel` directive value from the world's `FieldRuntime`; false for closed worlds.
6. **`FrameStats.resident_sectors`**: updated each `tick()` from `streamer.resident_count()`.

- [ ] **Step 1: Write the failing test** — `world_stream_tests.cpp`, api-tests style (real GL window via the same fixture `api-tests` uses; `GALLIUM_DRIVER=d3d12`). Create a minimal world fixture under `tests/` (do NOT depend on Task 10's MeadowWorld): a `world.manifest` containing `TestWorld world`, a `TestWorld.js` schema (copy the Task 4 test world class verbatim), plus `WorldSector.js` and the asset schemas symlinked/copied per existing test-fixture conventions. Assertions:

```cpp
// MatterEngine3/tests/world_stream_tests.cpp  — structure sketch (bind to the
// api-tests fixture exactly; these are the REQUIRED assertions)
// 1. open_world(world-kind fixture) + request_bake -> poll until BakeFinished
//    (budget 120s): CHECK no BakeError events with fatal semantics; CHECK
//    frame_stats().resident_sectors > 0.
// 2. sea_level(): CHECK true and value == 0.0f (fixture's seaLevel).
// 3. render() one frame at a camera inside the world: CHECK
//    frame_stats().triangles > 0 (terrain visible).
// 4. Move bake focus +200 units; tick/pump in a loop for up to 60s: CHECK
//    resident_sectors changes and RefineTileDone events arrive (streaming
//    follows focus).
// 5. regenerate(7): poll to BakeFinished again; CHECK resident_sectors > 0.
//    CHECK the parts cache dir gained no WorldSector artifacts (transient
//    policy holds): scan parts_dir before/after for file-count delta == 0
//    beyond the asset installs.
// 6. Destroy the session: CHECK the scratch dir /tmp/matter_transient/<pid>
//    is removed (or empty) — no transient leaks.
```

- [ ] **Step 2: Run it** — Expected: FAIL (world-kind manifest rejected / no streaming).

- [ ] **Step 3: Implement** per the binding contract above, in this order: (a) public header additions, (b) load path in the provider, (c) worker-loop streaming, (d) GL-side publish/evict, (e) `regenerate` ordering, (f) stats + `sea_level`. Keep the closed-world path byte-for-byte untouched — every new branch is gated on `world_module()` being non-empty.

- [ ] **Step 4: Run** — `GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-worldstream` — Expected: ALL PASS.
Covering regression (closed world untouched): `GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-api-tests run-demandbake run-refineloop` — Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/include/matter/world_session.h MatterEngine3/include/matter/events.h \
        MatterEngine3/src/matter_engine.cpp MatterEngine3/src/provider/local_provider.h \
        MatterEngine3/src/provider/local_provider.cpp \
        MatterEngine3/tests/world_stream_tests.cpp MatterEngine3/tests/Makefile \
        MatterEngine3/tests/<fixture files>
git commit -m "feat(engine): world-kind sessions — field binding, sector streaming worker loop, publish/evict"
```

---

### Task 10: `MeadowWorld` definition + explorer integration (water plane, HUD)

**Files:**
- Create: `MatterEngine3/examples/world_demo/schemas/MeadowWorld.js`
- Create: `MatterEngine3/examples/world_demo/WorldData/MeadowWorld/world.manifest`
- Modify: `ExplorerDemo/main.cpp` (world name, water plane, sea-level floor for the camera)
- Modify: `ExplorerDemo/hud.cpp` (resident-sector count line)

**Interfaces:**
- Consumes: everything landed in Tasks 1–9. Produces: the shipping demo world.

- [ ] **Step 1: Write `MeadowWorld.js`**

```js
// The Meadow — infinite world definition. Declares the terrain field graph
// and the per-biome scatter table; enumerates nothing. Sectors stream in
// around the camera (WorldSector.js) and are baked transiently on demand.
class MeadowWorld extends World {
  static params = { worldSeed: 20260709 };
  static world  = { sectorSize: 16, yMin: -64, yMax: 192 };
  static biomeThresholds = { mountRelief: 0.65, rockyMoisture: 0.35 };

  field(p) {
    // Relief picks plains vs mountains; moisture picks lush vs rocky.
    const relief   = noise2(p.worldSeed ^ 1, 1/900, 3);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 3);
    const plains   = noise2(p.worldSeed ^ 3, 1/160, 4).mul(8);
    const mounts   = ridge2(p.worldSeed ^ 4, 1/340, 5).mul(110);
    const height   = blend(plains, mounts, relief.smoothstep(0.45, 0.75)).add(-6);
    return { density: heightToDensity(height), moisture, relief, seaLevel: 0.0 };
  }

  biomes() {
    // Per-sector (16x16) scatter counts, matched to the old Meadow densities.
    return {
      meadow:    { grass: 156, pebbles: 16, rocks: 2, trees: true },
      foothills: { grass: 39,  rocks: 2 },
      mountains: { rocks: 1 },
      ocean:     {},
    };
  }
}
```

- [ ] **Step 2: Write the manifest**

```
# MeadowWorld: infinite streamed world (Phase C redesign). The `world` kind
# binds the terrain field and streams WorldSector columns around the camera;
# nothing is enumerated up front and sector artifacts are transient.
MeadowWorld world
```

- [ ] **Step 3: Point the explorer at it** — in `ExplorerDemo/main.cpp` set `desc.world_name = "MeadowWorld"`. Delete the Meadow-specific refine/`set_bake_focus` scaffolding ONLY if it was Meadow-specific — `set_bake_focus(cam.position)` per frame stays (it drives the streamer).

- [ ] **Step 4: Water plane** — after `sea_level(&sl)` returns true, draw a camera-following alpha quad at `y = sl` each frame after `session.render(...)`: a single large quad (side ≥ 2× outer ring = 1600, centered on camera x/z), color `{30, 90, 140, 140}`, `rlDisableBackfaceCulling` for it (visible from below), drawn with raylib immediate mode (`DrawTriangle3D`/`rlBegin`) inside the existing 3D pass. Camera floor: clamp fly camera `y >= sl + 0.5` so you can't fly under the world.

- [ ] **Step 5: HUD** — add one line to `hud.cpp`: `sectors: <frame_stats().resident_sectors>` next to the existing triangle/instance counters.

- [ ] **Step 6: Verify in-viewer (the golden path)** — `make -C ExplorerDemo && (cd ExplorerDemo && GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=120,shot=/tmp/meadowworld_first.png" ./explorer)`. Review the shot: terrain visible, water plane at the horizon, HUD sector count > 0, no black frame. This is a REQUIRED visual gate — do not skip; attach findings to the report.

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/MeadowWorld.js \
        MatterEngine3/examples/world_demo/WorldData/MeadowWorld/world.manifest \
        ExplorerDemo/main.cpp ExplorerDemo/hud.cpp
git commit -m "feat(demo): MeadowWorld infinite world definition; explorer water plane + sector HUD"
```

---

### Task 11: Retire closed-Meadow gates; endless-flight smoke

**Files:**
- Modify: `MatterEngine3/tests/Makefile` + the sweep driver (`build-all.sh test` path) — remove `run-meadow` / `run-meadow-check` / `valley-layout-tests` / `run-valley` from the sweep (the 51×51 closed Meadow is superseded; DELETE the test sources and targets rather than skip-flagging them — `git log` keeps them recoverable)
- Delete: `MatterEngine3/tests/<the meadow_bake_check / valley layout test sources>` (identify by what `run-meadow-check`/`run-valley` compile)
- Keep: `Meadow.js`/`Terrain.js` schemas and the `Meadow` WorldData entry (still loadable manually; just no longer a gate)
- Modify: `ExplorerDemo/main.cpp` (smoke mode gains `fly=dx,dz,speed`)
- Create: `ExplorerDemo/tools/flight_smoke.sh`

**Binding contract:**
- `EXPLORER_SMOKE="secs=180,fly=1,0,40"` → after the world is ready, the smoke camera flies in direction `(dx,dz)` (normalized) at `speed` units/sec for the remaining duration, camera at cruise height `sea+25` or terrain-following (whichever the rig already supports — reuse `staged_camera`/`camera_rig`; do not build a new rig).
- `flight_smoke.sh`: launches the explorer with a fly smoke (self-terminating, log-teed like `time_to_flying.sh`), then asserts from the log: **zero** `BakeError` lines, `fps_summary` present, final HUD/stat line shows `resident_sectors` below 9500 (bounded), and the run reached the full duration (no crash). Non-zero exit on any violation.

- [ ] **Step 1: Add `fly=` smoke parsing + motion** (main.cpp, ~30 lines, mirroring the existing `secs=`/`shot=` parsing).
- [ ] **Step 2: Write `flight_smoke.sh`** (copy `time_to_flying.sh`'s run_smoke/log plumbing; single run; the four assertions above as greps with clear failure echoes).
- [ ] **Step 3: Run it** — `GALLIUM_DRIVER=d3d12 bash ExplorerDemo/tools/flight_smoke.sh` — Expected: PASS (exit 0). This is the endless-flight exit criterion made executable.
- [ ] **Step 4: Retire the closed-world gates** — remove from sweep + delete sources/targets; run `./build-all.sh test 2>&1 | tail -30` and confirm the sweep is green with the retired entries gone (run-viewer-logic and other known-failing entries from the pre-redesign sweep must be triaged: anything failing that this branch touched gets fixed, anything pre-existing gets listed in the report).
- [ ] **Step 5: Commit**

```bash
git add -A MatterEngine3/tests ExplorerDemo/main.cpp ExplorerDemo/tools/flight_smoke.sh build-all.sh
git commit -m "test: endless-flight smoke gate; retire closed-Meadow bake gates"
```

---

### Task 12: Final gates — wall-clock, Windows build, full sweep

**Files:**
- Modify: `ExplorerDemo/tools/time_to_flying.sh` (gate text: cold AND warm targets now both ≤60s — the redesign removes the install wall; keep `t_ready`/`t_silhouette` extraction as-is; `BakeFinished` now means "initial ring resident")
- No other source changes expected — this task runs gates and fixes only what they surface.

- [ ] **Step 1: Wall-clock gate** — `GALLIUM_DRIVER=d3d12 bash ExplorerDemo/tools/time_to_flying.sh`. Targets: cold `t_silhouette` ≤ 60s (the asset install is ~28 parts, not 5,203), warm ≤ 60s. Record both in the report; final adjudication by Jack.
- [ ] **Step 2: Reroll gate** — in a live explorer run, Escape → new seed; terrain must re-stream (visibly different) with asset cache hits (watch the log for `parts_baked` vs `cache_hits`). Use the FIFO driver (`MATTER_CMD_FIFO`) or the menu path per `feedback_viewer_test_lifecycle` — self-terminating.
- [ ] **Step 3: Windows build** — clear ALL object files first (struct/header changes landed: `BakeOptions`, `DslState`, `FrameStats`), then `make -C MatterViewer windows` and the ExplorerDemo Windows target; confirm both link. (Global constraint: stale objects after header changes cause wandering crashes.)
- [ ] **Step 4: Full sweep** — `GALLIUM_DRIVER=d3d12 ./build-all.sh test 2>&1 | tail -40`. Expected: green (with Task 11's retirements applied). Any red that this branch caused → fix and re-run; pre-existing reds → list in the report for adjudication.
- [ ] **Step 5: Commit** any gate-driven fixes; report cold/warm numbers, reroll observation, sweep tail, and the two screenshots (`/tmp/explorer_cold_run1.png`, `/tmp/meadowworld_first.png`).

---

## Execution Order & Model Notes (for the SDD controller)

Dependency chain: 1 → (2, 3 independent) → 4 → 5 → 6 → 7 → 8 (independent of 4–7, needs nothing) → 9 → 10 → 11 → 12. Tasks 2, 3 and 8 can slot anywhere after Task 1 before their consumers; execute serially regardless (no parallel implementers).

Suggested models: Tasks 1, 2, 6 mechanical-with-complete-code (cheap tier); Tasks 3, 5, 7, 8, 11 complete-spec but multi-file or algorithmic (mid tier); Tasks 4, 9, 10, 12 integration/judgment (capable tier; Task 9 is the riskiest — most capable available).

## Verification Summary

Exit criteria → gates: instant world (Task 12 wall-clock ≤60s cold+warm) · endless flight (Task 11 flight_smoke.sh, zero BakeError, bounded residency) · detail follows camera (Task 9 world_stream test §4 + Task 8 streamer tests) · reroll (Task 12 step 2 + Task 9 test §5) · stable memory (Task 8 long-flight bound + Task 9 test §6 transient cleanup).

