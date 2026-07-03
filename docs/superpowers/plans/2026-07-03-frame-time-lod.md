# Frame-Time Reduction Package Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get the meadow benchmark world below 16 ms/frame at 1280×720 on the raster path, via a measured CPU-floor fix, an LOD ladder retune, and opt-in procedural triangle-budget LOD for grass.

**Architecture:** Four measured stages (spec: `docs/superpowers/specs/2026-07-03-frame-time-lod-design.md`). Stage 0 adds a repeatable 5-camera FIFO benchmark sweep with a per-frame CPU timing split. Stage 1 removes the ~70 ms fixed CPU floor (-O2 build, resolver sector-binning cache keyed on a new WorldState version counter, cheap batch fingerprint). Stage 2 retunes the QEM ladder (ratio-2 divisor schedule, 32-tri stop rule, runtime pixel-budget dial, never-invisible guarantee) with a flat-format version bump for invalidation. Stage 3 adds schema-opt-in budget LOD: the baker re-runs Grass.js at reduced triangle budgets (prefix-subset blades) and flatten assembles the ladder from those variants instead of QEM.

**Tech Stack:** C++17, raylib/ImGui viewer, QuickJS script host, GNU Make. Repo root for all paths: `MatterEngine3/`.

## Global Constraints

- **Target:** meadow sweep < 16 ms/frame at 1280×720, raster path (no `MATTER_RT`). Work stops early if a stage meets it.
- **MatterSurfaceLib is read-only.** No MSL changes (QEM `max_error` mode already suffices).
- **Run `make windows` after every engine/viewer code change** (from `MatterEngine3/viewer/`). After header/struct changes, clean-rebuild: `make clean` first (or `rm -rf build`). A stale `viewer.exe` silently ships old engine code.
- **Resolver interface and outputs unchanged by caching** — the same results are simply not recomputed when inputs haven't changed. No threading.
- Instant LOD switches accepted (no crossfade/dither).
- **Benchmark gate:** run `MatterEngine3/tools/meadow_sweep.sh <stage-label>` after every stage; commit the appended CSV rows in `MatterEngine3/docs/perf/meadow_sweep.csv`.
- Tests live in `MatterEngine3/tests/`; the tests Makefile does NOT track header deps reliably — `touch` the suite's `.cpp` before rebuilding after a header change.
- Viewer env vars: `MATTER_WORLD=meadow` selects the world, `MATTER_CMD_FIFO=<path>` enables the live-command FIFO (Linux only), no `MATTER_RT` = raster path.
- Matrix convention everywhere: engine float[16] row-major storage of column-vector affine matrices; translation at m[3], m[7], m[11].

---

## Stage 0 — Measurement Pass

### Task 1: CPU timing split + `stats` FIFO command

**Files:**
- Modify: `MatterEngine3/viewer/ui.h` (ViewerStats fields)
- Modify: `MatterEngine3/viewer/ui.cpp:42` (HUD timing line)
- Modify: `MatterEngine3/viewer/main.cpp` (timers around resolve/build/draw; `stats` FIFO command)

**Interfaces:**
- Produces: `ViewerStats::resolve_ms/build_ms/draw_ms` (float, ms); stdout line `STATS,<label>,<frame_ms>,<resolve_ms>,<build_ms>,<draw_ms>,<active>,<batches>,<tris>,<culled>` in response to FIFO command `stats <label>`. Task 2's sweep script greps `^STATS,`.

Note: this is GL main-loop code — no headless unit test exists for it. Verification is a live FIFO check (step 3). That is the intended test for this task.

- [ ] **Step 1: Add ViewerStats fields and HUD line**

In `MatterEngine3/viewer/ui.h`, after the `batch_cache_hit` field (line 30):

```cpp
    // Raster-path CPU timing split (ms) — Stage 0 of the frame-time package.
    float    resolve_ms = 0.0f;   // SectorResolver::resolve
    float    build_ms   = 0.0f;   // RasterComposer::build_batches
    float    draw_ms    = 0.0f;   // RasterComposer::draw (CPU submit side)
```

In `MatterEngine3/viewer/ui.cpp`, after the `FPS:` line (line 42):

```cpp
    ImGui::Text("CPU: resolve %.2f  build %.2f  draw %.2f ms",
                s.resolve_ms, s.build_ms, s.draw_ms);
```

- [ ] **Step 2: Wire timers and the `stats` command in main.cpp**

Add `#include <chrono>` to the include block at the top of `MatterEngine3/viewer/main.cpp`.

Near the FIFO state at line 234 (`std::string shot_path;`), add:

```cpp
    std::string stats_label;   // pending `stats <label>` FIFO request
```

In the FIFO parse chain (main.cpp:257-270), add a branch before the final `else`. A `char labelbuf[64];` declaration goes next to the existing `char pathbuf[256];`:

```cpp
                } else if (sscanf(line.c_str(), "stats %63s", labelbuf) == 1) {
                    stats_label = labelbuf;   // printed after this frame's stats fill
```

Replace the raster branch at main.cpp:284-288 with timed calls:

```cpp
        } else {
            auto t0 = std::chrono::steady_clock::now();
            auto resolved = resolver.resolve(state, lods, cam);
            auto t1 = std::chrono::steady_clock::now();
            batches = raster->build_batches(resolved, *store, renderer.camera());
            auto t2 = std::chrono::steady_clock::now();
            stats.resolve_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
            stats.build_ms   = std::chrono::duration<float, std::milli>(t2 - t1).count();
            for (const auto& b : batches) active += (int)b.transforms.size();
        }
```

Wrap the draw call at main.cpp:300 (`stats.raster_tris = raster->draw(...)`):

```cpp
                auto d0 = std::chrono::steady_clock::now();
                stats.raster_tris     = raster->draw(batches, *store, renderer.camera());
                stats.draw_ms = std::chrono::duration<float, std::milli>(
                                    std::chrono::steady_clock::now() - d0).count();
```

After `EndDrawing();` (main.cpp:309), print the pending stats line (all counters for this frame are filled by then; `frame_ms` is the previous frame's — fine once settled):

```cpp
        if (!stats_label.empty()) {
            printf("STATS,%s,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d\n",
                   stats_label.c_str(), stats.frame_ms,
                   stats.resolve_ms, stats.build_ms, stats.draw_ms,
                   stats.instances_active, stats.raster_batches,
                   stats.raster_tris, stats.culled_clusters);
            fflush(stdout);
            stats_label.clear();
        }
```

- [ ] **Step 3: Build and verify live**

```bash
cd MatterEngine3/viewer && make viewer
mkfifo /tmp/mtest_fifo 2>/dev/null || true
MATTER_WORLD=meadow MATTER_CMD_FIFO=/tmp/mtest_fifo ./viewer > /tmp/mtest.log 2>&1 &
sleep 20   # world load
echo "stats smoke" > /tmp/mtest_fifo; sleep 1
echo "quit" > /tmp/mtest_fifo; wait
grep '^STATS,smoke,' /tmp/mtest.log
```

Expected: one line like `STATS,smoke,97.42,24.81,31.05,12.33,44896,412,16900000,1034`.

- [ ] **Step 4: Windows build**

```bash
cd MatterEngine3/viewer && make windows
```

Expected: `viewer.exe` links (the FIFO code is already `#ifdef`-guarded off on Windows; `stats_label` stays unused there — that's fine).

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/viewer/ui.h MatterEngine3/viewer/ui.cpp MatterEngine3/viewer/main.cpp
git commit -m "feat: per-frame CPU timing split + stats FIFO command (Stage 0)"
```

### Task 2: Benchmark sweep script + baseline CSV

**Files:**
- Create: `MatterEngine3/tools/meadow_sweep.sh` (executable)
- Create: `MatterEngine3/docs/perf/meadow_sweep.csv`

**Interfaces:**
- Consumes: Task 1's `STATS,` stdout lines and FIFO commands `cam`/`stats`/`quit`.
- Produces: CSV rows `date,stage,camera,frame_ms,resolve_ms,build_ms,draw_ms,active,batches,tris,culled` appended per run. Every later stage runs `tools/meadow_sweep.sh <stage-label>` and commits the rows.

- [ ] **Step 1: Write the sweep script**

Create `MatterEngine3/tools/meadow_sweep.sh` (then `chmod +x`):

```bash
#!/usr/bin/env bash
# Meadow benchmark sweep (frame-time package, Stage 0).
# Usage: tools/meadow_sweep.sh <stage-label>   e.g. tools/meadow_sweep.sh baseline
set -euo pipefail
STAGE="${1:?usage: meadow_sweep.sh <stage-label>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../viewer"

FIFO="/tmp/matter_sweep_$$.fifo"
LOG="/tmp/matter_sweep_$$.log"
mkfifo "$FIFO"
MATTER_WORLD=meadow MATTER_CMD_FIFO="$FIFO" ./viewer > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; rm -f "$FIFO"' EXIT

sleep 25   # world bake/load + first frames; generous on a cold cache

run_cam() {  # name px py pz tx ty tz
  echo "cam $2 $3 $4 $5 $6 $7" > "$FIFO"
  sleep 2                        # let LOD/batches settle at the new view
  echo "stats $1" > "$FIFO"
  sleep 1
}
# Fixed camera set from the spec (meadow world spans 0..256):
run_cam aerial    128 260 -40   128 0 128    # whole scene in frustum
run_cam corner      8   2   8    60 1  60    # in-crowd ground view
run_cam midfield   40   6  40   128 2 128    # mid-distance, grass tri-share peak
run_cam far         4   3   4   250 0 250    # far ground view across the world
run_cam empty     -40   5 -40  -200 5 -200   # outside, looking away: fixed CPU cost

echo "quit" > "$FIFO"
wait "$PID" || true

CSV="$HERE/../docs/perf/meadow_sweep.csv"
TODAY="$(date +%F)"
grep '^STATS,' "$LOG" | while IFS= read -r line; do
  echo "$TODAY,$STAGE,${line#STATS,}" >> "$CSV"
done
echo "--- appended to $CSV:"
tail -n 5 "$CSV"
```

- [ ] **Step 2: Create the CSV with its header**

Create `MatterEngine3/docs/perf/meadow_sweep.csv` containing exactly:

```csv
date,stage,camera,frame_ms,resolve_ms,build_ms,draw_ms,active,batches,tris,culled
```

- [ ] **Step 3: Capture the baseline**

```bash
chmod +x MatterEngine3/tools/meadow_sweep.sh
MatterEngine3/tools/meadow_sweep.sh baseline
```

Expected: 5 new rows (aerial/corner/midfield/far/empty) with `stage=baseline`; aerial frame_ms in the ~140 ms class, empty ~70 ms (matches the spec's post-convention-fix numbers). If a camera row is missing, re-run — the STATS line is one-per-request.

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/tools/meadow_sweep.sh MatterEngine3/docs/perf/meadow_sweep.csv
git commit -m "feat: meadow benchmark sweep script + Stage 0 baseline CSV"
```

---

## Stage 1 — CPU Floor

### Task 3: -O2 viewer build

**Files:**
- Modify: `MatterEngine3/viewer/Makefile` (CFLAGS line)

**Interfaces:**
- Produces: viewer and viewer.exe built at -O2. No API changes.

- [ ] **Step 1: Add -O2**

In `MatterEngine3/viewer/Makefile`, change:

```make
CFLAGS = -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33
```

to:

```make
CFLAGS = -std=c++17 -O2 -Wall -Wno-missing-braces -Wno-unused-variable -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33
```

(`CXX_FLAGS_BUILD` includes `CFLAGS`, so this covers both the Linux and MinGW targets. The QuickJS/pipeline C objects are already -O2.)

- [ ] **Step 2: Clean rebuild both targets**

```bash
cd MatterEngine3/viewer && make clean && make viewer && make windows
```

Expected: full rebuild of all objects, both binaries link.

- [ ] **Step 3: Re-run the sweep**

```bash
MatterEngine3/tools/meadow_sweep.sh stage1-O2
```

Expected: `empty` frame_ms drops well below the ~70 ms baseline (micro-benchmarks showed ~3× on the hot loops); all five rows appended.

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/viewer/Makefile MatterEngine3/docs/perf/meadow_sweep.csv
git commit -m "perf: build viewer at -O2 (Stage 1); sweep rows"
```

### Task 4: WorldState version counter

**Files:**
- Modify: `MatterEngine3/viewer/world_source.h:37-46` (WorldState class)
- Modify: `MatterEngine3/viewer/world_state.cpp` (reset/apply)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Produces: `uint64_t WorldState::version() const` — monotonic, bumped by every `reset()` and `apply()`. Tasks 5 and 6 key their caches on it.

- [ ] **Step 1: Write the failing test**

In `MatterEngine3/tests/viewer_logic_tests.cpp`, add (reuse this file's existing `mk_entry(instance_id, part_hash, x, y, z)` fixture helper — it writes translation into m[3]/m[7]/m[11]) and register the function in `main()` alongside the existing tests:

```cpp
static void test_world_state_version() {
    viewer::WorldState s;
    assert(s.version() == 0);

    viewer::WorldManifest m;
    m.instances.push_back(mk_entry(1, 0xAAu, 0, 0, 0));
    s.reset(m);
    assert(s.version() == 1);

    viewer::WorldDelta d;
    d.added.push_back(mk_entry(2, 0xAAu, 4, 0, 4));
    s.apply(d);
    assert(s.version() == 2);

    s.reset(m);                    // reset always bumps, even to the same content
    assert(s.version() == 3);
    printf("  test_world_state_version OK\n");
}
```

- [ ] **Step 2: Run it, verify it fails**

```bash
cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests
```

Expected: compile FAILS — `'class viewer::WorldState' has no member named 'version'`.

- [ ] **Step 3: Implement**

In `MatterEngine3/viewer/world_source.h`, extend WorldState:

```cpp
class WorldState {
public:
    void reset(const WorldManifest& m);          // replace all entries
    void apply(const WorldDelta& d);              // add/move/remove by instance_id
    const std::vector<WorldManifestEntry>& entries() const { return entries_; }
    const WorldManifestEntry* find(uint32_t instance_id) const;
    // Monotonic content version: bumped by every reset()/apply(). Resolver and
    // composer caches key on it so per-frame work skips re-derivation when the
    // world hasn't changed (frame-time package, Stage 1).
    uint64_t version() const { return version_; }

private:
    std::vector<WorldManifestEntry> entries_;
    uint64_t version_ = 0;
};
```

In `MatterEngine3/viewer/world_state.cpp`, add `++version_;` as the first line of BOTH `WorldState::reset()` and `WorldState::apply()`.

- [ ] **Step 4: Run tests**

```bash
cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests
```

Expected: PASS, including all pre-existing tests.

- [ ] **Step 5: Rebuild viewer (header change ⇒ clean) and commit**

```bash
cd MatterEngine3/viewer && make clean && make viewer && make windows
git add MatterEngine3/viewer/world_source.h MatterEngine3/viewer/world_state.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat: WorldState monotonic version counter (Stage 1)"
```

### Task 5: Resolver sector-binning cache

**Files:**
- Modify: `MatterEngine3/viewer/sector_resolver.h:41-55` (SectorLodResolver members)
- Modify: `MatterEngine3/viewer/resolvers.cpp:28-45` (resolve: cache the binning)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Consumes: `WorldState::version()` (Task 4).
- Produces: `int SectorLodResolver::rebin_count() const` (how many times the sector table was rebuilt — test/HUD visibility). Resolve output is bit-identical to the uncached implementation.

- [ ] **Step 1: Write the failing test**

Add to `MatterEngine3/tests/viewer_logic_tests.cpp` and register in `main()`:

```cpp
static void test_resolver_binning_cache() {
    viewer::WorldState s;
    viewer::WorldManifest m;
    m.instances.push_back(mk_entry(1, 0xAAu,  5, 0,  5));
    m.instances.push_back(mk_entry(2, 0xAAu, 40, 0, 40));
    s.reset(m);

    lod_select::PartLodTable lods;
    lods[0xAAu] = { 1.0f, { 0.5f, 0.0f } };   // 2-level ladder

    viewer::SectorLodResolver r(16.0f, 1000.0f);
    float3 cam = make_float3(0, 0, 0);

    auto out1 = r.resolve(s, lods, cam);
    assert(r.rebin_count() == 1);

    // Same world + camera: identical output, no re-bin.
    auto out2 = r.resolve(s, lods, cam);
    assert(r.rebin_count() == 1);
    assert(out1.size() == out2.size());
    for (size_t i = 0; i < out1.size(); ++i) {
        assert(out1[i].part_hash == out2[i].part_hash);
        assert(out1[i].lod_level == out2[i].lod_level);
        assert(std::memcmp(out1[i].transform, out2[i].transform,
                           sizeof(float) * 16) == 0);
    }

    // Camera move alone must NOT re-bin (LOD selection still re-runs).
    float3 cam2 = make_float3(30, 0, 30);
    (void)r.resolve(s, lods, cam2);
    assert(r.rebin_count() == 1);

    // World delta bumps the version -> re-bin; new instance shows up.
    viewer::WorldDelta d;
    d.added.push_back(mk_entry(3, 0xAAu, 60, 0, 60));
    s.apply(d);
    auto out4 = r.resolve(s, lods, cam);
    assert(r.rebin_count() == 2);
    assert(out4.size() == out1.size() + 1);
    printf("  test_resolver_binning_cache OK\n");
}
```

- [ ] **Step 2: Run it, verify it fails**

```bash
cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests
```

Expected: compile FAILS — `no member named 'rebin_count'`.

- [ ] **Step 3: Implement**

`MatterEngine3/viewer/sector_resolver.h` — extend SectorLodResolver:

```cpp
class SectorLodResolver : public SectorResolver {
public:
    SectorLodResolver(float pitch, float active_radius)
        : pitch_(pitch), active_radius_(active_radius) {}
    std::vector<ResolvedInstance>
        resolve(const WorldState&, const lod_select::PartLodTable&, const float3&) override;
    const char* name() const override { return "SectorLod"; }
    void set_active_radius(float r) { active_radius_ = r; }
    void set_min_projected_size(float v) { min_projected_size_ = v; }
    // Times the sector table was (re)built — bumps only when WorldState::version()
    // changes, never on camera motion.
    int rebin_count() const { return rebin_count_; }

private:
    float pitch_;
    float active_radius_;
    float min_projected_size_ = 0.0f;
    // Binning cache (Stage 1): re-binning ~44k instances into a std::map every
    // frame dominated the CPU floor. Sectors only change when the world does.
    sector_grid::Sectors sectors_;
    uint64_t cached_version_ = UINT64_MAX;
    int      rebin_count_    = 0;
};
```

`MatterEngine3/viewer/resolvers.cpp` — replace steps 1-2 of `SectorLodResolver::resolve` (lines 32-45) with:

```cpp
    // 1+2. (Re)build the sector binning only when the world content changed.
    // LOD selection below stays exact per frame — identical output to the
    // uncached implementation (Stage 1 constraint).
    if (state.version() != cached_version_) {
        std::vector<world_flatten::FlatInstance> flat;
        flat.reserve(state.entries().size());
        for (const auto& e : state.entries()) {
            world_flatten::FlatInstance fi;
            fi.resolved_hash = e.part_hash;
            std::memcpy(fi.world.cell, e.transform, sizeof(fi.world.cell));  // mat4::cell[16]
            flat.push_back(fi);
        }
        sector_grid::SectorGrid grid(pitch_);
        sectors_ = sector_grid::bin_instances(flat, grid);
        cached_version_ = state.version();
        ++rebin_count_;
    }
    const sector_grid::Sectors& sectors = sectors_;
    auto chosen = lod_select::select_sector_lods(sectors, lods, cam_pos, min_projected_size_);
```

The emit loop (step 3 of the function) is unchanged — it already iterates `sectors`.

- [ ] **Step 4: Run tests**

```bash
cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests
```

Expected: PASS (new test + all existing resolver tests — output equivalence is also implicitly covered by the pre-existing SectorLod tests).

- [ ] **Step 5: Rebuild viewer (header change ⇒ clean) and commit**

```bash
cd MatterEngine3/viewer && make clean && make viewer && make windows
git add MatterEngine3/viewer/sector_resolver.h MatterEngine3/viewer/resolvers.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "perf: cache resolver sector binning on WorldState version (Stage 1)"
```

### Task 6: Cheap batch fingerprint + Stage 1 sweep

**Files:**
- Modify: `MatterEngine3/viewer/raster_composer.h:39-42` (build_batches signature)
- Modify: `MatterEngine3/viewer/raster_composer.cpp:169-194` (fingerprint region)
- Modify: `MatterEngine3/viewer/main.cpp:286` (call site)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Consumes: `WorldState::version()` (Task 4).
- Produces: `std::vector<RasterBatch> RasterComposer::build_batches(const std::vector<ResolvedInstance>& resolved, PartStore& store, const Camera3D& cam, uint64_t world_version)` — the 4th param is REQUIRED (no default): transforms drop out of the fingerprint, so callers MUST bump `world_version` whenever any transform can change. Task 9 later folds `pixel_budget_` into the same fingerprint.

- [ ] **Step 1: Write the failing test**

This file already has task13 fingerprint tests exercising `build_batches` + `cache_hit()`; follow their fixture (they construct a `PartStore` and a `std::vector<viewer::ResolvedInstance>`). Add, and register in `main()`:

```cpp
static void test_fingerprint_world_version() {
    // Fingerprint semantics after Stage 1: (camera, world_version, per-instance
    // (part_hash, lod)) — transform bytes no longer participate.
    // Reuse the PartStore + camera fixture from the existing task13
    // fingerprint tests in this file.
    viewer::RasterComposer rc;              // build_batches is GL-free
    Camera3D cam{};
    cam.position = { 0, 5, -10 };
    cam.target   = { 0, 0, 0 };
    cam.up       = { 0, 1, 0 };
    cam.fovy     = 60.0f;
    cam.projection = CAMERA_PERSPECTIVE;

    std::vector<viewer::ResolvedInstance> resolved(1);
    resolved[0].part_hash = 0xBEEFu;        // absent from the store: emit skips it,
    resolved[0].lod_level = 0;              // fingerprint/cache logic still runs
    for (int i = 0; i < 16; ++i) resolved[0].transform[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    viewer::PartStore store = mk_empty_store();   // same helper the task13 tests use

    (void)rc.build_batches(resolved, store, cam, /*world_version=*/1);
    assert(!rc.cache_hit());                       // first build: miss
    (void)rc.build_batches(resolved, store, cam, 1);
    assert(rc.cache_hit());                        // identical inputs: hit

    (void)rc.build_batches(resolved, store, cam, 2);
    assert(!rc.cache_hit());                       // version bump: miss

    cam.position.x += 1.0f;                        // camera move: miss
    (void)rc.build_batches(resolved, store, cam, 2);
    assert(!rc.cache_hit());

    resolved[0].lod_level = 1;                     // LOD flip: miss
    (void)rc.build_batches(resolved, store, cam, 2);
    assert(!rc.cache_hit());
    printf("  test_fingerprint_world_version OK\n");
}
```

(If the existing fixture builds the store differently — e.g. a temp cache dir — mirror that construction; the assertions are the contract.)

- [ ] **Step 2: Run it, verify it fails**

```bash
cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests
```

Expected: compile FAILS — `build_batches` takes 3 arguments, not 4.

- [ ] **Step 3: Implement**

`MatterEngine3/viewer/raster_composer.h` — change the declaration (and its doc comment):

```cpp
    // ... Batches are fingerprinted over (camera, world_version, per-instance
    // (part_hash, lod)) and reused across frames when nothing changed. Transform
    // bytes are NOT hashed: transforms only change via world deltas, which bump
    // world_version (Stage 1 — drops ~3.3 MB/frame of FNV input).
    std::vector<RasterBatch> build_batches(
        const std::vector<ResolvedInstance>& resolved,
        PartStore& store,
        const Camera3D& cam,
        uint64_t world_version);
```

`MatterEngine3/viewer/raster_composer.cpp` — in `build_batches`, add the parameter and replace the per-instance fingerprint fold (currently hashing `r.part_hash`, `r.lod_level`, and `r.transform`) with:

```cpp
    fnv_fold(fp, &world_version, sizeof(world_version));
    for (const auto& r : resolved) {
        fnv_fold(fp, &r.part_hash, sizeof(r.part_hash));
        fnv_fold(fp, &r.lod_level, sizeof(r.lod_level));
    }
```

(The camera position/target/fovy folds above it stay as-is; only the transform fold is removed.)

`MatterEngine3/viewer/main.cpp:286` — update the call site:

```cpp
            batches = raster->build_batches(resolved, *store, renderer.camera(), state.version());
```

Also update any existing `build_batches` calls in `MatterEngine3/tests/viewer_logic_tests.cpp` (the task13 tests): pass an explicit version, bumping it wherever those tests mutate transforms to force a rebuild.

- [ ] **Step 4: Run tests**

```bash
cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests
```

Expected: PASS.

- [ ] **Step 5: Rebuild (header change ⇒ clean), Stage 1 sweep, commit**

```bash
cd MatterEngine3/viewer && make clean && make viewer && make windows
cd ../.. && MatterEngine3/tools/meadow_sweep.sh stage1
```

Expected (Stage 1 exit criterion): `empty` row at low single-digit ms; `aerial` row shows resolve_ms + build_ms < ~3 ms combined.

```bash
git add MatterEngine3/viewer/raster_composer.h MatterEngine3/viewer/raster_composer.cpp MatterEngine3/viewer/main.cpp MatterEngine3/tests/viewer_logic_tests.cpp MatterEngine3/docs/perf/meadow_sweep.csv
git commit -m "perf: fingerprint batches on world version, not transform bytes (Stage 1); sweep rows"
```

---

## Stage 2 — Ladder Retune

### Task 7: Ratio-2 ladder + 32-tri stop rule

**Files:**
- Modify: `MatterEngine3/include/part_flatten.h:18-44` (FlattenTargets)
- Modify: `MatterEngine3/src/part_flatten.cpp:216-267` (per-cluster floor + ladder loop)
- Test: `MatterEngine3/tests/part_flatten_tests.cpp`

**Interfaces:**
- Produces: `FlattenTargets::radius_divisor = {512,256,128,64,32,16,8,4,2}` and `int min_level_tris = 32` (REPLACES `min_tris` — grep for `min_tris` and update every reference, including tests that construct FlattenTargets). Task 14 reuses `pixel_angle`/`pixel_budget` unchanged.

- [ ] **Step 1: Write the failing tests**

`MatterEngine3/tests/part_flatten_tests.cpp` already has flatten fixtures (temp cache dir, synthesized `.part` writers, a dense UV-sphere for the error-bound calibration test). Reuse those helpers; the assertions below are the contract. Add and register in `main()`:

```cpp
static void test_small_part_gets_ladder() {
    // Spec Stage 2.1: the old min_tris=2000 floor left small parts (a ~100-tri
    // grass clump) at one level forever. New stop rule: keep adding rungs until
    // a level stops shrinking or reaches ~32 tris.
    // Fixture: a small part well under 2000 tris (reuse the sphere builder at
    // low resolution, ~200-400 tris), saved as a childless .part in a temp cache.
    uint64_t hash = write_small_sphere_part(cache_dir);   // existing-style fixture helper
    auto res = part_flatten::flatten_part(cache_dir, hash);
    assert(res.ok);
    assert(res.levels >= 2);                  // laddered despite being "small"
    assert(res.coarsest_tris <= 64);          // driven down near the 32-tri floor
    printf("  test_small_part_gets_ladder OK\n");
}

static void test_ratio2_ladder_shape() {
    // Spec Stage 2.2: ratio-2 divisor schedule yields a deep ladder with
    // monotonically decreasing rung tri-counts on a dense fixture.
    uint64_t hash = write_dense_sphere_part(cache_dir);   // >= ~20k tris fixture
    auto res = part_flatten::flatten_part(cache_dir, hash);
    assert(res.ok);
    assert(res.levels >= 6);                  // spec: >= 6 levels on a dense fixture

    // Load the flat artifact and check per-cluster monotonic decrease.
    BLASManager blas; TLASManager tlas(4);
    std::vector<part_asset::FlatCluster> clusters;
    std::string p = cache_dir + "/" + part_asset::cache_path_flat(hash);
    assert(part_asset::load_flat_v3(p, hash, blas, tlas, clusters));
    for (const auto& cl : clusters) {
        size_t prev = SIZE_MAX;
        for (const auto& lvl : cl.lods) {
            assert(lvl.blas_indices.size() == 1);
            size_t tris = (size_t)blas.get_entries()[lvl.blas_indices[0]]->tri_count;
            assert(tris < prev || prev == SIZE_MAX);
            prev = tris;
        }
    }
    printf("  test_ratio2_ladder_shape OK\n");
}
```

(If the BLAS entry's triangle count field is named differently — check `blas_manager.hpp`, the flat-load tests in this file already read entry triangle counts — mirror that access.)

- [ ] **Step 2: Run, verify failure**

```bash
cd MatterEngine3/tests && touch part_flatten_tests.cpp && make part_flatten_tests && ./part_flatten_tests
```

Expected: FAIL — `res.levels >= 2` assertion fires for the small part (old floor stops its ladder), and/or `>= 6` for the dense one (4-divisor schedule maxes at 5 levels).

- [ ] **Step 3: Implement**

`MatterEngine3/include/part_flatten.h` — replace the two knobs in FlattenTargets:

```cpp
    // eps_i = bound_radius / radius_divisor[i], finest ladder step first.
    // Ratio-2 schedule (Stage 2): finer near rungs (switch sooner, smaller pops),
    // coarser far rungs (a terrain tile drops to tens of tris at distance).
    std::vector<float> radius_divisor = {512.0f, 256.0f, 128.0f, 64.0f,
                                         32.0f, 16.0f, 8.0f, 4.0f, 2.0f};
```

and replace `int min_tris = 2000;` (and its comment) with:

```cpp
    // Stop adding coarser rungs once a level lands at/below this triangle
    // count, or when a rung stops shrinking. Replaces the old min_tris=2000
    // floor that froze small parts at LOD0 forever (Stage 2).
    int min_level_tris = 32;
```

`MatterEngine3/src/part_flatten.cpp` — delete the `per_cluster_floor` computation (lines 216-220) and change the ladder-loop break (line 266) to:

```cpp
            if (prev_count <= (size_t)targets.min_level_tris) break;
```

The existing `if (geo.empty() || geo.size() >= prev_count) continue;` no-progress guard stays — with the ratio-2 schedule a later, coarser eps may still make progress.

- [ ] **Step 4: Run tests**

```bash
cd MatterEngine3/tests && touch part_flatten_tests.cpp && make part_flatten_tests && ./part_flatten_tests
```

Expected: PASS (fix any existing tests that referenced `min_tris`).

- [ ] **Step 5: Rebuild (header change ⇒ clean) and commit**

```bash
cd MatterEngine3/viewer && make clean && make viewer && make windows
git add MatterEngine3/include/part_flatten.h MatterEngine3/src/part_flatten.cpp MatterEngine3/tests/part_flatten_tests.cpp
git commit -m "feat: ratio-2 LOD ladder schedule + 32-tri stop rule (Stage 2)"
```

### Task 8: Flat-format version bump (bake-version byte)

**Files:**
- Modify: `MatterEngine3/include/part_asset_v2.h:20-21` (version constants)
- Modify: `MatterEngine3/src/part_asset_v2.cpp` (save_flat_v3/load_flat_v3 version uses)
- Modify: `MatterEngine3/viewer/part_store.cpp:40` (version sniff)
- Modify: `MatterEngine3/viewer/local_provider.cpp:174` (stale-flat check)
- Test: `MatterEngine3/tests/part_flatten_tests.cpp`

**Interfaces:**
- Produces: `constexpr uint32_t part_asset::kFormatVersionFlat = 4u;` — save_flat_v3 writes it, load_flat_v3 requires it, `peek_format_version` exposes it. All existing v3 flats become stale and regenerate on next connect (this IS the spec's Stage 2 cache invalidation — no manual wipe).

- [ ] **Step 1: Write the failing test**

Add to `MatterEngine3/tests/part_flatten_tests.cpp`, register in `main()`:

```cpp
static void test_flat_version_bump() {
    uint64_t hash = write_small_sphere_part(cache_dir);   // Task 7 fixture
    auto res = part_flatten::flatten_part(cache_dir, hash);
    assert(res.ok);
    std::string p = cache_dir + "/" + part_asset::cache_path_flat(hash);

    // New flats carry the bumped version.
    assert(part_asset::peek_format_version(p) == part_asset::kFormatVersionFlat);
    assert(part_asset::kFormatVersionFlat == 4u);

    // Patch the version field back to 3 (a pre-retune bake): loader must reject.
    // Header layout: magic (u32) then format_version (u32) — verify the write
    // offset against write_file_atomic in part_asset_v2.cpp before relying on it.
    {
        FILE* f = fopen(p.c_str(), "r+b");
        assert(f);
        uint32_t old = 3u;
        fseek(f, 4, SEEK_SET);
        fwrite(&old, sizeof old, 1, f);
        fclose(f);
    }
    assert(part_asset::peek_format_version(p) == 3u);
    BLASManager b2; TLASManager t2(4);
    std::vector<part_asset::FlatCluster> cl2;
    assert(!part_asset::load_flat_v3(p, hash, b2, t2, cl2));
    printf("  test_flat_version_bump OK\n");
}
```

- [ ] **Step 2: Run, verify failure**

```bash
cd MatterEngine3/tests && touch part_flatten_tests.cpp && make part_flatten_tests && ./part_flatten_tests
```

Expected: compile FAILS — `kFormatVersionFlat` not declared.

- [ ] **Step 3: Implement**

`MatterEngine3/include/part_asset_v2.h:21` — after `kFormatVersionV3`:

```cpp
// Flat-artifact bake version: bump whenever FlattenTargets defaults change so
// stale flats regenerate automatically (Stage 2 ladder retune bumped 3 -> 4).
constexpr uint32_t kFormatVersionFlat = 4u;
```

`MatterEngine3/src/part_asset_v2.cpp` — in `save_flat_v3` and `load_flat_v3`, replace every use of `kFormatVersionV3` with `kFormatVersionFlat` (the write at ~line 410 and the load-time guard at ~line 431). `kFormatVersionV3` itself stays declared (documents history; load rejection of old files is by inequality).

`MatterEngine3/viewer/part_store.cpp:40` — the flat-load version sniff `ver == 3` becomes `ver == part_asset::kFormatVersionFlat`.

`MatterEngine3/viewer/local_provider.cpp:174` — `peek_format_version(flat_abs_path) == 3` becomes `== part_asset::kFormatVersionFlat` (update the comment above it: "regenerate when the flat artifact is missing OR from an older bake version").

- [ ] **Step 4: Run tests (flatten + viewer logic + integration suites)**

```bash
cd MatterEngine3/tests && touch part_flatten_tests.cpp viewer_logic_tests.cpp && make part_flatten_tests viewer_logic_tests && ./part_flatten_tests && ./viewer_logic_tests
```

Expected: PASS.

- [ ] **Step 5: Rebuild (header change ⇒ clean) and commit**

```bash
cd MatterEngine3/viewer && make clean && make viewer && make windows
git add MatterEngine3/include/part_asset_v2.h MatterEngine3/src/part_asset_v2.cpp MatterEngine3/viewer/part_store.cpp MatterEngine3/viewer/local_provider.cpp MatterEngine3/tests/part_flatten_tests.cpp
git commit -m "feat: flat-artifact bake version 4 so retuned ladders regenerate (Stage 2)"
```

### Task 9: Runtime pixel-budget dial

**Files:**
- Modify: `MatterEngine3/include/lod_select.h:34-37` + `MatterEngine3/src/lod_select.cpp:27-53` (budget param)
- Modify: `MatterEngine3/viewer/sector_resolver.h` (setter) + `MatterEngine3/viewer/resolvers.cpp` (pass-through)
- Modify: `MatterEngine3/viewer/raster_cull.h:68-89` (cluster_lod_select budget param)
- Modify: `MatterEngine3/viewer/raster_composer.h` (setter) + `MatterEngine3/viewer/raster_composer.cpp` (fingerprint fold + call site)
- Modify: `MatterEngine3/viewer/ui.h` + `MatterEngine3/viewer/ui.cpp` (HUD slider)
- Modify: `MatterEngine3/viewer/main.cpp` (FIFO `budget` command + per-frame propagation)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Produces:
  - `lod_select::select_sector_lods(..., float min_projected_size = 0.0f, float pixel_budget = 1.0f)` — budget scales the projected size (`size *= pixel_budget`) BEFORE both the floor check and level selection, so one dial scales thresholds AND the sub-pixel cull (spec Stage 2.3).
  - `viewer::cluster_lod_select(const LoadedCluster&, const float* inst, const float* cam_eye, float pixel_budget = 1.0f)`.
  - `SectorLodResolver::set_pixel_budget(float)`, `RasterComposer::set_pixel_budget(float)` (folded into the batch fingerprint), `ViewerStats::pixel_budget` (writable, default 1.0f), FIFO command `budget <f>`.

- [ ] **Step 1: Write the failing tests**

Add to `MatterEngine3/tests/viewer_logic_tests.cpp`, register in `main()`:

```cpp
static void test_pixel_budget_dial() {
    // Budget 0.5 selects coarser-or-equal levels than 1.0 for every part (spec test).
    sector_grid::Sectors sectors;
    // One sector at origin with one instance of each part; camera 10 m away.
    world_flatten::FlatInstance fi{};
    fi.resolved_hash = 0xA1u;
    for (int i = 0; i < 16; ++i) fi.world.cell[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    sectors[sector_grid::SectorCoord{0,0,0}].push_back(fi);
    fi.resolved_hash = 0xA2u;
    sectors[sector_grid::SectorCoord{0,0,0}].push_back(fi);

    lod_select::PartLodTable parts;
    parts[0xA1u] = { 2.0f,  { 0.15f, 0.05f, 0.0f } };   // 3-level ladder
    parts[0xA2u] = { 0.5f,  { 0.04f, 0.0f } };          // small part

    float3 cam = make_float3(10, 0, 0);
    auto full = lod_select::select_sector_lods(sectors, parts, cam, 0.0f, 1.0f);
    auto half = lod_select::select_sector_lods(sectors, parts, cam, 0.0f, 0.5f);
    for (const auto& sk : full)
        for (const auto& pk : sk.second) {
            int lf = pk.second;
            int lh = half.at(sk.first).at(pk.first);
            assert(lh >= lf);                     // coarser-or-equal at lower budget
        }

    // The dial also scales the floor cull: visible at budget 1, culled at 0.1.
    auto floored = lod_select::select_sector_lods(sectors, parts, cam, 0.02f, 0.1f);
    assert(floored.at(sector_grid::SectorCoord{0,0,0}).at(0xA2u) == -1);
    auto unfloored = lod_select::select_sector_lods(sectors, parts, cam, 0.02f, 1.0f);
    assert(unfloored.at(sector_grid::SectorCoord{0,0,0}).at(0xA2u) >= 0);
    printf("  test_pixel_budget_dial OK\n");
}

static void test_cluster_budget_dial() {
    viewer::LoadedCluster cl{};
    cl.aabb_min[0] = -1; cl.aabb_min[1] = -1; cl.aabb_min[2] = -1;
    cl.aabb_max[0] =  1; cl.aabb_max[1] =  1; cl.aabb_max[2] =  1;
    cl.radius = 1.7f;
    cl.thresholds = { 0.3f, 0.1f, 0.0f };
    float inst[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float eye[3] = { 10, 0, 0 };
    int lf = viewer::cluster_lod_select(cl, inst, eye, 1.0f);
    int lh = viewer::cluster_lod_select(cl, inst, eye, 0.5f);
    assert(lh >= lf);
    printf("  test_cluster_budget_dial OK\n");
}
```

(Adjust `LoadedCluster` member init to its actual field list in `part_store.h` if it differs.)

- [ ] **Step 2: Run, verify failure**

```bash
cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests
```

Expected: compile FAILS — `select_sector_lods` takes 4 args; `cluster_lod_select` takes 3.

- [ ] **Step 3: Implement**

`MatterEngine3/include/lod_select.h:34-37` — extend the declaration (and doc: "pixel_budget scales the projected size before both the floor check and level selection — the runtime quality/speed dial"):

```cpp
std::map<sector_grid::SectorCoord, std::map<uint64_t,int>>
select_sector_lods(const sector_grid::Sectors& sectors,
                   const PartLodTable& parts, const float3& cam_pos,
                   float min_projected_size = 0.0f,
                   float pixel_budget = 1.0f);
```

`MatterEngine3/src/lod_select.cpp` — add the parameter; change line 46 to:

```cpp
            float size = pit->second.bound_radius / closest * pixel_budget;
```

`MatterEngine3/viewer/sector_resolver.h` — add to SectorLodResolver:

```cpp
    void set_pixel_budget(float b) { pixel_budget_ = b; }
```
and the member `float pixel_budget_ = 1.0f;`. In `resolvers.cpp`, pass it:

```cpp
    auto chosen = lod_select::select_sector_lods(sectors, lods, cam_pos,
                                                 min_projected_size_, pixel_budget_);
```

`MatterEngine3/viewer/raster_cull.h` — `cluster_lod_select` gains `float pixel_budget = 1.0f` as 4th param; line 81 becomes:

```cpp
    float psize = cl.radius * scale / dist * pixel_budget;
```

`MatterEngine3/viewer/raster_composer.h` — add:

```cpp
    // Runtime LOD quality/speed dial (Stage 2); folded into the batch fingerprint.
    void set_pixel_budget(float b) { pixel_budget_ = b; }
```
and member `float pixel_budget_ = 1.0f;`. In `raster_composer.cpp`: fold it into the fingerprint next to `world_version` (`fnv_fold(fp, &pixel_budget_, sizeof(pixel_budget_));`) and pass `pixel_budget_` to the `cluster_lod_select` call (~line 236).

`MatterEngine3/viewer/ui.h` — add to ViewerStats:

```cpp
    // Writable: runtime LOD quality/speed dial. main propagates it to the
    // resolver + composer each frame; also settable via FIFO `budget <f>`.
    float    pixel_budget = 1.0f;
```

`MatterEngine3/viewer/ui.cpp` — before the Resolver combo (line 64):

```cpp
    ImGui::SliderFloat("Pixel budget", &s.pixel_budget, 0.1f, 2.0f, "%.2f");
```

`MatterEngine3/viewer/main.cpp` — FIFO branch (next to `stats`):

```cpp
                } else if (sscanf(line.c_str(), "budget %f", &c[0]) == 1) {
                    stats.pixel_budget = c[0];
```

and per-frame propagation just before the resolver runs (after the `SectorResolver& resolver = ...` selection at line 277):

```cpp
        sec.set_pixel_budget(stats.pixel_budget);
        raster->set_pixel_budget(stats.pixel_budget);
```

- [ ] **Step 4: Run tests**

```bash
cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests
```

Expected: PASS.

- [ ] **Step 5: Rebuild (header change ⇒ clean) and commit**

```bash
cd MatterEngine3/viewer && make clean && make viewer && make windows
git add MatterEngine3/include/lod_select.h MatterEngine3/src/lod_select.cpp MatterEngine3/viewer/sector_resolver.h MatterEngine3/viewer/resolvers.cpp MatterEngine3/viewer/raster_cull.h MatterEngine3/viewer/raster_composer.h MatterEngine3/viewer/raster_composer.cpp MatterEngine3/viewer/ui.h MatterEngine3/viewer/ui.cpp MatterEngine3/viewer/main.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat: runtime pixel-budget LOD dial (HUD slider + budget FIFO cmd) (Stage 2)"
```

### Task 10: Never-invisible guarantee + Stage 2 sweep

**Files:**
- Modify: `MatterEngine3/src/lod_select.cpp` (floor-cull clamp)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Produces: in `select_sector_lods`, parts with `bound_radius >= 4.0f` are never floor-culled (-1) — they clamp to their coarsest rung instead. Small scatter stays floor-cullable. No signature change.

- [ ] **Step 1: Write the failing test**

Add to `MatterEngine3/tests/viewer_logic_tests.cpp`, register in `main()`:

```cpp
static void test_never_invisible_guarantee() {
    sector_grid::Sectors sectors;
    world_flatten::FlatInstance fi{};
    for (int i = 0; i < 16; ++i) fi.world.cell[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    fi.resolved_hash = 0xB16u;   // terrain-tile class: radius 14 m
    sectors[sector_grid::SectorCoord{0,0,0}].push_back(fi);
    fi.resolved_hash = 0x5A11u;  // small scatter: radius 0.5 m
    sectors[sector_grid::SectorCoord{0,0,0}].push_back(fi);

    lod_select::PartLodTable parts;
    parts[0xB16u]  = { 14.0f, { 0.3f, 0.1f, 0.03f, 0.0f } };
    parts[0x5A11u] = { 0.5f,  { 0.04f, 0.0f } };

    // Extreme distance: both project below the floor.
    float3 cam = make_float3(20000, 0, 0);
    auto chosen = lod_select::select_sector_lods(sectors, parts, cam, 0.0015f);
    const auto& sec = chosen.at(sector_grid::SectorCoord{0,0,0});
    assert(sec.at(0xB16u) == 3);      // radius >= 4 m: clamped to coarsest, never -1
    assert(sec.at(0x5A11u) == -1);    // small part: still floor-culled
    printf("  test_never_invisible_guarantee OK\n");
}
```

- [ ] **Step 2: Run, verify failure**

```bash
cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests
```

Expected: FAIL — `sec.at(0xB16u) == 3` fires (current code returns -1).

- [ ] **Step 3: Implement**

`MatterEngine3/src/lod_select.cpp` — add near the top of the namespace:

```cpp
// Parts at least this large (terrain tiles: ~14 m) are never floor-culled,
// only clamped to their coarsest rung — a structural never-invisible
// guarantee inside the active radius (Stage 2.4). Small scatter (grass,
// pebbles) stays floor-cullable.
static constexpr float kNeverCullRadius = 4.0f;
```

and replace the selection expression (lines 46-49, as modified by Task 9) with:

```cpp
            float size = pit->second.bound_radius / closest * pixel_budget;
            int level;
            if (size < min_projected_size) {
                level = (pit->second.bound_radius >= kNeverCullRadius)
                            ? (pit->second.thresholds.empty()
                                   ? 0
                                   : (int)pit->second.thresholds.size() - 1)
                            : -1;
            } else {
                level = select_level(size, pit->second.thresholds);
            }
            out[coord][f.resolved_hash] = level;
```

- [ ] **Step 4: Run tests**

```bash
cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests
```

Expected: PASS.

- [ ] **Step 5: Rebuild, Stage 2 sweep (regenerates all flats via the v4 bump — first run is slow), screenshots, commit**

```bash
cd MatterEngine3/viewer && make viewer && make windows
cd ../.. && MatterEngine3/tools/meadow_sweep.sh stage2
```

Expected (Stage 2 exit criterion): `aerial` tris drops from the 16.9M class to low millions at budget 1.0; frame times improve across rows.

```bash
git add MatterEngine3/src/lod_select.cpp MatterEngine3/tests/viewer_logic_tests.cpp MatterEngine3/docs/perf/meadow_sweep.csv
git commit -m "feat: never-invisible clamp for radius >= 4 m parts (Stage 2); sweep rows"
```

---

## Stage 3 — Opt-in Procedural Triangle-Budget LOD (grass)

### Task 11: Grass.js budget restructure + grass_lod_tests

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/Grass.js`
- Create: `MatterEngine3/tests/grass_lod_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile` (new stanza)

**Interfaces:**
- Produces: Grass.js exports `static lodBudgets = [1.0, 0.3, 0.08]`, `static lodAnchorSize = 0.5`, declares `lodBudget: 1.0` in `static params`, and its generator honors the budget with prefix-subset blades. Tasks 12-14 read exactly these statics/params.
- Coherence contract (spec): ALL blade parameters are drawn from the seeded RNG first; only the first `max(1, ceil(lodBudget * BLADES))` blades are emitted; surviving blades widen by `sqrt(BLADES / count)` (~1/√k, exactly 1.0 at full budget).

- [ ] **Step 1: Rewrite Grass.js**

Replace the `build` body of `MatterEngine3/examples/world_demo/schemas/Grass.js` (the per-blade RNG draw order — ang, d, hgt, w, lean, g, yaw — is preserved exactly, so full-budget geometry is unchanged):

```js
import { rng } from 'shared-lib/rng';

// A grass clump: BLADES tapered mesh blades (one 5-vertex triangle strip each
// = 3 tris/blade) with a slight bend and per-blade tint variation, plus a small
// root skirt below y=0 so slope placement never leaves floating blades. A few
// seeded variants are baked once each and instanced tens of thousands of times
// by Meadow. BLADES is the per-clump density lever.
class Grass extends Part {
  static params = { seed: 0, lodBudget: 1.0 };

  // Opt-in procedural triangle-budget LOD (frame-time package, Stage 3): the
  // baker re-runs build() at each fraction; level N+1's blades are a strict
  // prefix of level N's, so LOD switches never reshuffle the field.
  static lodBudgets = [1.0, 0.3, 0.08];
  static lodAnchorSize = 0.5;   // blade height (m): full res holds until ~2 px

  build(p) {
    const r = rng(3000 + p.seed);
    const BLADES = 25 + r.int(11);      // 25-35 blades
    const SKIRT = 0.08;                 // root depth below y=0

    // Draw ALL blade parameters first so the RNG stream is identical at every
    // budget; emission below is a prefix subset.
    const blades = [];
    for (let b = 0; b < BLADES; ++b) {
      blades.push({
        ang:  r.range(0, Math.PI * 2),
        d:    r.range(0, 0.35),          // clump radius
        hgt:  r.range(0.35, 0.7),
        w:    r.range(0.02, 0.035),      // half-width at base
        lean: r.range(0.05, 0.25),       // tip lateral offset (bend)
        g:    r.range(0.75, 1.1),        // per-blade tint variation
        yaw:  r.range(0, Math.PI * 2),
      });
    }
    const count = Math.max(1, Math.ceil(p.lodBudget * BLADES));
    const widen = Math.sqrt(BLADES / count);   // coverage ~1/sqrt(k); 1.0 at full budget

    this.fill(MAT.grass);
    for (let b = 0; b < count; ++b) {
      const bl = blades[b];
      const w = bl.w * widen;
      this.tint(0.32 * bl.g, 0.55 * bl.g, 0.18 * bl.g, 0.6);
      this.pushMatrix();
      this.translate(Math.cos(bl.ang) * bl.d, 0, Math.sin(bl.ang) * bl.d);
      this.rotateY(bl.yaw);
      // 5-vertex strip: root pair (below y=0), tapered mid pair, tip = 3 tris.
      this.beginShape(SHAPE.strip);
        this.vertex(-w, -SKIRT, 0);
        this.vertex( w, -SKIRT, 0);
        this.vertex(-w * 0.6, bl.hgt * 0.55, bl.lean * 0.5);
        this.vertex( w * 0.6, bl.hgt * 0.55, bl.lean * 0.5);
        this.vertex( 0, bl.hgt, bl.lean);
      this.endShape();
      this.popMatrix();
    }
  }
}
```

- [ ] **Step 2: Write the test suite (failing until Grass.js honors lodBudget)**

Create `MatterEngine3/tests/grass_lod_tests.cpp`. Bake Grass.js through the ScriptHost at several budgets and inspect the baked `.part` meshes. Mirror the bake fixture from the gallery/tree-bake suites in this directory (temp cache dir + chdir dance, `host.set_shared_lib_root("../examples/world_demo/shared-lib")`, read schema source from `../examples/world_demo/schemas/Grass.js`, `host.bake_source(source, params_json, {})`, then `load_v2(r.written_path, r.resolved_hash, ...)` and read tris via `blas.get_entries()`).

Key geometry facts the assertions rely on:
- 3 tris per blade → `BLADES = tris(budget 1.0) / 3`.
- In a 5-vertex strip, exactly two vertex positions appear in exactly ONE triangle: the first root vertex (y == -0.08) and the TIP (y >= 0.35). Tip position is `translate + rotY` applied to `(0, hgt, lean)` — independent of blade width, so tips are invariant under the 1/√k widening and identify blades exactly.

```cpp
// Collect blade TIP positions: vertex positions used by exactly one triangle
// with y > 0. Width-independent -> exact prefix-subset witness.
static std::vector<float3> blade_tips(const std::vector<Tri>& tris);   // multiset count per position

static void test_budget_tri_counts() {
    size_t t100 = bake_grass_tris(1.0);
    size_t t30  = bake_grass_tris(0.3);
    size_t t8   = bake_grass_tris(0.08);
    size_t blades = t100 / 3;
    assert(t100 % 3 == 0 && blades >= 25 && blades <= 35);
    assert(t30 == 3 * (size_t)std::ceil(0.3  * blades));   // per-level count exact
    assert(t8  == 3 * std::max<size_t>(1, (size_t)std::ceil(0.08 * blades)));
    printf("  test_budget_tri_counts OK\n");
}

static void test_prefix_subset() {
    auto full = bake_grass_mesh(1.0);    // std::vector<Tri>
    auto low  = bake_grass_mesh(0.3);
    auto tips_full = blade_tips(full);
    auto tips_low  = blade_tips(low);
    assert(tips_low.size() < tips_full.size());
    for (const auto& t : tips_low) {
        bool found = false;
        for (const auto& u : tips_full)
            if (t.x == u.x && t.y == u.y && t.z == u.z) { found = true; break; }
        assert(found);   // exact float equality: same RNG values, same ops
    }
    printf("  test_prefix_subset OK\n");
}

static void test_determinism() {
    // Same source+params -> same resolved hash and byte-identical artifact.
    auto r1 = bake_grass(0.3, "cacheA");
    auto r2 = bake_grass(0.3, "cacheB");
    assert(r1.resolved_hash == r2.resolved_hash);
    assert(file_bytes(r1.written_path) == file_bytes(r2.written_path));
    printf("  test_determinism OK\n");
}

static void test_triex_present_all_levels() {
    for (double b : {1.0, 0.3, 0.08}) {
        auto entries = bake_grass_entries(b);   // BLAS entries after load_v2
        for (const auto* e : entries)
            assert(e->tri_extra != nullptr);    // native TriEx (tint) at every level
    }
    printf("  test_triex_present_all_levels OK\n");
}
```

Write the `bake_grass*` helpers concretely against the ScriptHost API (params_json e.g. `{"seed":0,"lodBudget":0.3}`); the gallery suite shows the exact incantation.

Add a `grass_lod_tests` stanza to `MatterEngine3/tests/Makefile`, cloned from the tree-bake/gallery stanza (same objects incl. `script_rng_binding.cpp`, `-DMATTER_HAVE_SCRIPT_HOST`, `$(filter-out example_world.cpp, $(EXAMPLE_CPP))` pattern), and add the binary to the suite list / `.gitignore`d outputs as the other suites are.

- [ ] **Step 3: Run**

```bash
cd MatterEngine3/tests && make grass_lod_tests && ./grass_lod_tests
```

Expected: PASS (Grass.js from Step 1 already honors the budget; the suite pins the contract. If run against the OLD Grass.js the tri-count test fails — that's the failing-test evidence; order Steps 1-2 either way as long as both states are observed).

- [ ] **Step 4: Visual sanity + commit**

The meadow re-bakes on next connect (grass source changed → new hashes). No viewer code changed in this task, but per project rule:

```bash
cd MatterEngine3/viewer && make viewer && make windows
git add MatterEngine3/examples/world_demo/schemas/Grass.js MatterEngine3/tests/grass_lod_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat: Grass.js prefix-subset triangle-budget LOD + grass_lod_tests (Stage 3)"
```

### Task 12: ScriptHost::eval_lod_budgets

**Files:**
- Modify: `MatterEngine3/include/script_host.h` (LodBudgetSpec + declaration)
- Modify: `MatterEngine3/src/script_host.cpp` (implementation after eval_requires, line ~430)
- Test: `MatterEngine3/tests/script_host_tests.cpp`

**Interfaces:**
- Produces:

```cpp
struct LodBudgetSpec {
    std::vector<double> budgets;    // e.g. {1.0, 0.3, 0.08}; empty = not opted in
    double anchor_size = 0.0;       // lodAnchorSize (m); 0 = unset
};
LodBudgetSpec eval_lod_budgets(const std::string& source);
```

Reads `static lodBudgets` (array of numbers in (0,1]) and `static lodAnchorSize` (positive number) from the part class WITHOUT running build(). Fail-closed: any eval error, missing/invalid lodBudgets → empty spec. Task 13's HostBaker consumes it.

- [ ] **Step 1: Write the failing tests**

Add to `MatterEngine3/tests/script_host_tests.cpp`, register in `main()`:

```cpp
static void test_eval_lod_budgets() {
    script_host::ScriptHost host;
    const char* opted =
        "class G extends Part {\n"
        "  static params = { seed: 0, lodBudget: 1.0 };\n"
        "  static lodBudgets = [1.0, 0.3, 0.08];\n"
        "  static lodAnchorSize = 0.5;\n"
        "  build(p) {}\n"
        "}\n";
    auto spec = host.eval_lod_budgets(opted);
    assert(spec.budgets.size() == 3);
    assert(spec.budgets[0] == 1.0 && spec.budgets[1] == 0.3 && spec.budgets[2] == 0.08);
    assert(spec.anchor_size == 0.5);

    const char* plain =
        "class P extends Part { static params = {}; build(p) {} }\n";
    assert(host.eval_lod_budgets(plain).budgets.empty());

    const char* malformed =
        "class M extends Part {\n"
        "  static lodBudgets = [1.0, 'x'];\n"      // non-number: fail closed
        "  build(p) {}\n"
        "}\n";
    assert(host.eval_lod_budgets(malformed).budgets.empty());

    const char* broken = "not even javascript {{{";
    assert(host.eval_lod_budgets(broken).budgets.empty());
    printf("  test_eval_lod_budgets OK\n");
}
```

- [ ] **Step 2: Run, verify failure**

```bash
cd MatterEngine3/tests && touch script_host_tests.cpp && make script_host_tests && ./script_host_tests
```

Expected: compile FAILS — no member `eval_lod_budgets`.

- [ ] **Step 3: Implement**

`MatterEngine3/include/script_host.h` — add LodBudgetSpec + the method to ScriptHost's public section, next to `eval_requires` (declaration as in Interfaces above, with the doc comment: "Fail-closed like eval_requires; does not run build()").

`MatterEngine3/src/script_host.cpp` — after `eval_requires` (~line 430), same structure (module fold so imports work; fresh isolated runtime; free everything):

```cpp
ScriptHost::LodBudgetSpec ScriptHost::eval_lod_budgets(const std::string& source) {
    LodBudgetSpec out;

    std::string className = find_part_class_name(source);
    if (className.empty()) return out;

    ModuleStore store;
    bool use_module = false;
    if (!shared_lib_root_.empty()) {
        module_resolver::FoldResult fr; std::string ferr;
        if (!module_resolver::fold_sources(source, shared_lib_root_, fr, ferr)) return out;
        if (!fr.modules.empty()) { store = store_from_fold(fr); use_module = true; }
    }

    JSRuntime* rt = nullptr; JSContext* ctx = nullptr;
    BakeError eerr;
    if (!eval_part_publish_class(source, className, use_module ? &store : nullptr,
                                 rt, ctx, eerr))
        return out;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue authored = JS_GetPropertyStr(ctx, global, "__partClass");
    JS_FreeValue(ctx, global);
    if (JS_IsFunction(ctx, authored)) {
        JSValue budgets = JS_GetPropertyStr(ctx, authored, "lodBudgets");
        if (JS_IsArray(budgets)) {                       // match eval_requires' usage
            JSValue lenv = JS_GetPropertyStr(ctx, budgets, "length");
            uint32_t len = 0; JS_ToUint32(ctx, &len, lenv); JS_FreeValue(ctx, lenv);
            for (uint32_t i = 0; i < len; ++i) {
                JSValue el = JS_GetPropertyUint32(ctx, budgets, i);
                double d = 0.0;
                bool ok = !JS_IsException(el) && JS_IsNumber(el) &&
                          JS_ToFloat64(ctx, &d, el) == 0 && d > 0.0 && d <= 1.0;
                JS_FreeValue(ctx, el);
                if (!ok) { out.budgets.clear(); break; }   // fail closed
                out.budgets.push_back(d);
            }
        }
        JS_FreeValue(ctx, budgets);
        JSValue anchor = JS_GetPropertyStr(ctx, authored, "lodAnchorSize");
        if (JS_IsNumber(anchor)) {
            double a = 0.0;
            if (JS_ToFloat64(ctx, &a, anchor) == 0 && a > 0.0) out.anchor_size = a;
        }
        JS_FreeValue(ctx, anchor);
    }
    JS_FreeValue(ctx, authored);
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    return out;
}
```

(If this file's `JS_IsArray` calls take a `ctx` argument, match them.)

- [ ] **Step 4: Run tests**

```bash
cd MatterEngine3/tests && touch script_host_tests.cpp && make script_host_tests && ./script_host_tests
```

Expected: PASS.

- [ ] **Step 5: Rebuild (header change ⇒ clean) and commit**

```bash
cd MatterEngine3/viewer && make clean && make viewer && make windows
git add MatterEngine3/include/script_host.h MatterEngine3/src/script_host.cpp MatterEngine3/tests/script_host_tests.cpp
git commit -m "feat: ScriptHost::eval_lod_budgets reads schema LOD budget statics (Stage 3)"
```

### Task 13: Budget-variant baking + sidecar

**Files:**
- Modify: `MatterEngine3/include/part_graph.h` (Baker::bake_lod_variants default; HostBaker override)
- Modify: `MatterEngine3/src/part_graph.cpp` (install hook; HostBaker impl)
- Modify: `MatterEngine3/include/part_asset_v2.h` + `MatterEngine3/src/part_asset_v2.cpp` (`cache_path_lods`, `LodVariants`, `load_lod_sidecar`)
- Test: `MatterEngine3/tests/part_graph_integration_tests.cpp` (has `-DMATTER_HAVE_SCRIPT_HOST`)

**Interfaces:**
- Consumes: `eval_lod_budgets` (Task 12); `merge_params_canonical` keeps unknown override keys (verified: `Object.assign({}, defaults, overrides)` at script_host.cpp:326-329), so a `lodBudget` param override reaches `build(p)` and folds into the resolved hash → content-addressed variants for free.
- Produces:
  - `virtual bool Baker::bake_lod_variants(const std::string& source, const Params& params, const std::vector<uint64_t>& child_hashes, uint64_t resolved_hash) { return true; }` — default no-op (FakeBaker tests untouched); called by `PartGraph::install` for EVERY node after its bake/cache check.
  - `std::string part_asset::cache_path_lods(uint64_t)` → `"parts/<16-hex>.lods"`.
  - Sidecar text format: line 1 = `<anchor_size>`, then one `<budget> <16-hex-hash>` line per level, finest first.
  - `struct part_asset::LodVariants { double anchor_size; std::vector<double> budgets; std::vector<uint64_t> hashes; }` + `bool load_lod_sidecar(const std::string& path, LodVariants& out)` (false if missing/unparseable). Task 14 consumes it.
  - Only childless opted-in parts are supported (grass is childless): a part with children + lodBudgets logs a warning and is skipped (returns true).

- [ ] **Step 1: Write the failing integration test**

Add to `MatterEngine3/tests/part_graph_integration_tests.cpp` (mirror its existing temp-schemas-dir fixture), register in `main()`:

```cpp
static void test_lod_variant_sidecar() {
    // Opted-in childless schema in a temp schemas dir; real host + HostBaker.
    write_schema("BudgetGrass",
        "class BudgetGrass extends Part {\n"
        "  static params = { seed: 0, lodBudget: 1.0 };\n"
        "  static lodBudgets = [1.0, 0.5];\n"
        "  static lodAnchorSize = 0.5;\n"
        "  build(p) {\n"
        "    const n = Math.max(1, Math.ceil(p.lodBudget * 4));\n"
        "    this.fill(1);\n"
        "    for (let i = 0; i < n; ++i) {\n"
        "      this.beginShape(SHAPE.strip);\n"
        "      this.vertex(i, 0, 0); this.vertex(i + 1, 0, 0); this.vertex(i, 1, 0);\n"
        "      this.endShape();\n"
        "    }\n"
        "  }\n"
        "}\n");
    auto ir = install_roots({"BudgetGrass"});          // existing fixture helper
    assert(ir.ok);
    uint64_t root = ir.root_hashes[0];

    part_asset::LodVariants v;
    std::string sidecar = parts_dir() + "/" + part_asset::cache_path_lods(root);
    assert(part_asset::load_lod_sidecar(sidecar, v));
    assert(v.anchor_size == 0.5);
    assert(v.budgets.size() == 2 && v.hashes.size() == 2);
    assert(v.hashes[0] == root);                       // budget 1.0 == the main bake
    assert(v.hashes[1] != root);                       // variant is a distinct part
    // The variant .part exists on disk.
    std::ifstream in(parts_dir() + "/" + part_asset::cache_path_resolved(v.hashes[1]),
                     std::ios::binary);
    assert(in.good());

    // Re-install: everything cached, sidecar untouched (no re-bake).
    auto ir2 = install_roots({"BudgetGrass"});
    assert(ir2.ok && ir2.baked.empty());

    // A schema WITHOUT lodBudgets gets no sidecar.
    write_schema("PlainBox", "class PlainBox extends Part { static params = {};\n"
                             "  build(p) { this.fill(1); this.beginShape(SHAPE.strip);\n"
                             "  this.vertex(0,0,0); this.vertex(1,0,0); this.vertex(0,1,0);\n"
                             "  this.endShape(); } }\n");
    auto ir3 = install_roots({"PlainBox"});
    assert(ir3.ok);
    std::ifstream nos(parts_dir() + "/" +
                      part_asset::cache_path_lods(ir3.root_hashes[0]));
    assert(!nos.good());
    printf("  test_lod_variant_sidecar OK\n");
}
```

- [ ] **Step 2: Run, verify failure**

```bash
cd MatterEngine3/tests && touch part_graph_integration_tests.cpp && make part_graph_integration_tests && ./part_graph_integration_tests
```

Expected: compile FAILS — `cache_path_lods` / `load_lod_sidecar` / `LodVariants` undeclared.

- [ ] **Step 3: Implement the part_asset pieces**

`MatterEngine3/include/part_asset_v2.h` — next to `cache_path_flat` (line 59):

```cpp
// Sidecar listing a part's budget-LOD variant bakes: "parts/<16-hex>.lods".
// Written by HostBaker::bake_lod_variants for schemas exporting `static
// lodBudgets`; consumed by part_flatten to assemble a budget ladder instead
// of QEM. Text format: line 1 = anchor_size, then "<budget> <16-hex-hash>"
// per level, finest (1.0) first. Content-addressed alongside the .part: any
// source/param change changes the root hash and orphans the stale sidecar.
std::string cache_path_lods(uint64_t resolved_hash);

struct LodVariants {
    double                anchor_size = 0.0;
    std::vector<double>   budgets;   // parallel to hashes, finest first
    std::vector<uint64_t> hashes;
};
// False if the file is missing or unparseable (callers fall back to QEM).
bool load_lod_sidecar(const std::string& path, LodVariants& out);
```

`MatterEngine3/src/part_asset_v2.cpp` — next to `cache_path_flat`'s definition:

```cpp
std::string cache_path_lods(uint64_t resolved_hash) {
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)resolved_hash);
    return std::string("parts/") + hex + ".lods";
}

bool load_lod_sidecar(const std::string& path, LodVariants& out) {
    std::ifstream in(path);
    if (!in) return false;
    LodVariants v;
    if (!(in >> v.anchor_size)) return false;
    double budget; std::string hex;
    while (in >> budget >> hex) {
        if (hex.size() != 16) return false;
        v.budgets.push_back(budget);
        v.hashes.push_back((uint64_t)strtoull(hex.c_str(), nullptr, 16));
    }
    if (v.hashes.empty()) return false;
    out = std::move(v);
    return true;
}
```

(Match this file's include style for `<fstream>`/`<cstdlib>`.)

- [ ] **Step 4: Implement the Baker seam + install hook**

`MatterEngine3/include/part_graph.h` — add to the `Baker` struct after `bake`:

```cpp
    // Optional: bake budget-LOD variants for an opted-in part (schemas exporting
    // `static lodBudgets`). Called for EVERY node after its bake (or cache hit),
    // so a missing sidecar regenerates even on fully-cached installs. Default
    // no-op keeps logic-test fakes untouched. False = hard bake failure.
    virtual bool bake_lod_variants(const std::string& source, const Params& params,
                                   const std::vector<uint64_t>& child_hashes,
                                   uint64_t resolved_hash) {
        (void)source; (void)params; (void)child_hashes; (void)resolved_hash;
        return true;
    }
```

and to `HostBaker` the override declaration:

```cpp
    bool bake_lod_variants(const std::string& source, const Params& params,
                           const std::vector<uint64_t>& child_hashes,
                           uint64_t resolved_hash) override;
```

`MatterEngine3/src/part_graph.cpp` — restructure the topo bake loop (lines 194-202):

```cpp
    for (uint64_t key : topo) {
        const InternalNode& n = memo.at(key);
        if (baker_.cached(n.resolved_hash)) {
            ++result.hits;
        } else {
            if (!baker_.bake(n.source, n.params, n.child_hashes, n.child_modules,
                             n.child_params, n.resolved_hash)) {
                result.error = "bake failed for part: " + n.module;
                return result;
            }
            result.baked.push_back(n.resolved_hash);
        }
        if (!baker_.bake_lod_variants(n.source, n.params, n.child_hashes,
                                      n.resolved_hash)) {
            result.error = "lod-variant bake failed for part: " + n.module;
            return result;
        }
    }
```

HostBaker impl in the `MATTER_HAVE_SCRIPT_HOST` section (after `HostBaker::bake`, ~line 311):

```cpp
bool HostBaker::bake_lod_variants(const std::string& source, const Params& params,
                                  const std::vector<uint64_t>& child_hashes,
                                  uint64_t resolved_hash) {
    script_host::ScriptHost::LodBudgetSpec spec = host_.eval_lod_budgets(source);
    if (spec.budgets.empty()) return true;                    // not opted in
    if (!child_hashes.empty()) {
        printf("HostBaker: lodBudgets on a part with children is unsupported; skipping\n");
        return true;
    }
    const std::string sidecar = parts_dir_ + "/" + part_asset::cache_path_lods(resolved_hash);
    { std::ifstream in(sidecar); if (in.good()) return true; }  // content-addressed: done

    std::vector<uint64_t> variant_hashes;
    for (double b : spec.budgets) {
        if (b >= 1.0) {
            // Full budget == the main bake: lodBudget defaults to 1.0 in the
            // schema's static params, so the merged params (and hash) match.
            variant_hashes.push_back(resolved_hash);
            continue;
        }
        Params p2 = params;
        p2["lodBudget"] = ParamValue::number(b);
        script_host::BakeResult r = host_.bake_source(source, params_to_json(p2), {});
        if (!r.error.ok) return false;
        variant_hashes.push_back(r.resolved_hash);
    }

    const std::string tmp = sidecar + ".tmp";
    {
        std::ofstream o(tmp);
        o << spec.anchor_size << "\n";
        for (size_t i = 0; i < spec.budgets.size(); ++i) {
            char hex[17];
            snprintf(hex, sizeof hex, "%016llx", (unsigned long long)variant_hashes[i]);
            o << spec.budgets[i] << " " << hex << "\n";
        }
        if (!o.good()) return false;
    }
    return std::rename(tmp.c_str(), sidecar.c_str()) == 0;
}
```

(Match `ParamValue::number` to its actual factory in part_graph.h; `params_to_json` is file-local and already used by `bake`.)

- [ ] **Step 5: Run tests (integration + logic suites — FakeBaker must be untouched)**

```bash
cd MatterEngine3/tests && touch part_graph_integration_tests.cpp part_graph_tests.cpp && make part_graph_integration_tests part_graph_tests && ./part_graph_integration_tests && ./part_graph_tests
```

Expected: PASS (default no-op keeps part_graph_tests green with zero edits).

- [ ] **Step 6: Rebuild (header change ⇒ clean) and commit**

```bash
cd MatterEngine3/viewer && make clean && make viewer && make windows
git add MatterEngine3/include/part_graph.h MatterEngine3/src/part_graph.cpp MatterEngine3/include/part_asset_v2.h MatterEngine3/src/part_asset_v2.cpp MatterEngine3/tests/part_graph_integration_tests.cpp
git commit -m "feat: bake budget-LOD variants + .lods sidecar in PartGraph install (Stage 3)"
```

### Task 14: Flatten assembles the budget ladder + final sweep

**Files:**
- Modify: `MatterEngine3/src/part_flatten.cpp` (sidecar branch before split_clusters)
- Test: `MatterEngine3/tests/part_flatten_tests.cpp`

**Interfaces:**
- Consumes: `part_asset::load_lod_sidecar` / `cache_path_lods` (Task 13); the existing `Gatherer` (loads a part's level-0 mesh + TriEx), `radius`, `save_flat_v3`, and `FlattenTargets::pixel_angle/pixel_budget`.
- Produces: for a part with a sidecar, `flatten_part` writes a SINGLE-cluster flat artifact whose level i is variant i's full mesh (native TriEx — no decimation, no reprojection). Thresholds: `thr[0] = 2 * radius * pixel_angle * pixel_budget / anchor_size` (full res holds until an anchor-sized feature is ~2 px), `thr[i] = thr[0] / 2^i`, last level = 0. Below the last level the normal sub-pixel floor applies (grass may vanish at extreme distance; Task 10 keeps terrain visible). The viewer's existing v3/v4 cluster loader consumes it with zero changes.

- [ ] **Step 1: Write the failing test**

Add to `MatterEngine3/tests/part_flatten_tests.cpp`, register in `main()`:

```cpp
static void test_budget_ladder_assembly() {
    // Two hand-built childless parts standing in for budget variants:
    // "full" (larger) and "low" (smaller), written with save_v2 via the
    // existing fixture writers. A hand-written sidecar binds them.
    uint64_t full_hash = write_small_sphere_part(cache_dir);           // e.g. ~300 tris
    uint64_t low_hash  = write_tiny_part(cache_dir);                   // e.g. ~40 tris
    {
        std::ofstream o(cache_dir + "/" + part_asset::cache_path_lods(full_hash));
        char hex[17];
        o << 0.5 << "\n";
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)full_hash);
        o << 1.0 << " " << hex << "\n";
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)low_hash);
        o << 0.3 << " " << hex << "\n";
    }

    auto res = part_flatten::flatten_part(cache_dir, full_hash);
    assert(res.ok);
    assert(res.clusters == 1);                    // budget ladder: single cluster
    assert(res.levels == 2);

    BLASManager blas; TLASManager tlas(4);
    std::vector<part_asset::FlatCluster> clusters;
    std::string p = cache_dir + "/" + part_asset::cache_path_flat(full_hash);
    assert(part_asset::load_flat_v3(p, full_hash, blas, tlas, clusters));
    assert(clusters.size() == 1);
    const auto& lods = clusters[0].lods;
    assert(lods.size() == 2);
    // Level tri counts match the variant parts exactly (no decimation).
    size_t t0 = (size_t)blas.get_entries()[lods[0].blas_indices[0]]->tri_count;
    size_t t1 = (size_t)blas.get_entries()[lods[1].blas_indices[0]]->tri_count;
    assert(t0 > t1);
    // Thresholds: fine->coarse descending, last == 0 (never disappears above
    // the floor), thr0 anchored at anchor_size ~2 px.
    assert(lods[0].screen_size_threshold > 0.0f);
    assert(lods[1].screen_size_threshold == 0.0f);
    // Native TriEx present at both levels.
    assert(blas.get_entries()[lods[0].blas_indices[0]]->tri_extra != nullptr);
    assert(blas.get_entries()[lods[1].blas_indices[0]]->tri_extra != nullptr);
    printf("  test_budget_ladder_assembly OK\n");
}
```

(As in Task 7, match the BLAS entry field names to `blas_manager.hpp`; ensure the fixture parts carry TriEx — the existing writers do.)

- [ ] **Step 2: Run, verify failure**

```bash
cd MatterEngine3/tests && touch part_flatten_tests.cpp && make part_flatten_tests && ./part_flatten_tests
```

Expected: FAIL — `res.clusters == 1` fires (flatten currently QEM-ladders the "full" part and ignores the sidecar; a ~300-tri sphere still yields 1 cluster but `res.levels == 2` and the tri-count equality will not hold — whichever asserts first).

- [ ] **Step 3: Implement**

`MatterEngine3/src/part_flatten.cpp` — add `#include` for the sidecar API if not already covered by part_asset_v2.h, then a file-static helper above `flatten_part`:

```cpp
// Stage 3: assemble a flat artifact from budget-variant bakes (sidecar) instead
// of QEM. Single cluster; level i = variant i's full mesh with native TriEx.
static FlattenResult flatten_budget_ladder(const std::string& cache_root,
                                           uint64_t root_hash,
                                           const FlattenTargets& targets,
                                           Gatherer& g0, float radius,
                                           const part_asset::LodVariants& v) {
    FlattenResult res;
    res.full_tris = g0.tris().size();

    BLASManager blas;
    TLASManager tlas(4);
    part_asset::LodLevels lods;

    // Full res holds until an anchor-sized feature (a grass blade) is ~2 px;
    // each coarser level halves the switch size (same ratio-2 spirit as the
    // mesh ladder). Selection metric is the PART's projected size
    // (radius / dist), so convert: at blade==2px, dist = anchor/(2*pixel_angle)
    // => part psize = 2 * radius * pixel_angle / anchor.
    const float anchor = (v.anchor_size > 0.0) ? (float)v.anchor_size : radius;
    const float thr0 = 2.0f * radius * targets.pixel_angle * targets.pixel_budget / anchor;

    // Union AABB over ALL variants (widened blades overhang the level-0 box).
    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
    auto acc = [&](const std::vector<Tri>& ts) {
        for (const auto& t : ts) {
            const float3* vs[3] = { &t.vertex0, &t.vertex1, &t.vertex2 };
            for (int k = 0; k < 3; ++k) {
                mn[0]=std::fmin(mn[0],vs[k]->x); mx[0]=std::fmax(mx[0],vs[k]->x);
                mn[1]=std::fmin(mn[1],vs[k]->y); mx[1]=std::fmax(mx[1],vs[k]->y);
                mn[2]=std::fmin(mn[2],vs[k]->z); mx[2]=std::fmax(mx[2],vs[k]->z);
            }
        }
    };

    for (size_t i = 0; i < v.hashes.size(); ++i) {
        std::vector<Tri> tris; std::vector<TriEx> ex;
        if (v.hashes[i] == root_hash) {
            tris = g0.tris(); ex = g0.triex();
        } else {
            Gatherer gi(cache_root, targets);
            if (!gi.gather(v.hashes[i], kIdentity, 0, res.error)) return res;
            tris = gi.tris(); ex = gi.triex();
        }
        if (tris.empty()) { res.error = "flatten: empty budget variant"; return res; }
        acc(tris);

        const TriEx* exp = ex.empty() ? nullptr : ex.data();
        BLASHandle h = blas.register_triangles(tris.data(), (int)tris.size(), exp);
        uint32_t idx = UINT32_MAX;
        const auto& entries = blas.get_entries();
        for (size_t k = 0; k < entries.size(); ++k)
            if (entries[k]->handle == h) { idx = (uint32_t)k; break; }
        if (idx == UINT32_MAX) { res.error = "flatten: BLAS registration failed"; return res; }

        part_asset::LodLevel lvl;
        lvl.screen_size_threshold =
            (i + 1 < v.hashes.size()) ? thr0 / (float)(1u << i) : 0.0f;
        lvl.blas_indices.push_back(idx);
        lods.push_back(std::move(lvl));
        res.coarsest_tris = tris.size();
    }

    part_asset::FlatCluster fc;
    for (int a = 0; a < 3; ++a) { fc.aabb_min[a] = mn[a]; fc.aabb_max[a] = mx[a]; }
    fc.lods = std::move(lods);
    std::vector<part_asset::FlatCluster> clusters;
    clusters.push_back(std::move(fc));

    res.levels   = v.hashes.size();
    res.clusters = 1;

    const std::string out_path = cache_root + "/" + part_asset::cache_path_flat(root_hash);
    if (!part_asset::save_flat_v3(out_path, blas, tlas, clusters, root_hash)) {
        res.error = "flatten: save_flat_v3 failed for " + out_path;
        return res;
    }
    res.ok = true;
    return res;
}
```

In `flatten_part`, after the radius computation (line ~210) and BEFORE `split_clusters`:

```cpp
    // Opt-in procedural budget ladder (Stage 3): a variant sidecar next to the
    // part means the baker re-ran the generator at reduced budgets — assemble
    // the ladder from those meshes instead of QEM (aggregate thin geometry
    // like grass decimates to sparseness, not coarseness).
    {
        part_asset::LodVariants variants;
        const std::string sidecar =
            cache_root + "/" + part_asset::cache_path_lods(root_hash);
        if (part_asset::load_lod_sidecar(sidecar, variants))
            return flatten_budget_ladder(cache_root, root_hash, targets,
                                         g, radius, variants);
    }
```

- [ ] **Step 4: Run the full test battery**

```bash
cd MatterEngine3/tests && touch part_flatten_tests.cpp && make part_flatten_tests grass_lod_tests viewer_logic_tests part_graph_integration_tests script_host_tests && ./part_flatten_tests && ./grass_lod_tests && ./viewer_logic_tests && ./part_graph_integration_tests && ./script_host_tests
```

Expected: all PASS.

- [ ] **Step 5: Rebuild, final sweep, commit**

Grass hashes changed in Task 11, so the meadow's first connect re-bakes grass + variants + flats automatically (content addressing — no cache wipe needed).

```bash
cd MatterEngine3/viewer && make clean && make viewer && make windows
cd ../.. && MatterEngine3/tools/meadow_sweep.sh stage3
```

Expected (package exit criterion): all five rows < 16 ms at pixel_budget 1.0. If not met, the gap goes to the GPU-driven-culling backlog item — record it, don't scope-creep.

```bash
git add MatterEngine3/src/part_flatten.cpp MatterEngine3/tests/part_flatten_tests.cpp MatterEngine3/docs/perf/meadow_sweep.csv
git commit -m "feat: flatten assembles grass budget-LOD ladder from variant bakes (Stage 3); final sweep"
```

---

## Final Verification (after Task 14)

1. Full headless battery: `cd MatterEngine3/tests && make && ./run` each suite (part_flatten_tests, viewer_logic_tests, part_graph_tests, part_graph_integration_tests, script_host_tests, grass_lod_tests, plus the pre-existing composition/tree_bake/example_world suites).
2. Screenshot set: drive the five sweep cameras via FIFO `cam` + `shot` and eyeball midfield grass coverage + popping vs. the baseline captures.
3. `make windows` output is fresh (Task 14 step 5) — Jack does the interactive fly-through on `viewer.exe` (grass coverage, LOD pops, terrain never vanishing).
4. Benchmark gate: `docs/perf/meadow_sweep.csv` has committed rows for baseline, stage1-O2, stage1, stage2, stage3.

## Out of Scope (tracked)

- GPU-driven culling/instancing — NEXT package (explicit backlog, ahead of imposters).
- Imposter far-field tier; LOD crossfade/dither; converting leaves/twigs to budgets.
- MatterSurfaceLib changes of any kind.



