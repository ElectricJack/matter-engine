# World picker panel — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a standalone ImGui "Worlds" panel to the MatterEngine3 viewer that enumerates available example worlds and lets the user switch between them at runtime (implicit unload on switch).

**Architecture:** Startup scans `../examples/*/WorldData/*` to build a `std::vector<WorldEntry>`. A new `Ui::draw_worlds_panel` renders one ImGui button per entry (current world disabled). On click, the panel writes an index to `ViewerStats::world_switch_requested`; the main loop mutates `cfg`, recreates the `LocalProvider`, re-runs the existing `connect_sequence` lambda (which fully rebuilds `store`/`composer`/`raster`/`probe_tex`/`lods`), then reapplies per-world resolver defaults.

**Tech Stack:** C++17, ImGui, raylib, existing `LocalProvider`/`WorldComposer`/`RasterComposer` machinery, MinGW cross-compile for Windows.

## Global Constraints

- Codebase rule: run `make windows` after any engine/viewer code change — stale `viewer.exe` silently ships old engine (per user memory).
- Codebase rule: for header/struct changes, `rm -rf build/windows` before rebuild — defensive against silent crashes even though `-MMD -MP` header dep tracking is now on.
- Codebase rule: viewer isn't part of the headless test sweep (needs GL context) — verification is via runtime `printf` and interactive click-through, not a headless test binary.
- Behavior: no camera reset on world switch; the user has "Reset View" in the Camera panel.
- Behavior: failure of `connect_sequence` during a switch follows the same policy as the existing reload path — print the error and `break` the main loop.
- Behavior: `MATTER_WORLD` env var still applies at startup only — seeds the initial world by case-insensitive match against `world_name`; unknown value falls through to index 0.
- Scope: no "true empty" unloaded state; no runtime add/remove of worlds; no changes to `local_provider.*`, `world_composer.*`, `part_store.*`, `sector_resolver.h`, or shader code.

---

### Task 1: Header changes (types, stats fields, declarations)

**Files:**
- Modify: `MatterEngine3/viewer/ui.h`

**Interfaces:**
- Produces:
  - `struct viewer::WorldEntry { std::string label; std::string schemas_dir; std::string world_data_dir; std::string world_name; };`
  - `std::vector<viewer::WorldEntry> viewer::scan_worlds(const std::string& examples_root);`
  - `void viewer::Ui::draw_worlds_panel(const std::vector<viewer::WorldEntry>& worlds, viewer::ViewerStats& stats);`
  - `int viewer::ViewerStats::world_current` (default 0)
  - `int viewer::ViewerStats::world_switch_requested` (default -1)

- [ ] **Step 1: Add `<string>` and `<vector>` includes and the `WorldEntry` struct**

At the top of `ui.h`, after the existing includes (`<cstdint>`, `"raylib.h"`), add:

```cpp
#include <string>
#include <vector>
```

Then inside `namespace viewer {` and above `struct ViewerStats`, add:

```cpp
// One available world for the runtime picker. Populated by scan_worlds at
// startup; consumed by draw_worlds_panel and the main-loop switch handler.
struct WorldEntry {
    std::string label;           // display name (WorldData/ subdir name)
    std::string schemas_dir;     // e.g. "../examples/world_demo/schemas"
    std::string world_data_dir;  // e.g. "../examples/world_demo/WorldData"
    std::string world_name;      // e.g. "Demo"
};

// Scan a root like "../examples" for available worlds. Every subdirectory
// <demo>/ that contains both schemas/ and WorldData/ contributes one entry
// per subdirectory of WorldData/. Sorted by label.
std::vector<WorldEntry> scan_worlds(const std::string& examples_root);
```

- [ ] **Step 2: Extend `ViewerStats` with the two new fields**

Inside `struct ViewerStats`, immediately after `float pixel_budget = 1.0f;`, add:

```cpp
    // World picker: main sets `world_current` after each connect; panel writes
    // `world_switch_requested` (index into the enumerated worlds list, -1 = none).
    int  world_current = 0;
    int  world_switch_requested = -1;
```

- [ ] **Step 3: Declare `draw_worlds_panel` on the `Ui` class**

Inside `class Ui`, after the existing `draw_camera_panel(Camera3D& cam);` line, add:

```cpp
    // Standalone panel listing available worlds as buttons. Clicking a non-current
    // world sets stats.world_switch_requested; main handles the swap next frame.
    void draw_worlds_panel(const std::vector<WorldEntry>& worlds, ViewerStats& stats);
```

- [ ] **Step 4: Build viewer to confirm the header compiles**

Run:
```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterEngine3/viewer" && make viewer 2>&1 | tail -20
```

Expected: undefined-reference link errors for `viewer::scan_worlds` and `viewer::Ui::draw_worlds_panel` (that's fine — Task 2 and Task 3 add the definitions). Compile stage must succeed — no `ui.h`-related compile errors.

- [ ] **Step 5: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && git add MatterEngine3/viewer/ui.h && git commit -m "feat(me3-viewer): declare WorldEntry, scan_worlds, draw_worlds_panel

Header-only step: types + stats fields for the runtime world picker.
Definitions land in the next commits."
```

---

### Task 2: Implement `scan_worlds`

**Files:**
- Modify: `MatterEngine3/viewer/ui.cpp`

**Interfaces:**
- Consumes: `WorldEntry`, `scan_worlds` decl from Task 1.
- Produces: definition of `scan_worlds` returning a sorted, deduped list.

- [ ] **Step 1: Add `<algorithm>`, `<filesystem>`, `<system_error>` includes to ui.cpp**

At the top of `ui.cpp`, after `#include "ui.h"` and `#include <cmath>`, add:

```cpp
#include <algorithm>
#include <filesystem>
#include <system_error>
```

- [ ] **Step 2: Implement `scan_worlds` at the top of the `viewer` namespace body**

Immediately below `namespace viewer {` in `ui.cpp`, before `void Ui::setup()`, add:

```cpp
std::vector<WorldEntry> scan_worlds(const std::string& examples_root) {
    namespace fs = std::filesystem;
    std::vector<WorldEntry> out;
    std::error_code ec;

    // examples_root/<demo>/
    for (auto it = fs::directory_iterator(examples_root, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path demo = it->path();
        if (!fs::is_directory(demo, ec)) continue;

        const fs::path schemas   = demo / "schemas";
        const fs::path world_data = demo / "WorldData";
        if (!fs::is_directory(schemas, ec) || !fs::is_directory(world_data, ec)) continue;

        // examples_root/<demo>/WorldData/<world_name>/
        std::error_code ec2;
        for (auto wit = fs::directory_iterator(world_data, ec2);
             !ec2 && wit != fs::directory_iterator(); wit.increment(ec2)) {
            const fs::path world_dir = wit->path();
            if (!fs::is_directory(world_dir, ec2)) continue;
            WorldEntry e;
            e.label          = world_dir.filename().string();
            e.schemas_dir    = schemas.string();
            e.world_data_dir = world_data.string();
            e.world_name     = world_dir.filename().string();
            out.push_back(std::move(e));
        }
    }

    std::sort(out.begin(), out.end(),
              [](const WorldEntry& a, const WorldEntry& b) { return a.label < b.label; });
    return out;
}
```

- [ ] **Step 3: Build viewer and check that only `draw_worlds_panel` remains unresolved**

Run:
```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterEngine3/viewer" && make viewer 2>&1 | tail -20
```

Expected: link error mentioning only `viewer::Ui::draw_worlds_panel` — `scan_worlds` no longer appears in the unresolved-symbols list.

- [ ] **Step 4: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && git add MatterEngine3/viewer/ui.cpp && git commit -m "feat(me3-viewer): scan examples/*/WorldData/* into a WorldEntry list

Uses std::filesystem to enumerate all worlds available under the examples
root. Sorted by label. Consumed by the picker panel in the next commit."
```

---

### Task 3: Implement `draw_worlds_panel` and call it from main

**Files:**
- Modify: `MatterEngine3/viewer/ui.cpp`
- Modify: `MatterEngine3/viewer/main.cpp` (add startup scan + panel call + verification printf)

**Interfaces:**
- Consumes: `WorldEntry`, `scan_worlds`, `ViewerStats::world_current`/`world_switch_requested`, `draw_worlds_panel` decl.
- Produces: panel visible with per-world buttons; click writes `stats.world_switch_requested`.

- [ ] **Step 1: Implement `draw_worlds_panel` in `ui.cpp`**

Append to `ui.cpp`, immediately before the closing `} // namespace viewer`:

```cpp
void Ui::draw_worlds_panel(const std::vector<WorldEntry>& worlds, ViewerStats& stats) {
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(200, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Worlds");

    for (int i = 0; i < (int)worlds.size(); ++i) {
        const bool is_current = (i == stats.world_current);
        if (is_current) ImGui::BeginDisabled(true);
        if (ImGui::Button(worlds[i].label.c_str())) {
            stats.world_switch_requested = i;
        }
        if (is_current) ImGui::EndDisabled();
    }

    ImGui::End();
}
```

- [ ] **Step 2: Scan worlds at startup in `main.cpp` and print the list**

In `main.cpp`, immediately after `Ui ui; ui.setup();` (currently at line ~38), add:

```cpp
    auto worlds = scan_worlds("../examples");
    printf("worlds available (%d):\n", (int)worlds.size());
    for (size_t i = 0; i < worlds.size(); ++i) {
        printf("  [%zu] %s  (%s / %s)\n",
               i, worlds[i].label.c_str(),
               worlds[i].schemas_dir.c_str(), worlds[i].world_data_dir.c_str());
    }
    if (worlds.empty()) {
        printf("FATAL: no worlds found under ../examples\n");
        return 1;
    }
```

- [ ] **Step 3: Call `draw_worlds_panel` inside the render loop in `main.cpp`**

Locate the block (currently ~line 327):
```cpp
            ui.begin_frame();
            ui.draw_debug_panel(stats);
            ui.draw_camera_panel(renderer.camera());
            ui.end_frame();
```
Insert `ui.draw_worlds_panel(worlds, stats);` between `draw_debug_panel` and `draw_camera_panel`:
```cpp
            ui.begin_frame();
            ui.draw_debug_panel(stats);
            ui.draw_worlds_panel(worlds, stats);
            ui.draw_camera_panel(renderer.camera());
            ui.end_frame();
```

- [ ] **Step 4: Add a temporary debug printf for click detection**

The switch flow isn't wired yet — we need to prove the panel is writing the request field. In the main render loop, immediately after the existing `if (stats.reload_requested) { ... }` block (~line 366), add:

```cpp
        if (stats.world_switch_requested >= 0) {
            printf("DBG: world switch requested -> [%d] %s\n",
                   stats.world_switch_requested,
                   worlds[stats.world_switch_requested].label.c_str());
            stats.world_switch_requested = -1;   // consume so it doesn't spam
        }
```

This block is temporary — Task 4 replaces it with the real switch handler.

- [ ] **Step 5: Build viewer**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterEngine3/viewer" && make viewer 2>&1 | tail -5
```

Expected: link succeeds, produces `./viewer`.

- [ ] **Step 6: Interactive verification**

Ask the user to run:
```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterEngine3/viewer" && ./viewer
```

Expected in stdout: `worlds available (3):` with three entries — Demo, Meadow, Primitives. Expected in the window: a "Worlds" panel top-left with three buttons; the first (Demo, per index 0) is greyed out. Clicking Meadow prints `DBG: world switch requested -> [1] Meadow`; clicking Primitives prints similarly. No crash.

Close the viewer before Task 4.

- [ ] **Step 7: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && git add MatterEngine3/viewer/ui.cpp MatterEngine3/viewer/main.cpp && git commit -m "feat(me3-viewer): draw Worlds panel; scan worlds at startup

Panel shows one button per enumerated world; clicking a non-current world
writes the target index to ViewerStats. Main-loop switch handler in the
next commit — a temporary DBG printf proves the click plumbing."
```

---

### Task 4: Wire the switch flow and per-world resolver defaults

**Files:**
- Modify: `MatterEngine3/viewer/main.cpp`

**Interfaces:**
- Consumes: `WorldEntry`, `scan_worlds` output, `ViewerStats::world_switch_requested`, `connect_sequence` lambda, `LocalProviderConfig cfg`, `SectorLodResolver sec`.
- Produces: fully working world switching + `MATTER_WORLD` seeding + `apply_world_resolver_defaults(name, sec, stats)` helper.

- [ ] **Step 1: Seed `cfg` from the scanned list + `MATTER_WORLD`**

Replace the existing hard-coded cfg block in `main.cpp` (currently ~lines 64-82):

```cpp
    LocalProviderConfig cfg;
    cfg.schemas_dir    = "../examples/world_demo/schemas";
    cfg.world_data_dir = "../examples/world_demo/WorldData";
    cfg.world_name     = "Demo";
    cfg.shared_lib_dir = "../shared-lib";
    cfg.cache_root     = "cache";   // persistent: viewer/cache/parts/<hash>.part

    // MATTER_WORLD=primitives switches to the primitive-demo gallery (every DSL
    // op) instead of the default tree scene; local_provider scatters it by name.
    const char* world_env = getenv("MATTER_WORLD");
    if (world_env && std::string(world_env) == "primitives") {
        cfg.schemas_dir    = "../examples/primitive_demo/schemas";
        cfg.world_data_dir = "../examples/primitive_demo/WorldData";
        cfg.world_name     = "Primitives";
    }
    // MATTER_WORLD=meadow loads the dense meadow benchmark world (same
    // world_demo schemas; the Meadow manifest root carries the expand flag).
    const bool meadow = world_env && std::string(world_env) == "meadow";
    if (meadow) cfg.world_name = "Meadow";
```

with:

```cpp
    // Pick the initial world from the scanned list. MATTER_WORLD (case-
    // insensitive match against world_name) overrides the default; unknown
    // value falls through to index 0.
    int initial_world = 0;
    if (const char* world_env = getenv("MATTER_WORLD")) {
        std::string want = world_env;
        std::transform(want.begin(), want.end(), want.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        for (size_t i = 0; i < worlds.size(); ++i) {
            std::string have = worlds[i].world_name;
            std::transform(have.begin(), have.end(), have.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (have == want) { initial_world = (int)i; break; }
        }
    }

    LocalProviderConfig cfg;
    cfg.schemas_dir    = worlds[initial_world].schemas_dir;
    cfg.world_data_dir = worlds[initial_world].world_data_dir;
    cfg.world_name     = worlds[initial_world].world_name;
    cfg.shared_lib_dir = "../shared-lib";
    cfg.cache_root     = "cache";   // persistent: viewer/cache/parts/<hash>.part
    stats.world_current = initial_world;
```

Then add these includes near the top of `main.cpp` if not already present:

```cpp
#include <algorithm>   // std::transform
#include <cctype>      // std::tolower
```

Note: `stats` is declared *below* this block currently (around line 87). You must move the `ViewerStats stats{};` declaration up so it precedes `cfg` — or set `stats.world_current` right after the `ViewerStats stats{};` line. Simpler: after the block above, just remember `initial_world`, and set `stats.world_current = initial_world;` right after the existing `ViewerStats stats{};` line.

Use this simpler ordering — leave `ViewerStats stats{};` where it is, and immediately after that line add:

```cpp
    stats.world_current = initial_world;
```

- [ ] **Step 2: Add the `apply_world_resolver_defaults` helper**

Above `int main()`, at file scope inside the `using namespace viewer;` region, add:

```cpp
static void apply_world_resolver_defaults(const std::string& world_name,
                                          SectorLodResolver& sec,
                                          ViewerStats& stats) {
    // Per-world resolver knobs: the Meadow spans ~256x256 units and needs a
    // wider active radius plus sub-pixel culling; every other world uses the
    // tight defaults.
    if (world_name == "Meadow") {
        sec.set_active_radius(400.0f);
        sec.set_min_projected_size(0.0015f);   // ~1 px at 720p (fov/height)
        stats.resolver_choice = 1;             // SectorLod by default
    } else {
        sec.set_active_radius(64.0f);
        sec.set_min_projected_size(0.0f);
        stats.resolver_choice = 0;             // PassThrough by default
    }
}
```

- [ ] **Step 3: Replace the inline meadow block with a helper call**

In `main.cpp`, replace the existing resolver-setup block (currently ~lines 182-190):

```cpp
    PassThroughResolver pass;
    // Per-world resolver config: the Meadow spans ~256x256 units, so activate
    // sectors across the whole world and floor-cull sub-pixel parts (grass/
    // pebbles self-cull at distance; their epsilon ladders stop well above 1 px).
    const float kActiveRadius     = meadow ? 400.0f : 64.0f;
    const float kMinProjectedSize = meadow ? 0.0015f : 0.0f;   // ~1 px at 720p (fov/height)
    SectorLodResolver sec(16.0f, kActiveRadius);
    sec.set_min_projected_size(kMinProjectedSize);
    if (meadow) stats.resolver_choice = 1;   // SectorLod by default for the benchmark
```

with:

```cpp
    PassThroughResolver pass;
    // Constructor radius is overwritten immediately by apply_world_resolver_defaults;
    // the placeholder 64.0f keeps the resolver in a valid state before the first call.
    SectorLodResolver sec(16.0f, 64.0f);
    apply_world_resolver_defaults(cfg.world_name, sec, stats);
```

- [ ] **Step 4: Replace the temporary DBG block with the real switch handler**

Remove the Task-3 debug block:

```cpp
        if (stats.world_switch_requested >= 0) {
            printf("DBG: world switch requested -> [%d] %s\n",
                   stats.world_switch_requested,
                   worlds[stats.world_switch_requested].label.c_str());
            stats.world_switch_requested = -1;
        }
```

Replace with the real handler (placed at the same location — after `if (stats.reload_requested)`):

```cpp
        if (stats.world_switch_requested >= 0) {
            int idx = stats.world_switch_requested;
            stats.world_switch_requested = -1;
            const auto& w = worlds[idx];
            printf("world switch -> [%d] %s\n", idx, w.label.c_str());
            cfg.schemas_dir    = w.schemas_dir;
            cfg.world_data_dir = w.world_data_dir;
            cfg.world_name     = w.world_name;
            // LocalProvider takes cfg by value at construction — mutating cfg
            // alone doesn't reach the existing provider instance.
            provider = std::make_unique<LocalProvider>(cfg);
            // Re-enable the cursor before rebuilding so a failure can't strand it
            // (mirrors the reload path).
            if (camera_capture) { camera_capture = false; EnableCursor(); }
            if (!connect_sequence()) { printf("world switch failed; exiting\n"); break; }
            stats.world_current = idx;
            apply_world_resolver_defaults(cfg.world_name, sec, stats);
        }
```

- [ ] **Step 5: Build viewer**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterEngine3/viewer" && make viewer 2>&1 | tail -5
```

Expected: clean link. No new warnings beyond the pre-existing set.

- [ ] **Step 6: Interactive verification**

Ask the user to run the viewer and:
1. Confirm default startup shows Demo loaded (Demo button disabled).
2. Click "Meadow" → console prints `world switch -> [1] Meadow`, scene reloads as the meadow, Meadow button becomes disabled, Demo/Primitives become clickable. Instances-active count in the debug HUD reflects the meadow.
3. Click "Primitives" → console prints `world switch -> [2] Primitives`, scene shows the primitive gallery, resolver drops back to PassThrough defaults (check debug panel).
4. Click "Demo" → back to Demo.
5. Exit with Q or Esc; confirm no crash on shutdown.
6. Relaunch with `MATTER_WORLD=meadow ./viewer` → Meadow disabled from the start.

- [ ] **Step 7: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && git add MatterEngine3/viewer/main.cpp && git commit -m "feat(me3-viewer): wire runtime world switching

Startup seeds cfg from the scanned worlds list (MATTER_WORLD still applies).
World-switch handler in the main loop mutates cfg, rebuilds the provider,
re-runs connect_sequence, and reapplies per-world resolver defaults via a
new apply_world_resolver_defaults helper. Replaces the one-shot meadow
inline block."
```

---

### Task 5: Rebuild Windows viewer

**Files:**
- No source changes.

- [ ] **Step 1: Clean Windows build tree**

Header/struct additions warrant a clean windows rebuild per the codebase's silent-crash safety rule.

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterEngine3/viewer" && rm -rf build/windows
```

- [ ] **Step 2: Cross-compile the Windows viewer**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterEngine3/viewer" && make windows 2>&1 | tail -10
```

Expected: clean link ending with `x86_64-w64-mingw32-g++-posix ... -o viewer.exe`. Pre-existing warnings (misleading indentation, TriEx memset on non-trivial type) may still appear — no new warnings, no errors.

- [ ] **Step 3: Confirm viewer.exe timestamp**

```bash
ls -la "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterEngine3/viewer/viewer.exe"
```

Expected: mtime is within the last minute.

- [ ] **Step 4: Commit (only if something changed — normally nothing to commit here)**

`viewer.exe` is not tracked (per the build artefacts convention). This task is verification only — no commit expected. Confirm with:

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && git status
```

Expected: clean tree (no unstaged changes beyond untracked build artefacts already ignored).
