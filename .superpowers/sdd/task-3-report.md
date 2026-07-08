# Task 3 Report: cwd-independent bake writes — drop the install chdir bracket

## Status: DONE

---

## Step 1: Write-Path Trace

### `bake_source` (script_host.cpp)
- **Line 1220** (pre-fix): `std::string path = part_asset::cache_path_resolved(r.resolved_hash);`
  — returns the relative string `"parts/<hex>.part"`.
- **Line 1222**: `part_asset::save_v2(path, ...)` — writes to that **relative** path.
  If cwd != cache_root, the write fails (ENOENT).
- `BakeResult::written_path` carries the same relative string back to `HostBaker::bake`.

### `HostBaker::cached` (part_graph.cpp line 311)
- Joins `parts_dir_ + "/" + part_asset::cache_path_resolved(h)`.
- `parts_dir_` was historically `"."` (the chdir-ed cwd), making this work only under chdir.
- This read path was already absolute-capable; only `bake_source` was the write-path bug.

### `HostBaker::bake` (part_graph.cpp line 330)
- Called `host_.bake_source(... /*opts*/{} ...)` — empty `BakeOptions`, so `parts_dir` was
  absent and `bake_source` fell through to the cwd-relative write path.

### `HostBaker::bake_lod_variants` (part_graph.cpp line 392)
- `cached()`, sidecar reads, and variant `.part` existence checks all join `parts_dir_ + "/"` —
  already absolute-capable.
- **Bug also present** on the variant-bake call: `host_.bake_source(source, params_to_json(p2), {})`.
  Fixed: passes `vopts.parts_dir = parts_dir_`.

### `LocalProvider::connect` (local_provider.cpp)
- **Lines 187–196** (pre-fix): saved cwd, `chdir(abs_cache_root)`, then `fs_chdir(orig_cwd)`
  in three places (manifest error, install error, success path).
- **Lines 458–501** (pre-fix): second chdir bracket for tileset loop, two more `fs_chdir(orig_cwd)`.
- `RecordingBaker baker(host, ".", ...)` — the `"."` worked only because cwd == abs_cache_root.

### ScriptHost internal artifact writes
`bake_source` is the only place SP-2 writes `.part` bytes (`save_v2`). `eval_tileset`,
`eval_requires`, `eval_lod_budgets`, and `resolve_hash` write nothing. `tileset_phase.cpp`
already passed `parts_cache_dir` (absolute) to `HostBaker` — the fix completes the chain by
propagating it through `BakeOptions.parts_dir` to `bake_source`.

---

## Step 2: TDD — RED

Added `test_foreign_cwd_install()` to `MatterEngine3/tests/part_graph_integration_tests.cpp`.

Test procedure:
1. Create absolute sandbox `/tmp/me3_foreign_cwd` with `schemas/` and `parts/`.
2. `chdir("/")` — foreign cwd with no `parts/` subdir.
3. Construct `HostBaker(host, root)` where `root` is the absolute sandbox path.
4. Call `graph.install({ ChildRequest{"ForeignBox", {}} })`.
5. Assert artifact exists at `root + "/" + cache_path_resolved(hash)`.
6. Restore original cwd so subsequent tests run correctly.

**RED output (before fix):**
```
save_v2: fopen('parts/8fbf636ca4665b6c.part.tmp', 'wb') failed: errno=2 (No such file or directory)
  HostBaker::bake: save_v2 failed
FAIL: foreign_cwd: install with absolute parts_dir succeeds
  foreign_cwd install error: bake failed for part: ForeignBox
```

---

## Step 3: Fix

### Files changed

**`MatterEngine3/src/script_host.h`**
- Added `std::string parts_dir` field to `BakeOptions`.
- When non-empty, `bake_source` prefixes the relative cache path: `parts_dir + "/" + rel_path`.
- Empty `parts_dir` preserves backward compat for callers that still chdir themselves.

**`MatterEngine3/src/script_host.cpp`** (near line 1220)
- Build write path: if `opts.parts_dir` non-empty, use `opts.parts_dir + "/" + rel_path`;
  otherwise fall back to cwd-relative `rel_path`.

**`MatterEngine3/src/part_graph.cpp`**
- `HostBaker::bake`: construct `script_host::BakeOptions bopts; bopts.parts_dir = parts_dir_;`
  and pass it to `host_.bake_source(...)`.
- `HostBaker::bake_lod_variants` (line 392): same for the variant-bake call.

**`MatterEngine3/src/provider/local_provider.cpp`**
- Deleted the chdir bracket around `graph.install()` (lines 187–196, 293–294, plus the
  install-error and manifest-error `fs_chdir(orig_cwd)` calls).
- Deleted the second chdir bracket around the tileset loop (lines 458–501, including the two
  `fs_chdir(orig_cwd)` calls in the tileset error paths).
- Removed `fs_getcwd`/`fs_chdir` platform shims (now unused); trimmed `<direct.h>` comment.
- Changed `RecordingBaker baker(host, ".", ...)` to `RecordingBaker baker(host, abs_cache_root, ...)`.
- Updated stale comment on the SP-2/SP-3 wiring block.

**`MatterEngine3/tests/part_graph_integration_tests.cpp`**
- Added `test_foreign_cwd_install()`.
- Added call to it in `main()` between the demo-tree test and `test_lod_variant_sidecar`.

---

## Step 4: TDD — GREEN

After fixes, `make run-graph-integration` output (relevant lines):

```
  test_foreign_cwd_install done
  test_lod_variant_sidecar OK
```

No FAIL for `foreign_cwd`. Test passes.

Pre-existing 6 failures (`test_demo_tree_has_leaves`) remain unchanged — confirmed by baseline
run with `git stash` showing identical failures before my changes. Root cause: that helper
uses `load_v2(cache_path_resolved(hash), ...)` with a relative path after cwd is restored to
the test-binary dir (no `parts/` there). Out of scope for Task 3.

---

## Suite Results

| Target | Result | Notes |
|---|---|---|
| `run-graph` | ALL PASS | |
| `run-script` | ALL PASS | |
| `run-graph-integration` | 6 pre-existing FAILs; `foreign_cwd` PASS | See pre-existing note above |
| `run-meadow` | ALL PASS | |
| `run-meadow-check` | ALL PASS | |
| `run-flatten` | ALL PASS | |
| `run-grasslod` | ALL PASS | |
| `run-treebake` | exit 0 | |

---

## Files Changed

- `MatterEngine3/src/script_host.h`
- `MatterEngine3/src/script_host.cpp`
- `MatterEngine3/src/part_graph.cpp`
- `MatterEngine3/src/provider/local_provider.cpp`
- `MatterEngine3/tests/part_graph_integration_tests.cpp`
