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

## Identity and revision contract

Object references always have this shape:

```json
{"kind":"entity","id":"18446744073709551615"}
```

`kind` is `entity` or `baked_root`; the namespaces may contain the same numeric
ID without collision. `id` must be a non-zero unsigned 64-bit decimal string.
Flecs runtime handles are not part of this contract. Later scene commands add
provenance and authored IDs without changing this base identity.

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
