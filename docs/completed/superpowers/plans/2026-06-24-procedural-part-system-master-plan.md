# Procedural Part System — Master Implementation Plan (SP-1 … SP-7)

> **For agentic workers:** This is the **orchestration** plan for the single-player
> procedural-part foundation. It does **not** restate the per-task TDD steps — each
> sub-project has its own bite-sized plan (linked below). This document fixes the **build
> order**, the **cross-plan interface contracts** (so the local mirrors/shims the
> sub-plans declare get reconciled to their real owners), and the **integration gates**
> that cross plan boundaries and therefore can't live in any single sub-plan. Steps use
> checkbox (`- [ ]`) syntax. REQUIRED SUB-SKILL when executing any sub-plan:
> superpowers:subagent-driven-development (recommended) or superpowers:executing-plans.

**Goal:** Stand up the full single-player procedural-part pipeline — author a part in JS,
bake it to a content-addressed `.part`, assemble many parts into a sectored, LOD'd world,
and live-edit it — by executing seven sub-plans in dependency order with verified
integration seams between them.

**Architecture:** All new code lives in a **new sibling project `MatterEngine3/`** that
**consumes `MatterSurfaceLib/` read-only** (the raytracing + meshing prototype is preserved
untouched for reference/experiments). MatterEngine3 layers seven sub-projects on the
existing MatterSurfaceLib backend (`part_asset` v1, `BLASManager`/`TLASManager`, `Cluster`,
`mesh_simplifier`). SP-1 is the serialization foundation; SP-2 the JS bake host; SP-3 the
graph/install orchestrator; SP-4 the world composition + LOD; SP-5 the live-edit loop; SP-6
the triangle/variation path; SP-7 the shared script library. Each sub-plan is independently
testable; this plan wires them together at the seams.

**Tech Stack:** C++17, QuickJS-ng (vendored under `Libraries/quickjs-ng/`), FNV-1a content
hashing, existing Cluster/BLAS/TLAS + mesh_simplifier (consumed from MatterSurfaceLib),
Linux inotify, headless GL-free tests under `MatterEngine3/tests/`.

---

## Project relocation: MatterEngine3 (authoritative — all sub-plans obey this)

All SP-1…SP-7 work is built in a **new top-level project `MatterEngine3/`**, a sibling of
`MatterSurfaceLib/`. The MatterSurfaceLib prototype is **never modified** — it is a
read-only dependency (monorepo convention: `-I../MatterSurfaceLib/include` + link its
`.cpp`). This section overrides any `MatterSurfaceLib/...` path a sub-plan names for a
**new** file.

**Layout**
```
MatterEngine3/
  Makefile                # builds the lib objects (optional; tests link sources directly)
  include/                # NEW headers (part_asset_v2.h, script_host.h, part_graph.h,
                          #   module_resolver.h, composition.h, live_edit_interfaces.h, …)
  src/                    # NEW impls (part_asset_v2.cpp, script_host.cpp, …)
  shared-lib/             # SP-7 JS helper modules (lsystem.js, bezier.js, … rng.js)
  tests/
    Makefile              # run-partv2/script/graph/trivar/shlib/comp/dev targets
    *_tests.cpp           # all SP-1…SP-7 headless tests
```

**Path-classification rule (apply per reference):**
- A **new** file the sub-plans author → `MatterEngine3/{include,src,tests}/…`.
- A file that **already exists in MatterSurfaceLib** (consumed) → stays
  `MatterSurfaceLib/…`, referenced from MatterEngine3 as `../MatterSurfaceLib/…` (Makefile
  sources) or via `-I../MatterSurfaceLib/include` (headers).

**Dependency files that STAY `MatterSurfaceLib` (consumed, never relocated):**
headers — `bvh.h`, `blas_manager.hpp`, `tlas_manager.hpp`, `mesh_simplifier.hpp`,
`occupancy.h`, `vertex_ao.h`, `material_registry.h`, `cluster.h`, `cell.h`, `particle.h`,
`marching_cubes_algorithm.h`, `oriented_cube_algorithm.h`, `mesh_build_utils.h`,
`precomp.h`, **`part_asset.h` (v1)**; sources — `blas_manager.cpp`, `bvh.cpp`,
`tlas_manager.cpp`, `occupancy.cpp`, `vertex_ao.cpp`, `material_registry.c`,
`part_asset.cpp` (v1), `cluster.cpp`, `cell.cpp`, `marching_cubes_algorithm.cpp`,
`oriented_cube_algorithm.cpp`, `mesh_build_utils.cpp`, `mesh_simplifier.cpp`.

**SP-1 special case (no prototype edit):** SP-1 does **not** modify
`MatterSurfaceLib/include/part_asset.h`. It **creates** `MatterEngine3/include/part_asset_v2.h`
+ `MatterEngine3/src/part_asset_v2.cpp`, which `#include "part_asset.h"` (the v1 header, via
`-I../MatterSurfaceLib/include`) to reuse `part_asset::fnv1a64`, `cache_path`, `kMagic`, and
defines the v2 additions (`compute_resolved_hash`, `ChildInstance`, `LodLevel` with
`screen_size_threshold`, `LodLevels`, `save_v2`, `load_v2`) in the same `part_asset`
namespace. Consumers `#include "part_asset_v2.h"`.

**Makefile path conventions (from `MatterEngine3/tests/`):**
- New project headers: `-I../include`; consumed prototype headers: add
  `-I../../MatterSurfaceLib/include`.
- New test/impl sources: `../src/<new>.cpp` (unchanged form).
- Consumed prototype sources: `../../MatterSurfaceLib/src/<dep>.cpp` (one extra `../` vs the
  old MatterSurfaceLib/tests Makefile, which used `../src/<dep>.cpp`).
- raylib is at repo-root `Libraries/` — **unchanged** (`MatterEngine3/tests` is the same
  depth as `MatterSurfaceLib/tests`, so `../../Libraries/raylib/build/linux/libraylib.a`
  and `-I../../Libraries/raylib/src` carry over verbatim). Compile C deps (`material_registry.c`)
  with `gcc` to keep `extern "C"` symbols unmangled, as the prototype's test Makefile does.

**Registration:** add `MatterEngine3` to `build-all.sh` (build + `test` paths) so
`./build-all.sh test` runs the new `run-*` targets.

---

## Sub-plans (each its own bite-sized TDD plan)

| SP | Plan file | Owns |
|----|-----------|------|
| SP-1 | `2026-06-24-part-artifact-v2-plan.md` | `.part` v2: `compute_resolved_hash`, `ChildInstance`, `LodLevels` (+`screen_size_threshold`), `save_v2`/`load_v2`, `cache_path` |
| SP-2 | `2026-06-24-script-host-plan.md` | `ScriptHost`, `Part` base, DSL state + build buffer, voxel/CSG session, seeded RNG hook, time budget, bake→`.part` |
| SP-3 | `2026-06-24-part-graph-install-plan.md` | `requires` discovery, resolved-hash assembly, cache-miss bake, topo order, cycle/reachability, install, `ModuleResolver`, `Baker` seam |
| SP-4 | `2026-06-24-composition-to-world-plan.md` | LOD bake (mesh_simplifier), recursive flatten, sector binning, screen-size LOD selection |
| SP-5 | `2026-06-24-dev-live-edit-plan.md` | `FileWatcher` (inotify), debounce, upward-cone rebake, affected-subtree re-flatten, fail-closed |
| SP-6 | `2026-06-24-triangle-path-variations-plan.md` | direct-triangle session → part BLAS (Tri/TriEx), `line` tuber, `instance(child,variation)` |
| SP-7 | `2026-06-24-shared-script-library-plan.md` | `shared-lib/*.js` helpers, module resolve + source-fold into hash, xoshiro128** RNG |

---

## Build order & rationale

```
SP-1  (foundation; no deps)
  └─> SP-2  (needs SP-1 save_v2 + compute_resolved_hash)
        ├─> SP-3  (needs SP-2 Baker; seam-buildable in parallel, integrates after SP-2)
        ├─> SP-6  (needs SP-2 build buffer + session API)
        └─> SP-7  (needs SP-2 host/module loader + SP-1 hash; folds via SP-3 resolve)
  SP-4  (needs SP-1 LOD format + a populated cache from SP-3)
  SP-5  (needs SP-2 budget/fail-closed + SP-3 resolve/topo + SP-4 flatten)
```

Recommended linear execution: **SP-1 → SP-2 → SP-3 → SP-6 → SP-7 → SP-4 → SP-5**.
SP-3/SP-6/SP-7 each begin against seams/interfaces and only *require* SP-2 merged at their
integration task, so they can be drafted/executed in parallel by separate workers once SP-2
lands; SP-4 needs a real populated cache (SP-3) to be end-to-end meaningful; SP-5 is last
because it composes all of SP-2/3/4.

- [ ] **Decide execution mode** (subagent-driven vs inline) and record the chosen order
  above. Do not start a sub-plan until its upstream dependency's exit gate (below) is green.

---

## Cross-plan interface contracts (single source of truth)

Each sub-plan was written to be testable before its upstream exists, so several declare a
**local mirror or shim** of an upstream type. When the upstream lands, the mirror MUST be
replaced by the real owner. These are the canonical definitions; the reconciliation tasks
are gated in the Integration Gates section.

### C-1 — Identity & artifact (owner: SP-1)
```cpp
// part_asset.h  (the ONLY definitions of these; all other SPs include this header)
uint64_t compute_resolved_hash(const void* source_bytes, size_t source_len,
                               const void* params_bytes, size_t params_len,
                               const uint64_t* child_hashes, size_t child_count);
struct ChildInstance { uint64_t child_resolved_hash; float transform[16]; };
struct LodLevel  { float screen_size_threshold; std::vector<uint32_t> blas_indices; };
using  LodLevels = std::vector<LodLevel>;
bool save_v2(const std::string& path, const BLASManager&, const TLASManager&,
             const ChildInstance*, size_t, const LodLevels&, uint64_t resolved_hash);
bool load_v2(const std::string& path, uint64_t expected,
             BLASManager&, TLASManager&, std::vector<ChildInstance>&, LodLevels&);
std::string cache_path(uint64_t resolved_hash);   // parts/<16hex>.part
```
- **SP-6** declares a local `ChildInstance` + local `compute_resolved_hash` — replace with
  this header (layout is byte-identical, so the swap is source-compatible).
- **SP-4** declares an in-memory `serialize_lods`/`deserialize_lods` shim — replace its LOD
  round-trip with `save_v2`/`load_v2`.
- **SP-7** adds `compute_resolved_hash` only if SP-1 hasn't — after SP-1, delete the dup.

### C-2 — Bake host (owner: SP-2; **sole hash authority for script parts**)
- **Hash authority.** Only SP-2 can compute a script part's `resolved_hash`, because the
  hash folds the **merged** params (the class's `static params` defaults overlaid with the
  caller's overrides) and SP-2 is the only component that can evaluate the script to read
  those defaults. **SP-3 never hashes a script part itself** — it asks the host. This keeps
  param canonicalization in exactly one place, so the hash always matches the bytes baked.
- **Canonical public signature** (all consumers bind to this exact name/shape):
  ```cpp
  struct BakeResult { BakeError error; uint64_t resolved_hash; };

  // Hash-only: merge static+override params, fold child_hashes, return the content
  // hash WITHOUT running build()/baking. SP-3 calls this to check the cache and to
  // fold into parents. Same merge+canonicalization path bake_source uses, so the two
  // ALWAYS agree for identical inputs.
  uint64_t resolve_hash(const std::string& source,
                        const std::string& params_json,
                        const uint64_t* child_hashes = nullptr,
                        size_t child_count = 0);

  // child_hashes folds into the resolved hash so a PARENT's hash matches the value
  // resolve_hash returned; defaulted so SP-2's own standalone tests (childless parts)
  // are unaffected. On success writes one .part at cache_path(resolved_hash).
  BakeResult bake_source(const std::string& source,
                         const std::string& params_json,
                         BakeOptions opts,
                         const uint64_t* child_hashes = nullptr,
                         size_t child_count = 0);

  // Static discovery of a part's `requires(...)` child instances, evaluated
  // WITHOUT baking — SP-3 calls this to walk the graph leaves-first.
  struct RequiredChild { std::string module_specifier; std::string params_json; };
  std::vector<RequiredChild> eval_requires(const std::string& source,
                                           const std::string& params_json);
  ```
  `bake_source` writes one `.part` via SP-1; a settable **time budget** (≤0 = unbounded); a
  **seeded Math.random** hook (SP-7 supplies the algorithm); structured **fail-closed** error.
  `resolve_hash`/`bake_source`/`eval_requires` share the same param-merge + canonicalization
  helper, so a value `resolve_hash` returns equals the hash `bake_source` writes to.
- The C++-owned **build buffer + session API** that **SP-6** appends triangles into.
- The **module loader** that **SP-7** resolves shared-lib imports through.
- **SP-3** calls SP-2 only through its `Baker` seam: `resolve_hash(...)` for each node's hash
  (cache check + parent fold), `bake_source(...)` (passing the resolved `child_hashes`) on a
  miss, and `eval_requires(...)` to discover children.
  **Reconciliation:** SP-2 owns these three methods under these exact names. SP-3's internal
  `Baker::bake(...)` / SP-7's test `host.bake(...)` are seam/wrapper names that must
  delegate to `bake_source(...)`; rename or thin-wrap at the adapter, do not add a second
  public host entry point. **resolve_hash + eval_requires owner = SP-2** (it has the
  JSContext to merge params and evaluate `requires` statically); SP-3 consumes both.

### C-3 — Graph resolve (owner: SP-3)
- `resolve(part, params)` → resolved hash (memoized, leaves-first); `topo_order`,
  `ancestors`, `parts_for_file`, `roots_over`, cache-miss bake driver. The hash itself comes
  from SP-2 (`resolve_hash`, C-2) — SP-3 supplies `(source, params, child_hashes)` and
  memoizes the returned value; it does **not** compute the hash or canonicalize params itself.
- **SP-5** consumes these via its `GraphResolver` seam — wire to the real SP-3.
- **SP-7**'s source-fold (part source + canonically-ordered imported module sources) happens
  **inside SP-2** (the host owns the module loader, so it assembles the folded source before
  hashing in `resolve_hash`/`bake_source`). SP-3 passes only the part's own source; the
  transitive fold is host-side, keeping the importer-invalidation guarantee without SP-3
  re-implementing module resolution.

### C-4 — World compose (owner: SP-4)
- `flatten(roots)` → per-sector instance lists; `select_lod(sector, camera)`.
- **SP-5** re-flattens the affected subtree by calling SP-4's flatten over `roots_over(changed)`.

---

## Integration gates (cross-boundary; verify after each sub-plan)

Each gate is an integration test that no single sub-plan can own because it spans two
subsystems. Add these under `MatterEngine3/tests/` as they become reachable.

- [ ] **Gate M1 — SP-1 done:** `make -C MatterEngine3/tests run-partv2` green. A v2
  `.part` with children + multi-level LODs round-trips; v1 files rejected.

- [ ] **Gate M2 — SP-2 done (first geometry from a script):**
  - Reconcile: SP-2 uses real SP-1 `save_v2`/`compute_resolved_hash` (no shim).
  - Integration test: a hand-written `.js` part with a voxel sphere∖box bakes to a `.part`;
    `load_v2` restores BLAS geometry; re-baking the same source+params yields identical
    bytes. `make -C MatterEngine3/tests run-script` green.

- [ ] **Gate M3 — SP-3 done (a world installs):**
  - Reconcile: SP-3 `Baker` adapter calls the real `ScriptHost`; `ModuleResolver` real.
  - Integration test: a 3-part graph (root→2 children, one shared) installs to a populated
    `parts/`; second install bakes 0 (all hits); editing the shared leaf rebakes leaf+root
    only; a cycle errors. `make -C MatterEngine3/tests run-graph` green.

- [ ] **Gate M4 — SP-6 done (triangles + variations):**
  - Reconcile: delete SP-6's local `ChildInstance`/`compute_resolved_hash` mirror; include
    `part_asset.h`.
  - Integration test: a script mixing a voxel brush + a `beginShape` quad + a skinned
    `line` bakes a **single** part BLAS holding all three with per-triangle materials;
    `instance(child, variationParams)` writes the child-instance table; identical variation
    params dedup to one artifact. `make -C MatterEngine3/tests run-trivar` green.

- [ ] **Gate M5 — SP-7 done (shared lib + fold invalidation):**
  - Reconcile: SP-3 `resolve` assembles `source_bytes` via SP-7's fold; SP-2 Math.random
    bound to SP-7 `ScriptRng`; remove any duplicate `compute_resolved_hash`.
  - Integration test: a part `import`s `shared-lib/lsystem` + `shared-lib/rng`; bake is
    deterministic from `p.seed`; editing the imported module changes the part's resolved
    hash (rebakes) while a non-importer is untouched; import-order permutation → same hash.
    `make -C MatterEngine3/tests run-shlib` green.

- [ ] **Gate M6 — SP-4 done (world composes + LODs):**
  - Reconcile: SP-4 LOD round-trip uses real `save_v2`/`load_v2` (drop the shim).
  - Integration test: install a small world (SP-3), LOD-bake each part (~3 levels,
    monotone tri counts, `screen_size_threshold` tagged, written back into the `.part`),
    flatten roots into per-sector instance lists (composed transforms, dedup preserved,
    depth/budget guards), and select sector LOD by closest-instance screen size.
    `make -C MatterEngine3/tests run-comp` green.

- [ ] **Gate M7 — SP-5 done (live edit):**
  - Reconcile: SP-5 `GraphResolver`→real SP-3; `Baker`→real SP-2 (dev budget set);
    re-flatten→real SP-4.
  - Integration test: touch a leaf `.js` in a temp `WorldData/<world>/ObjectSchemas/`; the
    inotify path debounces, rebakes exactly the upward cone, re-flattens the affected
    subtree, and a script that throws keeps the last-good artifact + surfaces the error.
    `make -C MatterEngine3/tests run-dev` green.

- [ ] **Final gate — full suite:** `./build-all.sh test` green on Linux; all
  `run-partv2/script/graph/trivar/shlib/comp/dev` targets pass.

---

## Risk register (carried from sub-plan reports)

- **QuickJS-ng pin (SP-2):** vendored at v0.10.0, compiling `quickjs.c libregexp.c
  libunicode.c cutils.c` (+`libbf.c`/`xsum.c` only if the pin needs them). SP-2 Task 1
  iterates on link errors before any DSL work — treat a clean `1+1`→2 embed as that task's
  exit bar.
- **Box-brush lowering (SP-2):** no exact box mesher input exists today; SP-2 lowers `box`
  to an SDF-sampled stamp at session spacing (no mesher change). If a later need for exact
  box brushes arises, that's a separate mesher-input task, not a blocker here.
- **TLAS assembly path (SP-2):** confirm the bake uses the same TLAS build path as
  `part_asset_tests` (`draw_batch`/`build`) — flagged in SP-2 Task 9 to verify against
  `tlas_manager.hpp` at execution.
- **Screen-size metric (SP-4):** `projected_size = bound_radius / distance`; `bound_radius`
  supplied per-part via a table (GL-free, unit-testable). Deriving radius from the BLAS
  AABB at bake is a reasonable later refinement, not required.
- **Windows watcher (SP-5):** `ReadDirectoryChangesW` is a marked deferred stub; the Linux
  inotify path is fully implemented + tested. Cross-platform parity is a later task.

---

## Out of scope (deferred tranche — not in this plan)

Networking/replication SP-8/9/10, the merged far-proxy implementation and per-frame TLAS
rebuild + 32-bit/spatial-split TLAS builder fixes (the sector-LOD *runtime* spec), the
lattice session, `pointsOnSurface` mesh scatter, and any non-Linux file-watch backend.
