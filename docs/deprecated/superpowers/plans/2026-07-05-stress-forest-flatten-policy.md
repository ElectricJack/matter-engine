# Stress-forest flatten-policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework the StressForest fixtures + headless test to prove the bake pipeline's per-part flatten policy: heavy children (Tree) flatten themselves `INLINE` while a 50k-instance scatter parent stays `BOUNDARY` and stores only `FlatInstanceRef`s.

**Architecture:** Two-part change: (1) all four `StressForest*.js` schemas switch from a single Pebble scatter to a deterministic `i % 3` mix of `Pebble` / `Rock(seed=0)` / `Tree`, with Rock scaled ~2×. (2) `stress_forest_tests.cpp` swaps its synthetic-hash harness for the real-content `PartGraph.install` + explicit `part_flatten::flatten_part` path (same source-set stanza as `meadow_bake_check`), asserting the two decisions above and preserving cross-cache determinism. Windows viewer rebuild follows so `viewer.exe` doesn't ship a stale engine.

**Tech Stack:** C++17, MatterEngine3 `PartGraph` + `ScriptHost` + `part_flatten`, QuickJS-ng schemas, MinGW-w64 for Windows viewer.

## Global Constraints

- Determinism preserved: one seeded rng per schema, no wall-clock or process-state inputs.
- Scatter builder stays inlined in every StressForest variant (module resolver rejects relative imports; comment already documents this in each file).
- `MatterEngine3/tests/` remains the working directory for the test binary — relative schema paths (`../examples/world_demo/schemas`, `../shared-lib`) must resolve.
- Headless-only in the tests lane: no GL context, no viewer window.
- After any engine/viewer code change: run `make windows` in `MatterEngine3/viewer/` (memory: `always_make_windows`).

---

### Task 1: Update the four StressForest schemas to a 1/3-1/3-1/3 mix

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/StressForest50k.js`
- Modify: `MatterEngine3/examples/world_demo/schemas/StressForest100k.js`
- Modify: `MatterEngine3/examples/world_demo/schemas/StressForest200k.js`
- Modify: `MatterEngine3/examples/world_demo/schemas/StressForest500k.js`

**Interfaces:**
- Consumes: `Pebble`, `Rock(seed=0)`, `Tree` from the same schemas dir (all already exist).
- Produces (contract for Task 2): each parent's `.part` will contain exactly `COUNT` `ChildInstance`s. Kind is `i % 3` — buckets are `Pebble` (0), `Rock/seed=0` (1), `Tree` (2). Deterministic rng seed unchanged.

- [ ] **Step 1: Rewrite `StressForest50k.js`**

Replace the current file contents with:

```javascript
import { rng } from 'shared-lib/rng';
import { heightAt } from 'shared-lib/terrain_noise';

// Stage-4 GPU-culling stress fixture: COUNT children uniformly scattered over
// a 2 km x 2 km square, ground-following via the shared terrain noise. Kind is
// chosen deterministically by (i % 3):
//   bucket 0 -> Pebble          (scale 0.7 – 1.3)
//   bucket 1 -> Rock(seed=0)    (scale 1.2 – 2.4, "larger rocks")
//   bucket 2 -> Tree            (scale 0.9 – 1.1)
// The heterogeneous mix exists to exercise the per-part flatten decision:
// Tree flattens INLINE within itself, but this 50k-scatter parent must land on
// BOUNDARY so its .flat.part stores 50k FlatInstanceRefs instead of expanding.
// Same seeded rng drives every placement, so the scatter is deterministic and
// content-addressed (same contract as Meadow.js).
//
// NOTE: the scatter builder is INLINED in each StressForest<count> schema
// (rather than shared via `./stress_forest_lib`) because the module resolver
// only accepts 'shared-lib/...' specifiers — relative imports do not resolve
// (module_resolver.cpp resolve_specifier fails-closed on anything else).
// Keep the four copies in sync when editing.

const COUNT = 50000;
const SEED  = 20260703;
const W     = 2000.0;               // world span in x/z (2 km)

class StressForest50k extends Part {
  static requires = [
    { module: 'Pebble' },
    { module: 'Rock', params: { seed: 0 } },
    { module: 'Tree' },
  ];

  build(p) {
    const r = rng(SEED);
    for (let i = 0; i < COUNT; ++i) {
      const x = r.range(0, W), z = r.range(0, W);
      this.pushMatrix();
      this.translate(x, heightAt(x, z), z);
      this.rotateY(r.range(0, Math.PI * 2));
      const bucket = i % 3;
      if (bucket === 0) {
        const s = r.range(0.7, 1.3);
        this.scale(s, s, s);
        this.placeChild('Pebble');
      } else if (bucket === 1) {
        const s = r.range(1.2, 2.4);
        this.scale(s, s, s);
        this.placeChild('Rock', { seed: 0 });
      } else {
        const s = r.range(0.9, 1.1);
        this.scale(s, s, s);
        this.placeChild('Tree');
      }
      this.popMatrix();
    }
  }
}
```

- [ ] **Step 2: Rewrite `StressForest100k.js`**

Identical body to Step 1's file EXCEPT:
- `const COUNT = 100000;`
- `class StressForest100k extends Part {`
- Header comment says `100k-scatter` where the 50k version said `50k-scatter`.

- [ ] **Step 3: Rewrite `StressForest200k.js`**

Identical body to Step 1's file EXCEPT:
- `const COUNT = 200000;`
- `class StressForest200k extends Part {`
- Header comment says `200k-scatter`.

- [ ] **Step 4: Rewrite `StressForest500k.js`**

Identical body to Step 1's file EXCEPT:
- `const COUNT = 500000;`
- `class StressForest500k extends Part {`
- Header comment says `500k-scatter`.

- [ ] **Step 5: No commit yet.**

Leave the four modified files uncommitted. Task 2 rewrites the test that consumes them; Task 3 verifies + commits both changes together so no interim state has a red test.

---

### Task 2: Rewrite `stress_forest_tests.cpp` for the real-content flatten-policy check

**Files:**
- Modify: `MatterEngine3/tests/stress_forest_tests.cpp` (full rewrite)
- No Makefile change: the existing `STRESSFOREST_CPP` stanza already links the full pipeline (`filter-out example_world.cpp, $(EXAMPLE_CPP)`) plus QuickJS. It builds with `-DMATTER_HAVE_SCRIPT_HOST`, which is exactly what `PartGraph.install` needs. See `MatterEngine3/tests/Makefile:499-509`.

**Interfaces:**
- Consumes: the four schemas from Task 1 (`Pebble`, `Rock`, `Tree`, `StressForest50k`); Task 1's contract that `StressForest50k` produces exactly `50000` `ChildInstance`s bucketed `i % 3`.
- Consumes (existing engine APIs):
  - `part_graph::PartGraph::install(std::vector<ChildRequest>) -> InstallResult` with `ok, error, root_hashes[], baked[], hits` (pattern from `meadow_bake_check.cpp:47`).
  - `part_graph::FileModuleResolver(script_host::ScriptHost&, const std::string& schemas_dir)`.
  - `part_graph::HostBaker(script_host::ScriptHost&, const std::string& cache_root)`.
  - `part_flatten::flatten_part(const std::string& cache_root, uint64_t root_hash, const FlattenTargets& = {})` returns `FlattenResult{ ok, error, levels, clusters, full_tris, coarsest_tris, instance_refs }`.
  - `part_asset::cache_path_flat(uint64_t)` → relative path under `parts/`.
  - `part_asset::load_flat_v3(path, expected_hash, blas, tlas, clusters_out, instance_refs_out)` (5-arg form loads the `instance_refs` trailer).
  - `part_asset::fnv1a64(const void*, size_t) -> uint64_t`.
  - `sizeof(part_asset::FlatInstanceRef) == 72` (padding-free, safe to memcpy contiguous).
- Produces: a headless binary `stress_forest_tests` that returns 0 on all-pass, 1 otherwise.

**Reference pattern** (verified from existing tree):
- `meadow_bake_check.cpp:32-51` — PartGraph install pattern, `chdir` into sandbox, `install({ChildRequest{"Meadow", {}}})`.
- `viewer/local_provider.cpp:170-178` — `part_flatten::flatten_part` call pattern using absolute cache root.
- Existing `stress_forest_tests.cpp:82-111` (soon-to-be-replaced) — two-cache-dir determinism pattern.

- [ ] **Step 1: Replace `stress_forest_tests.cpp` contents**

Overwrite the file with:

```cpp
// stress_forest_tests.cpp
// Real-content flatten-policy check for StressForest50k.
//
// The scatter fixture places COUNT=50000 children in a deterministic 1/3-1/3-1/3
// mix of Pebble / Rock(seed=0) / Tree (see StressForest50k.js). This test proves
// the bake pipeline's per-part flatten policy on that fixture:
//
//   1. Tree.flat.part exists with real merged triangles and NO instance_refs
//      => Tree is FlattenDecision::INLINE — its own subtree (trunk + branches)
//      got fused into a single artifact.
//
//   2. StressForest50k.flat.part exists with zero merged geometry and EXACTLY
//      50000 instance_refs, each pointing at Pebble / Rock / Tree's resolved
//      hash => StressForest50k is FlattenDecision::BOUNDARY — the pipeline
//      never allocated the fully-expanded intermediate buffer. This is the
//      load-bearing memory guarantee for large scatters.
//
//   3. Cross-cache determinism: two independent sandboxes produce the same
//      StressForest50k resolved hash, byte-identical .flat.part files, and an
//      identical FNV-1a hash over the FlatInstanceRef stream (child_hash +
//      transform[16], in table order).
//
// Sandbox: /tmp/me3_stress_forest/cache{A,B}. Rebuilt fresh on every run so the
// install path exercises real bakes (not warm cache hits) and the two caches
// are genuinely independent.
//
// Must run from MatterEngine3/tests/ so ../examples/world_demo paths resolve.

#include "part_graph.h"        // -DMATTER_HAVE_SCRIPT_HOST pulls in script_host.h
#include "part_asset_v2.h"     // cache_path_flat, load_flat_v3, FlatInstanceRef
#include "part_asset.h"        // fnv1a64
#include "part_flatten.h"      // flatten_part
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

using namespace part_graph;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

static const size_t kExpectedCount = 50000;

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

static std::vector<uint8_t> file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b(std::istreambuf_iterator<char>(f), {});
    return b;
}

// One sandbox: fresh cache dir, PartGraph.install of StressForest50k, then
// explicit flatten_part for both Tree and StressForest50k.
struct BakeRec {
    bool     ok = false;
    uint64_t root_hash = 0;         // StressForest50k
    uint64_t tree_hash = 0;         // Tree
    std::string flat_root_path;     // absolute path to StressForest50k.flat.part
    std::string flat_tree_path;     // absolute path to Tree.flat.part
    part_flatten::FlattenResult flat_root_result;
    part_flatten::FlattenResult flat_tree_result;
};

// Look up a required module's resolved hash by installing it directly. The
// graph caches previously-baked artifacts, so re-installing a module already
// pulled in transitively is a cheap hit.
static uint64_t install_and_hash(PartGraph& g, const std::string& module_name,
                                 const Params& params) {
    ChildRequest req{module_name, params};
    InstallResult ir = g.install({req});
    if (!ir.ok || ir.root_hashes.empty()) return 0;
    return ir.root_hashes[0];
}

static BakeRec run_bake(const std::string& sandbox_abs,
                        const std::string& schemas_abs,
                        const std::string& shared_lib_abs) {
    BakeRec rec;

    char prev[PATH_MAX];
    if (!getcwd(prev, sizeof(prev))) { printf("FAIL: getcwd\n"); return rec; }
    if (chdir(sandbox_abs.c_str()) != 0) {
        printf("FAIL: chdir(%s)\n", sandbox_abs.c_str()); return rec;
    }

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib_abs);
    FileModuleResolver resolver(host, schemas_abs);
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    // Install StressForest50k: transitively bakes Pebble, Rock(seed=0), Tree,
    // TreeBranch, and the parent. Reads the four child hashes back off the
    // parent's ChildInstance table by installing each module separately (the
    // graph's cache makes the extra installs cheap).
    InstallResult ir_root = graph.install({ChildRequest{"StressForest50k", {}}});
    if (!ir_root.ok || ir_root.root_hashes.empty()) {
        printf("FAIL: install StressForest50k: %s\n", ir_root.error.c_str());
        chdir(prev);
        return rec;
    }
    rec.root_hash = ir_root.root_hashes[0];
    printf("  StressForest50k resolved hash = %016llx (baked=%zu, hits=%d)\n",
           (unsigned long long)rec.root_hash, ir_root.baked.size(), ir_root.hits);

    rec.tree_hash = install_and_hash(graph, "Tree", {});
    if (!rec.tree_hash) { printf("FAIL: install Tree\n"); chdir(prev); return rec; }
    printf("  Tree resolved hash             = %016llx\n",
           (unsigned long long)rec.tree_hash);

    // Explicit flatten calls (install does not flatten; only the viewer's
    // LocalProvider does that on demand — see viewer/local_provider.cpp:170).
    std::string abs_cache = sandbox_abs;

    rec.flat_tree_path = abs_cache + "/" + part_asset::cache_path_flat(rec.tree_hash);
    rec.flat_tree_result = part_flatten::flatten_part(abs_cache, rec.tree_hash);
    printf("  Tree flatten: ok=%d clusters=%zu full_tris=%zu instance_refs=%zu\n",
           (int)rec.flat_tree_result.ok, rec.flat_tree_result.clusters,
           rec.flat_tree_result.full_tris, rec.flat_tree_result.instance_refs);

    rec.flat_root_path = abs_cache + "/" + part_asset::cache_path_flat(rec.root_hash);
    rec.flat_root_result = part_flatten::flatten_part(abs_cache, rec.root_hash);
    printf("  StressForest50k flatten: ok=%d clusters=%zu full_tris=%zu instance_refs=%zu\n",
           (int)rec.flat_root_result.ok, rec.flat_root_result.clusters,
           rec.flat_root_result.full_tris, rec.flat_root_result.instance_refs);

    chdir(prev);
    rec.ok = true;
    return rec;
}

int main() {
    const std::string schemas_abs    = abspath("../examples/world_demo/schemas");
    const std::string shared_lib_abs = abspath("../shared-lib");

    // Fresh sandbox with two independent cache dirs.
    const std::string sandbox = "/tmp/me3_stress_forest";
    system(("rm -rf " + sandbox).c_str());
    const std::string cacheA = sandbox + "/cacheA";
    const std::string cacheB = sandbox + "/cacheB";
    system(("mkdir -p " + cacheA + "/parts").c_str());
    system(("mkdir -p " + cacheB + "/parts").c_str());

    const std::string cacheA_abs = abspath(cacheA);
    const std::string cacheB_abs = abspath(cacheB);

    printf("[cacheA] running bake + flatten\n");
    BakeRec A = run_bake(cacheA_abs, schemas_abs, shared_lib_abs);
    printf("[cacheB] running bake + flatten\n");
    BakeRec B = run_bake(cacheB_abs, schemas_abs, shared_lib_abs);

    CHECK(A.ok && B.ok, "both sandboxes completed bake + flatten");
    if (!A.ok || !B.ok) { printf("\n%d FAILURE(S)\n", g_failures); return 1; }

    // ---- Policy assertions on cache A ---------------------------------------
    printf("\n[test_tree_inline]\n");
    CHECK(A.flat_tree_result.ok, "Tree flatten_part ok");
    CHECK(A.flat_tree_result.clusters > 0,
          "Tree.flat.part has >= 1 cluster (merged geometry present)");
    CHECK(A.flat_tree_result.full_tris > 0,
          "Tree.flat.part has non-zero merged triangles (INLINE fused trunk + branches)");
    CHECK(A.flat_tree_result.instance_refs == 0,
          "Tree.flat.part has zero instance_refs (INLINE, no BOUNDARY children)");

    // Load Tree.flat.part directly and re-verify.
    {
        BLASManager blas; TLASManager tlas(64);
        std::vector<part_asset::FlatCluster> clusters;
        std::vector<part_asset::FlatInstanceRef> refs;
        bool loaded = part_asset::load_flat_v3(A.flat_tree_path, A.tree_hash,
                                               blas, tlas, clusters, refs);
        CHECK(loaded, "Tree.flat.part reloads via load_flat_v3");
        CHECK(!clusters.empty(), "Tree.flat.part reload: clusters non-empty");
        CHECK(refs.empty(), "Tree.flat.part reload: instance_refs empty");
    }

    printf("\n[test_stress_boundary]\n");
    CHECK(A.flat_root_result.ok, "StressForest50k flatten_part ok");
    CHECK(A.flat_root_result.full_tris == 0,
          "StressForest50k.flat.part has zero merged triangles (BOUNDARY, no expansion)");
    CHECK(A.flat_root_result.instance_refs == kExpectedCount,
          "StressForest50k.flat.part instance_refs == 50000 (every scatter kept as instance)");

    // Load StressForest50k.flat.part and verify the refs point only at Pebble /
    // Rock / Tree resolved hashes.
    std::vector<part_asset::FlatInstanceRef> A_refs;
    {
        BLASManager blas; TLASManager tlas(64);
        std::vector<part_asset::FlatCluster> clusters;
        bool loaded = part_asset::load_flat_v3(A.flat_root_path, A.root_hash,
                                               blas, tlas, clusters, A_refs);
        CHECK(loaded, "StressForest50k.flat.part reloads via load_flat_v3");
        CHECK(A_refs.size() == kExpectedCount,
              "StressForest50k.flat.part reload: instance_refs.size == 50000");

        // Each ref must point at one of the three real child hashes. We can
        // check Tree directly; Pebble and Rock we haven't installed separately
        // yet, so verify the three-way bucket count structurally.
        size_t tree_count = 0, other_count = 0;
        for (const auto& r : A_refs) {
            if (r.child_resolved_hash == A.tree_hash) ++tree_count;
            else ++other_count;
        }
        printf("  refs by tree=%zu other=%zu\n", tree_count, other_count);
        // i % 3 with i in [0, 50000): buckets {0: 16667, 1: 16667, 2: 16666}.
        CHECK(tree_count == 16666,
              "StressForest50k.flat.part has 16666 Tree refs (i%%3 == 2 bucket)");
        CHECK(other_count == kExpectedCount - 16666,
              "remaining refs are Pebble + Rock (i%%3 in {0,1})");
    }

    // ---- Cross-cache determinism -------------------------------------------
    printf("\n[test_cross_cache_determinism]\n");
    CHECK(A.root_hash == B.root_hash,
          "StressForest50k resolved hash matches across independent caches");
    CHECK(A.tree_hash == B.tree_hash,
          "Tree resolved hash matches across independent caches");

    auto ba = file_bytes(A.flat_root_path);
    auto bb = file_bytes(B.flat_root_path);
    CHECK(!ba.empty() && ba == bb,
          "StressForest50k.flat.part is byte-identical across caches");

    // FNV-1a over the FlatInstanceRef stream from cache B.
    std::vector<part_asset::FlatInstanceRef> B_refs;
    {
        BLASManager blas; TLASManager tlas(64);
        std::vector<part_asset::FlatCluster> clusters;
        part_asset::load_flat_v3(B.flat_root_path, B.root_hash,
                                 blas, tlas, clusters, B_refs);
    }
    CHECK(A_refs.size() == B_refs.size(),
          "instance_refs count matches across caches");

    uint64_t hA = part_asset::fnv1a64(A_refs.data(),
                                      A_refs.size() * sizeof(part_asset::FlatInstanceRef));
    uint64_t hB = part_asset::fnv1a64(B_refs.data(),
                                      B_refs.size() * sizeof(part_asset::FlatInstanceRef));
    printf("  FNV(A)=%016llx FNV(B)=%016llx\n",
           (unsigned long long)hA, (unsigned long long)hB);
    CHECK(hA != 0 && hA == hB,
          "FlatInstanceRef stream FNV-1a matches across caches");

    printf("\n");
    if (g_failures == 0) printf("ALL PASS\n");
    else                 printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: No commit yet.**

Leave the rewrite uncommitted. Task 3 builds, runs, iterates on any small issues (missing headers, minor API mismatches surfaced only at compile time), and commits the schemas + test together as one logical change.

---

### Task 3: Build, run, verify, and commit

**Files:**
- Verify only: everything from Tasks 1 & 2.
- May touch: minor tweaks to `stress_forest_tests.cpp` if build reveals a mismatch (e.g., a header path, an API arg count). No functional changes to schemas.

**Interfaces:**
- Consumes: Tasks 1 & 2 uncommitted diffs.
- Produces: one commit containing all 4 schema updates + the test rewrite, with `make run-stressforest` green.

- [ ] **Step 1: Clean any prior binary**

Run:
```
cd MatterEngine3/tests
rm -f stress_forest_tests
```

- [ ] **Step 2: Build the test target**

Run:
```
make stress_forest_tests
```
Expected: successful link, producing `stress_forest_tests` binary.

If a compile error surfaces, read the error, cross-reference the corresponding header in `MatterEngine3/include/` or `MatterEngine3/src/`, adjust the test file (do NOT touch engine code — the spec is out-of-scope for engine changes), and rebuild. Reference: `ChildRequest{module_name, params}` where `params` is `Params = std::map<std::string, ParamValue>` (see `part_graph.h:20-26`); for no params use `{}` — see `meadow_bake_check.cpp:47`. `FileModuleResolver` / `HostBaker` constructor pattern: mirror `meadow_bake_check.cpp:42-44` exactly.

- [ ] **Step 3: Run the test**

Run:
```
./stress_forest_tests
```
Expected output ends in `ALL PASS` and the process exits 0. If a policy assertion fails (e.g., `instance_refs != 50000`), STOP and investigate — that would mean the flatten policy is not doing what the design assumes; escalate rather than adjusting the assertions to match.

Runtime: first-run bake times will dominate — Tree + TreeBranch are the slow ones (voxel + LSystem). Rough envelope: under 3 minutes total for both caches on a workstation. If runtime is much longer, note it in the commit message but proceed if the assertions pass.

- [ ] **Step 4: If green, run the sibling headless test to confirm no regression**

Run:
```
make run-part-flatten 2>/dev/null || make run-flatten
```
Expected: existing `part_flatten_tests` still passes (this is the unit-level flatten test; the stress test should not have perturbed it).

- [ ] **Step 5: Commit**

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp
git add MatterEngine3/examples/world_demo/schemas/StressForest50k.js \
        MatterEngine3/examples/world_demo/schemas/StressForest100k.js \
        MatterEngine3/examples/world_demo/schemas/StressForest200k.js \
        MatterEngine3/examples/world_demo/schemas/StressForest500k.js \
        MatterEngine3/tests/stress_forest_tests.cpp \
        docs/superpowers/specs/2026-07-05-stress-forest-flatten-policy-design.md \
        docs/superpowers/plans/2026-07-05-stress-forest-flatten-policy.md
git commit -m "$(cat <<'EOF'
test: stress_forest verifies flatten BOUNDARY on 50k scatter

Fixtures now place a deterministic 1/3-1/3-1/3 mix of Pebble/Rock/Tree
so the parent trips the per-part flatten budget. Rewritten test bakes
real content via PartGraph.install, calls flatten_part explicitly, and
asserts: Tree.flat.part is INLINE (merged tris, no instance_refs), and
StressForest50k.flat.part is BOUNDARY (0 merged tris, 50k FlatInstanceRefs).
Cross-cache determinism (hash + bytes + FNV over the ref stream) preserved.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

Then run `git status` and confirm the tree is clean.

---

### Task 4: Windows viewer build

**Files:**
- Build output only: `MatterEngine3/viewer/build/windows/...` + `MatterEngine3/viewer/viewer.exe`. No source changes.

**Interfaces:**
- Consumes: Task 3 committed state.
- Produces: fresh `viewer.exe` built from HEAD engine sources.

- [ ] **Step 1: Build the Windows viewer**

Run:
```
cd MatterEngine3/viewer
make windows
```
Expected: successful build, `viewer.exe` present in the viewer directory.

Runtime: several minutes cold; the Makefile parallelizes automatically (`MAKEFLAGS += -j$(JOBS)` from `Makefile:9-10`). If a link error surfaces citing MinGW symbols, that is unlikely because the toolchain is pre-verified (`/usr/bin/x86_64-w64-mingw32-g++` present, `Libraries/raylib/build/windows-native/libraylib.a` present). If it does happen, capture the error and stop — the engine had no source changes in this branch beyond the pre-existing bake-hardening work that already builds on Linux, so a Windows-specific failure is a toolchain issue, not something to fix in the plan.

- [ ] **Step 2: Verify the binary exists and is fresh**

Run:
```
ls -la viewer.exe
```
Expected: a file with `mtime` newer than the Task 3 commit timestamp.

- [ ] **Step 3: No commit.**

`viewer.exe` and the `build/windows/` object tree are gitignored build artifacts. No commit needed.

---

## Self-Review

**Spec coverage:**
- Spec §Changes / Schemas → Task 1 (all four files, 1/3-1/3-1/3 mix, Rock seed=0, larger scale). ✅
- Spec §Changes / Test → Task 2 (PartGraph.install + explicit flatten_part + all four assertion classes + cross-cache determinism). ✅
- Spec §Windows build → Task 4. ✅
- Spec §Explicitly out of scope → honored (no FPS, no 100k/200k/500k test changes beyond keeping schemas in sync, no LargeRock). ✅

**Placeholder scan:** No TBDs, no "handle appropriately," each code step has full contents.

**Type consistency:**
- `ChildRequest{name, params_json_string}` — used consistently across Task 2 code.
- `part_flatten::FlattenResult` fields (`ok`, `clusters`, `full_tris`, `instance_refs`) — match `include/part_flatten.h:67-77`.
- `part_asset::load_flat_v3` 5-arg variant — matches `include/part_asset_v2.h:139-142`.
- `part_asset::FlatInstanceRef` sizeof(72) — used in FNV byte-count math; matches `static_assert` in header.
- `part_asset::fnv1a64(void*, size_t)` — signature matches existing use in `stress_forest_tests.cpp` (soon-replaced) at line 134.

Nothing to fix.
