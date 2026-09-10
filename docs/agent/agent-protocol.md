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

## Scene reads

Two commands answer "what is in this world" and "what exactly is this object".
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
