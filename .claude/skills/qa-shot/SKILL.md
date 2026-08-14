---
name: qa-shot
description: Take screenshots of a MatterEngine world via MatterEngine3/tools/drive.py and a QA timeline (FIFO command file). Use when asked to capture a shot, screenshot, or visual check of a running world/scene.
---

Drives the editor headlessly against a scripted **timeline** (a FIFO command
file; verb grammar in `docs/agent/control-surface.md` §b), verifying every
`shot`/`shot_now` produced its PNG + `.done` sidecar.

## Canonical invocation

```bash
python MatterEngine3/tools/drive.py --world <World> --timeline <file.txt> \
    --out-dir <out-dir> [--timeout 600] [--hide-ui] [--env K=V ...]
```

Exit 0 = every shot verified, editor exited 0. Exit 1 = missing file(s) or
nonzero editor exit. Exit 2 = timeout, or editor.exe/timeline not found.

## Example timeline (6 lines)

```
wait_idle 5
wait_event bake.finished 30
set viewer.budget.pixel_budget 1.0
wait_frames 3
shot C:/tmp/qa/out_01.png
quit
```

`wait_idle <sec>` blocks until `resident_sectors` holds steady that long AND
bake is ready. `wait_event <name> [timeout_s]` blocks on a named engine event
(`bake.finished`, `stream.refine_tile`, `cmd.completed`, ...) or times out
and continues. Both gate later lines.

## Three launch rules

1. **Shot paths must be absolute** — relative ones resolve against the
   editor's cwd (`MatterEditor/`), not `--out-dir` or your cwd; put absolute
   paths under `--out-dir` so PNGs, `cmd.txt`, `log.txt` colocate.
2. drive.py sets TMP/TEMP and the editor's cwd itself — no manual `cd`.
3. **Never build while `editor.exe` is running** (file lock).

See `docs/agent/qa-cookbook.md` (recipes 3-4) and `docs/agent/control-surface.md`
for the full verb table and env vars.
