# Final Review Fixes — Phase B Async Bake

## Finding 1: `classify_error` script-first priority (matter_engine.cpp:74-97)

**Problem.** The IoError bucket (`"manifest"`, `"not found"`, `"load"`) was checked before the ScriptError bucket (`"script"`, `"install"`, `"resolve hash"`, `"evaluate"`, `"bake "`). A script error message such as `"failed to resolve hash for part: BrokenPart"` does not contain "load" literally in this case, but any script error message that happens to contain "load" (e.g. a QuickJS message about a module load failure during script evaluation) would surface as `IoError` instead of `ScriptError`.

**Fix.** Swapped the IoError and ScriptError blocks so ScriptError is checked at priority 4 (after OOM, after GPU) and IoError at priority 5. The Cancelled and OutOfMemory top-priority buckets are unchanged. A block comment documents the rationale for the ordering.

**Test coverage.** Test (h) `broken_script_skips_part` asserts `ev.code == BakeErrorCode::ScriptError` for a JS syntax error. The error message from the pipeline is `"failed to resolve hash for part: BrokenPart"` — which contains `"resolve hash"` (ScriptError) but does not contain `"load"`. The test passes before and after the fix, but the fix ensures correctness for messages that do contain both indicators (e.g. QuickJS `"failed to load module"` during script evaluation).

---

## Finding 2: `publish_pipeline` shared helper (~230 lines deduplicated)

**Problem.** Steps 4-8 of the publish flow (GL reset job → reconcile job → per-part publish jobs → finalize job → BakeFinished event) were copy-pasted between `execute_bake` (~line 442–814) and `execute_rebake_cone` (~line 984–1223). Any future fix to the publish flow required two identical edits.

**Genuine differences parametrized** (identified by diffing the two blocks):

| Parameter | `execute_bake` value | `execute_rebake_cone` value | What it controls |
|---|---|---|---|
| `job_prefix` | `"bake"` | `"cone"` | GpuJob `name` strings and `assert_gl_thread` markers |
| `count_errors_seed` | install-phase error count | `0` | BakeFinished.errors initial value; bake counts install failures, cone does not |
| `init_culler` | `true` | `false` | First-success GpuCuller init in reset job (bake arms it; cone reuses prior init) |
| `update_probe_dims` | `true` | `false` | stats.probe_dims census in finalize job (bake has full manifest; cone is partial) |
| `verbose_reset_log` | `true` | `false` | printf raster-init error, probe-unavailable warning, sky-clear color in reset job |
| `fault_hook` | `cfg.test_fault_hook` | `{}` | Test injection hook in publish job (cone is not injection-tested) |
| `load_msg_include_hash` | `true` | `false` | Whether `" (hash N)"` is appended to load-failure message in publish job |

**Non-differences preserved verbatim** (these are identical in both executors and live in the shared helper without any flag):
- Publish order construction (want-first + trailing manifest hashes)
- BakePartDone post-time emit + cancellation checkpoint between parts
- CapState cap-growth logic
- skip-and-continue semantics (return true on part failure)
- GL-thread marshaling (all session-state mutation inside GL jobs)
- Finalize: lods, instances_total, parts_baked, cache_hits, exact-cap composer walk

**Implementation.** Added `PublishPipelineParams` struct and `publish_pipeline` member declaration to `WorldSession::Impl`. Implemented `WorldSession::Impl::publish_pipeline` between `execute_bake` and `execute_rebake_cone`. Both executors now call `publish_pipeline` after `compose_world`, passing their specific `PublishPipelineParams`. Total lines in each executor's publish section reduced from ~230 to ~10.

---

## Test run

Command: `make -C MatterEngine3/tests run-asyncbake`

Result: **ALL PASS** (a through j, including inotify e2e on Linux)

Notable confirmations:
- (g) OOM injection: `BakeError{OutOfMemory}` + `BakeFinished.errors==1` — fault_hook wired correctly in bake path
- (h) broken_script: `BakeError{ScriptError, module=BrokenPart}` — classify_error fix confirmed
- (i) load_failure: `BakeError{IoError, phase=parts}` + `BakeFinished.errors==1` — skip-and-continue in publish_pipeline
- (j) inotify e2e: BakeError{ScriptError, phase=cone} on syntax error — cone path through publish_pipeline

Library also builds clean: `make -C MatterEngine3` exits 0, no new warnings.
