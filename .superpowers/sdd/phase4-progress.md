# Phase 4 Runtime Scene and Editor Bridge Progress

**Branch:** `codex/phase4-runtime-scene-editor-bridge`  
**Worktree:** `.worktrees/phase4-runtime-scene-editor-bridge`  
**Design:** `docs/superpowers/specs/2026-07-19-phase4-runtime-scene-editor-bridge-design.md`  
**Plan:** `docs/superpowers/plans/2026-07-19-phase4-runtime-scene-editor-bridge.md`  
**Current status:** Task 3B ready to commit; git metadata approval blocked

## Completed

- Phase 3 manually accepted, merged into `main`, and locally cleaned up.
- Phase 4 isolated worktree created from `main` at `944e2f9`.
- Phase 4 baseline Flecs, Box3D, and sector-streaming closure checks passed.
- Physics-first scope approved.
- Dedicated dynamic ECS renderer lane selected.
- Generic Entities/Properties editor and Sector Streaming panel consolidation approved.
- Play/Pause/Step/Stop with reset-on-Stop approved.
- Full world-as-JS foundation migration plus declarative and hermetic DSL entity bootstrap approved.
- Design spec self-reviewed and committed as `bcef535`.
- Thirteen-task implementation plan completed and self-reviewed for design coverage,
  placeholder-free instructions, dependency order, and interface-name consistency.
- Planning resolution: project-local `shared-lib/` overrides the engine-owned support
  library without duplicating core DSL sources; `Meadow` and `MeadowWorld` retain
  separate selectable identities during migration.
- Pre-flight conflict scan corrected Task 3 to retain the runtime `WorldLights` data
  contract/fingerprint while deleting only manifest parsing.
- Task 2 pre-flight found that deleting legacy `WorldDesc` fields before Task 3
  migrates all fixtures would break intermediate compilation. The plan now permits a
  test-only `project_dir == nullptr` transition seam in Task 2 and requires Task 3 to
  delete it; MatterViewer switches to project-root discovery immediately.

## Active

- Task 1 approved after implementation `fbe0def` and hardening `c284a44`.
- Task 2 implementation committed as `1fec45e`; task review requires fixes for
  per-import project/engine shared-library fallback, procedural settings propagation,
  and tileset-root params/cache identity.
- Task 2 fix commit `228e23a` resolved ordered fallback/snapshot/live-edit and
  procedural settings. Re-review found one remaining Important gap: functional
  `static requires(params)` still receives `{}` instead of authored tileset root params;
  two stale cache comments are Minor cleanup.
- Task 2 final fix `95c2a3c` resolved functional `requires(params)` and comment
  cleanup; final full-task re-review approved with no findings.
- Task 3: migrate example worlds and all fixtures to project-root JavaScript,
  remove manifest parsing and the temporary descriptor fallback, and verify parity.
- Inventory found 27 legacy-reference files, so Task 3 is split into sequential
  review gates 3A examples, 3B async fixtures, 3C demand/streaming fixtures, and 3D
  remaining fixtures plus final parser/fallback deletion. No implementation agents
  run these groups in parallel.
- Task 3A example migration/parity committed as `83f171c`; scoped review pending.
- Task 3A scoped review approved with no findings; committed-head world-definition
  parity independently rebuilt and passed. Prepare async fixture migration (3B).
- Task 3B async fixture migration committed as `8f7304f`; zero legacy matches,
  all assertions preserved. Docs closed as `e8ded66`.
- Task 3C demand/streaming fixture migration committed as `16ba98f`. Migrated
  `demand_bake_tests.cpp`, `refine_loop_tests.cpp`, and `transient_tests.cpp` to
  project-root layout. Zero legacy scan matches across all three files. POSIX
  helpers replaced with portable `std::filesystem`. Prepare 3D closure.

## Pending task groups

| Group | Deliverable | Status | Verification |
|---|---|---|---|
| A | World-as-JS hard-cut migration | In progress (Tasks 1-2/3 complete) | Tasks 1-2 focused suites + review PASS |
| B | Public scene contract and bootstrap registry | Pending | Pending |
| C | Declarative/DSL recipes and transactional ECS bootstrap | Pending | Pending |
| D | Stable dynamic renderer slots and ECS render bridge | Pending | Pending |
| E | Picking and editor selection | Pending | Pending |
| F | Generic Entities/Properties framework | Pending | Pending |
| G | Play/Pause/Step/Stop snapshot restoration | Pending | Pending |
| H | Part/physics/streaming component editors and panel retirement | Pending | Pending |
| I | Physics playground, regression closure, and manual handoff | Pending | Pending |

## Verification ledger

| Date | Scope | Command/evidence | Result |
|---|---|---|---|
| 2026-07-19 | Phase 4 baseline | Flecs Task 7, Box3D Phase 2, Sector Streaming Phase 3 static closure scripts | PASS |
| 2026-07-19 | Phase 4 plan self-review | Spec coverage, forbidden-placeholder scan, interface/dependency consistency, `git diff --check` | PASS |
| 2026-07-19 | Task 1 implementation | `run-world-definition`, `run-evalworld`, `run-script` | PASS with pre-existing warning noise recorded |
| 2026-07-19 | Task 1 scoped review | `930415f..fbe0def` packaged diff | NEEDS FIXES: 2 Important, 1 Minor |
| 2026-07-19 | Task 1 fix verification | 11 focused RED cases; `world_definition_tests.exe`, `eval_world_tests.exe`, `script_host_tests.exe` from `MatterEngine3/tests` | PASS; existing ScriptHost geometry-warning noise remains |
| 2026-07-19 | Task 1 re-review | `930415f..c284a44` packaged diff | APPROVED: no remaining findings |
| 2026-07-19 | Task 2 implementation | `world_definition_tests` and `resolve_cache_tests` | PASS (162/162 cache); Viewer logic compile/link PASS, runtime duration not completed |
| 2026-07-19 | Task 2 scoped review | `423af88..1fec45e` packaged diff | NEEDS FIXES: 3 Important |
| 2026-07-19 | Task 2 fix verification | shared-lib, live-edit-prod, resolve-cache 168/168, world-definition, tileset-params; Viewer logic + `matter_engine.cpp` compile | PASS |
| 2026-07-19 | Task 2 re-review | `423af88..228e23a` packaged diff | NEEDS FIXES: 1 Important, 2 Minor comments |
| 2026-07-19 | Task 2 final verification | direct committed-head binaries: shared-lib, live-edit-prod, resolve-cache 168/168, world-definition, tileset-params | PASS |
| 2026-07-19 | Task 2 final re-review | `423af88..95c2a3c` packaged diff | APPROVED: no findings |
| 2026-07-19 | Task 3A example parity | `world_definition_tests` rebuilt/run from committed `83f171c`; `d69aef9..83f171c` review | PASS; APPROVED with no findings |
| 2026-07-19 | Task 3B async fixtures | forbidden-source scan; `run-asyncbake`; `git diff --check` | PASS; committed as `8f7304f` |
| 2026-07-19 | Task 3C demand/streaming | forbidden-source scan across 3 files; `git diff --check` | PASS; committed as `16ba98f`; compile/run pending |

## Blockers and deferred work

- No blockers.
- Persistent runtime QuickJS callbacks and live scripted spawning remain deferred.
- Networking, replication, save games, joints, vehicles, ragdolls, and character controllers remain deferred.
- Screenshot, cinematic, long-flight, and performance acceptance automation remain intentionally excluded.

## Update rule

Update this document whenever a task begins, lands, fails review, is corrected, or
changes the verification/blocker state. Do not mark a task complete from an agent
report alone; record root/reviewer verification evidence first.
