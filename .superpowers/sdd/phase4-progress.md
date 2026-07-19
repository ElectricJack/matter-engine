# Phase 4 Runtime Scene and Editor Bridge Progress

**Branch:** `codex/phase4-runtime-scene-editor-bridge`  
**Worktree:** `.worktrees/phase4-runtime-scene-editor-bridge`  
**Design:** `docs/superpowers/specs/2026-07-19-phase4-runtime-scene-editor-bridge-design.md`  
**Plan:** `docs/superpowers/plans/2026-07-19-phase4-runtime-scene-editor-bridge.md`  
**Current status:** Task 2 implementation in progress

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
- Task 2: switch production runtime and MatterViewer discovery to project-root world
  paths, retain only the documented temporary test seam, and verify cache identity.

## Pending task groups

| Group | Deliverable | Status | Verification |
|---|---|---|---|
| A | World-as-JS hard-cut migration | In progress (Task 1/3 complete) | Task 1 focused + regression PASS |
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

## Blockers and deferred work

- No blockers.
- Persistent runtime QuickJS callbacks and live scripted spawning remain deferred.
- Networking, replication, save games, joints, vehicles, ragdolls, and character controllers remain deferred.
- Screenshot, cinematic, long-flight, and performance acceptance automation remain intentionally excluded.

## Update rule

Update this document whenever a task begins, lands, fails review, is corrected, or
changes the verification/blocker state. Do not mark a task complete from an agent
report alone; record root/reviewer verification evidence first.
