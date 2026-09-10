# Matter agent request/result protocol v1

This is the machine-facing layer over the editor's existing control surface.
It does not add a socket or a second dispatcher:

1. requests are newline-delimited `agent <JSON>` records on
   `MATTER_CMD_FIFO`;
2. the app thread validates the envelope and dispatches the corresponding
   typed command through `matter::evt::CommandRegistry`;
3. exactly one terminal JSON record is appended to
   `MATTER_AGENT_RESULT_FILE`, correlated with both the caller's request ID and
   the registry's command ticket.

Legacy text FIFO verbs remain valid and their stdout wording is unchanged.
Human logs stay on stdout/stderr; the result file contains JSONL only.

## Starting and calling the editor

Set both paths when launching the editor:

```powershell
$env:MATTER_CMD_FIFO = 'C:\tmp\matter-commands.txt'
$env:MATTER_AGENT_RESULT_FILE = 'C:\tmp\matter-results.jsonl'
./MatterEditor/build/windows-msvc/editor.exe
```

Then call the scriptable client from another shell. It writes diagnostics to
stderr and the one terminal object to stdout, so stdout can be piped directly
to a JSON parser.

```powershell
py -3 tools/matter_agent.py agent.commands `
  --cmd-file C:\tmp\matter-commands.txt `
  --result-file C:\tmp\matter-results.jsonl | ConvertFrom-Json

py -3 tools/matter_agent.py agent.help `
  --cmd-file C:\tmp\matter-commands.txt `
  --result-file C:\tmp\matter-results.jsonl `
  --args '{"command":"agent.schema"}'
```

The client records the current result-file offset before appending its request,
polls incrementally, tolerates partial writes and transient Windows open races,
and resets its partial buffer if the result file is truncated or replaced.

**Driving the native editor from WSL.** A Win32 process launched from WSL does
NOT inherit arbitrary WSL environment variables — only the ones named in
`WSLENV`. An `env`-prefixed launch therefore starts an editor that silently
never opens the command file (no `MATTER_CMD_FIFO: polling command file …` line
on stdout) and answers nothing. Name the variables explicitly, and keep passing
Windows-style paths, since these entries take no `/p` translation flag:

```bash
WSLENV=MATTER_WORLD:MATTER_CMD_FIFO:MATTER_AGENT_RESULT_FILE:TMP:TEMP \
MATTER_WORLD=FloorDemo \
MATTER_CMD_FIFO='C:/tmp/matter-commands.txt' \
MATTER_AGENT_RESULT_FILE='C:/tmp/matter-results.jsonl' \
TMP='C:/Users/<you>/AppData/Local/Temp' TEMP='C:/Users/<you>/AppData/Local/Temp' \
./build/windows-msvc/editor.exe
```

`tools/matter_agent.py` itself runs fine under WSL's own `python3` as long as
its `--cmd-file` / `--result-file` name the same files through `/mnt/c/...`.

## Request envelope

One FIFO line is `agent ` followed by this JSON object:

```json
{
  "version": 1,
  "request_id": "caller-chosen-lossless-string",
  "command": "agent.commands",
  "args": {},
  "expect": {
    "session_generation": "1",
    "scene_generation": "4",
    "scene_revision": "12",
    "selection_revision": "3",
    "frame_id": "819",
    "view_id": "819"
  },
  "timeout_ms": 5000
}
```

`version`, `request_id`, and `command` are required. `args` defaults to an
empty object, `expect` is optional, and `timeout_ms` defaults to 5000. Expected
revisions and all response identities are decimal strings, never JSON numbers.
This avoids IEEE-754 truncation in JavaScript. A mismatch in any supplied
`expect` field fails before dispatch with `stale_revision`.

Request IDs are unique within the editor process's bounded replay window (4096
recent IDs, plus every pending ID). A duplicate is rejected and cannot replace
or complete the original pending request.

## Terminal result

Every accepted request reaches at most one terminal result. A command that
times out is removed from the pending set; a later registry completion is
ignored, so it cannot emit a second result.

```json
{
  "protocol": "matter-agent",
  "version": 1,
  "type": "result",
  "request_id": "caller-chosen-lossless-string",
  "command": "agent.commands",
  "ticket_id": "27",
  "ok": true,
  "code": "ok",
  "message": "",
  "context": {
    "session": {"id": "1", "generation": "1"},
    "scene": {"generation": "4", "revision": "12", "ready": true},
    "selection": {"revision": "3"},
    "frame": {"id": "819"},
    "view": {"id": "819"}
  },
  "result": {}
}
```

`ticket_id` is the actual `CommandRegistry::dispatch()` ticket, represented as
a string. It is `null` when validation failed before dispatch. Context is the
completion-time snapshot. In v1, `view.id` conservatively equals the presented
frame identity: coordinates from any older presented frame are stale even if
the camera later happens to compare equal.

Terminal codes are deliberately stable and distinct:

| Code | Meaning |
|---|---|
| `ok` | The registry command completed successfully. |
| `invalid_input` | Malformed JSON, envelope, argument, ID or limit. |
| `not_ready` | Required scene/output capacity is not ready. |
| `not_found` | A well-formed named object or help target does not exist. |
| `stale_revision` | Expected context differs, or a session-scoped registry ticket became stale. |
| `timeout` | The request's bounded deadline expired. |
| `execution_failure` | The handler failed, was rejected, shut down, superseded or could not be queued. |
| `unknown_command` | No descriptor is registered for the command name. |
| `unsupported_command` | The name is known but unavailable in this protocol/version/build. |
| `duplicate_request_id` | The request ID is already pending or recently used. |
| `output_too_large` | The real payload exceeded the result bound; a small error replaced it. |

Malformed JSON for which no trustworthy request ID can be recovered gets a
terminal record with `request_id`, `command`, and `ticket_id` set to `null`.

## Discovery and schemas

- `agent.commands` takes no arguments and returns deterministic name ordering,
  summaries, and current availability with a reason/code when unavailable.
- `agent.help` requires `args.command` and returns its arguments, required flags,
  types, description, result type and current availability.
- `agent.schema` requires `args.command` and returns the v1 request/result
  shapes, object identity, hard limits and that command's help record.

The reserved `agent.subscribe` descriptor is intentionally reported as
`unsupported_command`: v1 is request/result only. This makes known-but-not-
implemented distinct from a typo.

## Viewport picking

`viewport.pick` reads, but never changes, the object under one coordinate.
`viewport.pick_select` makes the same pick and applies it to the editor's one
`SelectionSet`; its optional `mode` is `replace` (the default), `add`, or
`toggle`. A replace miss clears selection, exactly like an ordinary empty-space
viewport click; add/toggle misses leave selection unchanged.

Both take required `args.x` and `args.y`: non-negative, finite **viewport-local
logical pixels**. `(0,0)` is the top-left of the 3D viewport content, not the
top-left of the window and not a screenshot/framebuffer pixel. The result
echoes the measured logical viewport rectangle, its framebuffer-pixel rectangle,
the X/Y scale between them, the submitted coordinate in both spaces, camera
pose/projection, and the presented `frame.id` / `view_id` used by the pick.
Coordinates outside the current logical rectangle are `invalid_input`.

The picker is exactly the interactive picker: first the renderer's GPU identity
buffer (needed for streamed/baked geometry), then its CPU oriented-bounds
fallback for live ECS entities. `result.geometry.source` states which path
answered. GPU identity is pixel-exact but contains no depth, so its
`world_position` and `distance_meters` are explicitly unavailable; the CPU
fallback returns both. A miss is `ok` with `hit:false`, not an error. A hit
whose typed identity no longer exists in the current inventory produces
`not_found` for `viewport.pick_select` and leaves selection unchanged.

Use `expect.frame_id` or `expect.view_id` from the screenshot/pick result when
coordinates were derived from an earlier presented image. The standard envelope
guard rejects a mismatch as `stale_revision` before dispatch. Without either
expectation, commands operate on the current last-presented view at dispatch;
there is no separate asynchronous "current-frame" mode. The camera is pinned
to that same presented production view; a request made before the first frame,
or while Part Workbench isolation owns the viewport, returns `not_ready` rather
than mixing a visible image with a different pick world/camera.

## Viewport capture and framing

`viewport.capture` and `view.focus` are what close the loop: read the image,
decide where to look, look there, read again.

### `viewport.capture`

| Argument | Type | Meaning |
| --- | --- | --- |
| `path` | string, required | Absolute `.png` path. The same rule the `shot_now` FIFO verb applies (`fifo_safe_absolute_png_path`: drive-absolute or UNC, no `..`, no reserved DOS names, no control characters). |
| `annotate_selection` | boolean | Also project every selected object's bounding box into the captured image. Default false, because it costs a bounds scan. |

It is the ONE command whose result is not produced on the app lane. The request
is accepted, a capture is armed on the same present/readback queue `shot_now`
uses, and the single terminal record is written only once a frame has actually
PRESENTED and its PNG has been written to disk — with `<path>.done` beside it,
exactly as the `shot` verbs write it. Nothing is reported "captured" from an
intent.

At most one capture is in flight; a second while one is armed is `not_ready`
rather than queued, so a request's own `timeout_ms` keeps meaning what it says.
The four terminal outcomes are deliberately distinct:

| Outcome | Code |
|---|---|
| the PNG landed | `ok` |
| `timeout_ms` passed with no presented capture | `timeout` |
| the editor stopped presenting and the shot deadman abandoned it | `not_ready` |
| a frame presented but its readback or PNG write failed | `execution_failure` |

`result` carries the image and everything needed to act on it:

- `path`, `completion_marker`;
- `image` — `width`, `height`, `format`, and `framebuffer_origin` (non-zero
  only for a cropped capture);
- `viewport` — the 3D view rectangle in `logical`, `framebuffer` and `image`
  pixels, plus `framebuffer_scale` and `production_view`. Every one of these is
  measured on the frame that presented, so a window resize between the request
  and the capture is reported as the size the PNG really is;
- `pick_mapping` — `image_offset`, `divide_by` and the `formula`
  `viewport_x = (image_x - image_offset.x) / divide_by.x`. That is the exact
  coordinate `viewport.pick` takes; a pixel outside the viewport rectangle has
  no pick coordinate at all, and clamping to the nearest edge would answer for
  a pixel the caller never saw;
- `camera` — the pose the image was rendered with;
- `captured` — the full context block AS OF THE CAPTURED FRAME. The envelope's
  own `context` is a completion-time snapshot; these two differ whenever a
  later frame presented in between, which is precisely when the newer numbers
  must not be used. Pass `captured.frame.id` as `expect.frame_id` on the
  follow-up `viewport.pick` and a moved camera is `stale_revision`, not a
  plausible wrong answer;
- `presented` — `id` / `view_id` for the same frame;
- `annotations` — always present. Without `annotate_selection` it is
  `{"available":false,"reason":…}` so a reader never has to tell "absent" from
  "empty selection". With it, one row per selected object carrying the typed
  `object`, a printable `label` (`entity:42`), `primary`, and either
  `image_rect` + `viewport_logical_rect` + `clipped`, or `available:false` with
  a reason. A row is unavailable when the object resolved to no bounds this
  frame, when its box crosses the camera's eye plane (the honest answer: the
  missing corners are the ones that would have been widest), when it projects
  entirely outside the viewport, or when the Part Workbench isolation view
  owned the frame. Annotations are a MEASUREMENT of the image: they change no
  authored content, no selection and no camera, and the boxes come from the
  same `selection_bounds` the on-screen selection outline draws.

The existing `shot` / `shot_now` FIFO verbs are untouched, keep their stdout
wording, and share the queue.

### `view.focus`

`args.object` is optional. With it, the camera frames that one `{kind,id}`
WITHOUT selecting it — framing is a view operation, and an agent that wanted
the selection changed has `selection.replace`. Without it, the camera frames
the current selection. Either way this is the F key's framing:
`camera_focus.h`'s merged world-space AABB over the same bounds the outline and
the pick use.

An id that is not in the current scene is `not_found`. A target that resolves
to no bounds in this world yet is `not_ready` — reported explicitly, because
`focus_camera_on_selection` leaves the camera untouched in exactly that case
and a silent no-op is indistinguishable from success. `result` carries the
`target` (`mode`, `object`, the `objects` actually framed), the `focus` centre
and bounding-sphere `radius_meters`, `camera.before` / `camera.after`, and
`applies_at: "next_presented_frame"` — the new pose is not in any image yet, so
a screenshot taken a moment ago still shows the old one.

## Scene reads

Three commands answer "what is in this world", "what exactly is this object",
and "which recorded procedural source produced it".
Both stay available before the first bake finishes — `context.scene.ready` tells
you the world is still filling in, which is more useful than a refusal. Their
join, ordering, paging and JSON live in `MatterEditor/src/scene_inventory.h`
(unit-tested by `MatterEditor/tests/test_scene_inventory.cpp`).

### `scene.list_objects`

All arguments are optional:

| Argument | Type | Meaning |
| --- | --- | --- |
| `kinds` | array of `"entity"` / `"baked_root"` | Restrict to one population. Omitted means both; an unknown spelling is `invalid_input`, never silently ignored. |
| `name_contains` | string (max 256 bytes) | Case-insensitive substring of the object NAME (module name for a baked root). IDs are never searched. |
| `offset` | integer >= 0 | Rows to skip within the matched set. |
| `limit` | integer 1..200 | Rows to return; default 100. The cap leaves headroom under the 1 MiB result bound; a page that still overflows comes back as `output_too_large`, and the fix is a smaller `limit`, not a retry. |

```powershell
py -3 tools/matter_agent.py scene.list_objects `
  --cmd-file C:\tmp\matter-commands.txt `
  --result-file C:\tmp\matter-results.jsonl `
  --args '{"kinds":["baked_root"],"limit":50}'
```

`result` carries:

- `scene_revision` — decimal string, the same value as `context.scene.revision`;
- `ordering` — `"kind_then_id"`. The order is a TOTAL order over (kind, id), so
  two listings at the same `scene_revision` return the same objects in the same
  order and `offset` paging is safe to resume. Sorting on the id alone would
  interleave the two namespaces;
- `filter` — the applied kinds and `name_contains`, echoed back;
- `scene_counts` — whole-scene per-kind counts, unaffected by the filter;
- `page` — `offset`, `limit`, `returned`, `total_matched`, `has_more` and
  `next_offset` (`null` on the last page). These are plain numbers: they are
  bounded by the object count and a caller has to add to them. IDs and revisions
  stay decimal strings;
- `objects` — one row each (see below).

### `scene.get_object`

Requires `args.object`, the `{kind,id}` pair a listing returned:

```powershell
py -3 tools/matter_agent.py scene.get_object `
  --cmd-file C:\tmp\matter-commands.txt `
  --result-file C:\tmp\matter-results.jsonl `
  --args '{"object":{"kind":"entity","id":"42"}}'
```

An object that is not in the current scene answers `not_found` with a `result`
of `{"found":false,"object":…,"identity":…,"scene_revision":…,"reason":…}`. A
deleted entity, a baked root a rebake re-addressed, and an id that is only valid
in the other kind's namespace all take that path.

### `scene.trace_provenance`

Requires `args.object`, the same typed `{kind,id}` pair `scene.get_object`
takes. Optional `max_depth` is 0 through 8 (default 3), and optional
`max_nodes` is 1 through 100 (default 64). Both bounds apply to the returned
module traversal, so it is safe to use while locating code in a large world.

The response preserves the inspected object identity and scene revision, then
classifies it as `authored_entity`, `baked_root`, or `runtime_only`. A live
entity with a recorded `PartInstance` additionally carries a separate
`generated_instance` record; this prevents a generated part from being
misrepresented as the entity's authored source. Runtime-only means the
session-allocated runtime-id bit is set, so no authored mapping is invented.

For a recorded part, `traversal.nodes` contains module identity, content hash,
source location, canonical parameters, a deterministic FNV-1a hash of those
canonical parameter bytes, and `world_seed` only when the parameters actually
record an integral `worldSeed`. Source paths, parameters, and seeds are
availability records with reasons when absent. Each node reports direct
parents, child modules, shared imports, and selected shared-source paths; the
walk follows parent and child module edges only. `truncated:true` means a
requested bound or per-node edge cap omitted graph data, never that an edge was
silently ignored.

The graph is a module DAG rather than an instance graph. Its node record is the
first representative parameter set seen for a module, so a trace never claims
to enumerate every parametric instance. A deleted entity, a rebake-replaced
root hash, or a typed id from the other namespace returns normal `not_found`
at the current scene revision. A graph that has not published yet, or a part
hash absent from it, returns an explicit unavailable traversal rather than
guessing from filenames.

## Regeneration jobs

### `procedural.parameters` and `procedural.update`

These commands expose and change the effective `static params` of one
published `baked_root`; they do not edit arbitrary JavaScript source. Start
with `scene.list_objects`, choose a current baked-root identity, then ask for
its schema:

```json
{"object":{"kind":"baked_root","id":"123"}}
```

The response names the owning module and source availability, returns every
effective parameter with its JSON type and current default, and reports
`persistence:"session_only_root_override"`. The procedural source model has no
portable min/max declaration, so each field's `range` is explicitly
unavailable rather than guessed.

`procedural.update` accepts the same `object`, a non-empty `changes` object,
and optional `dry_run` (default `false`). Keys must already be declared in the
published object; values must have the same JSON type (numbers accept lossless
integer representation) and be finite. All fields validate before an override
is constructed, so an unsupported key or type error leaves every parameter
unchanged. Supplying `expect.scene_revision` makes a stale preflight fail before
dispatch. A `dry_run:true` response returns `before`, `after`, and
`changed_parameters` without scheduling work.

An applied update returns that same receipt plus a normal regeneration `job`
record. Wait on its job id with `job.wait`; completion carries the resulting
scene identity and deterministic content digest. The override is held only for
this editor session and root module; it is intentionally not persisted and
never rewrites JS. A module published as more than one root is explicitly
refused for update: the engine's override seam is module-scoped, and guessing
which duplicate root an identity should alter would be unsafe.

`job.start`, `job.status`, `job.wait`, `job.cancel` and `job.list` make the
regeneration work the engine already does OBSERVABLE. Before them, an agent
that reloaded a world had to infer completion from a sleep or from log text.
They add no second lifecycle: a job is a ledger entry over
`WorldSession::reload()` / `WorldSession::regenerate(seed)`, written at exactly
two points — the editor's post-frame seam, the only place a session-heavy
operation may run, and the drain of the engine's own `BakeStarted` /
`BakePartDone` / `BakeError` / `BakeFinished` events. The rules live in
`MatterEditor/src/regen_jobs.h` (unit-tested by
`MatterEditor/tests/test_regen_jobs.cpp`).

### `job.start`

| Argument | Type | Meaning |
| --- | --- | --- |
| `operation` | string, required | `"reload"` rebakes the world as authored; `"regenerate"` rebakes it with a `worldSeed` override. |
| `seed` | string | Unsigned 64-bit decimal string. REQUIRED for `regenerate`, and REFUSED for `reload` — silently ignoring it would let a caller believe a reload rerolled the world. |

```powershell
py -3 tools/matter_agent.py job.start `
  --cmd-file C:\tmp\matter-commands.txt `
  --result-file C:\tmp\matter-results.jsonl `
  --args '{"operation":"regenerate","seed":"12345"}'
```

`ok` here means QUEUED, not finished — the result says so in `note` and the
job's own `state` is `accepted`. The heavy operation runs at the next
post-frame seam. `not_ready` when no world session is open.

### Job states

Six states, kept distinct because a caller acts differently on each:

| State | Meaning |
| --- | --- |
| `accepted` | Queued in the editor; the seam has not handed it to the engine. |
| `running` | The seam called `reload()`/`regenerate()`; the engine owns it. |
| `completed` | A `BakeFinished` with zero failed parts landed for this job. |
| `failed` | `BakeFinished` reported failed parts, or the editor shut down while the job was live. |
| `cancelled` | `job.cancel` reached it while it was STILL QUEUED, so nothing ever reached the engine. |
| `superseded` | A newer regeneration replaced it — the engine's own contract is that a new `request_bake()`/`reload()` supersedes an in-flight bake. A toolbar reload, the `reload` FIFO verb and a world switch supersede a running job too, and then `superseded_by` is unavailable rather than naming a job. |

**Known limit.** A bake that ABORTS never terminates its job. A top-level
install/compose failure inside the engine's `execute_bake` emits one
`BakeError` and returns, with no `BakeFinished` behind it, so the job stays
`running`: its diagnostics are recorded and readable through `job.status`, and
a bounded `job.wait` on it expires as `timeout` carrying them. It is
deliberately not called `failed` — nothing in the event stream separates an
aborting `BakeError` from the per-part skip-and-continue errors that DO reach a
`BakeFinished`, and guessing would trade a missing terminal state for a wrong
one. A per-part script failure, the common case, does reach `failed`.

### `job.status` and `job.list`

`job.status{job_id}` is a READ: it succeeds even when the job it describes
failed, because the failure is in `job.state`, not in the query. An id that is
gone answers `not_found` with `total_accepted` and `oldest_retained_job_id`, so
"aged out of the retained window" stays distinguishable from "never existed";
the window holds 64 jobs, and a job that has not reached a terminal state is
never evicted from it.

`job.list{limit?}` returns the most recent jobs (1..64, default 16) ordered
`job_id_ascending`, plus `running_job_id`.

Every job record carries:

- `inputs` — the `operation`, the `world` and `project` the editor recorded
  (not caller-supplied: naming the world is what makes a job id readable after
  a world switch), the `world_seed`, and a `digest` — FNV-1a over
  (operation, project, world, seed). Two requests digest equal exactly when
  they asked the engine for the same thing;
- `accepted_context` — the scene revision and generation the job started FROM;
- `progress` — `parts_done` / `parts_total` / `phase` / `module` straight from
  `BakePartDone`, marked `advisory` and `indeterminate` because `matter/events.h`
  says `total` may be 0 and may GROW mid-bake;
- `result` — the `scene_revision` and `scene_generation` observed when this
  job's own `BakeFinished` was drained, plus `content_digest`: FNV-1a over the
  SORTED published part-graph roots (module + resolved content hash — the same
  identities `scene.list_objects` hands out as `baked_root` ids). Sorted because
  part-graph iteration order is a hash-map order; two `regenerate` jobs with the
  same seed against the same world report the same `content_digest`, which is
  how deterministic seeded regeneration is checked. Unavailable — never 0 — for
  a job that published no `BakeFinished`, AND for one whose graph published no
  roots at all: a world-kind (streamed) world such as `StreamMountain` installs
  sector assets and publishes none, so there is nothing for the digest to
  distinguish and it says so instead of returning the empty-set constant.
  Check determinism on such a world through `viewport.capture` plus
  `MatterEngine3/tools/img_diff.py` instead;
- `diagnostics` — one row per `BakeError`, with the engine's `module`, `phase`
  and classification `code` (`script_error`, `io_error`, `gpu_error`,
  `out_of_memory`, `cancelled`, `internal`), the original `message`, and
  `source`: the `file` / `line` / `column` PARSED out of the message or its
  QuickJS stack (the innermost frame — the line that actually threw), or
  `{"available":false,...}` when the message carried no location. Capped at 32
  rows with `diagnostics_truncated` reporting the overflow;
- `superseded_by`, `terminal_reason` and `timing`
  (`queued_ms` / `running_ms` / `total_ms`).

### `job.wait`

`job.wait{job_id}` is the second command in this protocol (after
`viewport.capture`) whose answer is not knowable on the app lane. The request's
own `timeout_ms` bounds it, so a wait is at most 30,000 ms — for a long bake,
call it again; each call reports the current state. The outcomes are distinct
and a timeout is never a success:

| Outcome | Code |
| --- | --- |
| the job completed with no failed parts | `ok` |
| the job ended `failed`, `cancelled` or `superseded` | `execution_failure` (the state name and `terminal_reason` say which; `message` carries the first diagnostic's `file:line`) |
| `timeout_ms` passed with the job still live | `timeout`, with `timed_out: true`, `completed: false` and the job's LAST OBSERVED state |
| the editor shut down first | `execution_failure` — every live job fails at shutdown and every open wait gets its one terminal record, rather than "running" being the last word |
| no such job | `not_found` |

A job that is already terminal when the wait dispatches is answered
immediately.

### `job.cancel`

Cancellation is NOT uniformly supported, and says so rather than pretending.
`WorldSession` exposes no cancel entry point — supersession is the only
mechanism it has — so:

| Target | Code | `result.cancel` |
| --- | --- | --- |
| a job still `accepted` | `ok` | `supported: true`, `cancelled: true` — it was dropped before the engine ever saw it |
| a job already `running` | `unsupported_command` | `supported: false`, `cancelled: false`, plus `alternative`: start a newer job, which supersedes this one |
| a job already terminal | `ok` | `supported: true`, `cancelled: false`, with the state it had already reached |
| no such job | `not_found` | — |

An unsupported cancel changes nothing. It does not fake a stop, and the job
keeps running.

## Shared selection

`selection.replace`, `selection.add`, `selection.remove`, `selection.toggle`,
`selection.clear`, and `selection.list` operate on the one editor
`SelectionSet`: the same state consumed by the picker, Scene tree, outlines,
and gizmo. They never edit authored source or geometry.

The four object-taking commands require `args.objects`, a non-empty array of
unique typed `{kind,id}` identities from `scene.list_objects`. The complete
array is checked against one current inventory snapshot before any selection
mutation. A missing/rebaked/wrong-kind ID returns `not_found` and leaves the
selection unchanged; duplicate identities return `invalid_input`. Use
`expect.session_generation` and `expect.scene_generation` from the listing
context when acting on a prior observation, so a world switch or rebake returns
`stale_revision` rather than allowing a reused numeric ID to be retargeted.

Primary policy is deterministic: `replace` makes the last request item primary;
`add` and `toggle` make the last newly added item primary; and removing the
primary promotes the last surviving item. `add`, `remove`, and `clear` report
`changed:false` when repeated with no state change and retain the same
`selection_revision`.

Every selection result (including `selection.list`) returns `operation`,
`changed`, `scene_revision`, `selection_revision`, ordered `objects` (typed
identities), and typed `primary` or `null`. Stale entries are pruned before a
selection command/list response, so a rebake or world switch cannot leave an
old root/entity selected.

### Object rows

Every row (in a listing and in an inspection) carries:

| Field | Meaning |
| --- | --- |
| `object` | `{kind,id}` — the identity, ids always decimal strings |
| `kind` | `entity` or `baked_root` |
| `identity` | `namespace` / `source` / `stability` / `notes` — see below |
| `name`, `path` | `{"available":…}` with `value` or `reason`; a path needs a name on the object AND every ancestor |
| `parent`, `depth`, `child_count`, `components` | authored hierarchy (baked roots are flat: `parent` is `null`) |
| `provenance` | `available` plus `module`, `source_path`, `params_json` — each with its own availability |
| `selected`, `primary` | shared `SelectionSet` state |

An inspection adds:

| Field | Meaning |
| --- | --- |
| `found` | `true` |
| `visibility` | `{"available":…}` with the authored `PartInstance.visible` flag, or a reason |
| `placement` | `world_matrix` (row-major local→world), `local_bounds`, derived `world_bounds`, or a reason |
| `operations` | `name` / `available` / `reason` / `command` per capability |

Availability is never faked. A baked root with no recorded source path reports
`{"available":false,"reason":…}` rather than an empty string; a root that is in
the part graph but placed nowhere in this world reports `placement.available`
false rather than an identity matrix; an authored entity says outright that it
carries no part-graph provenance rather than borrowing the module of some part
it happens to place.

`operations` is the editor's real capability surface for that object, not a
wish list: `duplicate` / `delete` / `reparent` name the registered command that
performs them and are unavailable on a baked root (a bake output — edit the
authoring source and rebake), and `transform_gizmo` is unavailable there too
because the gizmo edits scene entities only.

### Which IDs are stable

`identity` states this per object rather than leaving it to be assumed:

| kind | `namespace` | `source` | `stability` | `classified_by` |
| --- | --- | --- | --- | --- |
| `entity`, runtime bit clear | `scene_entity` | `world_authored_id_hash` | `world_definition` | `runtime_id_bit` |
| `entity`, runtime bit set | `scene_entity` | `session_allocated_id` | `session` | `runtime_id_bit` |
| `baked_root` | `baked_part_hash` | `resolved_part_content_hash` | `content` | `part_graph_root` |

No id in this protocol is a Flecs handle; Flecs handles are live-world values
with recycled generations and are never serialized.

`SceneEntityId` splits its own space by its top bit: `matter::scene::kRuntimeIdBit`
in `MatterEngine3/include/matter/scene.h` is the one definition of that
split. An id with the bit CLEAR is FNV-1a over the world definition's
authored id STRING (`hash_authored_id`) and is stable across reloads of that
definition; an id with the bit SET was minted at runtime by
`SceneService::allocate_id`, and is not. Both allocators are held to the split,
so an entity the editor creates reports `session_allocated_id` rather than
being mistaken for an authored one. Either way a DIFFERENT world can hand out
the same number for a different object, so pair an entity id with
`expect.session_generation` across a world switch.

`classified_by` is reported because the classification is a rule APPLIED to the
id at read time, not a provenance field recorded next to it: `runtime_id_bit`
says the answer came from `matter::scene::is_runtime_id`, so a caller can tell a
derived answer from a recorded one and a namespace added later cannot silently
pass itself off as one of these two.

A baked-root id is the resolved content hash, so a rebake that changes the part
changes the id and the old one stops resolving.

## Identity and revision contract

Object references always have this shape:

```json
{"kind":"entity","id":"18446744073709551615"}
```

`kind` is `entity` or `baked_root`; the namespaces may contain the same numeric
ID without collision. `id` must be a non-zero unsigned 64-bit decimal string.
Flecs runtime handles are not part of this contract. `scene.list_objects` /
`scene.get_object` add provenance and authored names/paths on top of this base
identity without changing it, and state per object which namespace an ID belongs
to and how long it is good for (see "Which IDs are stable" above).

`session.id` never aliases a replacement session. `session.generation` marks
the command-scope epoch. `scene.generation` is the baked part-graph generation;
`scene.revision` is the authoritative editor scene-row revision;
`selection.revision` changes only when the shared `SelectionSet` actually
changes. `frame.id` and `view.id` identify the presented image/view.

## Hard bounds and transport recovery

- request line: 65,536 bytes;
- JSON depth: 32; JSON values: 4096; decoded string: 16,384 bytes;
- pending requests: 256;
- result record: 1,048,576 bytes;
- wait: default 5000 ms, maximum 30,000 ms.

The parser is strict JSON: trailing bytes, duplicate keys, invalid escapes,
non-finite/out-of-range numbers and malformed number spellings fail. The line
framer buffers partial reads. On Windows, command-file truncation/replacement
resets both the file offset and partial line, preventing bytes from disconnected
writers from being joined. Lines beyond the request bound are discarded through
their newline and reported explicitly in stderr plus a request-less JSON error
when a result file is configured.
