# Repository Layout and Cache Consolidation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move authored content out of the engine library directory, stop bake artifacts leaking into arbitrary working directories, and rename the application to match what it actually is. Four independent phases, ordered so each is separately shippable and revertable.

**Architecture:** No engine behaviour changes. The engine already treats `project_dir` and `cache_root` as caller-supplied configuration — `LocalProviderConfig::for_project()` derives `objects/`, `shared-lib/` and `schemas/` from `project_dir`, and `PartStore`/`resolve_cache` derive everything from `cache_root`. This plan changes what callers pass and where directories sit; it does not change how the provider resolves anything. Phase 1 is the only phase that alters runtime behaviour, and only by removing a relative-path default.

**Tech Stack:** C++17, GNU Make, MSYS2 UCRT64, existing MatterEngine `.part` cache/provider pipeline.

## Global Constraints

- Phases are independent and ordered by value/risk. Stopping after any phase must leave the tree green.
- This repository does not discover sources by glob. Every path change must be reflected in the explicit source lists in `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile`, `MatterSurfaceLib/Makefile`, `MatterSurfaceLib/tests/Makefile` and `MatterViewer/Makefile`.
- **The verification gate for every phase includes `make -C MatterViewer windows`.** A prior refactor (`c3f0577a`) passed the kernel build and all headless suites while leaving the Windows viewer target broken for two independent reasons — vpath and a static pattern rule. Kernel-plus-tests is not sufficient evidence.
- Do not rewrite historical documents. `.superpowers/sdd/*.md` and `docs/superpowers/{specs,plans}/*` are records of what was done at the time; they may keep stale names.
- Run Windows builds from MSYS2 UCRT64 with:

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
```

and pass TMP/TEMP **as make variables** (MSYS2's make clobbers the environment; exporting them is not enough):

```bash
make -C <dir> <target> TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp"
```

## Standard Verification Gate

Run after every phase. Referred to below as "the gate".

```bash
make -C MatterEngine3 clean && make -C MatterEngine3 -j4
make -C MatterViewer windows
for t in run-script run-evalworld run-world-definition run-iso; do make -C MatterEngine3/tests $t GRAPHICS=GRAPHICS_API_OPENGL_43; done
make -C MemoryLib && make -C SpatialQueryLib && make -C ParticleFlowLib && make -C MeshChartingLib
```

Known pre-existing failures, not regressions: `make -C MatterSurfaceLib` fails because its Makefile hardcodes `x86_64-w64-mingw32-g++-posix`, which is not installed (see `tech-debt.md` §7); ASan-based `test` targets cannot link under UCRT64.

---

## Phase 1 — Stop bake artifacts leaking (behaviour fix)

**Why first:** this is a bug, not organisation, and it is independent of every other phase. `MatterEngine3/src/matter_engine.cpp:3652` defaults `cache_root` to the **relative** string `"cache"`, so any process started from a different working directory scatters `.part` files wherever it happened to launch.

The symptom is `.gitignore` carrying **seven rules for one concept** (lines 55, 56, 65, 99, 100, 105, 107, 108) — five distinct locations have been observed emitting `parts/`. Currently on disk: 32 stray `.part` at repo root, 2 `.vxi` under `imposters/`, 1 under `MatterSurfaceLib/parts/`.

- [ ] Add a failing test asserting that constructing the engine without an explicit `cache_root` either fails loudly or produces an absolute path — never a bare relative `"cache"`.
- [ ] Replace the relative default at `matter_engine.cpp:3652`. Preferred: derive from `project_dir` (`<project_dir>/.cache`), matching the convention `AssetBrowser` already documents at `MatterViewer/asset_browser.cpp:148-157` and `LocalProviderConfig::for_project()`. Fall back to erroring if `project_dir` is also unset.
- [ ] Audit `MATTER_CACHE_ROOT` handling at `MatterViewer/main.cpp:831` so an explicit override still wins and is canonicalised to absolute.
- [ ] Confirm no remaining writer composes an output path from a relative root. `MatterEngine3/src/part_asset_v2.cpp:99,106` builds `"parts/" + hash + ".part"` — that is a *layout within* `cache_root` and is correct; verify every caller joins it onto an absolute root.
- [ ] Delete the stray artifacts (`parts/`, `imposters/`, `MatterSurfaceLib/parts/`) and re-run a bake to confirm they do not reappear outside the intended cache.
- [ ] Collapse the seven `.gitignore` rules into one (plus one for `.cache/`). Removing a rule that still catches real output is a regression — verify by running a bake first, then pruning.
- [ ] Run the gate.

**Decision required before starting:** keep caches co-located (`<project>/.cache/<world>`, self-contained and deletable per project) or centralise (`.cache/<project>/<world>` at repo root, one place to purge, content dirs stay clean). The leak fix is valuable under either choice; centralising is the larger change because the co-located convention is embedded in `AssetBrowser` and `for_project()`.

---

## Phase 2 — `MatterEngine3/examples/` → `projects/`

**Why:** `world_demo` is the primary test corpus (279 `.part`, 46 `.js`, referenced by 14 test files), not an example. Authored content living inside a *library* directory is the actual complaint, and "examples" understates what it is.

**Why `projects/`:** the code already calls this concept a project — `project_dir`, `LocalProviderConfig::for_project()`, `AssetBrowser::project_index`, `WorldEntry::project_dir`. Reusing the existing vocabulary removes a translation step; `assets/` or `content/` would introduce one.

- [ ] `git mv MatterEngine3/examples projects`. Both `primitive_demo` and `world_demo` move whole, preserving `worlds/`, `objects/`, `shared-lib/`, `schemas/`, `editor/`, `WorldData/`, `.cache/`.
- [ ] Update the 14 test files under `MatterEngine3/tests/` that hardcode `../examples/...`. From `MatterEngine3/tests/` the new relative path is `../../projects/...`. Known sites include `api_tests.cpp:24`, `example_world.cpp:85-86`, `gallery_bake_tests.cpp:52-53`, `grass_lod_tests.cpp:313`, `lighting_garden_tests.cpp:55-56`, `part_graph_integration_tests.cpp:747`, `rock_bake_profile.cpp:38`, `sector_bake_tests.cpp:28`, `stress_forest_tests.cpp:146`, `tileset_meadow_manifest_tests.cpp:26`.
- [ ] Update the editor's world-scan roots so the asset browser finds `projects/`.
- [ ] Update `.gitignore` paths that referenced `MatterEngine3/examples/**/.cache`.
- [ ] Update `CLAUDE.md` and `README.md` references.
- [ ] Run the gate, plus the bake-dependent suites that consume the corpus (`run-iso`, and any of `gallery_bake`/`example_world`/`sector_bake` that link on this platform).

---

## Phase 3 — `MatterViewer` → `MatterEditor`

**Why:** the source list is unambiguous — `asset_browser`, `bake_lab`, `editor_model`, `scene_tree_panel`, `properties_panel`, `properties_registry`, `specialized_editors`, `toolbar_panel`, `console_panel`, `gizmo`, `selection_set`, `selection_outline`, `viewport_pick`, `part_workbench`, `lod_inspector`, `event_inspector`. That is an editor shell with a viewport, not a viewer.

**On "runtime vs editor":** `libmatter_engine3.a` is the runtime. This target is the authoring shell wrapping it. If a shipping runtime is wanted later it is a separate thin target linking the same archive — which is an argument for naming this one `MatterEditor` now rather than `MatterRuntime`.

- [ ] `git mv MatterViewer MatterEditor`.
- [ ] Update `MatterEditor/Makefile` internal paths, and the sibling references in `MatterEngine3/Makefile` and `MatterEngine3/tests/Makefile`.
- [ ] Update `setup-worktree.sh` — it creates NTFS junctions for `MatterViewer/shaders` and `MatterViewer/shaders_gpu`; both paths change.
- [ ] Update `build-all.sh`.
- [ ] Update `.gitignore` (`MatterViewer/cache`, build paths).
- [ ] Update `CLAUDE.md` (Project Relationships, build commands, shader-symlink notes) and `README.md`.
- [ ] Leave `.superpowers/sdd/*.md` and existing `docs/superpowers/**` untouched — historical records.
- [ ] Decide whether the binary is renamed too (`viewer` / `viewer.exe` → `editor` / `editor.exe`). Renaming is more consistent; not renaming avoids breaking anyone's muscle memory and scripts.
- [ ] Run the gate. `make -C MatterEditor windows` is now the target name.

**Note:** 114 tracked files mention "MatterViewer", but the overwhelming majority are historical reports under `.superpowers/sdd/`. The live surface is the directory, ~5 Makefiles, `setup-worktree.sh`, `build-all.sh`, and the two docs.

---

## Phase 4 — Group libraries under `libs/`, rename vendored to `third_party/` (optional)

**Why the flat root is noisy but nesting is wrong:** the dependency chain runs

```
MatterEditor → MatterEngine3 → MatterSurfaceLib → SpatialQueryLib → MemoryLib
```

MatterEngine3 sits *above* those libraries. Nesting them inside it — the intuitive move — would put containment and dependency in opposite directions, making `MatterSurfaceLib` a subdirectory of something that depends on it. Grouping by layer achieves the legibility without inverting anything.

Target layout:

```
libs/          MemoryLib, SpatialQueryLib, ParticleFlowLib, MatterSurfaceLib
MatterEngine3/
MatterEditor/
projects/
Prototypes/
third_party/   (rename of Libraries/)
```

- [ ] Rename `Libraries/` → `third_party/` **first**. Without this, `libs/` and `Libraries/` sit adjacent and mean different things, which is worse than the status quo.
- [ ] `git mv` the four libraries under `libs/`.
- [ ] Update every `-I../X/include` and every source path in the five Makefiles, plus `vpath` lines in `MatterEngine3/Makefile` and `MatterEditor/Makefile` (both were a source of breakage in `c3f0577a`; see the note under Global Constraints).
- [ ] Update the `../../` depth in `Prototypes/*/Makefile` and `Prototypes/GPURayTraceExample/platform-status.sh` — these were already corrected once for the `Prototypes/` move (`a8c36027`) and will shift again.
- [ ] Update `build-all.sh`, `setup-worktree.sh`, `.gitignore`, `CLAUDE.md`, `README.md`, `tech-debt.md`.
- [ ] Run the gate.

**Assessment:** highest churn, lowest functional payoff — this is purely legibility and it touches every include path in the repository. Recommended last, and genuinely optional.

---

## Sequencing Summary

| Phase | Nature | Risk | Independent? |
|---|---|---|---|
| 1 — cache leak | behaviour fix | low, well-scoped | yes |
| 2 — `projects/` | move + 14 test paths | low, mechanical | yes |
| 3 — `MatterEditor` | rename | low, wide but shallow | yes |
| 4 — `libs/` + `third_party/` | move | moderate, every include path | yes |

Each phase lands as its own commit. Any phase can be skipped or reverted without affecting the others.
