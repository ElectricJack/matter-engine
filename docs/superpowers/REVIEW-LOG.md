# MatterEngine3 Autonomous Build — Review Log

This log tracks an autonomous subagent-driven build of the MatterEngine3 sub-projects
(SP-1 … SP-7) while Jack is away. Read top-to-bottom when you return. Each sub-project
section records: status, commits, test results, code-review verdict, and **anything
flagged for your attention**.

Build order (from the master plan): **SP-1 → SP-2 → SP-3 → SP-6 → SP-7 → SP-4 → SP-5**.

Branch: `feat/voxel-box-imposter`. All NEW code lives under `MatterEngine3/`; the
`MatterSurfaceLib/` prototype is consumed READ-ONLY (no edits).

---

## SP-1 — Part Artifact v2 — ✅ DONE (reviewed: approved)

- **Commits:** `da52690..654b092` (8 commits)
- **Tests:** `make -C MatterEngine3/tests run-partv2` → "All part_asset_v2 tests passed" (clean rebuild verified).
- **Final code review:** APPROVED. No Critical/Important issues.
- **What it is:** `part_asset_v2.{h,cpp}` — `compute_resolved_hash` (order-independent
  child fold), `cache_path_resolved`, `save_v2`/`load_v2` with a 40-byte header
  (`sizeof(ChildInstance)` layout guard, FNV-1a content hash) + child-instance table +
  ordered LOD array appended to the v1 body byte-for-byte. Created the first
  `MatterEngine3/tests/Makefile`.
- **FOR YOUR REVIEW (optional, non-blocking):**
  1. Reviewer suggested a one-line comment explaining *why* the v1 serialization helpers
     (`put`/`put_bytes`/`Reader`) are duplicated in `part_asset_v2.cpp`'s anon namespace
     (they have internal linkage in v1's TU, so they're cross-TU invisible — a future
     reader might wrongly try to dedupe them). Not applied.
  2. `load_v2` doc says "passive — no backend action", but it does the internal
     `tlas.draw_batch`/`tlas.build` rebuild like v1. Wording refers to children/LODs being
     returned, not installed. Accurate in context; left as-is.

---

## SP-2 — Script Host (QuickJS-ng) — ✅ DONE (reviewed: approve with minor notes)

- **Commits:** `4838d4e..075af18` (14 commits, includes the vendored `Libraries/quickjs-ng/`).
- **Tests:** `make -C MatterEngine3/tests run-script` → all ScriptHost tests pass; `run-partv2`
  still green from clean. Zero changes to `MatterSurfaceLib/`.
- **Final code review:** APPROVE WITH MINOR NOTES. All 5 high-priority determinism/safety
  points verified positive (fresh `JSRuntime`+`JSContext` per bake; restricted intrinsics via
  `JS_NewContextRaw` with Date deliberately omitted → `typeof Date === "undefined"`; seeded
  SplitMix64 `Math.random` from canonical params; byte-identical determinism test comparing
  full `.part` vectors; fail-closed `save_v2` gated behind `error.ok` with a real
  `JS_SetInterruptHandler` wall-clock budget).
- **What it is:** `script_host.{h,cpp}` embeds QuickJS-ng v0.10.0; one fresh runtime/context
  per bake; a Processing-style `Part` DSL (`part_base.js.h`); CSG lowering (`csg_lowering`)
  that turns sphere/box primitives into particle stamps with NO mesher change; seeded PRNG
  (`dsl_rng.h`). New `MatterEngine3/Makefile` builds `libmatter_engine3.a` from 5 QuickJS-ng C
  objects + the host/DSL/CSG/part-asset C++ objects.
- **⚠️ FOR YOUR REVIEW (key item):** SP-2 **modified SP-1's `part_asset_v2.cpp` `save_v2`** to
  fix a real determinism bug — `TriEx` trailing padding (named members occupy 92 of the
  96-byte struct) held allocator garbage, so identical bakes byte-differed and broke
  content-addressing. Fix zeroes a staged `TriEx` and copies only the named-member extent
  (`kTriExPad = 92`) before writing. Verified correct + SP-1 regression passes. **Reviewer's
  Important note:** `kTriExPad = 92` is a magic offset guarded only by `sizeof`; recommended
  adding `static_assert(sizeof(TriEx) == 96)` and `static_assert(offsetof(TriEx, ao2) == 88)`
  so a future TriEx layout change fails at compile time instead of silently re-breaking
  determinism. Non-blocking; logged for your call.
- **FOR YOUR REVIEW (minor, non-blocking):**
  1. `MatterEngine3/Makefile` was *created* (plan said "modified") as a static library — no app
     `main` exists yet. Reviewer judged this reasonable.
  2. `derive_seed` finds the seed via a substring scan for `"seed":` in the canonical params,
     which could in principle false-match a nested value. Still fully deterministic; cosmetic.

---

## SP-3 — Part Graph Install — ✅ DONE (reviewed: approved)

- **Commits:** `9223267..b31b1a1` (12 commits).
- **Tests:** `make -C MatterEngine3/tests run-graph` → "All part_graph tests passed" (verified
  from clean). Zero changes to `MatterSurfaceLib/` (diff scoped there is empty).
- **Final code review:** APPROVED. All three hard constraints confirmed; every high-value
  correctness point (deterministic children-first topo order, back-edge cycle detection,
  incremental cache hit/miss + params dedup, transitive invalidation + no-orphan-bake,
  fail-closed child-failure propagation) holds under scrutiny.
- **What it is:** `part_graph.{h,cpp}` — resolves each part's content-addressed hash (delegated
  to the SP-1 fold via a `Baker` seam, never recomputed locally), bakes children-first in
  deterministic topological order, skips already-cached parts (incremental), dedupes by
  resolved hash, detects cycles on the resolution stack, and propagates child-bake failures up
  to a named install error. World roots discovered from a `world.manifest`. Only 4 files
  touched, all under `MatterEngine3/`.
- **⚠️ FOR YOUR REVIEW (cross-subproject debt — affects later work):** The master contract
  (C-2) lists a `ScriptHost::eval_requires(source, params_json) → vector<RequiredChild>` method,
  but **SP-2 never shipped it** — `script_host.h` exposes only `bake_source` and `resolve_hash`.
  So SP-3's *real* `FileModuleResolver`/`HostBaker` adapters (the thin SP-2 glue, ~120 lines)
  are compiled out behind `#if defined(MATTER_HAVE_SCRIPT_HOST)` (default OFF). All graph
  *orchestration logic* (resolve/memoize/topo/cycle/dedup/invalidation/failure) is in the
  **unguarded** path and fully tested via fakes that mirror SP-1's real `compute_resolved_hash`.
  Reviewer's read: a reasonable seam, not a hidden gap — but `eval_requires` should be landed in
  SP-2 so the guard can flip on and an end-to-end integration test can be added. **I will carry
  this into SP-7 (shared script library), which extends the same import/resolve mechanism.**
- **FOR YOUR REVIEW (minor):** `params_from_json` is a hand parser (v1 scope: skips unknown
  shapes, assumes escape-free strings). It lives entirely inside the OFF guard, so it has no
  test coverage yet — a latent footgun if param shapes broaden.

---

## SP-6 — Triangle-path Variations — ✅ DONE (reviewed: approved)

- **Commits:** `a89f296..69d8c19` (7 commits).
- **Tests:** `make -C MatterEngine3/tests run-trivar` → "All triangle_variation tests passed"
  (verified from clean). Zero changes to `MatterSurfaceLib/` (diff scoped there is empty).
- **Final code review:** APPROVED. All 3 hard constraints confirmed; thin-surface-vs-field-stamp
  distinction, single-BLAS merge, deterministic tube lowering, and variation/LOD independence all
  verified. Two non-blocking Minor notes: a redundant (dead) loop condition in `endShape`
  (`triangle_emit.cpp:55`) and an unguarded degenerate-tube case (zero-length line / zero radius).
- **What it is:** `triangle_emit.{hpp,cpp}` — direct-triangle emission into the Tri/TriEx build
  buffer (thin surfaces, no field interaction), voxel + direct triangles merged into one part
  BLAS, skinned-line tubing lowered to stepped-sphere solid geometry, and `instance(child,
  variation)` records with content-hash variation dedup that consumes SP-1's real
  `ChildInstance` + `compute_resolved_hash` (re-exported into `tri_emit`). Variation and LOD are
  independent axes. Only 4 files touched, all under `MatterEngine3/`.
- **FOR YOUR REVIEW (minor deviations, all reasonable):**
  1. Renamed local `PI` → `kPI` in `triangle_emit.cpp` — raylib's `PI` macro leaks in via
     `blas_manager.hpp` and broke the plan's verbatim `const float PI`. Pure rename.
  2. `run-trivar` Makefile target links more sources than the plan listed (part_asset_v2.cpp,
     MSL part_asset.cpp, tlas_manager.cpp, material_registry.c) because Task 5 calls the real
     `compute_resolved_hash` — mirrors the existing PARTV2/GRAPH targets. Still GL-free at logic
     level.
  3. Tests use qualified `tri_emit::ChildInstance` (re-export of SP-1's type) rather than the
     plan's bare local mirror — identical semantics, matches the rest of the pipeline.

---

## Finish-up pass (after SP-6) — ✅ keystone gaps closed (`112edf7..0ff702b`)

Acted on the audit below. All three concrete gaps are now closed (3 commits, zero
`MatterSurfaceLib/` changes):

1. **`ScriptHost::eval_requires` shipped** (`112edf7`) — signature
   `std::vector<RequiredChild> eval_requires(const std::string& source, const std::string&
   params_json)`, matching the SP-3 adapter call site. A part declares children via
   `static requires(params)` / `static requires` array → `[{module, params}, ...]`; reuses the
   `merge_params_canonical` path, evals in the fresh restricted bake context (no Date/require/
   fetch), canonicalizes child params, fail-closed (empty on no-requires/throw/malformed), never
   runs `build()`. 5 new ScriptHost unit tests. **The SP-3 integration path now ACTUALLY RUNS**
   (not compile-only): new `run-graph-integration` target builds the guarded
   `FileModuleResolver`/`HostBaker` adapters with `-DMATTER_HAVE_SCRIPT_HOST` and runs an
   end-to-end test (writes real `Wall.js`+`Tower.js`, installs through a real `ScriptHost`,
   asserts `.part` files land + second install is all cache hits + identical children dedup to
   one artifact). → C-2 contract gap RESOLVED.
2. **TriEx layout pinned** (`c284c5b`) — `static_assert(sizeof(TriEx)==96)` +
   `static_assert(offsetof(TriEx,ao2)==88)` (verified `ao2` is the last named member at offset
   88 in the read-only `bvh.h`). Determinism can no longer silently re-break on a layout change.
3. **MatterEngine3 registered in `build-all.sh`** (`0ff702b`) — added to `SIMPLE_PROJECTS` and a
   test block running `run-partv2 run-script run-graph run-graph-integration run-trivar`.
   Verification scoped to the MatterEngine3 portion (the five targets pass; full `./build-all.sh
   test` not run to avoid the raylib rebuild + unrelated sibling projects).

Verified post-pass: `run-script` / `run-graph` / `run-graph-integration` / `run-partv2` all green.

- **⚠️ ONE THING TO REVIEW (path contract):** `HostBaker::cached` joins `parts_dir_ + "/" +
  cache_path_resolved(hash)`, and `cache_path_resolved` already returns `"parts/<hash>.part"` —
  so `parts_dir_` must be the **parent** of `parts/` (the working dir), not the `parts/` dir
  itself. The integration test passes `HostBaker(host, ".")` and chdirs into a sandbox. Matches
  the SP-3 plan as written, but it's a subtle contract — confirm SP-4/SP-5 callers pass
  `parts_dir_` consistently, else cache checks silently miss and everything re-bakes.

---

## Mid-build audit (after SP-6) — outstanding / finish-up work

A full gap audit of the four completed sub-projects (SP-1/2/3/6) found everything substantively
DONE and tested, with these loose ends carried forward (ordered by importance):

1. **⚠️ `ScriptHost::eval_requires` was never shipped (C-2 contract gap).** SP-2's plan said it
   lands in Task 5 alongside the hash plumbing; only `resolve_hash` landed. `script_host.h`
   exposes `bake_source` / `resolve_hash` / `merge_params_canonical` only. The `RequiredChild`
   struct exists but has no producer. Consequence: SP-3's real `FileModuleResolver`/`HostBaker`
   adapters are compiled out behind `#if defined(MATTER_HAVE_SCRIPT_HOST)` (OFF), so no
   end-to-end install runs against a real host. **This is the keystone gap — fixing it unblocks
   the guard flip, the SP-3 integration test, and SP-7's import/resolve mechanism.** → being
   addressed now (task #43).
2. **TriEx layout static_assert not applied.** `part_asset_v2.cpp` uses a bare
   `constexpr kTriExPad = 92`; add `static_assert(sizeof(TriEx)==96)` + `offsetof(ao2)==88` so a
   layout change can't silently re-break determinism (task #44).
3. **MatterEngine3 not registered in `build-all.sh`** (neither build nor `test` paths) — so
   `./build-all.sh test` never exercises run-partv2/script/graph/trivar. Master plan required it
   (task #45).
4. Smaller deferred nits (non-blocking): SP-3 `params_from_json` is an untested hand parser
   inside the OFF guard (gets coverage once the guard flips); SP-2 `derive_seed` substring scan
   for `"seed":`; SP-1 one-line comment on why v1 `put`/`Reader` helpers are duplicated.

Untracked items under `MatterEngine3/` are only build artifacts (`tests/parts/` fixtures, the
`triangle_variation_tests` binary) — no uncommitted source. SP-1→SP-6 history is clean.

---

## SP-7 — Shared Script Library — ✅ DONE (reviewed: approve with minor notes; host-wiring follow-up)

- **Commits:** `48d2d45..2221ae6` (8 commits).
- **Tests:** `make -C MatterEngine3/tests run-shlib` → "All shared_lib tests passed" (verified from
  clean). Zero changes to `MatterSurfaceLib/`.
- **Final code review:** APPROVE WITH MINOR NOTES. Source-fold determinism/ordering (sorted by
  canonical resolved specifier, NUL-separated, part source always segment 0), transitive
  invalidation (proven both directions: edit imported leaf → importer + transitive importer
  rehash; edit unimported module → no change), determinism contract (no Date/crypto/fetch in any
  shared module; `rng.js` xoshiro128** matches the C++ reference bit-for-bit, verified via node),
  and consumption of SP-1's real `compute_resolved_hash` all verified. Reviewer also confirmed the
  implementer's rewrite of `strip_comments_and_strings` is a genuine, correct bug fix (the plan's
  version blanked the `from '<spec>'` target string, yielding empty specifiers).
- **What it is:** v1 helper modules `MatterEngine3/shared-lib/{lsystem,bezier,vecmath,geometry,
  rng}.js`, a QuickJS-free `module_resolver` C++ unit (import-specifier parser → file resolution →
  transitive gather → canonical fold), and a seeded-RNG contract for `Math.random` / seed-from-
  params. New `run-shlib` test target.
- **⚠️ FOR YOUR REVIEW (Important — host wiring follow-up, being closed now):** SP-7's headline
  guarantee ("editing a shared module rebakes importers **through the real host**") is currently
  proven only at the C++ helper level — the fold→`compute_resolved_hash` path. The *end-to-end*
  bake test is compiled out behind `#ifdef SP2_SCRIPT_HOST` and SKIPPED, because `ScriptHost` has
  no `set_shared_lib_root` and its `resolve_hash`/`bake_source` assemble source bytes from raw
  `source` only — the folded buffer is never routed in. Same shape as the `eval_requires` gap.
  Reviewer's 4-step wiring: (1) add `set_shared_lib_root`; (2) fold via `module_resolver::
  fold_sources` and pass `fold.folded` as `source_bytes` in BOTH resolve_hash/bake_source
  (fail-closed); (3) feed resolved module sources into QuickJS-ng's module loader so
  `import 'shared-lib/x'` resolves at bake; (4) add `-DSP2_SCRIPT_HOST` + host/QuickJS sources to
  `run-shlib` so the end-to-end test runs. → **CLOSED** (`e35c382..86ef0c3`): `set_shared_lib_root`
  added; `resolve_hash`/`bake_source` now fold (part + transitively-imported shared-lib sources,
  NUL-separated, canonical order) and hash the identical buffer via `compute_resolved_hash`,
  fail-closed; a `JS_SetModuleLoaderFunc` loader serves module source from the resolver's
  in-memory set (no filesystem at eval). The SP-7 end-to-end test now RUNS: an importing part
  bakes through the real host, same seed → same hash, different seed → different hash, and editing
  `geometry.js` invalidates the importer's hash. **⚠️ Eval-path change to review:** importer parts
  (those with `import` statements) now eval as ES **modules** across `merge_params_canonical` /
  `eval_requires` / `bake_source` (import is illegal in `JS_EVAL_TYPE_GLOBAL`); non-importer parts
  and all existing SP-2/SP-3 parts still eval as classic GLOBAL scripts unchanged (gated on
  `shared_lib_root_` set AND the part actually importing). `Promise` intrinsic added only on the
  module path. Full regression (run-partv2/script/graph/graph-integration/trivar/shlib) green from
  clean — SP-2/SP-3 not regressed.
- **FOR YOUR REVIEW (minor):** `seed_from_params_json` is substring-based (could false-match a key
  inside a nested string); acknowledged as v1, SP-2's structured route bypasses it.

---

## SP-4 — Composition to World — ✅ DONE (reviewed: approved)

- **Commits:** `0ca5e7a..593eb16` (9 commits). Plus `e997a35` (my follow-up: added the missing
  `run-shlib` to the `build-all.sh` MatterEngine3 sweep — SP-7's end-to-end target had been left
  out).
- **Tests:** `make -C MatterEngine3/tests run-comp` → "OK" (13 tests, verified from clean). Zero
  changes to `MatterSurfaceLib/`.
- **Final code review:** APPROVED. No Critical/Important findings.
- **What it is:** `lod_bake` (bakes N monotone screen-size LOD levels per part, round-tripped
  through SP-1's `save_v2`/`load_v2`), `world_flatten` (recursive flatten to world-space instances
  with composed parent×child row-major transforms + dedup/depth/budget fail-closed guards),
  `sector_grid` (deterministic floor-binning), `lod_select` (closest-instance per-sector LOD
  selection with escalation), and a variation/LOD-independence test. New `run-comp` target.
- **Seam reconciliation (resolved cleanly):** SP-4 does `using part_asset::LodLevel;` and consumes
  SP-1's real type directly — NO divergent `blas_index` scalar mirror; the round-trip is a
  same-type `blas_indices` (vector) compare. Reviewer confirmed.
- **⚠️ FOR YOUR REVIEW (Minor — stale doc label, no bug):** SP-1's header comment
  (`part_asset_v2.h:47`) labels the LOD convention "coarsest-to-finest", but SP-4 (and its plan,
  and `lod_select.h`) actually use **index 0 = finest** (descending screen-size thresholds).
  Reviewer traced bake→save→load→select and confirmed SP-4 is internally consistent end-to-end —
  the only issue is the stale label in SP-1's read-only header (zero runtime effect). Worth a
  one-line comment fix next time SP-1 is touched.

---

## SP-5 — Dev Live-Edit — ✅ DONE (reviewed: approved)

- **Commits:** `3d22a00..910f255` (12 commits; the last wires `run-dev` into the build-all sweep).
- **Tests:** `make -C MatterEngine3/tests run-dev` → "ALL PASS" (14 test functions, verified from
  clean). Zero changes to `MatterSurfaceLib/`.
- **Final code review:** APPROVED. No Critical/Important findings.
- **What it is:** the developer live-edit loop — `inotify_watcher` (Linux backend, `#ifdef
  __linux__`; `win_watcher.h` is an explicit deferred throwing stub) feeds a `LiveEditSession`
  (`live_edit.{h,cpp}`) that debounces/coalesces rapid saves, maps changed files back to affected
  parts (shared module fans out to all transitive importers, deduped), computes the upward rebake
  cone, topo-rebakes only that scoped set via the same `reresolve→bake` seam SP-3 uses, and
  re-flattens affected roots. Fail-closed: a failed bake (or exceeded time budget → structured
  `BudgetExceeded`) keeps last-good and skips reflatten, retrying on the next save. New `run-dev`
  target.
- **Integration coverage:** two REAL Linux tests RUN (not skipped) — `test_real_inotify_temp_dir`
  (real `InotifyWatcher` + real file write) and `test_e2e_real_watch_to_rebuild` (real
  watcher → debounce → cone → bake → reflatten). They use fake SP-2/3/4 collaborators for the
  bake/flatten step, which the reviewer judged acceptable (the real ScriptHost bake path is
  already covered by run-script / run-graph-integration / run-shlib; SP-5's job is the
  watch/scope/coalesce/fail-closed orchestration, which the fakes exercise precisely).
- **FOR YOUR REVIEW (minor, non-blocking):** `run-dev` links raylib/GL though its sources are
  GL-free (harmless boilerplate from the shared Makefile); a cosmetic debounce clock detail
  (`last_event_ms_` resets to 0 after firing — safe with the monotonic-positive timestamps both
  real backends emit); Windows watcher deferred by design.

---

## 🏁 BUILD COMPLETE — all seven sub-projects done (SP-1…SP-7)

Build order executed: SP-1 → SP-2 → SP-3 → SP-6 → SP-7 → SP-4 → SP-5, plus the mid-build
finish-up pass (eval_requires / TriEx guard / build-all registration) and the SP-7 host-wiring
follow-up. All seven reviewed; SP-1/3/6/4/5 approved, SP-2/7 approved-with-minor-notes. Every
headless suite is wired into `./build-all.sh test`: run-partv2, run-script, run-graph,
run-graph-integration, run-trivar, run-shlib, run-comp, run-dev. A final full-build review across
the entire MatterEngine3 implementation follows below.

<!-- Subsequent sub-projects appended below as they complete. -->
