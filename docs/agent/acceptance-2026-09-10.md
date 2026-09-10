# Agent control-surface acceptance — 2026-09-10 (AQ smart-dune.6)

A single native-Windows acceptance run over the whole `matter-agent` v1 surface
built by AQ epics `nimble-dune` and `smart-dune`, plus the AQ-side delivery
evidence the same task asks for. Every row below was produced by a request that
is recorded in
[`evidence/2026-09-10-smart-dune-6/command-transcript.jsonl`](evidence/2026-09-10-smart-dune-6/command-transcript.jsonl)
(426 requests: phase, command, args, expectations, latency, result size,
terminal code, registry ticket).

Nothing here is inferred from a smoke test. Where a capability could not be
exercised, the row says so and names the reason rather than passing.

## Revisions under test

| | |
|---|---|
| Source revision | `a0ab93e0` (`feat(agent): add persistent CLI batches`), the tip of `aq/smart-dune.5` and this task's branch point |
| Build | `./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor`, exit 0 in 68 s, 34 targets |
| Binary | `MatterEditor/build/windows-msvc/editor.exe`, linked from that revision (no MinGW build was used) |
| Client | `tools/matter_agent.py` at the same revision, driven both in-process (for latency) and as a subprocess (for the CLI/exit-code contract) |
| Worlds | `PhysicsPlayground` (10 entities + 4 baked roots), `FogLab` (2 roots), `RockGallery` (1 root), `Meadow` (1 root, 41.3 s publish) |
| Run directory | `C:/tmp/aq-smart-dune6/` — `commands.txt`, `results.jsonl` (468 lines, 0 non-conforming), `editor.log`, 9 PNGs each with its `.done` marker |

## Capability matrix

`working` = exercised end to end against the native editor and behaved as
`docs/agent/agent-protocol.md` documents. `broken` = exercised and did not.
`unsupported` = the surface answers that it is unavailable, by design.
`untested` = not reachable in this run; the reason is given.

### Discovery, envelope and transport

| Capability | State | Evidence |
|---|---|---|
| `agent.commands` | working | 28 descriptors, deterministic order, 4.8 KB, 17 ms |
| `agent.help`, `agent.schema` | working | `agent.schema{command:"procedural.update"}` returns request/result shapes + bounds |
| `agent.subscribe` | unsupported (by design) | `unsupported_command`, "protocol v1 is request/result only" — the only descriptor reporting `available:false` |
| unknown command name | working | `unknown_command`, distinct from the above |
| `expect.scene_generation` / `scene_revision` / `selection_revision` / `session_generation` | working | all four pass on a match and return `stale_revision` on a mismatch (8 requests, 8 expected outcomes) |
| `expect.frame_id` / `view_id` | working, but see [note](#frame-guards-are-unusable-on-an-animated-world) | correct `stale_revision`; unsatisfiable on a continuously-presenting world |
| malformed JSON with no recoverable id | working | terminal record with `request_id`/`command`/`ticket_id` all `null`, `"expected JSON string at byte 1"` |
| duplicate JSON object key | working | `invalid_input`, `"duplicate object key at byte 80"` |
| `version` enforcement | working | `version:2` → `invalid_input`, `"version must be 1"` |
| missing `request_id` | working | `invalid_input` naming the 128-byte control-free rule |
| `duplicate_request_id` replay window | working | replaying `acc-dup-probe` → `duplicate_request_id`, `ticket_id:null`, original untouched |
| result file is JSONL only | working | 468/468 lines parse and carry `protocol:"matter-agent"`; all human logs stayed on stdout |
| legacy FIFO verbs preserved | working | `shot_now` → `shot_now: queued …` / `screenshot written to …`; `wait_idle` → `idle: settled after 1.0s`; unknown verb → `cmd: unrecognized 'worlds'`. Wording unchanged, none of it in the JSONL |
| `output_too_large` | untested | no reachable payload exceeds 1 MiB: the largest single result in 426 requests was 20 KB (`scene.diff`), because the object namespace is root-scoped (see [note](#the-object-namespace-is-root-scoped)) |

### Scene reads and provenance

| Capability | State | Evidence |
|---|---|---|
| `scene.list_objects` full page | working | PhysicsPlayground `{entity:10, baked_root:4}`, 13.3 KB, 32 ms |
| `kinds` / `name_contains` filters | working | `{kinds:["entity"],name_contains:"crate"}` → 5 of 10; unknown kind `"widget"` → `invalid_input` |
| paging | working | `limit:5,offset:10` → `returned:0, total_matched:14, has_more:false`; `limit:500` → `invalid_input` |
| `scene.get_object` | working | entity: `stability:"world_definition"`, placement available, `visibility.value:true`. Root: `stability:"content"`, placement unavailable with a reason |
| absent id | working | `not_found` with `found:false` and `"no authored entity carries this id in the current session"` |
| `scene.trace_provenance` (root) | working | Meadow root → 5 nodes, `truncated:false`, `params_json "{}"` + `params_hash 08f44b07b5901a25`, `world_seed {available:false, reason:"the recorded parameters contain no worldSeed"}`, children `[MeadowGround, Rock, Pebble, Grass]`, shared import `rng` resolving to `MatterEngine3/shared-lib/rng.js` |
| `scene.trace_provenance` (entity) | working | `classification.kind:"authored_entity"` with the honest "this inventory does not retain its declaration location" |
| id classification | working | every entity reported `world_authored_id_hash` / `classified_by:"runtime_id_bit"` |

### Selection

| Capability | State | Evidence |
|---|---|---|
| `selection.replace` / `add` / `remove` / `toggle` / `clear` / `list` on entities | working | revisions advance only on real change; repeat `add` → `changed:false`, revision retained at 16 |
| primary policy | working | `replace` makes the last item primary; `add` promotes the newly added |
| absent id | working | `not_found`, selection unchanged |
| duplicate identities in one request | working | `invalid_input` |
| **selection of an unplaced `baked_root`** | **broken** | `changed:true` + revision bump, then silently pruned; see [AQ `smart-dune.9`](#finding-2) |
| placed `baked_root` | working | `PlaygroundFloor`: replace → rev 15, repeat add → `changed:false`, rev retained |

### Viewport

| Capability | State | Evidence |
|---|---|---|
| `viewport.capture` | working | 5 consecutive captures on Meadow, all `ok`, p50 164 ms, every PNG on disk with its `.done` marker |
| measured geometry | working | `viewport.logical {283,59,663x523}`, `framebuffer_scale 1:1`, `production_view:true`, `pick_mapping` formula echoed |
| `annotate_selection` | working | PhysicsPlayground: one row, `image_rect (501.19,254.15) 57.55x45.71`, `clipped:false`; empty selection → `available:true` with zero rows, never a fake box |
| path safety | working | relative, `..`, `.jpg` and `NUL.png` all → `invalid_input` with the `shot_now` rule named |
| `viewport.pick` | working | derived coordinate `(331.5,261.5)` hit `entity:637278442326563570` through `gpu_identity`; `world_position`/`distance_meters` correctly unavailable on that path |
| `viewport.pick_select` | working | same hit applied to the shared `SelectionSet` |
| negative coordinate | working | `invalid_input` |
| `view.focus` | working | camera moved `(20,16,34)→(-2.13,12.48,6.33)`, `radius_meters 0.866`, `applies_at:"next_presented_frame"`, selection revision unchanged (16→16) |
| `view.focus` on an unplaced root | working | `not_ready`, camera untouched — the documented explicit refusal |
| second capture while one is armed (`not_ready`) | untested | the client is strictly sequential; forcing two in flight needs a concurrent writer |
| capture during Part Workbench isolation | untested | no isolation session was opened in this run |

### Regeneration jobs

| Capability | State | Evidence |
|---|---|---|
| `job.start{reload}` | working *as a job* / see [Finding 1](#finding-1) for its effect | accepted → completed, `queued_ms 6`, `running_ms 182` |
| `job.start{regenerate,seed}` | working | RockGallery seed `11111` → `total_ms 2863`; seed `22222` → `3044` |
| seed refused for `reload`, required for `regenerate` | working | both `invalid_input` |
| `job.wait` | working | terminal states answered immediately; `execution_failure` carries the state name |
| `job.status` / `job.list` | working | 12 jobs retained, `running_job_id {available:false, reason:"no regeneration job is running"}` |
| absent job id | working | `not_found` with `total_accepted:"7"` and `oldest_retained_job_id:"1"` |
| `job.cancel` while `accepted` | working | the documented same-frame two-line append reached the window: `state:"cancelled"`, `terminal_reason:"cancelled by job.cancel before the editor handed it to the engine"` |
| `job.cancel` while `running` | unsupported (by design) | `unsupported_command`, `cancel.supported:false`, alternative names supersession; the job kept running |
| `job.cancel` on a terminal job | working | `ok`, `cancelled:false`, reports the state it already had |
| supersession | working | job 8 → `superseded`, `superseded_by:"9"`, job 9 completed with digest `0aa9c47100a77636` |
| deterministic seeded regeneration | working | seed `11111` twice → `aa0d903f5bd69b6a` both times; seed `22222` → `a2911d23ca3ca41c` |
| aborting-bake job never terminating | untested | no world in this run produced a top-level `execute_bake` abort; the documented known limit was not reachable |

### Procedural parameters

| Capability | State | Evidence |
|---|---|---|
| `procedural.parameters` reachability | working | answers `ok` with `owner`, `source` and `persistence:"session_only_root_override"` on every root tried |
| **effective parameter set** | **broken** | `parameters: []` for every published root in `PhysicsPlayground`, `FogLab`, `RockGallery` and `Meadow` — including the Meadow root whose module declares `static params = { worldSeed: 20260721 }`. See [AQ `smart-dune.10`](#finding-3) |
| `procedural.update` validation | working | undeclared key → `invalid_input` `"unsupported parameter 'worldSeed'"`; empty `changes` → `invalid_input`; absent root → `not_found`; entity target → `invalid_input` |
| **`procedural.update` applied path** | **untestable on this build** | no root anywhere in `projects/world_demo` declares a reachable parameter, so `dry_run` and the applied receipt cannot be reached at all |

### Scene comparison

| Capability | State | Evidence |
|---|---|---|
| `scene.capture_snapshot` | working | PhysicsPlayground `counts {entities:10, baked_roots:4, captured:14}`, `truncated:false`, `content_digest b108073d1eeaafba`, `world_seed` unavailable with the correct reason |
| seed attribution | working | after a seeded reroll: `world_seed {value:"424242", source:"regeneration job 2"}` |
| `scene.list_snapshots` | working | `capacity 8`, `count 2`, `oldest_retained_snapshot_id "1"` |
| `scene.diff` `full` | working | see [Finding 1](#finding-1) — the mechanism is correct, what it measured is the defect |
| `scene.diff` `incomparable` | working | PhysicsPlayground snapshot vs RockGallery current → zero rows and `"their ids share a numbering scheme and nothing else"` |
| regenerated pairing | working | seeded reroll produced `changed:1, regenerated:1` on the logical key, not 2×N add/remove |
| `content_identical` availability | working | `true` after a cache-hit reload, `false` after a reroll |
| unretained / `"current"` baseline | working | `not_found` carrying `oldest_retained_snapshot_id`; `from:"current"` → `invalid_input` |
| snapshot eviction / 20,000-object truncation | untested | no world reached 8 retained snapshots or the object cap |
| `scene.diff` `partial` | untested | reaching it needs a return to an earlier world with runtime-minted entity ids still paired; this run's world switches produced `incomparable` instead |

### Spatial queries

| Capability | State | Evidence |
|---|---|---|
| sphere region | working | `radius 5` matched `PlaygroundFloor` only, `tested:11`, `unresolved.count:3` reported as neither accepted nor rejected |
| aabb `contains` | working | the same box under `contains` matched 0 |
| `module_contains` | working | `"crate"` → 2 roots |
| `has_provenance:false` | working | 0 (every named object in that world records provenance) |
| paging | working | `offset:10,limit:5` → `returned:0, total_matched:4` |
| inverted box / negative radius | working | both `invalid_input`, not an empty answer |

### CLI client (`tools/matter_agent.py`)

| Capability | State | Evidence |
|---|---|---|
| one-shot command, JSON on stdout | working | 0.37 s wall including interpreter start |
| named session targets | working | `session set acc` wrote `sessions/acc.json`; later calls used `--session acc` only |
| bounded batch, all steps OK | working | 6 steps (list → select → list → snapshot → capture → diff) in **0.40 s wall**, exit 0, tickets 128–133, one 10 KB `batch_result` |
| `stop_on_error:true` partial | working | exit **3**, `status:"partial"`, `completed_steps:2`, `remaining_steps:1`, completed prefix preserved |
| `stop_on_error:false` | working | exit **1**, `status:"completed"`, all 3 dispatched, the non-OK step reported in place |
| 64-step bound | working | 65 steps → exit **2**, `"batch steps must contain from 1 through 64 objects"` |
| local errors | working | missing batch file and unknown session both exit **2** with a stderr diagnostic and no request appended |
| single non-OK terminal result | working | exit **1** with the `invalid_input` record on stdout |
| deterministic step request ids | working | `batch_id` prefixes made every step id reconcilable in the transcript |

## Command overhead

Measured in-process (the harness imports `matter_agent.submit_request`), so a
sample is one full round trip: append to the command file → editor app-lane
dispatch → terminal record appended to the JSONL → client poll observes it. The
Python interpreter start-up that a subprocess call would add is measured
separately below.

The same fixed 140-request mix (`scene.list_objects` ×2 shapes,
`selection.list`, `scene.get_object`, `scene.trace_provenance`, `scene.query`,
`viewport.pick`) was run against a trivially small world and against the
heaviest one in the project.

| World | Bake weight | Requests | Wall | Throughput | p50 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| `RockGallery` | 176 ms publish (cache hit) | 140 | 4.64 s | 30.2 req/s | **32.9 ms** | 43.9 ms | 112.9 ms | 121.2 ms |
| `Meadow` | 41,256 ms publish, 16.7 s GPU job | 140 | 3.95 s | 35.5 req/s | **31.1 ms** | 34.7 ms | 61.4 ms | 90.0 ms |

Per command on `Meadow` (p50 / p90 / p99 / max ms, and p50 result bytes):

| Command | n | p50 | p90 | p99 | max | p50 bytes |
|---|---|---|---|---|---|---|
| `scene.list_objects` | 40 | 21.4 | 33.1 | 46.1 | 46.1 | 1,564 |
| `selection.list` | 20 | 27.8 | 32.6 | 45.6 | 45.6 | 470 |
| `scene.query` | 20 | 31.0 | 32.8 | 61.4 | 61.4 | 1,936 |
| `scene.get_object` | 20 | 31.5 | 34.9 | 43.2 | 43.2 | 2,433 |
| `viewport.pick` | 20 | 31.9 | 34.6 | 44.0 | 44.0 | 1,008 |
| `scene.trace_provenance` | 20 | 32.2 | 37.8 | 90.0 | 90.0 | 4,631 |

Read this as: **command latency is set by the app-lane frame seam and the
result-file poll, not by scene size.** A world that takes 41 s to publish
answers reads at the same ~31 ms median as one that publishes in 176 ms, and
the heavier world was in fact marginally *faster* at the tail. Output sizes are
small and bounded — the largest single result in the entire run was 20 KB.

Two commands are deliberately not on the app lane and are measured separately:

| Command | n | p50 | Note |
|---|---|---|---|
| `viewport.capture` | 5 | **164.2 ms** | one presented frame + readback + PNG write; 163.5–172.3 ms, no outliers |
| `job.wait` (terminal job) | — | ~185 ms | bounded by the bake, not by the protocol |

Subprocess overhead, for callers using the CLI rather than the library:
a one-shot `matter_agent.py` invocation costs **0.37 s** wall against a ~31 ms
round trip, so a 6-step plan run as six processes would cost ~2.2 s while the
same plan as one `--batch` cost **0.40 s**. Batching is worth roughly 5× here,
entirely in interpreter start-up.

**These are engine/command-surface numbers and say nothing about AQ task
throughput.** They are requests per second against one editor process; AQ
tasks/day is a separate quantity measured on the daemon, and this run
establishes nothing about it.

## Findings

### Finding 1

**`job.start{reload}` destroys the authored entity population.** Filed as AQ
`smart-dune.8`.

On `PhysicsPlayground`, a fresh session lists `{entity:10, baked_root:4}`. One
reload job completes `ok` — `state:"completed"`, `total_ms 220`,
`diagnostics: []`, and the log prints `bake finished (0 errors)` — and the
scene then lists `{entity:0, baked_root:4}`. `scene.diff` from the pre-reload
snapshot reports `compatibility.level:"full"`,
`{added:0, removed:10, changed:0, unchanged:4}`, `content_identical:true`: ten
`removed` rows, one per authored entity, every one `stability:"world_definition"`.

Re-issuing the FIFO verb `world PhysicsPlayground` starts session 2 and restores
all 10; a single reload in that fresh session empties it again (revision 8 → 9).
So this is the reload/regenerate path, not world load. `regenerate` does the
same.

This supersedes the "Current limitation" note in `qa-cookbook.md` §4b: the
renderer fatal that note describes (`dynamic instance part bucket is outside the
active part table`, AQ `nimble-dune.8`) no longer reproduces on this revision.
Reload now succeeds — and silently empties the scene instead.

The `scene.diff` machinery reporting this is itself correct, and is what made
the defect visible at all; a `job.wait` returning `ok` would not have.

### Finding 2

**Selecting an unplaced `baked_root` returns a success receipt for a selection
that never exists.** Filed as AQ `smart-dune.9`.

Three of four `PhysicsPlayground` roots, and the single `Meadow` root, are in
the part graph but placed nowhere — `scene.get_object` answers `found:true`
with `placement.available:false`. For those:

```
selection.replace{that root}  -> ok, changed:true, selection_revision 11
selection.list                -> objects: [],     selection_revision 12
selection.add{same root}      -> ok, changed:true, selection_revision 13
selection.list                -> objects: [],     selection_revision 14
```

Controls behave exactly as documented: the placed root `PlaygroundFloor` and any
entity both report `changed:false` with the revision retained on a repeat.
`agent-protocol.md` promises a missing id returns `not_found` leaving selection
untouched, and that a no-op `add` retains the revision; neither holds here, and
an agent cannot tell "selected" from "pruned" without a second round trip.

### Finding 3

**`procedural.parameters` reports an empty parameter set for every published
root, so `procedural.update` has no working fixture.** Filed as AQ
`smart-dune.10`.

`procedural.parameters` answers `ok` with `available:true` and `parameters: []`
for every root in all four worlds. The decisive case is `Meadow`:
`projects/world_demo/scenes/Meadow/objects/Meadow.js:37` declares
`static params = { worldSeed: 20260721 }`, that module *is* the published root,
and `procedural.parameters` resolves its `source` to exactly that file — yet
reports no parameters, and `procedural.update{worldSeed:…}` is refused with
`"unsupported parameter 'worldSeed'"` in every spelling.

`scene.trace_provenance` shows why: the graph node records
`params_json "{}"` with `params_hash 08f44b07b5901a25`. The part graph stores
the root's *resolved* parameters, and a root placed with no explicit params
records `{}` rather than the module's declared defaults.

The engine seam is fine — `job.start{regenerate, seed}` demonstrably changes the
world and is deterministic per seed. Only the parameter projection is empty.
A survey of `projects/world_demo` found `Meadow` is the **only** world whose
`static roots` names a module declaring `static params` at all, so no other
fixture could cover this either.

**Resolved** by AQ `smart-dune.10`: the part-graph snapshot now records each
node's *effective* parameters — the merged object the resolved hash was already
folded from — alongside the placement params, and every parameter surface reads
that. The two rows above were true of `a0ab93e0` and are left as the record of
that run; re-running them against the fix is the follow-up acceptance pass's
job.

### Frame guards are unusable on an animated world

`agent-protocol.md` recommends passing a capture's `captured.frame.id` as
`expect.frame_id` on the follow-up pick. On `PhysicsPlayground` the frame id
advanced 2–6 per request while idle (`44344 → 44350 → 44354` across three
consecutive reads), so a guarded pick derived from a capture is *always*
`stale_revision` — the guard is correct, but unsatisfiable on any world that
keeps presenting. Every guarded pick in this run failed and every unguarded one
succeeded and hit the right object. This is a documentation gap, not a defect:
the guard protects against a *moved camera*, and on an animated world the caller
needs a camera-stability guard rather than a frame-identity one. No AQ task
filed; recorded here.

### The object namespace is root-scoped

The largest named-object population found anywhere was 14
(`PhysicsPlayground`: 10 entities + 4 roots). `Meadow` — millions of triangles,
41 s to publish — names exactly **one** object, because `scene.list_objects`
enumerates part-graph roots and authored entities, not placed instances. This is
a deliberate design (`agent-protocol.md`: "the editor names thousands of objects
rather than millions"), and it is why the `output_too_large` bound, the
20,000-object capture truncation and snapshot eviction were all unreachable in
this run. "Large scene" in this acceptance therefore means a heavy *world*, not
a long object list; the latency table above is the honest form of that
measurement.

## AQ delivery evidence

### Dependency gating and cross-epic prerequisite delivery — verified

Declared edges (`aq task show`) and their satisfaction in git:

| Task | Declared prerequisite | Prerequisite tip | Ancestor of the task's branch? |
|---|---|---|---|
| `smart-dune.1` | `nimble-dune` (epic) | all 8 completed children | yes, all 8 |
| `smart-dune.2` | `smart-dune.1` | `762ee7f5` | yes |
| `smart-dune.3` | `smart-dune.2` | `75bab508` | yes |
| `smart-dune.4` | `smart-dune.3` | `f43bb3d2` | yes |
| `smart-dune.5` | `smart-dune.4` | `b23bd373` | yes |
| `smart-dune.6` (this task) | `smart-dune.5` | `a0ab93e0` | yes — this workspace was handed exactly that commit |
| `nimble-dune.2…6` | the previous child | — | yes, chain intact |
| `nimble-dune.7/8/9` | the epic only | — | n/a (declared parallel, correctly not chained) |

The cross-epic edge is the strongest result: **every completed `nimble-dune`
child is an ancestor of `aq/smart-dune.1`**, so the dependent epic did start
from a branch that already carried its prerequisite artifacts, delivered through
the branch pipeline and not copied. Nothing in this run needed a manual
prerequisite fetch, and no sibling branch was merged ad hoc.

Workspace assignment was likewise concrete: `.aq/claim.json` records
`{task_id: smart-dune.6, claim_epoch: 2}`, the assigned `work_dir` is the
`slot-2` worktree, and its branch `aq/smart-dune.6` was created at the
prerequisite tip.

### Isolation from main — NOT met, as configured

Queried live, not from a cached ref:

```
$ git ls-remote origin refs/heads/main
a0ab93e041c65213827a2b27decb681ca4da4cc9  refs/heads/main
```

**Every completed child branch of both epics is already an ancestor of
`origin/main`** — all 8 `nimble-dune` children and all 7 `smart-dune` children,
verified individually with `git merge-base --is-ancestor`.

`main`'s first-parent chain shows how: after `dc8a61ac "Integrate task
nimble-dune.1"`, each subsequent task's commits sit directly on main's
first-parent chain (`69db0af7`, `eeb42744`, `1d30e9d3`, `406dfdcb`, `58514993`,
`73d8aab5`, `1645469e`, `762ee7f5`, `75bab508`, `f43bb3d2`, `a0ab93e0`),
interleaved with `Merge commit '<sha>' into HEAD` merges for the parallel tasks.

So the acceptance criterion "both Matter Engine epics remain isolated from main
until their configured aggregate checks and review pass" **does not hold**. Both
epics are fully on main while `smart-dune` is still `IN_PROGRESS` and while
`smart-dune.6` — the epic's own aggregate acceptance check, this document —
had never run. Whatever validated those batches, it was not this check.

This is a statement about the *configured* delivery mode, not an accusation of a
bug: this task was routed in development mode ("the daemon collects completed
source branches and publishes validated batches to main"). The finding is that
development-mode batching promotes per task, so an epic-level gate placed on the
epic's last child can never gate anything.

### Aggregate review and exact-commit CI — not verifiable from a worker session

`aq playbook list-runs` and `aq task get-result` both answer
`out of scope: <command>` for a worker token. A worker session therefore cannot
attest that review or aggregate checks ran. Confirming that criterion needs an
operator surface, and this run does not claim it either way.

## Reconciliation against the task's acceptance criteria

| Criterion | Outcome |
|---|---|
| Canonical MSVC build, repeatable fixture, transcripts, exact revisions, timings, artifacts | **met** — built from `a0ab93e0` via `build-windows-from-wsl.sh`, 426 recorded requests, four named worlds, latency tables, PNGs with `.done` markers |
| Bounded large-scene command workload; median/tail latency and output sizes; engine metrics kept distinct from AQ throughput | **met, with the scope stated** — 2 × 140-request workloads with per-command p50/p90/p99/max and byte sizes; "large scene" reinterpreted as world weight, with the reason given; AQ throughput explicitly not claimed |
| Dependent epic starts only when prerequisites are available through the feature-branch pipeline | **met** — every prerequisite tip proven an ancestor; the cross-epic edge holds for all 8 `nimble-dune` children |
| Both epics isolated from main until aggregate checks and review pass | **not met** — both epics are already on `origin/main`; evidence and cause above |
| Working/broken/unsupported capability matrix; every acceptance item reconciled; no blanket success | **met** — matrix above marks 3 broken capabilities and 7 untested ones with reasons; 3 AQ tasks filed |

## Reproducing this run

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor

mkdir -p /mnt/c/tmp/aq-smart-dune6 && cd MatterEditor
WSLENV=MATTER_WORLD:MATTER_CMD_FIFO:MATTER_AGENT_RESULT_FILE:TMP:TEMP \
MATTER_WORLD=PhysicsPlayground \
MATTER_CMD_FIFO='C:/tmp/aq-smart-dune6/commands.txt' \
MATTER_AGENT_RESULT_FILE='C:/tmp/aq-smart-dune6/results.jsonl' \
TMP='C:/Users/<you>/AppData/Local/Temp' TEMP='C:/Users/<you>/AppData/Local/Temp' \
./build/windows-msvc/editor.exe
```

Then, from the repository root in a second shell, drive it with
`tools/matter_agent.py` as `agent-protocol.md` and `qa-cookbook.md` §4a–4d
describe. Finding 1 reproduces in three requests:

```bash
python3 tools/matter_agent.py scene.list_objects --session acc --args '{"limit":200}'
python3 tools/matter_agent.py job.start        --session acc --args '{"operation":"reload"}'
python3 tools/matter_agent.py job.wait         --session acc --args '{"job_id":"1"}' --timeout 30
python3 tools/matter_agent.py scene.list_objects --session acc --args '{"limit":200}'
```

`scene_counts.entity` goes from 10 to 0 across the reload.
