# Task 10 Report: Inotify Live-Edit Wired Through RebakeCone Worker Command

## Summary

The inotify live-edit path is fully implemented and wired through the production
RebakeCone worker command. All three required test suites pass (run-asyncbake,
run-live, run-liveprod). The inotify end-to-end test (j) confirms the complete
pipeline: file edit → debounce → RebakeCone → cone rebuild via production seams
→ new part hashes → BakeFinished event; fail-closed on syntax error; recovery
on fix.

---

## Step 1: Failing Test Written

Added `test_live_edit_inotify_e2e` to
`MatterEngine3/tests/async_bake_tests.cpp` (guarded `#ifdef __linux__`, called
from `main()` under the same guard). The test:

1. Opens a world with `enable_live_edit = true` in a `/tmp` sandbox.
2. Confirms the initial bake completes and records `part_hash`.
3. Rewrites `Box.js` (S=0.6). Pumps tick+GPU events up to 30 s.
4. Asserts `BakeFinished` arrived and `part_hash` changed.
5. Injects a syntax error. Asserts `BakeError{ScriptError}` and last-good hash
   still served.
6. Fixes the file (S=0.7). Asserts `BakeFinished` again and a second hash
   change.

---

## Step 2: Production Code Implemented

### `MatterEngine3/src/live_edit.h` and `live_edit.cpp`

Added public `rebuild(paths)` method to `LiveEditSession`. It delegates to the
existing private `run_rebuild()`, bypassing the internal debounce state. This is
the entry point called by `execute_rebake_cone()` on the worker thread.

### `MatterEngine3/src/matter_engine.cpp`

**Includes:** Added `live_edit.h`, `live_edit_prod.h`, and (under
`#ifdef __linux__`) `inotify_watcher.h`.

**NullWatcher:** Added in an anonymous namespace — a `FileWatcher` that never
yields events. Used by `execute_rebake_cone()` so `LiveEditSession` can be
constructed without a real watcher on the worker thread; the app thread does the
actual debounce and only passes already-coalesced path sets to the worker.

**WorldSession::Impl additions:**
- `enable_live_edit` flag
- `#ifdef __linux__` block: `std::unique_ptr<InotifyWatcher>`, `inotify_watching`
- Debounce state: `le_pending_paths_`, `le_last_event_ms_`, `le_have_pending_`
- `k_debounce_ms = 150`
- `execute_rebake_cone(cmd)` declaration

**`open_world()` (eager watcher setup):** When `desc.enable_live_edit` is set,
creates `InotifyWatcher` and calls `add_watch(schemas_dir)` and optionally
`add_watch(shared_lib_dir)` immediately, before returning the session. This
ensures file events written during or immediately after the initial bake are
captured, not missed due to tick() being suppressed by `bake_active` during the
initial bake.

**`tick()` (inotify poll + debounce):** After the existing `bake_active` early
return, polls the inotify watcher, accumulates events into `le_pending_paths_`,
records the last-event timestamp, and pushes a `RebakeCone` command to the
worker once the 150 ms quiet window elapses.

**`execute_rebake_cone()`:** Worker-side handler:
1. Emits `BakeStarted`.
2. Gets a snapshot reference from the current provider.
3. Constructs `ProdGraphResolver`, `ProdBaker`, `ProdFlattener` over the
   snapshot and a fresh `ScriptHost`.
4. Wraps in a `LiveEditSession` with `NullWatcher` and zero debounce, calls
   `sess.rebuild(paths)`.
5. On failure: emits per-error `BakeError` events, returns without touching the
   rendered world (fail-closed).
6. On success: refreshes `cfg` callbacks (new token), re-creates the provider,
   calls `install_graph()` to update `ir_.root_hashes` with the new cone
   artifact hashes, calls `compose_world()`, runs the full reset+reconcile+
   publish GPU sequence identical to execute_bake steps 4–8, emits
   `BakeFinished`.

### `MatterViewer/main.cpp`

Added MATTER_LIVE_EDIT=1 opt-in in `open_world_and_start_bake()`:
```cpp
wd.enable_live_edit = (getenv("MATTER_LIVE_EDIT") != nullptr);
if (wd.enable_live_edit)
    printf("live-edit: enabled (MATTER_LIVE_EDIT=1)\n");
```

### `MatterEngine3/Makefile`

Added `src/live_edit.cpp`, `src/live_edit_prod.cpp`, and
`src/inotify_watcher.cpp` to `ME3_CPP`, and `live_edit.o`, `live_edit_prod.o`,
`inotify_watcher.o`, and `part_graph_snapshot.o` to `ME3_OBJ`, so they are
compiled into `libmatter_engine3.a`.

### `MatterEngine3/tests/Makefile`

Already contained the live_edit sources in `GPU_RENDER_CPP` (from Task 9
setup). Verified present.

---

## Step 3: Test Results

All three required suites pass:

```
run-asyncbake: ALL PASS   (includes (j) live_edit_inotify_e2e)
run-live:      ALL PASS   (SP-5 seam interface tests)
run-liveprod:  ALL PASS   (ProdGraphResolver/ProdBaker/ProdFlattener tests)
```

Representative async_bake test (j) output:
```
-- (j) live_edit_inotify_e2e
live-edit: watching /tmp/me3_asyncbake_926120/schemas
  instance_count before edit: 3
  part_hash before edit: 2427317033815505218
  Box.js rewritten (S=0.6)
  ev: BakeFinished code=0
  part_hash after edit: 106379543668099669       ← changed
  Box.js broken (syntax error)
  ev: BakeError code=3 phase=cone
  part_hash during broken: 106379543668099669    ← last-good kept
  Box.js fixed (S=0.7)
  ev: BakeFinished code=0
  part_hash after recovery: 2582868717262789136  ← changed again
```

---

## Step 4: Manual Viewer Check

**Setup:** MATTER_LIVE_EDIT=1, GALLIUM_DRIVER=d3d12, MATTER_WORLD=grass,
self-terminating FIFO script.

**Confirmed working:**
- Viewer printed `live-edit: watching ../MatterEngine3/examples/world_demo/schemas`
- Initial bake completed (`bake finished (0 errors)`) in ~15 s
- Watcher registered correctly; MATTER_CMD_FIFO listened on the FIFO
- Viewer exited cleanly via `quit` command
- Grass.js restored to original after the test (md5sum verified)

**WSL DrvFS constraint observed:** The schemas on `/mnt/d/` (Windows NTFS
mount via DrvFS) do not surface inotify `IN_CLOSE_WRITE` events. The kernel
inotify subsystem does not receive notifications from the Windows filesystem
driver. This is a WSL platform limitation, not a bug in the implementation:
confirmed by a direct Python ctypes test — `inotify_add_watch` returns a valid
descriptor (wd=1) but `read()` returns EAGAIN even after a file write and
200 ms wait. The same test on `/tmp` (native Linux tmpfs) immediately returns
the event.

**Implication:** The live-edit path functions correctly on native Linux
filesystems (verified by the automated test suite using `/tmp`). To use live
edit in the viewer on a WSL developer machine, schemas would need to reside on
the WSL Linux filesystem (e.g., `/home/<user>/...`) rather than the Windows
mount. A note should be added to documentation; this is deferred post-Task-10.

---

## Files Changed

| File | Change |
|---|---|
| `MatterEngine3/src/live_edit.h` | Added public `rebuild(paths)` declaration |
| `MatterEngine3/src/live_edit.cpp` | Added `rebuild()` implementation |
| `MatterEngine3/src/matter_engine.cpp` | NullWatcher + Impl live-edit fields + eager watcher in open_world + debounce in tick() + execute_rebake_cone() |
| `MatterViewer/main.cpp` | MATTER_LIVE_EDIT=1 env opt-in |
| `MatterEngine3/Makefile` | Added live_edit.cpp, live_edit_prod.cpp, inotify_watcher.cpp to ME3_CPP; added .o to ME3_OBJ |
| `MatterEngine3/tests/async_bake_tests.cpp` | Added test (j) live_edit_inotify_e2e + #ifdef __linux__ guard in main() |

---

## Deviations from Brief

1. **WSL DrvFS non-delivery of inotify:** Not a code deviation — the
   implementation is correct. The viewer check could not demonstrate a live
   trigger because the schemas reside on `/mnt/d/` (Windows mount). The test
   suite on `/tmp` fully exercises the same code path. Recorded here for the
   record; the brief anticipated this risk under "Step 4 environment rules."

2. **ME3 Makefile ME3_OBJ:** The brief did not explicitly mention adding the
   new sources to ME3_OBJ (only ME3_CPP was discussed). Both lists must be kept
   in sync for `ar rcs` to include the objects. Added both.

3. **`part_graph_snapshot.o` in ME3_OBJ:** Was already in ME3_CPP (from Task 9)
   but had not been added to ME3_OBJ. Added it as part of this correction.
