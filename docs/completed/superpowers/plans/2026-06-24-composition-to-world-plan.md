# Composition to World (SP-4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bake ~3 decimated LOD BLAS levels per part (tagged with a `screen_size_threshold` and round-tripped through SP-1 `save_v2`/`load_v2`), recursively flatten SP-1 child-instance tables into a flat `(resolved_hash, world transform)` world list with depth/budget guards, bin those instances into a static sector grid, and select a sector-uniform LOD by the closest instance's projected screen size.

**Architecture:** Four GL-free CPU modules layered bottom-up: (1) `lod_bake` decimates a BLAS's `Tri` set via `mesh_simplifier` into N levels with monotone target ratios, assigns each a `screen_size_threshold`, and emits an SP-1 `LodLevels` written back into the `.part`. (2) `world_flatten` walks a part-graph (a map of resolved_hash to `ChildInstance` rows) depth-first, composing `world = parent_world * child.transform` via `mat4`, enforcing max depth and a total instance budget as hard errors. (3) `sector_grid` bins each flat instance's world position into a fixed-pitch integer cell, deterministic on boundaries via floor. (4) `lod_select` computes each instance's projected screen size (`bound_radius / distance`, a defined normalized metric) and picks, per sector, the LOD band driven by the sector's closest instance. Variation is orthogonal: it only changes which resolved_hash a leaf points at; LOD bands and selection are identical across variations.

**Tech Stack:** C++17, mesh_simplifier (consumed from MatterSurfaceLib), BLAS/TLAS managers (consumed from MatterSurfaceLib), SP-1 part_asset v2 (new in MatterEngine3), headless tests under MatterEngine3/tests/.

> **Relocation note (from the master plan):** This sub-plan obeys the `MatterEngine3` relocation contract in `2026-06-24-procedural-part-system-master-plan.md`. All NEW files (`lod_bake`, `world_flatten`, `sector_grid`, `lod_select`, tests) live under `MatterEngine3/`; the consumed prototype backend (`mesh_simplifier.cpp`, `blas_manager.cpp`, `bvh.cpp`, `vertex_ao.cpp`, `occupancy.cpp`, `material_registry.c`) and SP-1's `part_asset.cpp` (v1) are referenced read-only as `../../MatterSurfaceLib/src/<dep>` with `-I../../MatterSurfaceLib/include`. SP-1's NEW `part_asset_v2.{h,cpp}` live in `MatterEngine3/`. raylib paths are unchanged (`MatterEngine3/tests` is the same depth as `MatterSurfaceLib/tests`).

---

## Interface notes (confirmed against source)

- `Mesh simplify_mesh(const Mesh& input, const SimplifyOptions& opts, const CellBounds* bounds = nullptr)` — **confirmed** at `MatterSurfaceLib/include/mesh_simplifier.hpp`. `SimplifyOptions { float target_ratio=0.5 (fraction of tris to KEEP, 0..1]; float max_error=FLT_MAX; bool lock_boundary=true; }`. Returns a NEW indexed raylib `Mesh` (MemAlloc'd; free with `UnloadMesh`); zeroed `Mesh` (vertexCount==0) on degenerate input. Does not mutate input. Works on raylib `Mesh` (`vertices` float[3*N], `indices` ushort, `triangleCount`), **not** on `Tri`. The bake module therefore needs a `Tri[]`↔`Mesh` bridge.
- BLAS geometry is stored as `std::vector<Tri>` per `BLASManager::BLASEntry` (see `blas_manager.hpp`); `Tri` is `vertex0/1/2 + centroid` (`bvh.h`). `register_triangles(Tri*, int, const TriEx*)` builds a BLAS and returns a `BLASHandle`.
- `mat4` (`bvh.h`) is row-major `float cell[16]`, has `operator()(i,j)`, `TransformPoint`, `Inverted`. `ChildInstance.transform` is `float[16]` row-major (SP-1). I will add a `mat4 mat4_mul(const mat4&, const mat4&)` helper (none exists) and a `float[16]`↔`mat4` bridge.
- SP-1 `save_v2`/`load_v2` and `struct ChildInstance { uint64_t child_resolved_hash; float transform[16]; }` and `LodLevels` are per the SP-1 spec (`docs/superpowers/specs/2026-06-24-part-artifact-v2-design.md`). They may not exist at execution time. Where SP-1 is absent, depend on the spec interface; the in-memory `LodLevels` shape below matches SP-1's "ordered level array, each level = `screen_size_threshold` + `blas_index[]`".
- Test convention (the prototype's `MatterSurfaceLib/tests/part_asset_tests.cpp`): a `static int failures` + `CHECK(cond, msg)` macro, `main` returns `failures ? 1 : 0`. The MatterEngine3 tests Makefile gets a `composition_tests` TARGET/SOURCES + `run-comp` rule appended (the Makefile already exists — SP-1 created it).

### Screen-size metric (defined concretely — not abstract)

For an instance with world-space bounding sphere radius `r` and camera at `cam_pos`, with the instance world center `c`:

```
distance        = length(c - cam_pos)                 // clamp to >= 1e-4
projected_size   = r / distance                        // normalized angular extent (radians-ish)
```

This is the tangent-approximation of angular radius (`r/d` ≈ `tan(theta)` for small theta), independent of viewport resolution and FOV, which is sufficient and deterministic for selection. `screen_size_threshold` values are compared on the SAME scale: a level is "satisfied" when `projected_size >= level.screen_size_threshold`. LOD0 (full detail) has the LARGEST threshold; LOD2 (coarsest) the smallest. Selection picks the **coarsest** level whose threshold is satisfied (`projected_size >= threshold`), i.e. iterate levels from coarse→fine and take the first that the size clears; if size clears even the finest, use the finest. A camera moving closer raises `projected_size`, escalating to a finer level.

---

## File Structure

```
MatterEngine3/
  include/
    lod_bake.h          # LodLevels (in-memory), decimate a BLAS Tri-set into N levels + thresholds
    world_flatten.h     # PartGraph, FlatInstance, flatten() with depth/budget guards
    sector_grid.h       # SectorGrid, sector_of(pos), bin_instances()
    lod_select.h        # projected_size(), select_sector_lods()
  src/
    lod_bake.cpp
    world_flatten.cpp
    sector_grid.cpp
    lod_select.cpp
  tests/
    composition_tests.cpp
    Makefile            # APPEND composition_tests target + run-comp (already exists from SP-1)
```
(Consumed read-only from `MatterSurfaceLib/`: `mesh_simplifier.{hpp,cpp}`, `blas_manager.{hpp,cpp}`, `bvh.{h,cpp}`, `vertex_ao.cpp`, `occupancy.cpp`, `material_registry.c`, and `part_asset.{h,cpp}` (v1). SP-1's `part_asset_v2.{h,cpp}` are new under `MatterEngine3/`.)

`lod_bake.h` defines the in-memory `LodLevels` used across SP-4 (matching SP-1's on-disk shape) so the modules are testable even before SP-1 lands:

```cpp
// lod_bake.h
struct LodLevel {
    float screen_size_threshold;          // selection metric scale (see plan)
    std::vector<uint32_t> blas_index;     // indices into the part's BLAS table
};
using LodLevels = std::vector<LodLevel>;  // ordered LOD0..LODn (fine -> coarse)
```

If SP-1 ships its own `LodLevels`, replace this typedef with `#include "part_asset_v2.h"` (SP-1's NEW MatterEngine3 header, via `-I../include`) and delete the local definition; the field names match by design. Note SP-1's `LodLevel` uses a `blas_indices` member, while SP-4's local mirror uses `blas_index` — reconcile at the swap.

---

## Task 1 — `Tri[]` ↔ raylib `Mesh` bridge + single-level decimate

**Files:**
- `MatterEngine3/include/lod_bake.h`
- `MatterEngine3/src/lod_bake.cpp`
- `MatterEngine3/tests/composition_tests.cpp`
- `MatterEngine3/tests/Makefile`

- [ ] **Append the test target to the MatterEngine3 tests Makefile.** The Makefile already exists (SP-1 created `MatterEngine3/tests/Makefile`). Append this block, and add `run-comp` + `$(COMP_TARGET)` to the `.PHONY`/`clean` lines. New impl sources are `../src/<new>.cpp`; consumed prototype sources are `../../MatterSurfaceLib/src/<dep>.cpp`:
  ```make
  # Composition-to-world (SP-4): LOD bake + flatten + sector grid + LOD select.
  # Headless, GL-free CPU steps. raylib linked only for MemAlloc/MemFree in the
  # Tri<->Mesh bridge. NEW impl ../src/*.cpp + consumed MatterSurfaceLib backend.
  COMP_TARGET = composition_tests
  COMP_SOURCES = composition_tests.cpp ../src/lod_bake.cpp ../src/world_flatten.cpp \
                 ../src/sector_grid.cpp ../src/lod_select.cpp \
                 ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
                 ../../MatterSurfaceLib/src/blas_manager.cpp \
                 ../../MatterSurfaceLib/src/bvh.cpp \
                 ../../MatterSurfaceLib/src/vertex_ao.cpp \
                 ../../MatterSurfaceLib/src/occupancy.cpp

  $(COMP_TARGET): $(COMP_SOURCES)
  	$(CC) $(COMP_SOURCES) -o $(COMP_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

  run-comp: $(COMP_TARGET)
  	./$(COMP_TARGET)
  ```
  Update `.PHONY` to include `run-comp` and `clean` to `rm -f ... $(COMP_TARGET)`.

- [ ] **Write the failing test** (`composition_tests.cpp`): create the harness + a test that decimating a subdivided grid keeps fewer triangles. Real code:
  ```cpp
  #include "../include/lod_bake.h"
  #include "../../MatterSurfaceLib/include/blas_manager.hpp"
  #include <cstdio>
  #include <cstdint>
  #include <vector>

  static int failures = 0;
  #define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

  // Build a flat NxN quad grid (2 tris per cell) as a Tri vector in [0,1]^2.
  static std::vector<Tri> grid_tris(int n) {
      std::vector<Tri> out;
      float step = 1.0f / n;
      for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
          float x0 = x*step, y0 = y*step, x1 = x0+step, y1 = y0+step;
          Tri a; a.vertex0 = make_float3(x0,y0,0); a.vertex1 = make_float3(x1,y0,0);
          a.vertex2 = make_float3(x1,y1,0); a.centroid = make_float3((x0+2*x1)/3,(2*y0+y1)/3,0);
          Tri b; b.vertex0 = make_float3(x0,y0,0); b.vertex1 = make_float3(x1,y1,0);
          b.vertex2 = make_float3(x0,y1,0); b.centroid = make_float3((2*x0+x1)/3,(y0+2*y1)/3,0);
          out.push_back(a); out.push_back(b);
      }
      return out;
  }

  static void test_decimate_one_level() {
      std::vector<Tri> tris = grid_tris(16);           // 512 tris
      std::vector<Tri> half = lod_bake::decimate_tris(tris, 0.5f);
      CHECK(!half.empty(), "decimate produced output");
      CHECK(half.size() < tris.size(), "decimate reduced tri count");
  }

  int main() {
      test_decimate_one_level();
      printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
      return failures ? 1 : 0;
  }
  ```

- [ ] **Run and expect FAIL** (no `lod_bake.h` yet): `make -C MatterEngine3/tests run-comp` → expect compile error (`lod_bake.h: No such file` / `decimate_tris` undeclared).

- [ ] **Write the header** (`include/lod_bake.h`):
  ```cpp
  #pragma once
  #include "bvh.h"            // Tri, make_float3
  #include <cstdint>
  #include <vector>

  namespace lod_bake {

  struct LodLevel {
      float screen_size_threshold;
      std::vector<uint32_t> blas_index;
  };
  using LodLevels = std::vector<LodLevel>;

  // Decimate a Tri set to approximately `keep_ratio` of its triangles via
  // mesh_simplifier (QEM edge collapse). keep_ratio in (0,1]. Returns a NEW Tri
  // vector; input is not mutated. Empty input -> empty output.
  std::vector<Tri> decimate_tris(const std::vector<Tri>& tris, float keep_ratio);

  } // namespace lod_bake
  ```

- [ ] **Write the impl** (`src/lod_bake.cpp`) — the `Tri[]`↔`Mesh` bridge + `simplify_mesh` call:
  ```cpp
  #include "../include/lod_bake.h"
  #include "../../MatterSurfaceLib/include/mesh_simplifier.hpp"
  extern "C" { #include "raylib.h" }
  #include <cstring>

  namespace lod_bake {

  // Pack a Tri vector into a non-indexed raylib Mesh (3 verts per tri).
  static Mesh tris_to_mesh(const std::vector<Tri>& tris) {
      Mesh m{};
      m.triangleCount = (int)tris.size();
      m.vertexCount = (int)tris.size() * 3;
      if (m.vertexCount == 0) return m;
      m.vertices = (float*)MemAlloc(sizeof(float) * 3 * m.vertexCount);
      for (size_t i = 0; i < tris.size(); ++i) {
          const Tri& t = tris[i];
          float* v = m.vertices + i * 9;
          v[0]=t.vertex0.x; v[1]=t.vertex0.y; v[2]=t.vertex0.z;
          v[3]=t.vertex1.x; v[4]=t.vertex1.y; v[5]=t.vertex1.z;
          v[6]=t.vertex2.x; v[7]=t.vertex2.y; v[8]=t.vertex2.z;
      }
      return m;
  }

  // Unpack an indexed-or-not raylib Mesh back into Tri (recompute centroid).
  static std::vector<Tri> mesh_to_tris(const Mesh& m) {
      std::vector<Tri> out;
      auto vert = [&](int idx) {
          return make_float3(m.vertices[idx*3+0], m.vertices[idx*3+1], m.vertices[idx*3+2]);
      };
      auto emit = [&](float3 a, float3 b, float3 c) {
          Tri t; t.vertex0=a; t.vertex1=b; t.vertex2=c;
          t.centroid = make_float3((a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3);
          out.push_back(t);
      };
      if (m.indices) {
          for (int i = 0; i < m.triangleCount; ++i)
              emit(vert(m.indices[i*3+0]), vert(m.indices[i*3+1]), vert(m.indices[i*3+2]));
      } else {
          for (int i = 0; i < m.triangleCount; ++i)
              emit(vert(i*3+0), vert(i*3+1), vert(i*3+2));
      }
      return out;
  }

  std::vector<Tri> decimate_tris(const std::vector<Tri>& tris, float keep_ratio) {
      if (tris.empty()) return {};
      Mesh in = tris_to_mesh(tris);
      SimplifyOptions opts; opts.target_ratio = keep_ratio; opts.lock_boundary = false;
      Mesh out = simplify_mesh(in, opts, nullptr);
      std::vector<Tri> result = (out.vertexCount > 0) ? mesh_to_tris(out) : tris;
      // simplify_mesh allocates with MemAlloc; free both scratch meshes.
      if (in.vertices) MemFree(in.vertices);
      if (out.vertices) MemFree(out.vertices);
      if (out.indices) MemFree(out.indices);
      return result;
  }

  } // namespace lod_bake
  ```
  Note: `lock_boundary=false` so an isolated grid actually collapses (boundary-locked would freeze a flat sheet). If `simplify_mesh` returns the input unchanged for tiny meshes, the grid(16)=512-tri input is large enough to reduce.

- [ ] **Run and expect PASS**: `make -C MatterEngine3/tests run-comp` → `OK`.

- [ ] **Commit**: `lod_bake: Tri<->Mesh bridge + single-level decimate via mesh_simplifier`
  ```
  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
  ```

---

## Task 2 — Multi-level LOD bake: monotone tri counts + thresholds + BLAS registration

**Files:**
- `MatterEngine3/include/lod_bake.h`
- `MatterEngine3/src/lod_bake.cpp`
- `MatterEngine3/tests/composition_tests.cpp`

- [ ] **Write the failing test** — append to `composition_tests.cpp` and call from `main`. Covers the spec's "decimates to 3 levels with monotonically decreasing tri counts". Real code:
  ```cpp
  static void test_bake_three_levels() {
      std::vector<Tri> tris = grid_tris(32);           // 2048 tris (LOD0)
      BLASManager blas;
      lod_bake::BakeTargets t;                         // defaults: {1.0, 0.1, 0.01}
      lod_bake::LodLevels lods = lod_bake::bake_lods(tris, t, blas);

      CHECK(lods.size() == 3, "three LOD levels");
      // Each level registered exactly one BLAS (single-material part).
      CHECK(lods[0].blas_index.size() == 1, "lod0 one blas");
      CHECK(lods[2].blas_index.size() == 1, "lod2 one blas");
      // Tri counts strictly decrease LOD0 -> LOD2.
      auto tri_count = [&](uint32_t bi) {
          return blas.get_entries()[bi]->triangles.size();
      };
      size_t c0 = tri_count(lods[0].blas_index[0]);
      size_t c1 = tri_count(lods[1].blas_index[0]);
      size_t c2 = tri_count(lods[2].blas_index[0]);
      CHECK(c0 > c1 && c1 > c2, "monotonically decreasing tri counts");
      CHECK(c0 == tris.size(), "lod0 is full geometry");
      // Thresholds: LOD0 largest (nearest), LOD2 smallest (farthest).
      CHECK(lods[0].screen_size_threshold > lods[1].screen_size_threshold, "thr0 > thr1");
      CHECK(lods[1].screen_size_threshold > lods[2].screen_size_threshold, "thr1 > thr2");
  }
  ```
  Add `test_bake_three_levels();` to `main`.

- [ ] **Run and expect FAIL**: `make -C MatterEngine3/tests run-comp` → compile error (`BakeTargets`/`bake_lods` undeclared).

- [ ] **Extend the header** (`include/lod_bake.h`) — add before the closing namespace brace:
  ```cpp
  #include "blas_manager.hpp"

  // Per-level decimation targets (keep-ratios) and matching selection thresholds.
  // Defaults: LOD0 = full (1.0), LOD1 ~ 1/10, LOD2 ~ 1/100. Thresholds are on the
  // projected-size scale (bound_radius / distance) used by lod_select: a finer
  // level demands a LARGER projected size to be chosen.
  struct BakeTargets {
      std::vector<float> keep_ratio = {1.0f, 0.1f, 0.01f};
      std::vector<float> threshold  = {0.20f, 0.05f, 0.0125f};
  };

  // Decimate `tris` into N LOD levels (N = BakeTargets size), register each level's
  // geometry as a BLAS in `blas`, and return the LodLevels (each level holds the
  // registered BLAS index + its screen_size_threshold). LOD0 with keep_ratio 1.0 is
  // the full input (no decimation). The returned blas_index values index
  // blas.get_entries() in registration order.
  LodLevels bake_lods(const std::vector<Tri>& tris, const BakeTargets& targets,
                      BLASManager& blas);
  ```

- [ ] **Extend the impl** (`src/lod_bake.cpp`):
  ```cpp
  LodLevels bake_lods(const std::vector<Tri>& tris, const BakeTargets& targets,
                      BLASManager& blas) {
      LodLevels out;
      // entries() index of the next registration == current size (entries only grow here).
      for (size_t lvl = 0; lvl < targets.keep_ratio.size(); ++lvl) {
          float keep = targets.keep_ratio[lvl];
          std::vector<Tri> geo = (keep >= 0.999f) ? tris : decimate_tris(tris, keep);
          if (geo.empty()) geo = tris;     // never register empty geometry
          uint32_t idx = (uint32_t)blas.get_entries().size();
          std::vector<Tri> copy = geo;     // register_triangles takes Tri*
          blas.register_triangles(copy.data(), (int)copy.size(), nullptr);
          LodLevel L;
          L.screen_size_threshold = targets.threshold[lvl];
          L.blas_index.push_back(idx);
          out.push_back(std::move(L));
      }
      return out;
  }
  ```
  Note: monotonicity holds because `keep_ratio` is strictly decreasing and `simplify_mesh` keeps ≈ratio of tris; if a coarser level fails to drop below a finer one for a pathological mesh the test grid(32) is large enough that it will. The plan's targets {1.0,0.1,0.01} guarantee clear separation.

- [ ] **Run and expect PASS**: `make -C MatterEngine3/tests run-comp` → `OK`.

- [ ] **Commit**: `lod_bake: bake N monotone LOD levels with screen-size thresholds`

---

## Task 3 — LOD round-trip through SP-1 `save_v2`/`load_v2`

**Files:**
- `MatterEngine3/tests/composition_tests.cpp`
- `MatterEngine3/include/lod_bake.h` (only if a thin shim is needed)

This task depends on SP-1's `save_v2`/`load_v2`. If SP-1 has NOT landed at execution time, implement the round-trip against an **in-memory serializer** (`lod_bake::serialize_lods`/`deserialize_lods`) so the spec's "round-trip through save_v2/load_v2" is covered structurally; swap to the real `save_v2`/`load_v2` once SP-1 exists (the `LodLevels` field names match by design). Decide which path at execution time; do BOTH steps below for the chosen path.

- [ ] **Write the failing test** — append + call from `main`. Covers spec "each level's BLAS indices + `screen_size_threshold` round-trip" and SP-1's degenerate empty/single-level cases.
  - **If SP-1 present** (`#include "part_asset_v2.h"`):
    ```cpp
    #include "../include/part_asset_v2.h"
    static void test_lod_roundtrip_v2() {
        std::vector<Tri> tris = grid_tris(32);
        BLASManager blas; TLASManager tlas(64);
        lod_bake::LodLevels lods = lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, blas);
        const char* path = "/tmp/sp4_lod_roundtrip.part";
        uint64_t rh = 0xABCDEF1234567890ull;
        bool saved = part_asset::save_v2(path, blas, tlas, nullptr, 0, lods, rh);
        CHECK(saved, "save_v2 ok");
        BLASManager blas2; TLASManager tlas2(64);
        std::vector<part_asset::ChildInstance> children_out;
        part_asset::LodLevels lods_out;
        bool loaded = part_asset::load_v2(path, rh, blas2, tlas2, children_out, lods_out);
        CHECK(loaded, "load_v2 ok");
        CHECK(lods_out.size() == lods.size(), "lod level count round-trips");
        for (size_t i = 0; i < lods.size(); ++i) {
            CHECK(lods_out[i].screen_size_threshold == lods[i].screen_size_threshold, "threshold round-trips");
            // SP-1's LodLevel member is `blas_indices`; SP-4's bake_lods mirror is `blas_index`.
            CHECK(lods_out[i].blas_indices == lods[i].blas_index, "blas indices round-trip");
        }
    }
    // Degenerate cases per SP-1.
    static void test_lod_roundtrip_degenerate() {
        BLASManager blas; TLASManager tlas(8);
        const char* path = "/tmp/sp4_lod_empty.part";
        part_asset::LodLevels empty;
        CHECK(part_asset::save_v2(path, blas, tlas, nullptr, 0, empty, 7), "save empty lods");
        BLASManager b2; TLASManager t2(8);
        std::vector<part_asset::ChildInstance> c; part_asset::LodLevels out;
        CHECK(part_asset::load_v2(path, 7, b2, t2, c, out), "load empty lods");
        CHECK(out.empty(), "empty lods round-trip");
    }
    ```
  - **If SP-1 absent** (in-memory shim): assert `deserialize_lods(serialize_lods(lods)) == lods` byte-for-byte (level count, each threshold, each `blas_index`), plus the empty and single-level cases.

- [ ] **Run and expect FAIL**: `make -C MatterEngine3/tests run-comp` → link/compile error (`save_v2` unresolved, or `serialize_lods` undeclared). If using the SP-1 path, add the NEW `../src/part_asset_v2.cpp` + the consumed `../../MatterSurfaceLib/src/part_asset.cpp` + `material_registry.o` (compiled from `../../MatterSurfaceLib/src/material_registry.c` via gcc) to `COMP_SOURCES`/Makefile, exactly as SP-1's `run-partv2` target does.

- [ ] **Make it pass**: if SP-1 present, no impl needed (SP-1 owns `save_v2`/`load_v2`); just wire the Makefile sources. If SP-1 absent, add `serialize_lods(const LodLevels&) -> std::vector<uint8_t>` and `deserialize_lods(const uint8_t*, size_t) -> LodLevels` to `lod_bake.{h,cpp}` (write `level_count u32`, then per level `screen_size_threshold f32`, `blas_index_count u32`, `blas_index u32[]` — exactly SP-1's on-disk LOD layout).

- [ ] **Run and expect PASS**: `make -C MatterEngine3/tests run-comp` → `OK`.

- [ ] **Commit**: `lod_bake: round-trip LOD levels through SP-1 save_v2/load_v2`

---

## Task 4 — Recursive world flatten with composed transforms

**Files:**
- `MatterEngine3/include/world_flatten.h`
- `MatterEngine3/src/world_flatten.cpp`
- `MatterEngine3/tests/composition_tests.cpp`

- [ ] **Write the failing test** — covers spec "a 2-level child graph (parent with N children, each with M grandchildren) flattens to N*M leaf instances with correctly composed world transforms". Real code:
  ```cpp
  #include "../include/world_flatten.h"

  // Identity-ish translate matrix, row-major float[16].
  static void set_translate(float m[16], float x, float y, float z) {
      for (int i=0;i<16;++i) m[i]=0; m[0]=m[5]=m[10]=m[15]=1; m[3]=x; m[7]=y; m[11]=z;
  }

  static void test_flatten_n_times_m() {
      using namespace world_flatten;
      // Hashes: root=1, mid=2 (the N children part), leaf=3 (the M grandchildren part).
      // root has N=2 children of part 2; part 2 has M=3 children of part 3 (a leaf, no children).
      PartGraph g;
      ChildInstance c;
      for (int i = 0; i < 2; ++i) { c.child_resolved_hash = 2; set_translate(c.transform, (float)(i*100),0,0); g[1].push_back(c); }
      for (int j = 0; j < 3; ++j) { c.child_resolved_hash = 3; set_translate(c.transform, 0,(float)(j*10),0); g[2].push_back(c); }
      g[3];   // leaf part: present, no children

      FlattenLimits lim; lim.max_depth = 8; lim.max_instances = 100;
      std::vector<FlatInstance> flat;
      std::string err;
      bool ok = flatten(g, /*root=*/1, lim, flat, err);
      CHECK(ok, "flatten ok");
      CHECK(flat.size() == 2*3, "N*M = 6 leaf instances");
      // Find the instance for child i=1 (x=100), grandchild j=2 (y=20): world = (100,20,0).
      bool found = false;
      for (auto& f : flat) {
          if (f.resolved_hash == 3 && f.world.cell[3]==100 && f.world.cell[7]==20 && f.world.cell[11]==0) found = true;
      }
      CHECK(found, "composed translate (100,20,0) present");
  }
  ```
  Add to `main`. **Decision:** only leaf parts (no children) emit `FlatInstance`s — interior parts contribute transform composition only. This matches the spec's "flat list of leaf instances."

- [ ] **Run and expect FAIL**: `make -C MatterEngine3/tests run-comp` → compile error (`world_flatten.h` missing).

- [ ] **Write the header** (`include/world_flatten.h`):
  ```cpp
  #pragma once
  #include "bvh.h"          // mat4
  #include <cstdint>
  #include <map>
  #include <string>
  #include <vector>

  namespace world_flatten {

  // Mirror of SP-1 ChildInstance (depend on spec interface; identical fields).
  struct ChildInstance {
      uint64_t child_resolved_hash;
      float    transform[16];   // row-major, child placement under parent's frame
  };

  // A part-graph: resolved_hash -> its child-instance rows. A part with no entry
  // (or an empty vector) is a leaf. SP-3 guarantees this is a DAG (no cycles).
  using PartGraph = std::map<uint64_t, std::vector<ChildInstance>>;

  // One flattened world instance: a leaf part placed by a composed world transform.
  struct FlatInstance {
      uint64_t resolved_hash;
      mat4     world;
  };

  struct FlattenLimits {
      uint32_t max_depth     = 32;
      uint32_t max_instances = 1000000;
  };

  // Recursively flatten `root`'s child graph into leaf instances, composing
  // world = parent_world * child.transform down the tree. Returns false and sets
  // `err` (naming the offending part/path) if max_depth or max_instances is
  // exceeded. Leaf parts (no children) emit a FlatInstance; interior parts only
  // compose transforms.
  bool flatten(const PartGraph& graph, uint64_t root, const FlattenLimits& limits,
               std::vector<FlatInstance>& out, std::string& err);

  // Row-major 4x4 multiply: result = a * b.
  mat4 mat4_mul(const mat4& a, const mat4& b);

  } // namespace world_flatten
  ```

- [ ] **Write the impl** (`src/world_flatten.cpp`):
  ```cpp
  #include "../include/world_flatten.h"
  #include <cstdio>

  namespace world_flatten {

  mat4 mat4_mul(const mat4& a, const mat4& b) {
      mat4 r;
      for (int i = 0; i < 4; ++i)
          for (int j = 0; j < 4; ++j) {
              float s = 0;
              for (int k = 0; k < 4; ++k) s += a.cell[i*4+k] * b.cell[k*4+j];
              r.cell[i*4+j] = s;
          }
      return r;
  }

  static mat4 from_row16(const float t[16]) { mat4 m; for (int i=0;i<16;++i) m.cell[i]=t[i]; return m; }

  static bool recurse(const PartGraph& g, uint64_t hash, const mat4& world,
                      uint32_t depth, const FlattenLimits& lim,
                      std::vector<FlatInstance>& out, std::string& err) {
      if (depth > lim.max_depth) {
          char buf[128];
          snprintf(buf, sizeof(buf), "max_depth %u exceeded at part %llu",
                   lim.max_depth, (unsigned long long)hash);
          err = buf; return false;
      }
      auto it = g.find(hash);
      bool is_leaf = (it == g.end() || it->second.empty());
      if (is_leaf) {
          if (out.size() >= lim.max_instances) {
              char buf[128];
              snprintf(buf, sizeof(buf), "max_instances %u exceeded at part %llu",
                       lim.max_instances, (unsigned long long)hash);
              err = buf; return false;
          }
          out.push_back({hash, world});
          return true;
      }
      for (const ChildInstance& c : it->second) {
          mat4 child_world = mat4_mul(world, from_row16(c.transform));
          if (!recurse(g, c.child_resolved_hash, child_world, depth + 1, lim, out, err))
              return false;
      }
      return true;
  }

  bool flatten(const PartGraph& graph, uint64_t root, const FlattenLimits& limits,
               std::vector<FlatInstance>& out, std::string& err) {
      out.clear(); err.clear();
      return recurse(graph, root, mat4::Identity(), 0, limits, out, err);
  }

  } // namespace world_flatten
  ```

- [ ] **Run and expect PASS**: `make -C MatterEngine3/tests run-comp` → `OK`.

- [ ] **Commit**: `world_flatten: recursive flatten with composed world transforms`

---

## Task 5 — Dedup preserved + depth guard + budget guard

**Files:**
- `MatterEngine3/tests/composition_tests.cpp`

The guards already exist in Task 4's impl; this task adds the spec's three remaining flatten tests (dedup, depth guard, budget guard). No new impl unless a test reveals a bug.

- [ ] **Write the failing tests** — append + call from `main`:
  ```cpp
  static void test_dedup_preserved() {
      using namespace world_flatten;
      // Root instances the SAME leaf part (hash 9) five times at different positions.
      PartGraph g; ChildInstance c; c.child_resolved_hash = 9;
      for (int i = 0; i < 5; ++i) { set_translate(c.transform,(float)i,0,0); g[1].push_back(c); }
      g[9];   // leaf
      FlattenLimits lim; std::vector<FlatInstance> flat; std::string err;
      CHECK(flatten(g, 1, lim, flat, err), "dedup flatten ok");
      CHECK(flat.size() == 5, "instance count grows to 5");
      // Unique-geometry (distinct resolved_hash) count stays 1.
      std::vector<uint64_t> uniq;
      for (auto& f : flat) if (std::find(uniq.begin(),uniq.end(),f.resolved_hash)==uniq.end()) uniq.push_back(f.resolved_hash);
      CHECK(uniq.size() == 1, "one unique geometry hash despite 5 instances");
  }

  static void test_depth_guard() {
      using namespace world_flatten;
      // Chain: 1 -> 2 -> 3 -> 4 -> 5 (each interior part has one child).
      PartGraph g; ChildInstance c;
      set_translate(c.transform,0,0,0);
      for (uint64_t h = 1; h <= 4; ++h) { c.child_resolved_hash = h+1; g[h].push_back(c); }
      g[5];   // leaf at depth 4
      FlattenLimits lim; lim.max_depth = 2;     // too shallow for a depth-4 chain
      std::vector<FlatInstance> flat; std::string err;
      CHECK(!flatten(g, 1, lim, flat, err), "depth guard fires");
      CHECK(err.find("max_depth") != std::string::npos, "depth error message");
      CHECK(err.find("part") != std::string::npos, "depth error names offending part");
  }

  static void test_budget_guard() {
      using namespace world_flatten;
      PartGraph g; ChildInstance c; c.child_resolved_hash = 9;
      for (int i = 0; i < 50; ++i) { set_translate(c.transform,(float)i,0,0); g[1].push_back(c); }
      g[9];
      FlattenLimits lim; lim.max_instances = 10;  // 50 leaves > budget
      std::vector<FlatInstance> flat; std::string err;
      CHECK(!flatten(g, 1, lim, flat, err), "budget guard fires");
      CHECK(err.find("max_instances") != std::string::npos, "budget error message");
  }
  ```
  Add `#include <algorithm>` at the top of the test file if not present.

- [ ] **Run and expect** these PASS immediately (guards built in Task 4). If any FAIL, fix `world_flatten.cpp` until green (systematic-debugging). Expected: `OK`.

- [ ] **Commit**: `world_flatten: cover dedup-preserved, depth-guard, budget-guard`

---

## Task 6 — Sector grid binning (boundary-deterministic)

**Files:**
- `MatterEngine3/include/sector_grid.h`
- `MatterEngine3/src/sector_grid.cpp`
- `MatterEngine3/tests/composition_tests.cpp`

- [ ] **Write the failing test** — covers spec "instances land in expected sectors by position; an instance on a sector boundary is assigned deterministically". Real code:
  ```cpp
  #include "../include/sector_grid.h"

  static void test_sector_binning() {
      using namespace sector_grid;
      SectorGrid grid(10.0f);     // 10-unit pitch, origin at 0
      // (5,5,5) -> sector (0,0,0); (15,5,-5) -> (1,0,-1); (-1,0,0) -> (-1,0,0)
      SectorCoord a = grid.sector_of(make_float3(5,5,5));
      CHECK(a.x==0 && a.y==0 && a.z==0, "interior point sector (0,0,0)");
      SectorCoord b = grid.sector_of(make_float3(15,5,-5));
      CHECK(b.x==1 && b.y==0 && b.z==-1, "point sector (1,0,-1)");
      SectorCoord c = grid.sector_of(make_float3(-1,0,0));
      CHECK(c.x==-1, "negative coord uses floor not truncation");
      // Boundary: x exactly on 10.0 -> floor(10/10)=1 deterministically (half-open [lo,hi)).
      SectorCoord bd = grid.sector_of(make_float3(10.0f,0,0));
      CHECK(bd.x==1, "boundary point belongs to upper cell deterministically");
  }

  static void test_bin_instances() {
      using namespace sector_grid;
      using world_flatten::FlatInstance;
      SectorGrid grid(10.0f);
      std::vector<FlatInstance> flat(3);
      flat[0].resolved_hash=1; flat[0].world = mat4::Translate(make_float3(1,1,1));
      flat[1].resolved_hash=1; flat[1].world = mat4::Translate(make_float3(2,2,2));   // same sector as [0]
      flat[2].resolved_hash=2; flat[2].world = mat4::Translate(make_float3(25,0,0));  // sector (2,0,0)
      Sectors s = bin_instances(flat, grid);
      CHECK(s.size() == 2, "two distinct occupied sectors");
      SectorCoord k0{0,0,0};
      CHECK(s[k0].size() == 2, "two instances in sector (0,0,0)");
  }
  ```
  Add to `main`. **Decision:** sector position is taken from the instance world translation (`world.cell[3,7,11]`). Half-open `[lo,hi)` cells via `floor` make boundaries deterministic.

- [ ] **Run and expect FAIL**: `make -C MatterEngine3/tests run-comp` → compile error (`sector_grid.h` missing).

- [ ] **Write the header** (`include/sector_grid.h`):
  ```cpp
  #pragma once
  #include "bvh.h"          // float3, mat4
  #include "world_flatten.h"
  #include <cstdint>
  #include <map>
  #include <vector>

  namespace sector_grid {

  struct SectorCoord { int x, y, z; };
  inline bool operator<(const SectorCoord& a, const SectorCoord& b) {
      if (a.x != b.x) return a.x < b.x;
      if (a.y != b.y) return a.y < b.y;
      return a.z < b.z;
  }
  inline bool operator==(const SectorCoord& a, const SectorCoord& b) {
      return a.x==b.x && a.y==b.y && a.z==b.z;
  }

  // Fixed-pitch axis-aligned grid centered on the world origin. Half-open cells
  // [n*pitch, (n+1)*pitch) via floor -> boundary points are assigned deterministically.
  class SectorGrid {
  public:
      explicit SectorGrid(float pitch) : pitch_(pitch) {}
      SectorCoord sector_of(const float3& world_pos) const;
      float pitch() const { return pitch_; }
  private:
      float pitch_;
  };

  using Sectors = std::map<SectorCoord, std::vector<world_flatten::FlatInstance>>;

  // Bin each flattened instance into its sector by world translation.
  Sectors bin_instances(const std::vector<world_flatten::FlatInstance>& flat,
                        const SectorGrid& grid);

  // World translation of a flattened instance (row-major cell[3,7,11]).
  float3 instance_position(const world_flatten::FlatInstance& f);

  } // namespace sector_grid
  ```

- [ ] **Write the impl** (`src/sector_grid.cpp`):
  ```cpp
  #include "../include/sector_grid.h"
  #include <cmath>

  namespace sector_grid {

  SectorCoord SectorGrid::sector_of(const float3& p) const {
      return SectorCoord{
          (int)std::floor(p.x / pitch_),
          (int)std::floor(p.y / pitch_),
          (int)std::floor(p.z / pitch_)
      };
  }

  float3 instance_position(const world_flatten::FlatInstance& f) {
      return make_float3(f.world.cell[3], f.world.cell[7], f.world.cell[11]);
  }

  Sectors bin_instances(const std::vector<world_flatten::FlatInstance>& flat,
                        const SectorGrid& grid) {
      Sectors out;
      for (const auto& f : flat)
          out[grid.sector_of(instance_position(f))].push_back(f);
      return out;
  }

  } // namespace sector_grid
  ```

- [ ] **Run and expect PASS**: `make -C MatterEngine3/tests run-comp` → `OK`.

- [ ] **Commit**: `sector_grid: deterministic floor-binning of flat instances`

---

## Task 7 — Closest-instance screen-size LOD selection + escalation

**Files:**
- `MatterEngine3/include/lod_select.h`
- `MatterEngine3/src/lod_select.cpp`
- `MatterEngine3/tests/composition_tests.cpp`

- [ ] **Write the failing test** — covers spec "for a synthetic camera, a sector picks the level dictated by its closest instance's projected size; moving the camera closer escalates the level". Real code:
  ```cpp
  #include "../include/lod_select.h"

  static void test_lod_selection_and_escalation() {
      using namespace lod_select;
      // One part with 3 levels; thresholds match lod_bake defaults {0.20, 0.05, 0.0125}.
      // Level 0 = finest (needs projected_size >= 0.20), level 2 = coarsest (>= 0.0125).
      std::vector<float> thresholds = {0.20f, 0.05f, 0.0125f};
      // Instance bound radius 1.0 at world (0,0,0).
      float r = 1.0f; float3 c = make_float3(0,0,0);

      // Camera far away: distance 100 -> projected 0.01 -> below all -> clamp to coarsest (2).
      int far_lvl = select_level(projected_size(c, r, make_float3(0,0,100)), thresholds);
      CHECK(far_lvl == 2, "far camera -> coarsest level 2");
      // Mid: distance 10 -> projected 0.1 -> clears 0.05 (lvl1) and 0.0125 (lvl2), not 0.20 -> level 1.
      int mid_lvl = select_level(projected_size(c, r, make_float3(0,0,10)), thresholds);
      CHECK(mid_lvl == 1, "mid camera -> level 1");
      // Near: distance 2 -> projected 0.5 -> clears 0.20 -> finest level 0.
      int near_lvl = select_level(projected_size(c, r, make_float3(0,0,2)), thresholds);
      CHECK(near_lvl == 0, "near camera -> finest level 0");
      // Escalation monotonic: closer never selects a coarser level.
      CHECK(near_lvl <= mid_lvl && mid_lvl <= far_lvl, "moving closer escalates (lower index)");
  }

  static void test_sector_uses_closest_instance() {
      using namespace lod_select;
      using namespace sector_grid;
      using world_flatten::FlatInstance;
      // Two instances in one sector: one near, one far. Sector LOD uses the CLOSEST.
      std::vector<FlatInstance> flat(2);
      flat[0].resolved_hash=1; flat[0].world = mat4::Translate(make_float3(0,0,0));   // closest to a z=3 cam
      flat[1].resolved_hash=1; flat[1].world = mat4::Translate(make_float3(0,0,8));
      SectorGrid grid(100.0f);                 // both land in sector (0,0,0)
      Sectors sec = bin_instances(flat, grid);
      // Per-part data: hash 1 has bound radius 1.0 and thresholds {0.20,0.05,0.0125}.
      PartLodTable parts;
      parts[1] = PartLod{ /*radius=*/1.0f, /*thresholds=*/{0.20f,0.05f,0.0125f} };
      float3 cam = make_float3(0,0,3);
      auto chosen = select_sector_lods(sec, parts, cam);
      // Closest instance is at z=0 -> distance 3 -> projected 0.333 -> finest level 0.
      SectorCoord k{0,0,0};
      CHECK(chosen[k].at(1) == 0, "sector LOD for part 1 driven by closest instance -> level 0");
  }
  ```
  Add both to `main`. **Decision:** `select_sector_lods` returns, per sector, a per-part-hash chosen LOD index, where the projected size is computed from the sector's **closest instance** (min distance to camera). Per-part thresholds let different parts pick their own level within the closest-instance-driven evaluation, matching the spec.

- [ ] **Run and expect FAIL**: `make -C MatterEngine3/tests run-comp` → compile error (`lod_select.h` missing).

- [ ] **Write the header** (`include/lod_select.h`):
  ```cpp
  #pragma once
  #include "bvh.h"
  #include "sector_grid.h"
  #include <cstdint>
  #include <map>
  #include <vector>

  namespace lod_select {

  // Projected screen size = bound_radius / distance (normalized angular extent;
  // tan-approx of angular radius). Distance clamped to >= 1e-4 to avoid div-by-zero.
  float projected_size(const float3& world_center, float bound_radius,
                       const float3& cam_pos);

  // Pick the COARSEST level whose threshold is satisfied (projected_size >= thr).
  // thresholds are ordered fine->coarse (index 0 = finest, largest threshold).
  // If projected_size clears the finest threshold, returns 0 (finest). If it
  // clears nothing, clamps to the coarsest (last) level.
  int select_level(float projected_size, const std::vector<float>& thresholds);

  // Per-part LOD metadata needed for selection.
  struct PartLod {
      float bound_radius;
      std::vector<float> thresholds;   // matches the part's LodLevels, fine->coarse
  };
  using PartLodTable = std::map<uint64_t, PartLod>;     // resolved_hash -> PartLod

  // For each sector, find its CLOSEST instance to cam_pos, and for every distinct
  // part hash present in the sector compute its chosen LOD level using the closest
  // instance's distance. Returns sector -> (part hash -> chosen level index).
  std::map<sector_grid::SectorCoord, std::map<uint64_t,int>>
  select_sector_lods(const sector_grid::Sectors& sectors,
                     const PartLodTable& parts, const float3& cam_pos);

  } // namespace lod_select
  ```

- [ ] **Write the impl** (`src/lod_select.cpp`):
  ```cpp
  #include "../include/lod_select.h"
  #include <cmath>
  #include <limits>

  namespace lod_select {

  static float dist(const float3& a, const float3& b) {
      float dx=a.x-b.x, dy=a.y-b.y, dz=a.z-b.z;
      return std::sqrt(dx*dx + dy*dy + dz*dz);
  }

  float projected_size(const float3& c, float r, const float3& cam) {
      float d = dist(c, cam);
      if (d < 1e-4f) d = 1e-4f;
      return r / d;
  }

  int select_level(float size, const std::vector<float>& thr) {
      if (thr.empty()) return 0;
      // Walk coarse (last) -> fine (first); pick the finest level the size clears.
      // Equivalent: the smallest index i where size >= thr[i].
      for (size_t i = 0; i < thr.size(); ++i)
          if (size >= thr[i]) return (int)i;     // thr fine->coarse: thr[0] largest
      return (int)thr.size() - 1;                // cleared nothing -> coarsest
  }

  std::map<sector_grid::SectorCoord, std::map<uint64_t,int>>
  select_sector_lods(const sector_grid::Sectors& sectors,
                     const PartLodTable& parts, const float3& cam) {
      std::map<sector_grid::SectorCoord, std::map<uint64_t,int>> out;
      for (const auto& kv : sectors) {
          const auto& coord = kv.first;
          const auto& insts = kv.second;
          // Closest instance distance in this sector.
          float closest = std::numeric_limits<float>::max();
          for (const auto& f : insts) {
              float d = dist(sector_grid::instance_position(f), cam);
              if (d < closest) closest = d;
          }
          if (closest < 1e-4f) closest = 1e-4f;
          // For each distinct part in the sector, select its level at the closest distance.
          for (const auto& f : insts) {
              auto pit = parts.find(f.resolved_hash);
              if (pit == parts.end()) continue;
              float size = pit->second.bound_radius / closest;
              out[coord][f.resolved_hash] = select_level(size, pit->second.thresholds);
          }
      }
      return out;
  }

  } // namespace lod_select
  ```

- [ ] **Run and expect PASS**: `make -C MatterEngine3/tests run-comp` → `OK`.

- [ ] **Commit**: `lod_select: closest-instance sector LOD selection with escalation`

---

## Task 8 — Variation / LOD independence

**Files:**
- `MatterEngine3/tests/composition_tests.cpp`

Covers spec "two variations of a part get identical LOD level sets and identical screen-size selection behavior." No new impl: this asserts the orthogonality is a property of the existing modules (variation = different resolved_hash leaf; LOD bake + selection don't depend on which variation).

- [ ] **Write the failing test** — append + call from `main`:
  ```cpp
  static void test_variation_lod_independence() {
      // Two variations of "the same part kind" are two distinct resolved_hashes
      // (varA=100, varB=200) whose geometry is the SAME shape (same Tri set here),
      // so their baked LOD level sets must be identical, and their selection at a
      // given camera distance must be identical.
      std::vector<Tri> tris = grid_tris(32);
      BLASManager blasA, blasB;
      lod_bake::LodLevels lodsA = lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, blasA);
      lod_bake::LodLevels lodsB = lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, blasB);
      CHECK(lodsA.size() == lodsB.size(), "variations have same level count");
      bool same_thr = true, same_tris = true;
      for (size_t i = 0; i < lodsA.size(); ++i) {
          if (lodsA[i].screen_size_threshold != lodsB[i].screen_size_threshold) same_thr = false;
          size_t ca = blasA.get_entries()[lodsA[i].blas_index[0]]->triangles.size();
          size_t cb = blasB.get_entries()[lodsB[i].blas_index[0]]->triangles.size();
          if (ca != cb) same_tris = false;
      }
      CHECK(same_thr, "variations share identical thresholds");
      CHECK(same_tris, "variations share identical per-level tri counts");

      // Selection behavior identical: build the PartLodTable for both variations,
      // bin one instance of each in the same sector, same camera.
      using namespace lod_select; using namespace sector_grid; using world_flatten::FlatInstance;
      std::vector<float> thr; for (auto& L : lodsA) thr.push_back(L.screen_size_threshold);
      PartLodTable parts;
      parts[100] = PartLod{1.0f, thr};
      parts[200] = PartLod{1.0f, thr};
      std::vector<FlatInstance> flat(2);
      flat[0].resolved_hash=100; flat[0].world = mat4::Translate(make_float3(0,0,0));
      flat[1].resolved_hash=200; flat[1].world = mat4::Translate(make_float3(0,0,0));
      SectorGrid grid(100.0f);
      auto chosen = select_sector_lods(bin_instances(flat, grid), parts, make_float3(0,0,7));
      SectorCoord k{0,0,0};
      CHECK(chosen[k].at(100) == chosen[k].at(200), "two variations select identical LOD");
  }
  ```

- [ ] **Run and expect** PASS immediately (orthogonality is structural). If FAIL, the modules incorrectly couple variation to LOD — fix until green. Expected `OK`.

- [ ] **Commit**: `composition_tests: assert variation/LOD independence`

---

## Task 9 — Wire into build-all test sweep + final self-review

**Files:**
- `MatterEngine3/tests/Makefile`
- (verify only) `build-all.sh`

- [ ] **Verify** `build-all.sh test` invokes the MatterEngine3 test rules (SP-1 registered `MatterEngine3` in `build-all.sh`); if it runs explicit `run-*` targets, add `run-comp` to the sweep. If it globs, no change needed. Read `build-all.sh` first; do NOT invent a hook that isn't there.

- [ ] **Full sweep**: `make -C MatterEngine3/tests run-comp` and confirm `OK` with zero `FAIL:` lines. Re-run `make -C MatterEngine3/tests clean && make -C MatterEngine3/tests run-comp` to confirm a clean build links (validates the `Tri<->Mesh` bridge frees and the new sources compile from scratch).

- [ ] **Self-review** every module against the spec Testing bullets: LOD bake monotone + round-trip (Tasks 2,3), flatten N*M composed (Task 4), dedup preserved (Task 5), depth + budget guards (Task 5), sector binning + boundary determinism (Task 6), LOD escalation + closest-instance (Task 7), variation/LOD independence (Task 8). Confirm the screen-size metric is defined in code (`lod_select::projected_size`) and used consistently.

- [ ] **Commit**: `composition: wire SP-4 tests into build sweep + final review`

---

## Out of scope (do NOT implement — per spec Non-goals)

- Per-frame TLAS assembly/rebuild and TLAS builder fixes (32-bit node indices, spatial split) — owned by the sector-LOD runtime spec.
- The merged far-proxy implementation (deferred/pluggable; voxel-box imposter is an allowed later swap-in).
- Per-instance-within-sector LOD, LOD cross-fade/dither, hysteresis.
- Streaming sectors from disk / out-of-core worlds.
- Authoring parts (SP-2/SP-6) and resolving the graph (SP-3).
