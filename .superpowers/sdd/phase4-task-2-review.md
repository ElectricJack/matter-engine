# Phase 4 Task 2 Final Re-review

## Verdict

**Approved.** Fix commit `95c2a3c` closes the remaining functional Tileset
`requires(params)` defect, strengthens the focused regression so it distinguishes both
the required module and canonical child params, and corrects the stale cache-format and
settle-key comments. No Critical, Important, or Minor findings remain in the scoped
Task 2 gate.

Review scope: updated implementation report and cumulative
`review-423af88..95c2a3c.diff`, followed by focused inspection of the changed
production/test paths and the previously cleared I1/I2/I3/v4 paths. No git commands or
workspace mutations were performed other than this requested report. Reported suites
were not redundantly rerun under the review instructions; this approval is based on the
supplied verification evidence plus direct code-path inspection.

## Spec compliance

| Area | Final result | Evidence |
|---|---|---|
| One production world source and temporary Task 2 legacy seam | Compliant | `MatterEngine3/src/provider/local_provider.cpp:243-249`; `MatterEngine3/src/matter_engine.cpp:3393-3423` |
| Project paths and cache/output layout | Compliant | `MatterEngine3/src/provider/local_provider.h:102-113`; project outputs remain rooted in derived `cfg.cache_root` |
| I1 ordered project-first, per-import engine fallback | Compliant | `MatterEngine3/src/module_resolver.cpp:112-136,153-203`; `MatterEngine3/src/script_host.h:146-158` |
| I1 all named runtime paths | Compliant | provider install/restore, procedural evaluation, sector bake, tileset evaluation, and live edit retain ordered roots |
| I1 exact snapshot/live-edit paths | Compliant | `MatterEngine3/src/part_graph.cpp:338-380`; `MatterEngine3/src/live_edit_prod.cpp:38-74` |
| I2 authoritative project procedural settings | Compliant | `MatterEngine3/src/matter_engine.cpp:2095-2107,2128-2133,2812-2819` |
| I3 authored tileset root params through functional `requires(params)` | Compliant | `MatterEngine3/src/script_host.cpp:386-405,478-487` |
| I3 params through tileset build/evaluation | Compliant | `MatterEngine3/src/provider/local_provider.cpp:750-772`; `MatterEngine3/src/tileset_phase.cpp:46-77,134-141` |
| I3 settle-cache identity | Compliant | `MatterEngine3/src/tileset_phase.cpp:150-180`; `MatterEngine3/src/tileset_bake.cpp:615-647` |
| Resolve-cache v4 snapshot compatibility | Compliant | `MatterEngine3/src/resolve_cache.cpp:113-118,359-384,499-508,545-587` |
| Viewer discovery and descriptor lifetimes | Compliant | unchanged from the prior cleared review; `open_world` immediately copies Viewer-owned strings |
| Stale manifest isolation | Compliant | production project fingerprint remains sourced from world/object/project-shared/engine-shared inputs, not `WorldData/world.manifest` |

## Final fix verification

### Functional Tileset `requires(params)`

`run_tileset_phase_impl` passes `canonical_root_params_json` into
`eval_requires` (`MatterEngine3/src/tileset_phase.cpp:66-77`). When the shared Part-only
merger reports the expected no-Part-class condition for an `extends Tileset` source,
`eval_requires` now preserves the caller JSON instead of replacing it with `{}`
(`MatterEngine3/src/script_host.cpp:386-396`). The preserved JSON is parsed into
`paramsObj` and passed to functional `static requires(params)` at lines 478-487.

Module-mode behavior remains correct: imported Tileset sources install the folded module
loader, create the module-capable context, inject Part/Tileset bases, and evaluate via
`eval_part_as_module` (`MatterEngine3/src/script_host.cpp:407-459`). Thus the parameter
fix does not regress the prior imported-Tileset correction.

### Focused test quality

The updated test now detects the original failure directly:

- `ParamFloor.static requires(p)` selects `p.child` and forwards `p.childSeed`
  (`MatterEngine3/tests/tileset_params_tests.cpp:65-80`).
- Direct `eval_requires` assertions require exactly `Pebble` with `{"seed":3}` and
  `Stone` with `{"seed":7}` (`MatterEngine3/tests/tileset_params_tests.cpp:87-100`).
  The former `{}` behavior would yield an undefined module and cannot satisfy either
  assertion.
- End-to-end runs use Pebble/3 for the first and warm calls, then Stone/7 with changed
  density for the changed call (`MatterEngine3/tests/tileset_params_tests.cpp:102-133`).
  They assert cold, warm hit, then cold invalidation and changed placement.
- The separate `settle_cache_key` assertion holds child hashes constant while changing
  canonical root params (`MatterEngine3/tests/tileset_params_tests.cpp:135-140`), proving
  root-param bytes independently affect cache identity rather than relying only on the
  Pebble/Stone child-hash change.

### Previously cleared paths

- Ordered root resolution, fold-cache root ordering, exact selected shared-source paths,
  and project live-edit lookup remain intact.
- Project `WorldSettings` still select the procedural runtime profile and populate both
  provider binding and per-sector `BakeOptions`.
- Resolve-cache format remains version 4; selected shared paths are serialized,
  deserialized, and used to rebuild `by_file`, while older formats fail closed on the
  strict version check.
- Tileset root params still reach `eval_tileset` and the parameterized settle-cache key.

### Documentation

The format history now identifies version 4's `shared_source_paths` addition
(`MatterEngine3/src/resolve_cache.cpp:113-118`). Tileset phase and settle-key comments
now list canonical root params as a cache-key input
(`MatterEngine3/src/tileset_phase.cpp:150-154`;
`MatterEngine3/src/tileset_bake.cpp:621-627`).

## Strengths

- The final regression is behaviorally discriminating rather than a compile-only or
  cache-only check: it verifies exact module and exact canonical child params.
- The cache test separates child-hash invalidation from root-param invalidation, avoiding
  a false-positive green result.
- Compatibility boundaries remain explicit: ordered project roots are used in production
  project paths, while the single-root behavior stays available only for the temporary
  legacy seam.
- Cache v4 fails safely on older data and preserves the exact-path information needed by
  live edit after a warm restore.

## Critical findings

None.

## Important findings

None.

## Minor findings

None.

## Approval gate

**Approved.** All binding Task 2 requirements and all prior review findings are
satisfied within the scoped cumulative package through `95c2a3c`. The implementation's
reported Viewer-logic runtime non-run remains accurately disclosed and is not converted
into an unverified pass claim.
