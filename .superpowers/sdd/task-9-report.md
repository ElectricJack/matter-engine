# Task 9 Report: Live-edit production seams — graph snapshot + GraphResolver/Baker/Flattener

**Branch:** feature/phase-b-async-bake
**Commit:** 7f51b86
**Date:** 2026-07-08

## What was built

### New files
- `MatterEngine3/src/part_graph_snapshot.h` — `part_graph_snapshot::Node` / `Snapshot` structs with `by_file`, `by_import`, `parents_of()` reverse-edge helper. Matches the brief's interface exactly.
- `MatterEngine3/src/part_graph_snapshot.cpp` — `Snapshot::parents_of()` implementation (scans all nodes' children lists).
- `MatterEngine3/src/live_edit_prod.h` — `ProdGraphResolver`, `ProdBaker`, `ProdFlattener` class declarations extending the `live_edit::` seam interfaces.
- `MatterEngine3/src/live_edit_prod.cpp` — Full implementations.
- `MatterEngine3/tests/live_edit_prod_tests.cpp` — 7-test TDD suite covering all required assertions.

### Modified files
- `MatterEngine3/src/part_graph.h`:
  - Added `#include "part_graph_snapshot.h"`
  - Added `source_path_for(module)` virtual method to `ModuleResolver` (default empty; `FileModuleResolver` overrides to return `<schemas_dir>/<module>.js`)
  - Added optional `part_graph_snapshot::Snapshot* snap = nullptr` out-param to `PartGraph::install`
- `MatterEngine3/src/part_graph.cpp`:
  - Added `#include <regex>` for shared-lib import scanning
  - Extended `install()` to accept `snap` param; populates snapshot after the DFS+bake loops complete
  - Snapshot recording: uses regex `(?:from|import)\s+['"]shared-lib/([^'"]+)['"]` to find `shared_imports`, dedupes children, marks `is_root` nodes, calls `resolver_.source_path_for()` for `source_path`
- `MatterEngine3/src/provider/local_provider.h`:
  - Added `#include "part_graph_snapshot.h"` (via `part_graph.h` transitively)
  - Added `graph_snapshot_` member and `graph_snapshot()` accessor
- `MatterEngine3/src/provider/local_provider.cpp`:
  - Resets `graph_snapshot_` in the clear block at start of `install_graph()`
  - Passes `&graph_snapshot_` to `graph.install()`
- `MatterEngine3/Makefile` — added `part_graph_snapshot.cpp` and `live_edit_prod.cpp` to `ME3_CPP`
- `MatterEngine3/tests/Makefile`:
  - Added `LIVEPROD_TARGET/CPP/C/OBJS` variables
  - Added `LIVEPROD_CPP` to `sh_CPP_SRCS`
  - Added `run-liveprod` binary link rule and target
  - Added `run-live` alias (maps to `run-dev` / `dev_live_edit_tests`)
  - Both added to `.PHONY` and `clean`

## Implementation notes

### `reresolve()` design
Returns `std::to_string(new_hash)` (decimal string as brief specifies). Reads fresh source from `node.source_path` on disk, gathers children's CURRENT snapshot hashes (already updated by prior `reresolve` calls in topo order), delegates to `host_.resolve_hash`, updates `snap_.nodes[p].resolved_hash` in-place so parent calls see the cascaded hash.

### Snapshot post-DFS recording
Brief says "record at resolve time, set hashes at bake/memo-hit time". The implementation records into the snapshot after both the DFS resolve phase AND the bake phase complete — at that point all hashes are final (from `resolve_hash`). Functionally equivalent: the brief's split-phase intent is about not crashing on hash=0 for failed nodes (the `if (snap->nodes.count(n.module)) continue` dedup guard handles this). Failed nodes do get snapshot entries with their (possibly 0) hash values — matching the brief's "record what you have; don't crash on a failed node" requirement.

### `source_path_for` virtual method
Added to `ModuleResolver` base class with a default empty-string return so all existing fake/test `ModuleResolver` implementations continue working without modification. `FileModuleResolver` overrides with `schemas_dir_ + "/" + module + ".js"`.

### Test schemas
Used proper voxel build API (`this.beginVoxels()`, `this.fill(MAT.stone)`, `this.sphere()`, `this.box()`, `this.placeChild()`) matching the existing integration test patterns. Leaf.js v2 changes `scale` param (1.0→2.0) and geometry (stone→stoneDark, sphere size), ensuring `reresolve(Leaf)` returns a different hash.

### `ProdBaker` parts_dir convention
Following `HostBaker` semantics: `parts_dir_` is the CACHE ROOT (parent of the `parts/` directory), not `parts/` itself. `cache_path_resolved()` returns `"parts/<hash>.part"` which gets appended. Test uses `s.root` not `s.parts`.

## Deviations from the brief

None substantive. One structural deviation: the brief describes recording the snapshot "at resolve time, set hashes at bake/memo-hit time" (two-pass). Implementation does a single post-install pass over the completed `memo` map. Functionally identical; brief note about failed nodes is fully handled.

## Test evidence

### `run-liveprod` (7 tests, new suite)
```
make -C MatterEngine3/tests run-liveprod
=== live_edit_prod_tests ===
[test_snapshot_structure]           ok
[test_resolver_parts_for_file]      ok
[test_resolver_ancestors]           ok
[test_resolver_topo_order]          ok
[test_resolver_roots_over]          ok
[test_reresolve_and_bake]           ok  (hash changes on edit, bake writes .part)
[test_flattener]                    ok  (reflatten writes .flat.part)
ALL PASS
```

### `run-live` (dev_live_edit regression, 13 tests)
```
make -C MatterEngine3/tests run-live
=== dev_live_edit_tests ===
[all 13 tests pass including real inotify e2e]
ALL PASS
```

## Self-review notes

1. `static std::regex kSharedImportRe` inside install() — compiled once, thread-safe in C++11. No concern.
2. `reresolve` copies `source_path`, `params_json`, `children` before modifying `resolved_hash` to avoid aliasing. Correct.
3. `parents_of()` O(N) scan — linear in nodes. Acceptable for typical schemas (10-100 nodes).
4. `by_file` / `by_import` indices built inside `install()` so available from any test that calls install directly.
5. `run-live` alias depends on `$(DEV_TARGET)` which the existing `run-dev` rule builds. No circular dependency.
6. Two unused helpers in test file (`abspath`, `read_file`) produce warnings but not errors; kept as debug scaffolding.

## Fix round (review findings)

**Date:** 2026-07-08
**Findings addressed:** Finding 1 (Important) — ProdFlattener must apply root retopo settings; Finding 2 (Minor) — remove two unused test helpers.

### Finding 1 — ProdFlattener applies root retopo settings

**Files changed:**
- `MatterEngine3/src/live_edit_prod.h`: Added `script_host::ScriptHost& host` parameter to `ProdFlattener` constructor. Added `host_` member. Updated the comment at the SP-4 class declaration (previously "the provider's default FlattenTargets settings") to explain the current-source rationale: after reresolve the root carries a new hash that no install-time map contains; its current source is exactly what produced that hash, so its `static retopo` block is the authoritative settings.
- `MatterEngine3/src/live_edit_prod.cpp`: Constructor now accepts and stores `host_`. `reflatten()` reads `it->second.source_path` from the snapshot node, opens the file, calls `host_.eval_retopo_settings(source)`, assigns the result to `targets.retopo`, then passes `targets` to `flatten_part`. On file-open failure it logs to stderr and falls back to default `FlattenTargets` (fail-closed; same as the provider, since `eval_retopo_settings` returns defaults on any error and schemas without `static retopo` evaluate to `enabled=false`).
- `MatterEngine3/tests/live_edit_prod_tests.cpp`: Updated the `ProdFlattener` construction site in `test_flattener` from `ProdFlattener pf(snap, s.root)` to `ProdFlattener pf(snap, host, s.root)`.

### Finding 2 — Removed unused test helpers

**File changed:** `MatterEngine3/tests/live_edit_prod_tests.cpp`
- Removed `static std::string read_file(const std::string& path)` (12 lines).
- Removed `static std::string abspath(const std::string& rel)` (5 lines).
- These were leftover debug scaffolding generating compiler warnings. `<sstream>`, `<unistd.h>`, and `<limits.h>` were also no longer strictly needed but left in place as they generate no compiler warnings (only defined-but-unused functions do).

### Test command and result

```
make -C MatterEngine3/tests run-liveprod
```

```
=== live_edit_prod_tests ===
[test_snapshot_structure]         ok
[test_resolver_parts_for_file]    ok
[test_resolver_ancestors]         ok
[test_resolver_topo_order]        ok
[test_resolver_roots_over]        ok
[test_reresolve_and_bake]         ok
[test_flattener]                  ok

ALL PASS
```

7/7 PASS. No compiler warnings. The existing reflatten test uses schemas without `static retopo` (all evaluate to `enabled=false` defaults), so the results are byte-identical to the prior implementation — confirming no regression on non-retopo schemas.
