# VG+VT Stabilization and Measurement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the `vg-vt-improvements` branch safe on its default paths, give the engine the frame-level attribution it lacks, remove the one load-time cost that is structural rather than measured, and produce the numbers that decide the order of the large Tier 1 refactors.

**Architecture:** Three sequential phases on one branch. Phase A fixes the thirteen Tier 0 defects, each with a regression test in the suite that already owns the code. Phase B adds the missing instrumentation (CPU traversal zones, p99 GPU zone stats, a capture-and-report tool) and runs one matched StreamMountain profiling session whose findings doc re-ranks Tier 1. Phase C lands the structural fixes that need no measurement first: a persistent geometry cook writer, the lost cache early-out, lazy bank commits, an RT descriptor dirty flag, and CTest registration for the tests that currently never run.

**Tech Stack:** C++17 (MSVC v143 via `tools/build-windows-from-wsl.sh`), Vulkan, GLSL, GNU Make (rollback path), CMake/CTest, Node 24 for world-script tests, Python 3 for tools.

**Spec:** `docs/vg-vt-work-queue-2026-09-19.md` (the queue this plan executes, Tier 0 + Tier 1.1/1.2/1.10/1.11 + parts of Tier 3) and `docs/findings/vg-vt-performance-architecture-2026-09-19.md` (the analysis behind Tier 1).

## Global Constraints

- Canonical build is MSVC through `./tools/build-windows-from-wsl.sh RelWithDebInfo <target>`; test executables land in `MatterEditor/build/cmake/windows-msvc/relwithdebinfo/`. The Make path is rollback-only.
- Never run two C++ test suites at the same time (WSL2 OOM); chain them sequentially.
- Windows test executables launched from WSL need `WSLENV` to forward environment variables, e.g. `WSLENV=MATTER_VK_SMOKE_MODE MATTER_VK_SMOKE_MODE=vt-queue ./vulkan_smoke_tests.exe`.
- CPU tests registered through `matter_add_engine_cpu_test` run with `WORKING_DIRECTORY MatterEngine3/tests` unless overridden; `finite_surface_recipe_tests` and `solid_source_evaluation_tests` run from the repo root.
- `libs/MatterSurfaceLib` is read-only except for genuine bug fixes; none are planned here.
- Diagnostics use `MATTER_LOGE/W/I/D("tag", fmt, ...)`; deliberate machine-readable stdout (`STATS`, `perf:` lines) stays on `printf`.
- LOD selection has one rule in `MatterEngine3/src/render/lod_distance.h`; do not add a second projected-size comparison.
- Compile sibling library sources from their source directory via `-I`; never copy or symlink.
- Every commit message ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Do not commit anything under `docs/agent/evidence/` larger than 64 KB or any PNG/log/jsonl/zip.

## Review Focus

1. A VT compositor setter that fails once and then succeeds must resume page fills on the next frame; the retry path must not require a world reload. Pinned to Task 2.
2. Two scenes with the same stem in different folders must produce a logged warning and a world list that still opens, never a crash; the first one found in sorted order wins. Pinned to Task 3.
3. A cancelled command burst followed by shutdown must return from `pop_wait` within the caller's budget even when the token mirror and channel disagree. Pinned to Task 4.
4. A terrain sector whose geometry-page compile fails must still receive the full LOD ladder and must log why. Pinned to Task 8.
5. A page that failed three reads and is re-requested after a camera move must be dispatched again, not ignored forever. Pinned to Task 10.

---

## Phase A — Tier 0 correctness

### Task 1: Baseline the branch tip

**Files:**
- None modified. Verifies the state the remaining tasks build on.

- [ ] **Step 1: Build the editor and the CPU test set**

Run from the repo root:
```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
./tools/build-windows-from-wsl.sh RelWithDebInfo all
```
Expected: both exit 0. If `all` is not an accepted target name, use `matter_engine_cpu_tests` if it exists, otherwise build the explicit list `vt_residency_tests geometry_hierarchy_tests async_queue_tests async_stage_pipeline_tests world_definition_tests partstore_tests vulkan_smoke_tests vt_compositor_tests`.

- [ ] **Step 2: Run the CPU suites this plan will touch, one at a time**

```bash
B=MatterEditor/build/cmake/windows-msvc/relwithdebinfo
(cd MatterEngine3/tests && ../../$B/vt_residency_tests.exe | tail -1)
(cd MatterEngine3/tests && ../../$B/geometry_hierarchy_tests.exe | tail -1)
(cd MatterEngine3/tests && ../../$B/async_queue_tests.exe | tail -1)
(cd MatterEngine3/tests && ../../$B/async_stage_pipeline_tests.exe | tail -1)
(cd MatterEngine3/tests && ../../$B/world_definition_tests.exe | tail -1)
(cd MatterEngine3/tests && ../../$B/partstore_tests.exe | tail -1)
```
Expected: each prints `ALL PASS` (or `0 failures`) and exits 0. Record any failure before continuing; a pre-existing failure is a finding, not a reason to stop.

- [ ] **Step 3: Run the five gated smoke modes sequentially**

```bash
cd MatterEditor/build/cmake/windows-msvc/relwithdebinfo
for m in vt-feedback vt-input-snapshot vt-direct-source vt-surfaces sparse-voxel; do
  WSLENV=MATTER_VK_SMOKE_MODE MATTER_VK_SMOKE_MODE=$m ./vulkan_smoke_tests.exe > /tmp/smoke_$m.log 2>&1; echo "$m rc=$?"; grep -c 'validation errors: [1-9]' /tmp/smoke_$m.log
done
```
Expected: every mode prints `rc=0` and the validation-error grep prints `0`.

- [ ] **Step 4: Record the baseline**

Append the six CPU results and five smoke results to `docs/vg-vt-work-queue-2026-09-19.md` under a new heading `## Baseline 2026-09-27` as one line each. Commit:
```bash
git add docs/vg-vt-work-queue-2026-09-19.md
git commit -m "docs: record vg-vt branch baseline before Tier 0 fixes"
```

---

### Task 2: A failed VT input push must not freeze page fills

**Files:**
- Modify: `MatterEngine3/src/render/vt_residency.h:997` (add getter next to `set_input_update_pending`)
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp:6843-6960` (`push_vt_compositor_inputs`)
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h:1496` (test hooks) and the private member block near `:4148`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp` (the `vt-input-snapshot` section around `:7735-7800`)

**Interfaces:**
- Produces: `bool VtResidency::input_update_pending() const`; `void VkSceneRenderer::test_fail_next_vt_input_push() noexcept`; `bool VkSceneRenderer::test_vt_fills_gated() const`.

- [ ] **Step 1: Add the residency getter and renderer hooks**

`vt_residency.h`, next to line 997:
```cpp
    void set_input_update_pending(bool pending) { input_update_pending_ = pending; }
    bool input_update_pending() const { return input_update_pending_; }
```
`vk_scene_renderer.h`, after line 1496 (`test_vt_input_update_pending`):
```cpp
    // True while VtResidency refuses page fills because an input push is in
    // progress. After a rejected push this must read false on the next frame.
    bool test_vt_fills_gated() const { return vt_ && vt_->input_update_pending(); }
    // Rejects the next push_vt_compositor_inputs() before any setter runs.
    void test_fail_next_vt_input_push() noexcept { test_fail_next_vt_input_push_ = true; }
```
Private member block (next to `test_fail_next_atmosphere_generation_` at `:4148`):
```cpp
    bool test_fail_next_vt_input_push_ = false;
    uint32_t vt_input_push_failures_ = 0;
```

- [ ] **Step 2: Write the failing smoke check**

In `vulkan_smoke_tests.cpp`, inside the `vt-input-snapshot` section, immediately after the block that loads the tileset slot the second time (the second `renderer.load_tileset_slot(0, path, error)` near line 7776) and before `materials[kMaterialA].flags_misc[1] = 0u;`, add:
```cpp
        {
            // A rejected input push must not latch the fill gate. Before the
            // fix, one setter failure left input_update_pending_ true forever.
            const auto fills_before = renderer.vt_stats().fills_total;
            materials[kMaterialA].base_roughness[3] = .77f;
            renderer.test_fail_next_vt_input_push();
            update_input_materials(3);
            draw_pixel();
            CHECK(!renderer.test_vt_fills_gated(),
                  "input snapshot: rejected push releases the fill gate");
            settle();
            CHECK(renderer.vt_stats().fills_total > fills_before,
                  "input snapshot: fills resume after a rejected push");
        }
```

- [ ] **Step 3: Run it to verify it fails**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests
cd MatterEditor/build/cmake/windows-msvc/relwithdebinfo && WSLENV=MATTER_VK_SMOKE_MODE MATTER_VK_SMOKE_MODE=vt-input-snapshot ./vulkan_smoke_tests.exe | grep -E 'FAIL|ALL PASS'
```
Expected: `FAIL: input snapshot: rejected push releases the fill gate` (the hook exists but the gate is still latched).

- [ ] **Step 4: Implement the scope guard and bounded retry**

In `push_vt_compositor_inputs()` replace the first three lines of the body with:
```cpp
void VkSceneRenderer::push_vt_compositor_inputs() {
    if (!vt_compositor_ || !vt_inputs_dirty_) return;
    // The gate stays closed only while this function is publishing. Every
    // early return below reopens it: the previous snapshot keeps serving
    // fills, which is strictly better than a frozen world. After a bounded
    // number of consecutive rejections the dirty flag is dropped so the loop
    // stops retrying every frame; the next authoring change re-arms it.
    struct PendingGate {
        vt::VtResidency* vt; bool published = false;
        ~PendingGate() { if (!published) vt->set_input_update_pending(false); }
    } gate{vt_.get()};
    vt_->set_input_update_pending(true);
    if (test_fail_next_vt_input_push_) {
        test_fail_next_vt_input_push_ = false;
        note_vt_input_push_failure("injected test failure");
        return;
    }
```
Add a private helper (declare in the header next to `push_vt_compositor_inputs`):
```cpp
void VkSceneRenderer::note_vt_input_push_failure(const char* why) {
    constexpr uint32_t kMaxConsecutiveFailures = 8;
    ++vt_input_push_failures_;
    MATTER_LOGE("vk", "VT input push rejected (%u/%u): %s", vt_input_push_failures_,
                kMaxConsecutiveFailures, why);
    if (vt_input_push_failures_ >= kMaxConsecutiveFailures) {
        MATTER_LOGE("vk", "VT input push giving up; rendering continues from the last published snapshot");
        vt_inputs_dirty_ = false;
    }
}
```
Change the two failing returns:
```cpp
        if (!vt_compositor_->set_tilesets(slots, tileset::kMaxTilesetSlots, error)) {
            note_vt_input_push_failure(error.c_str());
            return; // keep dirty (until the cap); never publish a rejected snapshot
        }
```
```cpp
        if (!vt_->set_input_snapshot(snapshot, changed_materials, reason)) {
            note_vt_input_push_failure("draw input snapshot publication rejected");
            return;
        }
```
After the successful `vt_->set_input_snapshot(...)` call (just before `vt_draw_snapshot_registry_[bank] = snapshot;`) add:
```cpp
    gate.published = true; // set_input_snapshot already cleared the pending flag
    vt_input_push_failures_ = 0;
```
The bank-exhaustion return at `:6896` needs no change: the guard now reopens the gate there too.

- [ ] **Step 5: Run the smoke mode to verify it passes**

Same command as Step 3. Expected: `ALL PASS`, and `grep -c 'validation errors: [1-9]'` on the log prints `0`.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/src/render/vt_residency.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "vt: a rejected compositor input push no longer latches the page-fill gate"
```

---

### Task 3: Duplicate scene or object names must warn, not throw

**Files:**
- Modify: `MatterEngine3/include/matter/project_layout.h` (whole file, 113 lines)
- Modify: `MatterEditor/src/ui.cpp:109`
- Modify: `MatterEditor/src/asset_browser.cpp:271-279`
- Test: `MatterEngine3/tests/world_definition_tests.cpp:236-244`

**Interfaces:**
- Produces: `struct matter::project_layout::Diagnostics { std::vector<std::string> duplicates; }`; every discovery function gains a trailing `Diagnostics* diagnostics = nullptr` parameter; duplicates are skipped (first in sorted directory order wins) and recorded instead of thrown.
- Consumes: nothing new. Existing callers compile unchanged because the parameter defaults to null.

- [ ] **Step 1: Rewrite the failing test expectation**

Replace lines 236-244 of `world_definition_tests.cpp` with:
```cpp
    fixture.write("objects/another/Rock.js", "// ambiguous");
    matter::project_layout::Diagnostics diag;
    const auto roots = matter::project_layout::object_roots({cfg.scene_objects_dir, cfg.objects_dir}, &diag);
    CHECK(diag.duplicates.size() == 1 && diag.duplicates[0].find("Duplicate object 'Rock'") != std::string::npos,
          "duplicate module names inside a tier are reported");
    CHECK(!roots.empty(), "duplicate module names do not abort root discovery");
    CHECK(fs::path(resolver.source_path_for("Rock")) == rock, "the first Rock in sorted order still resolves");
    fixture.write("scenes/other/BrickProof/BrickProof.js", "// ambiguous");
    diag = {};
    const auto scripts = matter::project_layout::scene_scripts(fixture.root / "scenes", &diag);
    CHECK(diag.duplicates.size() == 1 && diag.duplicates[0].find("Duplicate scene 'BrickProof'") != std::string::npos,
          "duplicate scene identities are reported");
    bool threw = false;
    try { (void)viewer::LocalProviderConfig::for_project(fixture.root.string(), "BrickProof", ""); }
    catch (const std::exception&) { threw = true; }
    CHECK(!threw, "a duplicate scene never throws out of for_project");
```

- [ ] **Step 2: Run to verify it fails to compile**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo world_definition_tests
```
Expected: compile error, `Diagnostics` is not a member of `matter::project_layout`.

- [ ] **Step 3: Change the header**

Add after the `hidden()` helper:
```cpp
// Discovery never throws. A duplicate identity inside one tier is a content
// error the caller decides how to surface; the first entry in sorted directory
// order wins so the choice is stable across machines.
struct Diagnostics { std::vector<std::string> duplicates; };
inline void note_duplicate(Diagnostics* d, std::string message) {
    if (d) d->duplicates.push_back(std::move(message));
}
```
Change the four signatures and the two `throw` sites:
```cpp
inline std::vector<fs::path> scene_scripts(const fs::path& scenes_dir, Diagnostics* diagnostics = nullptr) {
    ...
        const auto prior = names.emplace(name, script);
        if (!prior.second) {
            note_duplicate(diagnostics, "Duplicate scene '" + name + "': " +
                prior.first->second.string() + " and " + script.string());
            it.disable_recursion_pending();
            continue;
        }
    ...
}
inline fs::path scene_script(const fs::path& project, const std::string& name, Diagnostics* diagnostics = nullptr) {
    for (const auto& script : scene_scripts(project / "scenes", diagnostics))
        if (script.stem() == name) return script;
    return {};
}
inline std::vector<std::string> object_roots(const std::vector<std::string>& tiers, Diagnostics* diagnostics = nullptr) {
    ...
                const auto prior = names.emplace(name, it->path());
                if (!prior.second)
                    note_duplicate(diagnostics, "Duplicate object '" + name + "' in tier " + tier +
                        ": " + prior.first->second.string() + " and " + it->path().string());
    ...
}
inline std::vector<fs::path> object_files(const fs::path& tier, Diagnostics* diagnostics = nullptr) {
    ...
    for (const auto& dir : object_roots({tier.string()}, diagnostics)) {
```
Remove `#include <stdexcept>`. Note the sorted-order guarantee: `recursive_directory_iterator` order is unspecified, so in `object_roots` replace the per-file `names.emplace` logic with a two-pass form: collect `(stem, path)` pairs, `std::sort` them by path, then emplace in that order. Same for `scene_scripts`: collect candidate `(name, script)` pairs, sort by script path, then dedupe. The existing final `std::sort(scripts...)` line stays.

- [ ] **Step 4: Surface the diagnostics in the editor**

`ui.cpp:109` becomes:
```cpp
            matter::project_layout::Diagnostics diag;
            for (const auto& script : matter::project_layout::scene_scripts(scenes, &diag)) {
```
and after the loop:
```cpp
            for (const auto& d : diag.duplicates) MATTER_LOGW("ui", "%s", d.c_str());
```
`asset_browser.cpp:271-279`: same pattern with one `Diagnostics diag;` before the two calls and the same log loop after. Include `matter/log.h` in both files if not already included.

- [ ] **Step 5: Build and run the test**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo world_definition_tests matter_editor
(cd MatterEngine3/tests && ../../MatterEditor/build/cmake/windows-msvc/relwithdebinfo/world_definition_tests.exe | tail -3)
```
Expected: `ALL PASS`. Also run `eval_world_tests.exe` the same way, since it shares the loader.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/include/matter/project_layout.h MatterEditor/src/ui.cpp MatterEditor/src/asset_browser.cpp MatterEngine3/tests/world_definition_tests.cpp
git commit -m "layout: report duplicate scene/object names instead of throwing out of discovery"
```

---

### Task 4: `CommandQueue::pop_wait` cannot spin

**Files:**
- Modify: `MatterEngine3/src/async_bake.cpp:243-261`
- Modify: `MatterEngine3/src/async_bake.h:160-168` (lock-order comment)
- Test: `MatterEngine3/tests/async_queue_tests.cpp` (add one test, register in `main()` at `:432`)

**Interfaces:**
- Consumes: `CommandQueue::push`, `pop_wait`, `wake_idle`, `shut_down` as declared in `async_bake.h:140-153`.
- Produces: unchanged signatures. Contract clarified: `pop_wait` returns within `ms` plus one scheduling quantum even when every queued command is cancelled, and never busy-waits.

- [ ] **Step 1: Write the failing test**

Add before `main()`:
```cpp
// A burst of superseded commands is skipped inside the caller's budget: the
// deadline is re-checked after every cancelled skip, so the loop cannot spin
// past `ms` even when the channel keeps handing back cancelled entries.
static void test_pop_wait_cancelled_burst_honours_budget() {
    std::printf("[test_pop_wait_cancelled_burst_honours_budget]\n");
    CommandQueue cq;
    for (int i = 0; i < 64; ++i) cq.push({CommandKind::BakeAll, {}, nullptr}); // each supersedes the last
    Command out; bool idle = false;
    const auto start = std::chrono::steady_clock::now();
    const bool got = cq.pop_wait(out, /*ms=*/0, idle);
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(got && !idle && out.kind == CommandKind::BakeAll, "the surviving BakeAll is delivered");
    CHECK(took < std::chrono::milliseconds(200), "cancelled burst is skipped promptly");
    cq.shut_down();
    printf("ok pop_wait_cancelled_burst_honours_budget\n");
}
```
Register it in `main()` after `test_idle_wake_releases_parked_consumer();`.

- [ ] **Step 2: Run to verify current behaviour**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo async_queue_tests
(cd MatterEngine3/tests && ../../MatterEditor/build/cmake/windows-msvc/relwithdebinfo/async_queue_tests.exe | tail -3)
```
Expected: this test passes today (the mirror invariant holds), which is fine: it pins the contract the fix must keep. The fix is defensive against mirror drift, which cannot be triggered from the public API.

- [ ] **Step 3: Make the loop drift-safe**

Replace the body of `pop_wait` (`async_bake.cpp:243-261`) with:
```cpp
bool CommandQueue::pop_wait(Command& out, int ms, bool& out_timed_out) {
    out_timed_out = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, ms));
    std::unique_lock<std::mutex> lock(m_);
    for (;;) {
        if (!service_cv_.wait_until(lock, deadline, [&] { return shut_down_ || !pending_.empty() || idle_wake_; })) {
            out_timed_out = true; return false;
        }
        if (shut_down_) return false;
        // Command priority is decided under the same lock as producers. A
        // wake cannot slip ahead of a queued reload or shutdown command.
        if (pending_.empty()) { idle_wake_ = false; out_timed_out = true; return false; }
        Command tmp;
        if (!ch_.try_pop(tmp)) {
            // pending_ says a command exists but the channel is empty. That is
            // a mirror drift, not a reason to spin: resynchronise and report
            // the wait as idle so the caller re-enters with a fresh budget.
            pending_.clear(); idle_wake_ = false; out_timed_out = true; return false;
        }
        pending_.pop_front();
        if (tmp.token && tmp.token->is_cancelled()) {
            if (std::chrono::steady_clock::now() >= deadline && pending_.empty()) { out_timed_out = true; return false; }
            continue;
        }
        in_flight_ = tmp.token; out = std::move(tmp); return true;
    }
}
```
Do NOT clear `idle_wake_` when a real command is delivered: `test_idle_wake_priority_and_coalescing` pins that a coalesced wake survives a preceding command.

- [ ] **Step 4: Fix the stale lock-order comment**

In `async_bake.h:160-168` replace the sentence beginning `pop touches ch_ first` with:
```
    // pop() touches ch_ first (blocking wait_pop, no m_ held) then acquires m_.
    // pop_wait() holds m_ across ch_.try_pop(), nesting m_ -> channel mutex,
    // which is the same order producers use, so neither path can deadlock.
```

- [ ] **Step 5: Run the suite**

Same command as Step 2. Expected: `ALL PASS`, including the four existing `pop_wait`/`idle_wake` tests.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/src/async_bake.cpp MatterEngine3/src/async_bake.h MatterEngine3/tests/async_queue_tests.cpp
git commit -m "async_bake: pop_wait resynchronises on mirror drift and re-checks its deadline per skip"
```

---

### Task 5: Replace `queued_keys_.at()` with guarded lookups

**Files:**
- Modify: `MatterEngine3/src/render/vt_residency.cpp:2314` and `:3241`
- Test: `MatterEngine3/tests/vt_queue_tests.h` (add a check inside `run_page_probe`, invoked by the `vt-queue` smoke mode)

**Interfaces:**
- Consumes: `VtResidency::queue_page` (private) returns without inserting when `!v.live || !indirection.in_range(...)`.
- Produces: no API change.

- [ ] **Step 1: Write the regression check**

`queue_page` skips a page outside the variant's indirection range. The durable dirty path re-feeds such a page after an owner promotion shrinks the table. Add to `vt_queue_tests.h`, at the end of `run_page_probe` (before its closing brace), a probe that dirties a page and then promotes the owner to a coarser rung so its mip range no longer contains the page:
```cpp
    {
        // A durable dirty page whose owner no longer covers its mip must be
        // dropped, not looked up with std::map::at (which threw before).
        const uint32_t owner = residency.register_variant(0x9010u, /*rung=*/0, context, atlas, error);
        CHECK(owner != vt::kVtNoSlot, "queue probe: coarse owner registers");
        const vt::VtFeedbackRequest fine{owner - 1u, /*mip=*/0, 0, 0};
        residency.inject_feedback_for_test(&fine, 1);
        CHECK(frames.next(residency, 40), "queue probe: fine request frame");
        residency.invalidate_owners({owner}, vt::VtInvalidationReason::SourceInputs);
        residency.release_rung_alias(0x9010u, /*rung=*/0);
        bool survived = true;
        try { survived = frames.next(residency, 41); } catch (const std::exception&) { survived = false; }
        CHECK(survived, "queue probe: dirty page outside the owner's range is dropped without throwing");
    }
```
Adjust the two helper names (`invalidate_owners`, `release_rung_alias`) to the exact public spellings in `vt_residency.h` if they differ; both exist per the residency review.

- [ ] **Step 2: Run to verify it fails**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests
cd MatterEditor/build/cmake/windows-msvc/relwithdebinfo && WSLENV=MATTER_VK_SMOKE_MODE MATTER_VK_SMOKE_MODE=vt-queue ./vulkan_smoke_tests.exe | grep -E 'FAIL|ALL PASS'
```
Expected: `FAIL: queue probe: dirty page outside the owner's range is dropped without throwing` (or an uncaught `std::out_of_range` abort, which also counts as failing).

- [ ] **Step 3: Guard both lookups**

`vt_residency.cpp:2314` (in `queue_dirty_pages`):
```cpp
            queue_page(v, pending.page, true, pending.preassigned_slot);
            const auto queued = queued_keys_.find(page_key(v.layer, pending.page));
            if (queued == queued_keys_.end()) { it = dirty_pages_.erase(it); continue; }
            queue_[queued->second].requested_frame =
                std::min(queue_[queued->second].requested_frame, pending.requested_frame);
```
`vt_residency.cpp:3241` (stale-fill retry):
```cpp
                if (owner_current && m.preassigned) {
                    queue_page(variants_[m.layer], m.page, true, m.slot);
                    const auto retry = queued_keys_.find(page_key(m.layer, m.page));
                    if (retry != queued_keys_.end())
                        queue_[retry->second].requested_frame =
                            std::min(queue_[retry->second].requested_frame, m.requested_frame);
                }
```

- [ ] **Step 4: Run to verify it passes**

Same command as Step 2. Expected: `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vt_residency.cpp MatterEngine3/tests/vt_queue_tests.h
git commit -m "vt: never index queued_keys_ with at() after a conditional queue_page"
```

---

### Task 6: Tail eviction retires the per-slot side tables

**Files:**
- Modify: `MatterEngine3/src/render/vt_residency.cpp:1708-1718` (`register_variant_impl`)
- Test: `MatterEngine3/tests/vt_queue_tests.h` (`run_replacements`, `vt-queue` mode)

**Interfaces:**
- Consumes: `retire_slot_input_snapshot`, `retire_slot_geometry`, `retire_slot_material_mapping`, `retire_slot_occlusion` (private, declared `vt_residency.h:1336,1409-1411`).
- Produces: no API change. Invariant restored: every path that recycles a live slot retires all four side tables.

- [ ] **Step 1: Write the regression check**

Append to `run_replacements` in `vt_queue_tests.h`:
```cpp
    {
        // Fill the pool so the next registration must evict a live page for
        // its tail, then verify the recycled slot carries no stale material
        // mapping: publish_receiver_materials compares slot content revisions
        // and would otherwise bind a mapping to a page never written.
        std::vector<uint32_t> owners;
        for (uint32_t i = 0; i < 600; ++i) {
            const uint32_t o = residency.register_variant(0xA000u + i, 0, context, atlas, error);
            if (o == vt::kVtNoSlot) break;
            owners.push_back(o);
        }
        CHECK(owners.size() >= 2, "replacements: pool saturates");
        const auto before = residency.stats();
        CHECK(frames.next(residency, 80), "replacements: frame after saturation");
        const auto after = residency.stats();
        CHECK(after.material_pages <= before.material_pages,
              "replacements: recycled tail slots do not retain material bindings");
        for (uint32_t o : owners) residency.release_variant(o);
    }
```
Use the exact release spelling from `vt_residency.h` (`release_variant` or `release_variant_key`).

- [ ] **Step 2: Run to verify it fails**

Same `vt-queue` command as Task 5. Expected: the new `replacements:` check fails or `material_pages` grows.

- [ ] **Step 3: Retire the tables on the tail path**

`vt_residency.cpp:1708-1718` becomes:
```cpp
    if (evicted.live) {
        // The pool was full of unpinned pages; recycle exactly as record_frame
        // does. Every side table keyed by slot must be retired here too, or
        // the new owner inherits the old owner's content revision, material
        // mapping and GPU metadata addresses.
        dirty_pages_.erase(tail_slot);
        material_pages_.release(tail_slot);
        retire_slot_input_snapshot(tail_slot);
        retire_slot_geometry(tail_slot);
        retire_slot_occlusion(tail_slot);
        retire_slot_material_mapping(tail_slot);
        if (event_log_)
            MATTER_LOGI("vt-evict", "frame=%llu owner=%016llx mip=%u x=%u y=%u slot=%u reason=tail",
                static_cast<unsigned long long>(frame_index_),
                static_cast<unsigned long long>(evicted.variant_key),
                evicted.page.mip, evicted.page.px, evicted.page.py, tail_slot);
        const auto owner_layer = layer_of_.find(evicted.variant_key);
        if (owner_layer != layer_of_.end())
            variants_[owner_layer->second].indirection.unmap(
                evicted.page.mip, evicted.page.px, evicted.page.py);
    }
```
Then in `record_frame`'s eviction branch (`:3100-3115`) add the one missing call `retire_slot_occlusion(slot);` after `retire_slot_geometry(slot);` so both paths are identical. Confirm each `retire_slot_*` marks the page-metadata upload range dirty (`input_indices_dirty_begin_/end_`); if `retire_slot_geometry` or `retire_slot_occlusion` do not, add the same two-line range update that `set_slot_input_snapshot` performs at `:923-924`.

- [ ] **Step 4: Run to verify it passes**

Same command. Expected: `ALL PASS`, zero validation errors.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vt_residency.cpp MatterEngine3/tests/vt_queue_tests.h
git commit -m "vt: retire every per-slot side table when a tail registration recycles a live page"
```

---

### Task 7: Zero-size buffers are never requested from the compositor

**Files:**
- Modify: `MatterEngine3/src/render/vt_compositor.cpp:1180-1195` (the `allocate` lambda)
- Test: `MatterEngine3/tests/vt_compositor_tests.cpp` (existing geometry-reuse test; add a check)

**Interfaces:**
- Produces: `allocate(buffer, bytes)` treats `bytes == 0` as success with `buffer.buffer == VK_NULL_HANDLE` and records a `kEmptyStreams` counter; every descriptor write for such a stream binds the compositor's shared 16-byte zero buffer instead.

- [ ] **Step 1: Add the shared empty buffer**

In `VtCompositor::Impl` add `RawBuffer empty_stream;` created once in `init()` right after the rings are created:
```cpp
    if (!create_raw_buffer(device, phys, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, impl->empty_stream, error,
                           /*prefer_device_local=*/false)) return nullptr;
    std::memset(impl->empty_stream.mapped, 0, 16);
```
and destroyed in the destructor alongside the other raw buffers.

- [ ] **Step 2: Write the failing check**

In `vt_compositor_tests.cpp`, find the test that exercises geometry reuse across surface invalidation (search for `invalidate_surface`). After the second `fill()` that reuses geometry, add:
```cpp
    CHECK(compositor->gpu_preparation_stats().empty_streams == 0 || compositor->gpu_preparation_stats().allocations > 0,
          "compositor: an entry with empty surface rows never issues a zero-size vkCreateBuffer");
```
and add a temporary fixture variant whose prepared surface has zero rows (a chart with no tape and no weights) so `surface_bytes == 0` on the reuse path. Run under the validation layer; the current code produces `VUID-VkBufferCreateInfo-size-00912`.

- [ ] **Step 3: Run to verify it fails**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo vt_compositor_tests
cd MatterEditor/build/cmake/windows-msvc/relwithdebinfo && ./vt_compositor_tests.exe 2>&1 | grep -E 'size-00912|FAIL|ALL PASS'
```
Expected: a `VUID-VkBufferCreateInfo-size-00912` line.

- [ ] **Step 4: Guard the lambda**

```cpp
    const auto allocate = [&](RawBuffer& buffer, size_t bytes, bool draw_geometry = false) {
        if (buffer.buffer) return true;
        if (bytes == 0) {
            // An empty stream is legal (no tape rows, no finite ids). Bind the
            // shared zero buffer rather than asking Vulkan for a 0-byte object.
            ++gpu_preparation.empty_streams;
            buffer.buffer = impl.empty_stream.buffer; buffer.memory = VK_NULL_HANDLE;
            buffer.mapped = impl.empty_stream.mapped; buffer.size = 0; buffer.shared = true;
            return true;
        }
        if (!take_allocation()) return false;
```
Add `bool shared = false;` to `RawBuffer` and make `destroy_raw_buffer` a no-op when `shared`. Add `uint64_t empty_streams = 0;` to `GpuPreparationStats`. Every `copy(...)` call already returns immediately when its byte count is zero (verify `copy` starts with `if (!bytes) return true;`; add it if absent).

- [ ] **Step 5: Run to verify it passes**

Same command. Expected: `ALL PASS`, no `size-00912` line.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/src/render/vt_compositor.cpp MatterEngine3/src/render/vt_compositor.h MatterEngine3/tests/vt_compositor_tests.cpp
git commit -m "vt: bind a shared zero buffer for empty preparation streams instead of a 0-byte VkBuffer"
```

---

### Task 8: Terrain page-compile failure falls back to the full ladder

**Files:**
- Modify: `MatterEngine3/src/render/part_store.cpp:1575-1640`
- Test: `MatterEngine3/tests/partstore_tests.cpp`

**Interfaces:**
- Consumes: `lod_bake::bake_lods` / `bake_terrain_lods` (`lod_bake.h:250, 371`), `geometry::cache_asset` (`geometry_asset.h`).
- Produces: a staged terrain sector always has either `geometry_pages` set or `thresholds.size() >= 2`.

- [ ] **Step 1: Write the failing test**

Add to `partstore_tests.cpp` (register in `main()`):
```cpp
// MATTER_GEOMETRY_CACHE_ONLY=1 makes every page compile a cache miss that is
// refused. The sector must then keep the ordinary ladder, not ship one rung.
static void test_terrain_page_failure_keeps_ladder(const fs::path& root) {
    std::printf("[test_terrain_page_failure_keeps_ladder]\n");
    setenv_compat("MATTER_GEOMETRY_TERRAIN", "1");
    setenv_compat("MATTER_GEOMETRY_CACHE_ONLY", "1");
    script_host::ScriptHost host; script_host::BakeOptions options;
    options.parts_dir = (root / "terrain_fallback").string(); options.retain_geometry = true;
    fs::create_directories(fs::path(options.parts_dir) / "parts");
    // A 64 m flat quad grid: large enough for the radius >= 32 terrain guard.
    const std::string source =
        "class Sector extends Part{static lodBudgets=[1];static noImpostor=true;"
        "build(){this.fill(8);this.beginShape(0);"
        "for(let z=0;z<8;++z)for(let x=0;x<8;++x){const a=[x*8-32,0,z*8-32],b=[x*8-24,0,z*8-32],c=[x*8-24,0,z*8-24],d=[x*8-32,0,z*8-24];"
        "this.vertex(...a);this.vertex(...b);this.vertex(...c);this.vertex(...a);this.vertex(...c);this.vertex(...d);}"
        "this.endShape();}}";
    const auto baked = host.bake_source(source, "{}", options);
    CHECK(baked.error.ok && baked.geometry, "terrain fallback fixture bakes");
    if (!baked.geometry) return;
    viewer::PartStore store(options.parts_dir);
    store.set_geometry_pages_enabled(true);
    const auto staged = store.stage_from_bake(baked.resolved_hash, *baked.geometry, /*first_rung=*/0, /*terrain_sector=*/true);
    CHECK(staged.ok, "terrain fallback fixture stages");
    CHECK(!staged.lp.geometry_pages, "cache-only compile refuses pages");
    CHECK(staged.lp.thresholds.size() >= 2, "sector keeps a multi-rung ladder when pages fail");
    unsetenv_compat("MATTER_GEOMETRY_TERRAIN"); unsetenv_compat("MATTER_GEOMETRY_CACHE_ONLY");
}
```
Add small `setenv_compat`/`unsetenv_compat` helpers at the top of the file using `_putenv_s` on `_WIN32` and `setenv`/`unsetenv` elsewhere. Match the exact `stage_from_bake` parameter order from `part_store.h:457-461`.

- [ ] **Step 2: Run to verify it fails**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo partstore_tests
(cd MatterEngine3/tests && ../../MatterEditor/build/cmake/windows-msvc/relwithdebinfo/partstore_tests.exe | grep -E 'FAIL|ALL PASS')
```
Expected: `FAIL: sector keeps a multi-rung ladder when pages fail`.

- [ ] **Step 3: Restructure the bake into a fallback-capable lambda**

Replace lines 1575-1590 (from `lod_bake::BakeTargets regular_targets;` through the `lods` initialiser) with:
```cpp
    lod_bake::BakeTargets regular_targets;
    const bool single_rung_source = snapshot.source_single_full_rep && !terrain_sector && !terrain_tile &&
        !animation_asset && !snapshot.animation_link && children.empty();
    const auto bake_ladder = [&](const lod_bake::BakeTargets& targets) {
        lod_handles.clear(); rung_charts.clear();
        return terrain_tile && !terrain_pages
            ? lod_bake::bake_terrain_lods(tris, skirt_mask, radius, terrain_targets,
                                          *staged.staging, triex_ptr, observer_,
                                          &lod_handles, &chart_opts, &rung_charts)
            : lod_bake::bake_lods(tris, targets, *staged.staging, triex_ptr, observer_,
                                  &lod_handles, &chart_opts, &rung_charts);
    };
    if (terrain_pages || single_rung_source) {
        regular_targets.keep_ratio.resize(1);
        regular_targets.threshold.resize(1);
    }
    lod_bake::LodLevels lods = bake_ladder(regular_targets);
```
Then, after the `if (terrain_pages && !lod_handles.empty()) { ... }` block and before `staged.ladder_ms = stage_split();`, add:
```cpp
    if (terrain_pages && !staged.lp.geometry_pages) {
        // Pages were the only reason the ladder was collapsed. Without them
        // the sector would draw its full rung at every distance. Re-bake the
        // ordinary ladder; rung 0 dedups onto the handle already registered.
        MATTER_LOGW("geometry", "terrain %016llx: page compile unavailable, restoring the %zu-rung ladder",
                    static_cast<unsigned long long>(part_hash), lod_bake::BakeTargets{}.keep_ratio.size());
        lods = bake_ladder(lod_bake::BakeTargets{});
    }
```
Also make the silent branch loud: change `if (source && source->tri_extra.size() == source->triangles.size()) {` to log when it is false:
```cpp
        if (!source || source->tri_extra.size() != source->triangles.size())
            MATTER_LOGW("geometry", "terrain %016llx: source rung lacks per-triangle attributes; no pages",
                        static_cast<unsigned long long>(part_hash));
```

- [ ] **Step 4: Run to verify it passes**

Same command. Expected: `ALL PASS`. Also run `sector_lod_tests.exe` and `sector_bake_tests.exe` sequentially to confirm the ladder shape elsewhere is unchanged.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/part_store.cpp MatterEngine3/tests/partstore_tests.cpp
git commit -m "part_store: restore the full LOD ladder when terrain geometry pages fail to compile"
```

---

### Task 9: Worker-lane exceptions degrade instead of terminating

**Files:**
- Modify: `MatterEngine3/src/streaming/async_stage_pipeline.h:60-100`
- Modify: `MatterEngine3/src/render/geometry_world_runtime.cpp:390-393, 659-661`
- Test: `MatterEngine3/tests/async_stage_pipeline_tests.cpp`

**Interfaces:**
- Produces: `AsyncStagePipeline` never propagates an exception out of either thread; a failure callback that throws marks the item failed with the message `"failure callback threw"`.

- [ ] **Step 1: Write the failing test**

Add a fourth block in `async_stage_pipeline_tests.cpp` before the final `std::puts`:
```cpp
    {
        // A failure callback that throws must not take the process down.
        streaming::AsyncStagePipeline<Work> pipeline(2,2,
            [](auto&){throw std::runtime_error("read failed");},
            [](Work&){},
            [](Work&,const char*){throw std::runtime_error("failure callback threw");});
        assert(pipeline.submit({9,{}}));
        std::deque<Work> done;until([&]{done=pipeline.take();return !done.empty();});
        assert(done.size()==1 && pipeline.available()==2);
    }
```

- [ ] **Step 2: Run to verify it fails**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo async_stage_pipeline_tests
(cd MatterEngine3/tests && ../../MatterEditor/build/cmake/windows-msvc/relwithdebinfo/async_stage_pipeline_tests.exe; echo rc=$?)
```
Expected: abnormal termination (rc != 0, no `ALL PASS`).

- [ ] **Step 3: Guard the failure callback**

Add a private helper to the class:
```cpp
    void report_failure(T& value, const char* why) noexcept {
        try { failure_(value, why); }
        catch (...) { /* the item is already failed; a throwing reporter must not kill the lane */ }
    }
```
and replace the four `failure_(...)` calls in `io_loop`/`prepare_loop` with `report_failure(...)`. Wrap each loop body in an outer `try { ... } catch (...) {}` around everything after the wait so that an exception from `std::move`/`push_back` (allocation) also cannot escape the thread.

- [ ] **Step 4: Degrade the runtime instead of failing the frame**

`geometry_world_runtime.cpp:390-393` (hierarchy build result):
```cpp
        if (!ready.error.empty()) {
            MATTER_LOGW("geometry", "hierarchy build for asset %016llx failed: %s; asset falls back to its source part",
                        static_cast<unsigned long long>(ready.asset), ready.error.c_str());
            d.rejected_admissions.insert(ready.asset);
            if (d.profiling) ++d.profile.hierarchy_failures;
            continue;
        }
```
`:659-661` (scene build result):
```cpp
        if (!ready.error.empty()) {
            MATTER_LOGW("geometry", "scene assembly failed: %s; keeping the previous scene", ready.error.c_str());
            if (d.profiling) ++d.profile.scene_discarded;
            continue;
        }
```
Add `uint64_t hierarchy_failures = 0;` to `GeometryPagingProfile` and include it in the profile line. Include `matter/log.h` if not already.

- [ ] **Step 5: Run to verify it passes**

Same command. Expected: `ALL PASS: asynchronous overlap, ...` and rc=0. Also rebuild `matter_editor` to confirm the runtime compiles.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/src/streaming/async_stage_pipeline.h MatterEngine3/src/render/geometry_world_runtime.cpp
git add MatterEngine3/tests/async_stage_pipeline_tests.cpp
git commit -m "geometry: worker-lane exceptions degrade the asset instead of terminating or failing the frame"
```

---

### Task 10: Evict-then-republish leaks nothing and failed pages can recover

**Files:**
- Modify: `MatterEngine3/src/geometry/geometry_residency.cpp:186-201` (`request`), `:269-275` (`fail`)
- Modify: `MatterEngine3/src/geometry/geometry_residency.h` (`ResidencyStats`)
- Modify: `MatterEngine3/src/render/geometry_world_runtime.cpp:265-277` (`collect`), `:590-604` (publish loop), `:838-841` (evict loop)
- Test: `MatterEngine3/tests/geometry_hierarchy_tests.cpp`

**Interfaces:**
- Produces: `Residency::request` on a page in `State::Failed` resets it to `Queued` with `attempts = 0`, increments `ResidencyStats::failed_retries`, and returns true. `GeometryWorldRuntime::Impl` gains `std::vector<GpuPage> retired_gpu_pages` drained by `collect`.

- [ ] **Step 1: Write the failing residency test**

Add to `geometry_hierarchy_tests.cpp` a function called from `main()` after `visibility_priority_checks(cache)`:
```cpp
static void failed_page_recovery_checks(asset_store::PageCache& cache) {
    using namespace geometry;
    std::string error;
    Residency residency;
    const auto lease=residency.attach(cache.read_manifest("unique").page,error);
    auto roots=residency.dispatch(8,0);
    CHECK(roots.size()==1,"recovery fixture has one root");
    if(roots.size()!=1)return;
    auto ticket=roots.front();
    const auto page=ticket.page;
    for(uint32_t attempt=0;attempt<3;++attempt) {
        residency.fail(ticket,/*retry_epoch=*/attempt+1);
        const auto again=residency.dispatch(8,attempt+2);
        if(attempt<2){CHECK(again.size()==1,"failed page is retried while attempts remain");if(again.empty())return;ticket=again.front();}
        else CHECK(again.empty(),"third failure parks the page");
    }
    CHECK(residency.request(lease,page),"a re-request of a parked page is accepted");
    const auto revived=residency.dispatch(8,100);
    CHECK(revived.size()==1 && revived.front().page==page,"re-requested page dispatches again");
    CHECK(residency.stats().failed_retries==1,"recovery is counted");
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo geometry_hierarchy_tests
(cd MatterEngine3/tests && ../../MatterEditor/build/cmake/windows-msvc/relwithdebinfo/geometry_hierarchy_tests.exe | grep -E 'FAIL|ALL PASS')
```
Expected: `FAIL: a re-request of a parked page is accepted` (and a compile error for `failed_retries` until Step 3).

- [ ] **Step 3: Let `request` revive a failed page**

`geometry_residency.h`: add `uint64_t failed_retries = 0;` to `ResidencyStats`.
`geometry_residency.cpp` `request`, replace the last three lines:
```cpp
    if (page.state == Impl::State::Failed) {
        // A parked page is re-armed by an explicit request (new visibility or a
        // refreshed location), never by the retry clock. Attempts start over.
        page.state = Impl::State::Queued; page.attempts = 0; page.retry_epoch = 0;
        ++d.counters.failed_retries;
    }
    d.queue(page);
    if (page.resident) d.discover(lease.id, page.resident->node);
    return true;
```
`stats()` must copy `counters.failed_retries` into the returned struct (check the existing `stats()` body copies every counter field; add the new one).

- [ ] **Step 4: Stop erasing locations on eviction and retire replaced GPU pages**

`geometry_world_runtime.cpp:838-841`: delete the line `d.locations.erase(candidate.second);`. Locations are already erased on detach (the `if(detached)` loop that follows `dispatch_pending()`), which is the only time a page's on-disk location changes.

`:590-604` publish loop, replace the `d.gpu_pages[...] = {...}` line with:
```cpp
        auto previous = d.gpu_pages.find(it->ticket.page);
        if (previous != d.gpu_pages.end() && previous->second.id != it->gpu_id) {
            // The old part may still be pinned by a submitted snapshot; hand it
            // to collect(), which releases it once that lease retires.
            d.retired_gpu_pages.push_back(previous->second);
            d.collection_needed = true;
        }
        d.gpu_pages[it->ticket.page] = {it->gpu_id, std::make_shared<uint64_t>(d.epoch), d.residency.resident(it->ticket.page)};
```
Add `std::vector<GpuPage> retired_gpu_pages;` to `Impl` (use the exact struct name `d.gpu_pages` maps to). In `collect()`, before the existing loop:
```cpp
        for (auto it = retired_gpu_pages.begin(); it != retired_gpu_pages.end();) {
            if (it->resident.expired()) { invalidate_scene(); renderer.release_part(it->id); it = retired_gpu_pages.erase(it); }
            else { collection_needed = true; ++it; }
        }
```
and in `reset()` release every entry of `retired_gpu_pages` unconditionally (the device is being torn down) and clear it.

- [ ] **Step 5: Run to verify it passes**

Same command. Expected: `ALL PASS`. Rebuild `matter_editor` to confirm the runtime compiles.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/src/geometry/geometry_residency.cpp MatterEngine3/src/geometry/geometry_residency.h MatterEngine3/src/render/geometry_world_runtime.cpp MatterEngine3/tests/geometry_hierarchy_tests.cpp
git commit -m "geometry: re-requested failed pages recover, and republished pages retire their old renderer part"
```

---

### Task 11: Sparse voxel timing readback respects the shadow-only pool size

**Files:**
- Modify: `MatterEngine3/src/render/vk_sparse_voxel.cpp:1357-1364`
- Test: `MatterEngine3/tests/sparse_voxel_shadow_tests.h` (invoked by the `sparse-voxel` smoke mode)

- [ ] **Step 1: Write the failing check**

In `sparse_voxel_shadow_tests.h`, after the first successful `create_shadow_casters` (or `create_shared_shadow_casters`) snapshot is recorded and submitted, add:
```cpp
    {
        vt_sparse::SparseVoxelTimings timings; std::string timing_error;
        const bool ok = scene->readback_timings(vulkan, /*slot=*/0, timings, timing_error);
        CHECK(ok && !timings.valid, "shadow-only snapshot reports no selection/visibility timings instead of reading past its 2-query pool");
    }
```
Use the actual namespace/type spelling from `vk_sparse_voxel.h:100-159`.

- [ ] **Step 2: Run to verify it fails**

`sparse-voxel` smoke mode (same launch pattern as Task 5). Expected: a validation error mentioning `vkGetQueryPoolResults` and `firstQuery`/`queryCount`, or the new CHECK failing.

- [ ] **Step 3: Guard the readback**

```cpp
    if(!allocation_ || allocation_->shadow_only || !allocation_->frames[slot].timestamps) return true;
```

- [ ] **Step 4: Run to verify it passes**

Expected: `ALL PASS`, validation error count 0.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vk_sparse_voxel.cpp MatterEngine3/tests/sparse_voxel_shadow_tests.h
git commit -m "sparse voxel: readback_timings skips shadow-only snapshots that own a 2-query pool"
```

---

### Task 12: `part_surface::prepare` rejects out-of-range sources and documents `bind` lifetimes

**Files:**
- Modify: `MatterEngine3/src/part_surface.cpp:86-96, 134-142`
- Modify: `MatterEngine3/src/part_surface.h:40-60`
- Test: `MatterEngine3/tests/part_surface_provider_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add to `part_surface_provider_tests.cpp` near the existing composite test (around `:175-200`), a function registered in `main()`:
```cpp
void test_placement_source_out_of_range(const fs::path& root,const viewer::LocalProviderConfig& cfg) {
    script_host::EvaluatedFiniteSurface bad;
    bad.present=true; bad.version=2;
    script_host::FiniteSurfacePlacement placement; placement.source=7; // no such source
    for(int i=0;i<16;++i)placement.matrix[i]=(i%5==0)?1.f:0.f;
    bad.placements.push_back(placement);
    part_surface::SourceCache cache;part_surface::Stats stats;gpu_meshing::Error e;
    std::shared_ptr<const part_surface::Prepared> out;
    CHECK(!part_surface::prepare(bad,root.string(),cfg.vk_solid_face_project,cfg.vk_face_material_bake,out,stats,e,{},&cache),
          "placement with an out-of-range source is refused");
    CHECK(!e.ok && e.message.find("source")!=std::string::npos,"refusal names the source index");
}
```
Match the `Error` field names (`ok`, `message`) to `gpu_meshing::Error` in `solid_face_projection.h`.

- [ ] **Step 2: Run to verify it fails**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo part_surface_provider_tests
(cd MatterEngine3/tests && ../../MatterEditor/build/cmake/windows-msvc/relwithdebinfo/part_surface_provider_tests.exe | grep -E 'FAIL|ALL PASS'; echo rc=$?)
```
Expected: a crash (rc != 0) or `FAIL: placement with an out-of-range source is refused`.

- [ ] **Step 3: Add the bounds checks**

At both `part_surface.cpp:93` and `:139`, before `const auto& catalog=*bank[p.source]->sources;`:
```cpp
            if(p.source>=bank.size()||!bank[p.source]||!bank[p.source]->sources)
                return fail(e,gpu_meshing::ErrorCode::InvalidInput,
                    "finite placement references source "+std::to_string(p.source)+" of "+std::to_string(bank.size()));
```
Use the file's own error helper: `prepare` already produces `gpu_meshing::Error` values; mirror the exact call the nearest existing refusal in the same function uses.

- [ ] **Step 4: Document the `bind` lifetime**

In `part_surface.h` above `bool bind(...)` replace the comment with:
```cpp
// Scratch survives registration (residency takes its own copy). The returned
// context ALSO aliases `prepared`: surface_tape_text and surface_materials point
// into it. The caller must keep the same shared_ptr<const Prepared> alive until
// VtResidency::register_variant / update_variant_surface has returned. Require
// each receiver vertex to match exactly one outward source plane; no texture scale.
```

- [ ] **Step 5: Run to verify it passes**

Same command. Expected: `ALL PASS`.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/src/part_surface.cpp MatterEngine3/src/part_surface.h MatterEngine3/tests/part_surface_provider_tests.cpp
git commit -m "part_surface: refuse placements whose source index is out of range; document bind() aliasing"
```

---

### Task 13: Make rollback build tracks the parallax GLSL includes

**Files:**
- Modify: `MatterEngine3/Makefile` (next to line 534 or in the "Primary RT consumes visible VT input tags" group near line 879)

- [ ] **Step 1: Add the edges**

After the existing line
```make
build/shaders_vk/vt_material_domain_probe.comp.spv: shaders_vk/vt_material_domain.glsl shaders_vk/vt_common.glsl shaders_vk/vt_visible_input.glsl
```
add:
```make
# Connected-surface POM march and its seed/surface walks are included by the
# G-buffer fragment stage, the material-domain probe and the RT surface hit.
build/shaders_vk/gbuffer.frag.spv build/shaders_vk/vt_material_domain_probe.comp.spv build/shaders_vk/rt_surface.rchit.spv: \
    shaders_vk/vt_parallax.glsl shaders_vk/vt_surface_walk.glsl shaders_vk/vt_seed_walk.glsl
```

- [ ] **Step 2: Verify the edge with a dry run**

```bash
touch MatterEngine3/shaders_vk/vt_parallax.glsl
make -C MatterEngine3 -n build/shaders_vk/gbuffer.frag.spv 2>/dev/null | grep -c 'gbuffer.frag'
```
Expected: `1` or more (the recipe is scheduled). Before the change the same command prints `0` when the `.spv` is newer than every listed prerequisite. If `make -n` fails because `GLSLC` is unset on Linux, prefix `GLSLC=true`.

- [ ] **Step 3: Confirm the CMake shader census still matches**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_embedded_spirv
```
Expected: exit 0 (the CMake glob-vs-Makefile `VK_SPV` check is unaffected because no stage was added).

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/Makefile
git commit -m "make: track vt_parallax/vt_surface_walk/vt_seed_walk includes for the shaders that use them"
```

---

### Task 14: World-script test regression, castle profile tool, leftover scene

**Files:**
- Modify: `projects/world_demo/tests/mountain_terrain_only_tests.mjs`
- Modify: `MatterEngine3/tools/castle_part_profile.py:63`
- Delete: `projects/world_demo/scenes/geometry/DenseTerrainDiagnostic/` (untracked)
- Modify: `CLAUDE.md` ("JS world-script tests" section)

- [ ] **Step 1: Decide the terrain-only contract and rewrite the test**

The geometry-site work intentionally places `mountainGeometrySamples` inside terrain-only sectors when `__geometryRocks` is set. The invariant that still holds is: without `__geometryRocks`, a terrain-only sector requests nothing and places nothing. Replace lines 12-22 of the test with:
```js
const placed = [];
class Part {
  terrainVolumeTiled(...args) { this.terrain = args; }
  placeChild(...args) { placed.push(args); }
  heightAt() { return 0; }
}
const Sector = new Function('Part', 'MAT', 'MOUNTAIN_FOREST_MIN_LOD',
  'mountainRockCatalog', 'mountainForestCatalog', 'mountainGeometrySamples', 'mountainGeometryCatalog',
  source + '\nreturn WorldSector;')(Part, {grass:1,dirt:2,rock:3,snow:4}, 3,
    unexpected, unexpected,
    (material) => [{x: 4, z: 4, material}],
    (material) => [{name: 'GeometryRock', material}]);
const terrainOnly = JSON.stringify({__terrainOnly:true, __terrain:{material:'dirt'}});
assert.deepEqual(Sector.requires({biomes: terrainOnly}), []);
for (const lod of [0, 2, 5]) {
  const sector = new Sector();
  sector.build({tx:-2,ty:1,tz:3,terrainLod:lod,biomes: terrainOnly});
  assert.deepEqual(sector.terrain, [-2,1,3,lod-5,[2,2,2,2]]);
}
assert.equal(placed.length, 0, 'terrain-only without geometry rocks places nothing');
const withRocks = JSON.stringify({__terrainOnly:true, __terrain:{material:'dirt'}, __geometryRocks:{material:'rock'}});
assert.deepEqual(Sector.requires({biomes: withRocks}), [{name: 'GeometryRock', material: 'rock'}]);
new Sector().build({tx:0,ty:0,tz:0,terrainLod:0,biomes: withRocks});
assert.ok(placed.length >= 1, 'terrain-only with geometry rocks places the geometry site samples');
console.log('Terrain-only sectors: terrain retained at each rung; catalogs and placement only with __geometryRocks');
```
Keep the rest of the file (the StreamMountain world section) unchanged.

- [ ] **Step 2: Run it**

```bash
node projects/world_demo/tests/mountain_terrain_only_tests.mjs; echo rc=$?
```
Expected: two log lines and `rc=0`.

- [ ] **Step 3: Fix the castle profile source glob and remove the leftover scene**

`castle_part_profile.py:63`:
```python
    sources += [str(p.relative_to(repo)) for p in (repo/'projects/world_demo/objects').rglob('Castle*.js')]
```
Verify: `python3 -c "from pathlib import Path;print(len(list(Path('projects/world_demo/objects').rglob('Castle*.js'))))"` prints `49`.
```bash
rm -r projects/world_demo/scenes/geometry/DenseTerrainDiagnostic
git status --short projects/world_demo | wc -l   # expected: 0
```

- [ ] **Step 4: Document the vm-modules flag**

In `CLAUDE.md`, "JS world-script tests", after the `node projects/world_demo/tests/alpine_ecology_tests.mjs` block add:
```
Suites that go through `projects/world_demo/tests/helpers/castle_dsl_harness.mjs`
(`conifer_tests.mjs`, `castle_upgraded_scene_tests.mjs`) need
`node --experimental-vm-modules <file>`; without it they die with
"vm.SourceTextModule is not a constructor".
```

- [ ] **Step 5: Run the full JS sweep**

```bash
for f in projects/world_demo/tests/*.mjs; do node --experimental-vm-modules "$f" >/dev/null 2>&1; rc=$?; [ $rc -ne 0 ] && echo "FAIL $f"; done; echo sweep-done
```
Expected: only the three pre-existing castle failures (`castle_paving_tests`, `castle_surface_paving_tests`, `castle_upgraded_scene_tests`) print `FAIL`.

- [ ] **Step 6: Commit**

```bash
git add projects/world_demo/tests/mountain_terrain_only_tests.mjs MatterEngine3/tools/castle_part_profile.py CLAUDE.md
git commit -m "world_demo: terrain-only test follows the geometry-site contract; castle profile finds nested objects"
```

---

## Phase B — Measurement

### Task 15: Name the five per-frame world traversals

**Files:**
- Modify: `MatterEngine3/src/provider/resolvers.cpp:91, 118`
- Modify: `MatterEngine3/src/lod_select.cpp:82`
- Modify: `MatterEngine3/src/render/vk_instance_cache.cpp:70`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp:7394, 16226, 17140`

**Interfaces:**
- Produces: ProfileLib zones `resolve.sector_lod`, `resolve.emit`, `instance_cache.match`, `cull.vt_demand` (existing), `rt.rung_select`, `rt.tlas_hash`, and counters `instances.sector_lod_scanned`, `instances.rt_scanned`, `instances.vt_scanned`.

- [ ] **Step 1: Add scopes and counters**

At each site, the first statement of the loop or function body:
```cpp
    PROFILE_SCOPE("resolve.sector_lod");            // lod_select.cpp, top of the function containing line 82
    PROFILE_SCOPE("resolve.emit");                  // resolvers.cpp, top of the emit function containing line 118
    PROFILE_SCOPE("instance_cache.match");          // vk_instance_cache.cpp, top of the compare at line 70
    PROFILE_SCOPE("rt.rung_select");                // vk_scene_renderer.cpp, just before the loop at 16226
    PROFILE_SCOPE("rt.tlas_hash");                  // vk_scene_renderer.cpp, just before `uint64_t key = 14695981039346656037ull;`
```
and one counter per traversal after its loop, e.g. in `lod_select.cpp`:
```cpp
    PROFILE_COUNT("instances.sector_lod_scanned", static_cast<int64_t>(scanned));
```
where `scanned` is a local `size_t` incremented once per inner-loop instance. Do the same with `instances.rt_scanned` after the `rt_instances_` loop and `instances.vt_scanned` after the `instance_staging_` loop in `update_vt_demand`. Include `profile.h` where absent (the renderer and resolvers already include it if `PROFILE_SCOPE` appears anywhere in the file; check with grep).

- [ ] **Step 2: Verify the zones appear in a trace**

```bash
cd MatterEditor
WSLENV=MATTER_WORLD:MATTER_SCREENSHOT:MATTER_SCREENSHOT_SETTLE:MATTER_PROFILE_TRACE \
MATTER_WORLD=demo MATTER_SCREENSHOT="C:/tmp/zone_probe.png" MATTER_SCREENSHOT_SETTLE=60 \
MATTER_PROFILE_TRACE="C:/tmp/zone_probe.json" ./build/windows-msvc/editor.exe > /tmp/zone_probe.log 2>&1
grep -o '"name":"\(resolve\.sector_lod\|resolve\.emit\|instance_cache\.match\|rt\.rung_select\|rt\.tlas_hash\)"' /mnt/c/tmp/zone_probe.json | sort | uniq -c
```
Expected: all five names present with non-zero counts. (`MATTER_SCREENSHOT` makes the editor capture-then-quit, so no window is left open.)

- [ ] **Step 3: Commit**

```bash
git add MatterEngine3/src/provider/resolvers.cpp MatterEngine3/src/lod_select.cpp MatterEngine3/src/render/vk_instance_cache.cpp MatterEngine3/src/render/vk_scene_renderer.cpp
git commit -m "profile: name the five per-frame instance traversals and count instances scanned"
```

---

### Task 16: p99 and max for every GPU zone, plus a frame-attribution report tool

**Files:**
- Modify: `MatterEditor/src/perf_gpu_stats.h`
- Create: `tools/frame_attribution.py`
- Create: `tools/tests/test_frame_attribution.py`
- Modify: `docs/agent/control-surface.md` (the `MATTER_PERF_*` entry near line 547)

**Interfaces:**
- Produces: `gpu_pass_statistics.passes.<zone>` JSON gains `p99_ms` and `max_ms`. `tools/frame_attribution.py <perf.json>... --out table.md` writes a markdown table with one row per zone and one column per input file (median / p95 / p99), sorted by the first file's p95 descending, plus a `frame_interval` row from `perf_frame_times`.

- [ ] **Step 1: Write the failing Python test**

`tools/tests/test_frame_attribution.py`:
```python
import importlib.util, json, pathlib, tempfile, unittest
HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("frame_attribution", HERE.parent / "frame_attribution.py")
fa = importlib.util.module_from_spec(spec); spec.loader.exec_module(fa)

def fixture(gbuffer, total):
    return {"gpu_pass_statistics": {"passes": {
        "gbuffer": {"samples": 3, "median_ms": gbuffer, "p95_ms": gbuffer * 1.5, "p99_ms": gbuffer * 2, "max_ms": gbuffer * 3},
        "total": {"samples": 3, "median_ms": total, "p95_ms": total * 1.5, "p99_ms": total * 2, "max_ms": total * 3},
        "vt": {"samples": 0, "median_ms": None, "p95_ms": None, "p99_ms": None, "max_ms": None}}},
        "frame_times_ms": [10, 12, 30]}

class FrameAttributionTests(unittest.TestCase):
    def test_table_sorted_by_first_file_p95_and_nulls_render_as_dash(self):
        with tempfile.TemporaryDirectory() as d:
            a = pathlib.Path(d) / "a.json"; b = pathlib.Path(d) / "b.json"
            a.write_text(json.dumps(fixture(20, 60))); b.write_text(json.dumps(fixture(5, 40)))
            table = fa.render([a, b])
            rows = [line for line in table.splitlines() if line.startswith("| ")]
            self.assertIn("total", rows[2]); self.assertIn("gbuffer", rows[3]); self.assertIn("vt", rows[-1])
            self.assertIn("—", rows[-1])
            self.assertTrue(rows[1].startswith("| frame_interval"))
    def test_missing_gpu_block_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "x.json"; p.write_text("{}")
            with self.assertRaises(fa.AttributionError): fa.render([p])
if __name__ == "__main__": unittest.main()
```

- [ ] **Step 2: Run to verify it fails**

```bash
python3 -m pytest tools/tests/test_frame_attribution.py -q
```
Expected: failure, `frame_attribution.py` not found.

- [ ] **Step 3: Write the tool**

`tools/frame_attribution.py`:
```python
#!/usr/bin/env python3
"""Tabulate GPU zone statistics from one or more MATTER_PERF_OUTPUT files.

Reads `gpu_pass_statistics.passes` (median/p95/p99/max per zone) and the raw
`frame_times_ms` array, and writes a markdown table so an A/B of render paths
(for example MATTER_GBUFFER_POM_PATH variants) is one command. It reports what
the perf run measured; it establishes no target and infers nothing.
"""
import argparse, json, pathlib, statistics

class AttributionError(RuntimeError): pass

def _load(path):
    data = json.loads(pathlib.Path(path).read_text())
    passes = data.get("gpu_pass_statistics", {}).get("passes")
    if not isinstance(passes, dict): raise AttributionError(f"{path}: no gpu_pass_statistics.passes block")
    frames = data.get("frame_times_ms") or []
    return passes, frames

def _fmt(v): return "—" if v is None else f"{v:.2f}"

def _pct(values, q):
    if not values: return None
    s = sorted(values); k = max(0, min(len(s) - 1, int(round(q * len(s) + 0.5)) - 1)); return s[k]

def render(paths):
    loaded = [_load(p) for p in paths]
    names = [pathlib.Path(p).stem for p in paths]
    zones = sorted(loaded[0][0].keys(), key=lambda z: -(loaded[0][0][z].get("p95_ms") or -1))
    header = "| zone | " + " | ".join(f"{n} median / p95 / p99" for n in names) + " |"
    lines = [header, "|" + "---|" * (len(names) + 1)]
    cells = []
    for passes, frames in loaded:
        med = statistics.median(frames) if frames else None
        cells.append(f"{_fmt(med)} / {_fmt(_pct(frames, .95))} / {_fmt(_pct(frames, .99))}")
    lines.append("| frame_interval | " + " | ".join(cells) + " |")
    for z in zones:
        cells = []
        for passes, _ in loaded:
            s = passes.get(z, {})
            cells.append(f"{_fmt(s.get('median_ms'))} / {_fmt(s.get('p95_ms'))} / {_fmt(s.get('p99_ms'))}")
        lines.append(f"| {z} | " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("perf_json", nargs="+"); ap.add_argument("--out")
    a = ap.parse_args()
    table = render(a.perf_json)
    if a.out: pathlib.Path(a.out).write_text(table)
    else: print(table, end="")

if __name__ == "__main__": main()
```
Note the table row order in the test: `frame_interval` is row index 1 (after the separator), and zones follow sorted by p95 descending; adjust the test indices if the separator line is counted differently.

- [ ] **Step 4: Extend `PerfGpuStats::append_json`**

After the `p95` computation add:
```cpp
            const size_t p99 = static_cast<size_t>(std::ceil(n * 0.99)) - 1;
            out << median << ",\"p95_ms\":" << sorted[p95] << ",\"p99_ms\":" << sorted[p99]
                << ",\"max_ms\":" << sorted.back() << '}';
```
and change the empty case to emit `"p95_ms":null,"p99_ms":null,"max_ms":null`. Update the header string's method note to `"p99_method":"nearest_rank"`. Confirm `write_perf_result` already writes `frame_times_ms` (it receives `perf_frame_times`); if the key is named differently, use that name in the tool and test.

- [ ] **Step 5: Run the tests and rebuild**

```bash
python3 -m pytest tools/tests/test_frame_attribution.py -q
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
```
Expected: `2 passed`; build exit 0.

- [ ] **Step 6: Register the Python test in CTest and document**

In `cmake/MatterPackaging.cmake` next to the existing `test_windows_package_stage.py` registration (line ~119), add an identical block for `tools/tests/test_frame_attribution.py` named `frame_attribution_tests`. In `control-surface.md`, under the `MATTER_PERF_*` entry, add one sentence: "`gpu_pass_statistics` carries median/p95/p99/max per GPU zone; `tools/frame_attribution.py a.json b.json` tabulates an A/B."

- [ ] **Step 7: Commit**

```bash
git add MatterEditor/src/perf_gpu_stats.h tools/frame_attribution.py tools/tests/test_frame_attribution.py cmake/MatterPackaging.cmake docs/agent/control-surface.md
git commit -m "perf: p99/max per GPU zone and a frame-attribution table tool"
```

---

### Task 17: The StreamMountain attribution session

**Files:**
- Create: `tools/streammountain_attribution.sh`
- Create: `docs/findings/streammountain-frame-attribution-2026-09-27.md`

**Interfaces:**
- Consumes: `MATTER_PERF_OUTPUT`, `MATTER_PERF_WARMUP_SECONDS`, `MATTER_PERF_SAMPLE_SECONDS`, `MATTER_GBUFFER_POM_PATH`, `MATTER_PROFILE_TRACE`, `tools/frame_attribution.py`, `tools/terrain_cache_audit.py`.

- [ ] **Step 1: Write the capture script**

`tools/streammountain_attribution.sh`:
```bash
#!/usr/bin/env bash
# Four matched StreamMountain perf captures: POM reference, chart_only, work,
# and POM disabled. Same warmup, same sample window, same camera (the world's
# default), sequential on one GPU. Output: <out>/<variant>.json + trace, and
# <out>/attribution.md from tools/frame_attribution.py.
set -euo pipefail
OUT=${1:?usage: streammountain_attribution.sh <out-dir-windows-path e.g. C:/tmp/attr>}
WARM=${WARMUP:-45}; SAMPLE=${SAMPLE:-20}
cd "$(dirname "$0")/../MatterEditor"
run() { # name, extra env
  local name=$1; shift
  echo "== $name"
  env WSLENV=MATTER_WORLD:MATTER_PERF_OUTPUT:MATTER_PERF_WARMUP_SECONDS:MATTER_PERF_SAMPLE_SECONDS:MATTER_PROFILE_TRACE:MATTER_GBUFFER_POM_PATH:MATTER_RENDER_POM \
      MATTER_WORLD=StreamMountain MATTER_PERF_OUTPUT="$OUT/$name.json" \
      MATTER_PERF_WARMUP_SECONDS=$WARM MATTER_PERF_SAMPLE_SECONDS=$SAMPLE \
      MATTER_PROFILE_TRACE="$OUT/$name.trace.json" "$@" \
      ./build/windows-msvc/editor.exe > "/mnt/c/${OUT#C:/}/$name.log" 2>&1
}
mkdir -p "/mnt/c/${OUT#C:/}"
run pom_reference  MATTER_GBUFFER_POM_PATH=reference
run pom_chart_only MATTER_GBUFFER_POM_PATH=chart_only
run pom_work       MATTER_GBUFFER_POM_PATH=work
run pom_off        MATTER_RENDER_POM=0
python3 ../tools/frame_attribution.py "/mnt/c/${OUT#C:/}"/pom_{reference,chart_only,work,off}.json --out "/mnt/c/${OUT#C:/}/attribution.md"
echo "wrote /mnt/c/${OUT#C:/}/attribution.md"
```
If POM cannot be disabled by environment, replace the `pom_off` run with a `props.json` toggle of `render.pom.enabled` per the blas-cache plan and restore it afterwards; record which mechanism was used.

- [ ] **Step 2: Run the session**

```bash
chmod +x tools/streammountain_attribution.sh
tools/streammountain_attribution.sh C:/tmp/attr
```
Expected: four `perf: wrote N frames to ...` lines in the logs, `attribution.md` written. Each run self-terminates (`quit_requested` after sampling).

- [ ] **Step 3: Capture a true cold load**

Delete or rename the project's geometry-page and prepared-sector caches (`projects/world_demo/.cache/StreamMountain/geometry-pages`, `.../prepared-sectors`, `.../blas`), then:
```bash
py -3 tools/terrain_cache_audit.py --world StreamMountain --out C:/tmp/attr/cold 2>&1 | tail -20
```
Record the prepare / reopen / load stage table it prints. Then run it a second time without deleting caches for the warm figure.

- [ ] **Step 4: Write the findings doc**

`docs/findings/streammountain-frame-attribution-2026-09-27.md` with these sections, filled from the outputs (numbers, not adjectives):
1. Setup: GPU, driver, resolution, warmup/sample seconds, camera, git SHA.
2. GPU zone table (paste `attribution.md`).
3. CPU zone table: from each trace, the per-frame mean and p95 of `resolve.sector_lod`, `resolve.emit`, `instance_cache.match`, `cull.vt_demand`, `rt.rung_select`, `rt.tlas_hash`, `geometry.scene_completion`, `geometry.scene_assembly`, and the `instances.*_scanned` counters (a 20-line Python snippet over the Chrome trace `X` events, included in the doc).
4. Load: cold vs warm stage tables.
5. What changed relative to the 2026-09-19 analysis: which of its inferences (POM 10-100 ms, geometry runtime 24-26 ms, five traversals ~8-15 ms) the measurements confirm, refute, or bound.
6. Re-ranked Tier 1 order with one sentence of evidence per item.

- [ ] **Step 5: Commit**

```bash
git add tools/streammountain_attribution.sh docs/findings/streammountain-frame-attribution-2026-09-27.md
git commit -m "findings: matched StreamMountain frame attribution (POM variants, CPU traversals, cold vs warm load)"
```

---

## Phase C — Structural fixes that need no measurement

### Task 18: Persistent geometry cook writer with batched index commits

**Files:**
- Modify: `MatterEngine3/src/geometry/geometry_asset.h` (`RootCache` API)
- Modify: `MatterEngine3/src/geometry/geometry_asset.cpp` (`RootCache::Impl`, `cache_asset`)
- Modify: `MatterEngine3/src/geometry/geometry_hierarchy.h:67` and `geometry_pages.cpp:143-200` (`write_hierarchy` gains `bool commit`)
- Modify: `libs/AssetStoreLib/include/asset_pages.h:126` and `src/asset_pages.cpp:344-362` (`publish_page_manifest` gains `bool commit`)
- Modify: `MatterEngine3/src/render/part_store.cpp` (flush at the end of each bake batch)
- Test: `MatterEngine3/tests/geometry_hierarchy_tests.cpp`

**Interfaces:**
- Produces: `bool RootCache::flush_writer(std::string& error)`; `RootCache::WriterStats RootCache::writer_stats() const` with `uint64_t assets_written, commits, pending_assets`; `publish_page_manifest(..., bool commit = true)`; `write_hierarchy(..., bool commit = true)`. When `commit` is false the manifest blob and ref are put but `flush_index()`/`refs.flush()` are not called; the caller commits later. `cache_asset(...)` with a `RootCache*` writes through the cache's persistent writer and returns a `CachedAsset` built from the in-memory hierarchy, so no reload of a not-yet-committed index is needed.

- [ ] **Step 1: Write the failing test**

Add to `geometry_hierarchy_tests.cpp` (call from `main()` after the existing cache tests, using the same `hierarchy` fixture and a fresh temp directory):
```cpp
static void batched_writer_checks(const geometry::Hierarchy& hierarchy, const geometry::MeshIndexed& source) {
    std::string error;
    const auto dir = (std::filesystem::temp_directory_path() / "me3_geometry_batched_writer").string();
    std::filesystem::remove_all(dir);
    geometry::RootCache roots(dir);
    geometry::CompileConfig config; geometry::CacheReport report;
    for (int i = 0; i < 5; ++i) {
        const auto asset = geometry::cache_asset(dir, "asset-" + std::to_string(i), source, config, {}, error, &roots, &report);
        CHECK(asset && !asset->roots.empty(), "batched writer returns a usable asset before commit");
    }
    const auto stats = roots.writer_stats();
    CHECK(stats.assets_written == 5 && stats.commits <= 1, "five assets cost at most one index commit");
    CHECK(roots.flush_writer(error), error.c_str());
    CHECK(roots.writer_stats().pending_assets == 0, "flush drains pending assets");
    geometry::RootCache reader(dir);
    CHECK(reader.load("asset-4", error) != nullptr, "a fresh reader sees every committed asset");
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo geometry_hierarchy_tests
```
Expected: compile error, no `writer_stats`.

- [ ] **Step 3: Thread `commit` through the store layer**

`asset_pages.h:126`: `bool publish_page_manifest(BlobStore&, RefTable&, const std::string& key, const std::vector<uint8_t>& manifest, const PageLimits&, BlobHash& out, std::string& error, bool commit = true);`
`asset_pages.cpp:344-362`: replace the two flush calls:
```cpp
    if (store.put(manifest.data(), manifest.size(), &hash) != Status::Ok || (commit && !store.flush_index())) {
        error = store.last_error(); return false;
    }
    ...
    if (!refs.put(key, hash, view.kind, manifest.size()) || (commit && !refs.flush())) {
```
`geometry_hierarchy.h:67` / `geometry_pages.cpp:143`: add `bool commit = true` as the last parameter and pass it to `publish_page_manifest`.

- [ ] **Step 4: Give `RootCache` a persistent writer**

`geometry_asset.h`, inside `class RootCache`:
```cpp
    struct WriterStats { uint64_t assets_written = 0, commits = 0, pending_assets = 0; };
    // Commits every asset written through cache_asset(..., this) since the
    // last commit. Called by the owner at bake-batch boundaries and by the
    // destructor. Cheap when nothing is pending.
    bool flush_writer(std::string& error);
    WriterStats writer_stats() const;
```
`geometry_asset.cpp`, extend `Impl`:
```cpp
struct RootCache::Impl {
    asset_store::PageCacheConfig config;
    std::unique_ptr<asset_store::PageCache> cache;
    mutable std::mutex mutex;
    // Writer session: opened on first write, kept for the cache lifetime so
    // the index is parsed once, not once per asset. Commits are batched.
    std::mutex writer_mutex;
    std::unique_ptr<asset_store::BlobStore> writer;
    std::unique_ptr<asset_store::RefTable> writer_refs;
    WriterStats writer_stats;
    std::chrono::steady_clock::time_point last_commit{};
    static constexpr uint32_t kCommitEveryAssets = 32;
    static constexpr std::chrono::seconds kCommitEvery{2};
    bool open_writer(std::string& error) {
        if (writer) return true;
        asset_store::StoreConfig store_config; store_config.dir = config.store.dir;
        writer = asset_store::BlobStore::open(store_config, &error);
        if (!writer) return false;
        writer_refs = asset_store::RefTable::open(*writer, {}, &error);
        if (!writer_refs) { writer.reset(); return false; }
        last_commit = std::chrono::steady_clock::now(); return true;
    }
    bool commit(std::string& error) {
        if (!writer || !writer_stats.pending_assets) return true;
        if (!writer->flush_index() || !writer_refs->flush()) { error = writer->last_error(); return false; }
        writer_stats.pending_assets = 0; ++writer_stats.commits; last_commit = std::chrono::steady_clock::now();
        return true;
    }
};
RootCache::~RootCache() { std::string e; std::lock_guard<std::mutex> lock(d_->writer_mutex); d_->commit(e); }
bool RootCache::flush_writer(std::string& error) { std::lock_guard<std::mutex> lock(d_->writer_mutex); return d_->commit(error); }
RootCache::WriterStats RootCache::writer_stats() const { std::lock_guard<std::mutex> lock(d_->writer_mutex); return d_->writer_stats; }
```
Add a private `write_asset` used by `cache_asset`:
```cpp
std::shared_ptr<const CachedAsset> RootCache::write_asset(const std::string& key, const Hierarchy& hierarchy,
        const std::vector<asset_store::PageSection>& metadata, std::string& error, double* write_ms) {
    std::lock_guard<std::mutex> lock(d_->writer_mutex);
    const auto start = std::chrono::steady_clock::now();
    if (!d_->open_writer(error)) return {};
    if (auto existing = load(key, error)) return existing;   // another worker won the race
    std::vector<NodeRef> roots, all_refs; // all_refs[i] is node i's encoded NodeRef, child-first
    if (!write_hierarchy(hierarchy, *d_->writer, *d_->writer_refs, key, {}, roots, error, metadata, /*commit=*/false, &all_refs)) return {};
    ++d_->writer_stats.assets_written; ++d_->writer_stats.pending_assets;
    const bool due = d_->writer_stats.pending_assets >= Impl::kCommitEveryAssets ||
                     std::chrono::steady_clock::now() - d_->last_commit >= Impl::kCommitEvery;
    if (due && !d_->commit(error)) return {};
    // Build the returned asset from memory: the writer's index may not be
    // committed yet, so a reader-side load could miss it.
    auto result = std::make_shared<CachedAsset>(); result->key = key; result->directory = directory();
    for (size_t i = 0; i < hierarchy.roots.size(); ++i) {
        std::vector<NodeRef> children; for (auto c : hierarchy.nodes[hierarchy.roots[i]].children) children.push_back(all_refs[c]);
        std::vector<uint8_t> bytes;
        if (!encode_node(hierarchy.nodes[hierarchy.roots[i]], children, {}, bytes, error)) return {};
        auto storage = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
        auto page = std::make_shared<asset_store::CachedPage>();
        page->hash = roots[i].page; page->bytes = storage->data(); page->size = storage->size(); page->allocation = storage;
        if (!asset_store::decode_page(page->bytes, page->size, {}, page->view, error)) return {};
        NodeView root; if (!decode_node(page, root, error)) return {};
        result->roots.push_back(std::move(root));
    }
    if (write_ms) *write_ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}
```
`write_hierarchy` already computes every node's `NodeRef` (its local `encoded` vector, indexed by hierarchy node id). Add a final defaulted parameter `std::vector<NodeRef>* all_refs = nullptr` to `write_hierarchy` (declaration in `geometry_hierarchy.h:67`, definition in `geometry_pages.cpp:143`) and assign `if (all_refs) *all_refs = encoded;` just before `roots = std::move(root_refs);`, so `write_asset` indexes it directly instead of recomputing hashes. `CachedAsset::manifest` stays empty on this path; audit its consumers (`grep -n 'manifest' MatterEngine3/src/render/*.cpp`) and make each tolerate an empty manifest by falling back to `roots` (they only read root refs today).

- [ ] **Step 5: Route `cache_asset` through the writer and flush at batch boundaries**

In `cache_asset`, replace everything from `static std::mutex writer_mutex;` to the closing of that block with:
```cpp
    if (!roots) { error = "geometry cache write requires a RootCache"; return {}; }
    double hierarchy_write_ms = 0;
    auto result = roots->write_asset(key, hierarchy, metadata, error, &hierarchy_write_ms);
    report->write_ms = elapsed(start);
    if (std::getenv("MATTER_GEOMETRY_PAGES_PROFILE"))
        MATTER_LOGI("geometry", "cache_write_profile key=%s hierarchy_write_ms=%.3f total_ms=%.3f pending=%llu",
                    key.c_str(), hierarchy_write_ms, report->write_ms,
                    static_cast<unsigned long long>(roots->writer_stats().pending_assets));
    return result;
```
Delete `load_asset` (dead: no production caller) and the temporary `RootCache` in `cache_asset`'s `load` lambda. In `part_store.cpp`, where the bake worker finishes a sector batch (the function that calls `stage_from_snapshot` for streamed sectors returns to the coordinator), call `geometry_roots_.flush_writer(error)` and log a `MATTER_LOGW` on failure. Also call it from `PartStore::~PartStore` (the `RootCache` destructor already commits, so this is belt-and-braces).

- [ ] **Step 6: Run the tests**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo geometry_hierarchy_tests matter_asset_store_tests matter_editor
(cd MatterEngine3/tests && ../../MatterEditor/build/cmake/windows-msvc/relwithdebinfo/geometry_hierarchy_tests.exe | grep -E 'FAIL|ALL PASS')
./MatterEditor/build/cmake/windows-msvc/relwithdebinfo/matter_asset_store_tests.exe | tail -1
```
Expected: both `ALL PASS`.

- [ ] **Step 7: Measure the cook A/B**

Delete `projects/world_demo/.cache/StreamMountain/geometry-pages`, run `py -3 tools/terrain_cache_audit.py --world StreamMountain --out C:/tmp/attr/cold_after` and compare the prepare stage against Task 17 Step 3. Append both numbers to the findings doc from Task 17 under a `## Cook after batched commits` heading.

- [ ] **Step 8: Commit**

```bash
git add libs/AssetStoreLib/include/asset_pages.h libs/AssetStoreLib/src/asset_pages.cpp MatterEngine3/src/geometry/geometry_asset.h MatterEngine3/src/geometry/geometry_asset.cpp MatterEngine3/src/geometry/geometry_hierarchy.h MatterEngine3/src/geometry/geometry_pages.cpp MatterEngine3/src/render/part_store.cpp MatterEngine3/tests/geometry_hierarchy_tests.cpp docs/findings/streammountain-frame-attribution-2026-09-27.md
git commit -m "geometry: persistent cook writer with batched index commits; assets return from memory before commit"
```

---

### Task 19: Restore the cached-node early-out on the demand bake path

**Files:**
- Modify: `MatterEngine3/src/provider/local_provider.cpp:3884-3925`
- Test: `MatterEngine3/tests/provider_bake_tests.cpp` if it exists, otherwise `MatterEngine3/tests/eval_world_tests.cpp` (add one test)

**Interfaces:**
- Consumes: `host_baker_->cached(hash)`, `bake_static_lods(...)`, `bake_lod_variants(...)`.
- Produces: `LocalProvider` gains a private `std::unordered_set<uint64_t> lod_plans_installed_` so `bake_static_lods`/`bake_lod_variants` run once per hash per session, not once per visit.

- [ ] **Step 1: Write the failing test**

Locate the existing test that drives `ensure_part_baked` twice for the same hash (search `ensure_part_baked` in `MatterEngine3/tests/*.cpp`). Add after the second call:
```cpp
    const auto evals_before = host.stats().lod_evaluations;   // add this counter if absent: ++ in eval_lods/eval_no_impostor/eval_lod_budgets
    CHECK(provider.ensure_part_baked(hash, error), error.c_str());
    CHECK(host.stats().lod_evaluations == evals_before, "a cached node does not re-evaluate its LOD plan on every visit");
```
If `ScriptHost` has no stats struct, add `struct Stats { uint64_t lod_evaluations = 0; }` and a `const Stats& stats() const` accessor, incrementing the counter at the top of `eval_lods`, `eval_no_impostor` and `eval_lod_budgets` (`script_host.cpp:1094, 1142, 1391`).

- [ ] **Step 2: Run to verify it fails**

Build and run the owning suite. Expected: the new CHECK fails (three evaluations per visit).

- [ ] **Step 3: Skip the plan install for already-installed hashes**

In `local_provider.cpp` after the `cached` block and before `bake_static_lods`:
```cpp
        // Authored LOD plans reach the flattener once per hash per session.
        // A cached node revisited by the publish loop must not rebuild three
        // QuickJS runtimes to re-derive a plan it already installed.
        if (cached && lod_plans_installed_.count(hash)) return true;
        ...
        lod_plans_installed_.insert(hash);
```
placing the `insert` after both `bake_static_lods` and `bake_lod_variants` succeed. Declare the set in `local_provider.h` next to `hit_count_`. Clear it wherever `hit_count_`/`baked_hashes_` are reset (world detach / reload).

- [ ] **Step 4: Run to verify it passes**

Expected: `ALL PASS` in the owning suite; also run `authored_world_provider_cache_tests.exe` if present.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/provider/local_provider.cpp MatterEngine3/src/provider/local_provider.h MatterEngine3/src/script_host.cpp MatterEngine3/src/script_host.h MatterEngine3/tests/
git commit -m "provider: install authored LOD plans once per hash instead of re-evaluating on every cached visit"
```

---

### Task 20: Lazy prepared-sector bank and no eager RootCache temporaries

**Files:**
- Modify: `MatterEngine3/src/render/prepared_sector_cache.h:107-125`
- Modify: `MatterEngine3/src/render/part_store.cpp:362`
- Test: `MatterEngine3/tests/partstore_tests.cpp`

**Interfaces:**
- Produces: `prepared_sector::Cache` allocates its reader banks on first `read`/`write`, not in the constructor; `PartStore` constructs it only when `MATTER_PREPARED_SECTOR_CACHE` is set or `set_geometry_pages_enabled(true)` is called; `bool PartStore::prepared_sector_cache_active() const`.

- [ ] **Step 1: Write the failing test**

```cpp
static void test_partstore_construction_commits_no_banks(const fs::path& root) {
    std::printf("[test_partstore_construction_commits_no_banks]\n");
    viewer::PartStore store((root / "lazy").string());
    CHECK(!store.prepared_sector_cache_active(), "PartStore does not commit a 128 MiB prepared-sector bank at construction");
}
```

- [ ] **Step 2: Run to verify it fails**

Compile error (`prepared_sector_cache_active` absent).

- [ ] **Step 3: Make the bank lazy**

In `prepared_sector_cache.h` move the `for(...) readers_[i].bank = PageBank::create(...)` loop out of the constructor into a private `bool ensure_banks(std::string& error)` called at the top of every public read/write entry point; the constructor keeps only configuration parsing and no longer throws. In `part_store.cpp:362` replace the unconditional `std::make_shared<prepared_sector::Cache>(...)` with a null initialiser, and add:
```cpp
prepared_sector::Cache& PartStore::prepared_sectors() {
    if (!prepared_sectors_) prepared_sectors_ = std::make_shared<prepared_sector::Cache>(cache_root_ + "/prepared-sectors");
    return *prepared_sectors_;
}
bool PartStore::prepared_sector_cache_active() const { return prepared_sectors_ != nullptr; }
```
and route the existing uses (`save_prepared_sector`, the load path) through `prepared_sectors()`.

- [ ] **Step 4: Run to verify it passes**

`partstore_tests.exe` → `ALL PASS`; then the `geometry-pages` smoke mode (Task 22 registers it; for now launch it by env var) → `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/prepared_sector_cache.h MatterEngine3/src/render/part_store.cpp MatterEngine3/src/render/part_store.h MatterEngine3/tests/partstore_tests.cpp
git commit -m "part_store: allocate the prepared-sector bank on first use, not in every constructor"
```

---

### Task 21: RT tileset sampler descriptors are written only when they change

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp:17331-17384`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h` (`FrameResources`)
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp` (`vt-surfaces` mode)

**Interfaces:**
- Produces: `FrameResources::rt_tileset_descriptor_revision` (uint64); the RT set's 432-sampler write runs only when `tileset_descriptor_revision_ != frame.rt_tileset_descriptor_revision`. `FrameStats::descriptors_written` counts descriptor writes per frame.

- [ ] **Step 1: Write the failing check**

In the `vt-surfaces` smoke section, after the scene has settled (two consecutive `draw_pixel()` calls with no input change), add:
```cpp
    {
        draw_pixel(); const auto a = renderer.frame_stats().descriptors_written;
        draw_pixel(); const auto b = renderer.frame_stats().descriptors_written;
        CHECK(b < 64, "rt tileset samplers are not rewritten on a frame with no tileset change");
        (void)a;
    }
```

- [ ] **Step 2: Run to verify it fails**

`vt-surfaces` mode: expected `FAIL` with the count around 456.

- [ ] **Step 3: Add the revision guard**

Mirror the guard the raster path uses at `:6515`: keep a `tileset_descriptor_revision_` member bumped wherever `write_tileset_descriptors_for_frame` is invoked for a slot change, store the revision in `FrameResources` when the RT set is written, and skip the 25 `VkWriteDescriptorSet` entries for bindings 6/7/26 when equal. Increment `frame_stats_.descriptors_written` by `descriptorCount` for every `vkUpdateDescriptorSets` call in the frame path (add the field to `FrameStats` in `world_session.h`).

- [ ] **Step 4: Run to verify it passes**

`vt-surfaces`, `vt-input-snapshot` and `vt-feedback` modes sequentially → `ALL PASS`, zero validation errors. The input-snapshot mode is the one that swaps tileset slots, so it proves the guard still rewrites when it must.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/include/matter/world_session.h MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "renderer: skip the 432-sampler RT descriptor rewrite when the tileset bank is unchanged"
```

---

### Task 22: Register the tests that build but never run

**Files:**
- Modify: `cmake/MatterViewer.cmake` (after line 426)
- Modify: `cmake/MatterPackaging.cmake` (Python test registrations)
- Modify: `.claude/skills/qa-smoke/SKILL.md`

- [ ] **Step 1: Add the ctests**

After the `sparse_voxel_gpu_tests` block:
```cmake
    add_test(NAME static_surface_vt_tests COMMAND static_surface_vt_tests)
    set_tests_properties(static_surface_vt_tests PROPERTIES LABELS vulkan
        PASS_REGULAR_EXPRESSION "ALL PASS" FAIL_REGULAR_EXPRESSION "validation errors: [1-9][0-9]*"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME solid_face_projection_gpu_tests COMMAND solid_face_projection_gpu_tests)
    set_tests_properties(solid_face_projection_gpu_tests PROPERTIES LABELS vulkan
        PASS_REGULAR_EXPRESSION "ALL PASS" FAIL_REGULAR_EXPRESSION "validation errors: [1-9][0-9]*"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    foreach(mode geometry-pages vt-queue vt-material-domain vt-pom-work vt-receiver-material vt-module-residency vt-export vt-surface-connections vt-feedback-pair rt-empty-tlas water-field-nort)
        string(REPLACE "-" "_" mode_name "${mode}")
        add_test(NAME smoke_${mode_name} COMMAND vulkan_smoke_tests)
        set_tests_properties(smoke_${mode_name} PROPERTIES LABELS vulkan
            ENVIRONMENT "MATTER_VK_SMOKE_MODE=${mode}"
            PASS_REGULAR_EXPRESSION "ALL PASS" FAIL_REGULAR_EXPRESSION "validation errors: [1-9][0-9]*"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    endforeach()
```
Also set `ENVIRONMENT "MATTER_VT_CLAY_FIXTURE=1;MATTER_VT_PERIODIC_CLAY_FIXTURE=1"` on `vt_compositor_tests` if the two gated blocks only need a flag; if they need a fixture file path, add the path the test expects and check the fixture into `MatterEngine3/tests/fixtures/` (only if under 1 MB; otherwise leave the gate and note it in the queue).

- [ ] **Step 2: Register the three Python suites**

Next to the existing `test_windows_package_stage.py` block in `cmake/MatterPackaging.cmake` add one `add_test` each for `tools/tests/test_vt_density.py`, `test_vt_acceptance.py`, `test_geometry_paging_report.py`, using the same Python interpreter variable that block uses.

- [ ] **Step 3: Run every newly registered test once, sequentially**

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo static_surface_vt_tests solid_face_projection_gpu_tests vulkan_smoke_tests
cd MatterEditor/build/cmake/windows-msvc/relwithdebinfo
./static_surface_vt_tests.exe | tail -1; ./solid_face_projection_gpu_tests.exe | tail -1
for m in geometry-pages vt-queue vt-material-domain vt-pom-work vt-receiver-material vt-module-residency vt-export vt-surface-connections vt-feedback-pair rt-empty-tlas water-field-nort; do WSLENV=MATTER_VK_SMOKE_MODE MATTER_VK_SMOKE_MODE=$m ./vulkan_smoke_tests.exe > /tmp/smoke_$m.log 2>&1; echo "$m rc=$? val=$(grep -c 'validation errors: [1-9]' /tmp/smoke_$m.log)"; done
```
Expected: every line `rc=0 val=0`. A mode that fails here is a real finding: file it in the work queue with the log excerpt and keep the registration (a red test is better than an unrun one) unless it blocks CI, in which case mark it `DISABLED` with a comment naming the queue item.

- [ ] **Step 4: Update the qa-smoke skill**

In `.claude/skills/qa-smoke/SKILL.md`: replace "12 fault/RT/VT modes" with "the 12-mode PowerShell gate plus the per-mode ctests in `cmake/MatterViewer.cmake` (`ctest -L vulkan`)", and replace the MinGW build/run lines with `./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests` and the `build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe` path.

- [ ] **Step 5: Commit**

```bash
git add cmake/MatterViewer.cmake cmake/MatterPackaging.cmake .claude/skills/qa-smoke/SKILL.md
git commit -m "ctest: run the GPU suites and smoke modes that were built but never executed"
```

---

### Task 23: Re-rank Tier 1 from the measurements

**Files:**
- Modify: `docs/vg-vt-work-queue-2026-09-19.md` (Tier 1 table)

- [ ] **Step 1: Rewrite the Tier 1 table**

Using section 6 of the Task 17 findings doc, reorder rows 1.3 through 1.9, replace each "Impact" cell's estimate with the measured number (or "not confirmed: <reason>"), mark 1.1 and 1.2 done with their commit SHAs, and add a `Measured` column citing the findings doc section. Keep 1.10/1.11 rows, marking the three landed items (Tasks 19, 20, 21) done.

- [ ] **Step 2: Commit**

```bash
git add docs/vg-vt-work-queue-2026-09-19.md
git commit -m "docs: re-rank VG+VT Tier 1 from the StreamMountain attribution session"
```

---

## Self-review notes

- Spec coverage: Tier 0 items 0.1–0.13 map to Tasks 2–14 (0.8 and 0.9 are Tasks 9 and 10; 0.10–0.12 are Tasks 11–13; 0.13 is Task 14). Tier 1.1 is Tasks 15–17, 1.2 is Task 18, 1.10 is Tasks 19 and 21, 1.11 is Task 20. Tier 3 test gating is Task 22. Tiers 1.3–1.9 and the rest of Tier 2/3 are deliberately out of scope until Task 23 re-ranks them.
- Type consistency: `Diagnostics` (Task 3), `WriterStats`/`flush_writer`/`write_asset` (Task 18), `failed_retries`/`retired_gpu_pages` (Task 10), `test_vt_fills_gated`/`test_fail_next_vt_input_push`/`note_vt_input_push_failure` (Task 2), `descriptors_written` (Task 21) are each defined in the task that introduces them and used by name afterwards.
- Review Focus 1–5 are pinned to Tasks 2, 3, 4, 8, 10 respectively.
- Known plan risks: Task 7's test needs a fixture with zero surface rows, which may require a small helper in `vt_compositor_tests.cpp`; Task 8's JS quad-grid fixture must exceed the `radius >= 32` terrain guard, so keep the 64 m extent; Task 18 changes an AssetStoreLib public signature with a defaulted parameter, which is additive and within the library's own scope (it is not MatterSurfaceLib).
