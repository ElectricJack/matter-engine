# MatterEngine3 world picker panel

## Goal

Add an ImGui panel to the MatterEngine3 viewer that lists every available world as a button and switches to it on click. Switching implicitly unloads the current world. Replaces the `MATTER_WORLD` env-var workflow for interactive use (the env var still seeds the initial selection).

Non-goals: no "true empty" unloaded state; no camera reset on switch; no runtime add/remove of worlds.

## World enumeration

At startup, before the first connect, scan `../examples/`. For each subdirectory `<demo>/` that contains both `schemas/` and `WorldData/`, iterate the subdirectories of `WorldData/`; each is one world. Result:

```cpp
struct WorldEntry {
    std::string label;           // display name, e.g. "Demo"
    std::string schemas_dir;     // "../examples/world_demo/schemas"
    std::string world_data_dir;  // "../examples/world_demo/WorldData"
    std::string world_name;      // "Demo"
};
std::vector<WorldEntry> worlds;  // sorted by label
```

`label` is the `WorldData/` subdirectory name. On today's tree this produces Demo, Meadow, Primitives.

`MATTER_WORLD` still applies at startup only: match its value against `world_name` case-insensitively; if it matches, seed `cfg` from that entry and set `stats.world_current` to its index. If it doesn't match (or the env var is unset), fall through to index 0.

If the scan yields zero entries, print an error and fail startup — the viewer has nothing to load. This is a bug in the source tree, not a runtime user error.

## UI

New standalone ImGui window titled `"Worlds"`, drawn by a new `Ui::draw_worlds_panel(const std::vector<WorldEntry>& worlds, ViewerStats& stats)`. Called from `main.cpp` between `ui.draw_debug_panel(stats)` and `ui.draw_camera_panel(...)`.

Position defaults to `(20, 20)` on first use (`ImGuiCond_FirstUseEver`), independent of the Viewer Debug and Camera panels.

Contents:

- One `ImGui::Button(w.label)` per entry, top-to-bottom.
- The currently-loaded world (index == `stats.world_current`) is wrapped in `ImGui::BeginDisabled(true)/EndDisabled()` so it renders greyed-out and non-clickable.
- On click of any other button, set `stats.world_switch_requested = <that index>`.

No labels, headers, or extra text — the window is a plain button column.

`WorldEntry` moves to `ui.h` so the panel signature can reference it without pulling in local_provider internals.

## ViewerStats additions

```cpp
int  world_current = 0;              // read-only for UI; index into the worlds list
int  world_switch_requested = -1;    // panel writes target index; main clears after handling
```

## Switch flow in main.cpp

Handled inside the render loop next to the existing `reload_requested` block. When `world_switch_requested >= 0`:

1. Read the target index; clear the request field.
2. Copy that entry's `schemas_dir`, `world_data_dir`, `world_name` into `cfg`.
3. Recreate the provider: `provider = std::make_unique<LocalProvider>(cfg);` — `LocalProvider` takes cfg by value at construction, so mutating `cfg` alone isn't enough.
4. If `camera_capture` is on, disable it and re-enable the cursor (matches the existing reload behavior — a failure mid-switch must not strand the cursor).
5. Call `connect_sequence()`. This already fully tears down and rebuilds `store`, `composer`, `raster`, `probe_tex`, `lods`, `sky_clear`, world lights, and reconciles/fetches parts. On failure, print and break the loop (same policy as reload).
6. Set `stats.world_current = idx`.
7. Call the new per-world resolver-defaults helper (below).

Camera is intentionally *not* reset — the user has "Reset View" in the Camera panel if they want to reframe.

## Per-world resolver defaults

Today the Meadow-specific resolver knobs are set once outside `connect_sequence`, using a local `meadow` bool computed from the initial `MATTER_WORLD`. Extract into:

```cpp
static void apply_world_resolver_defaults(
    const std::string& world_name,
    SectorLodResolver& sec,
    ViewerStats& stats);
```

Behavior:
- `world_name == "Meadow"` → `sec.set_pixel_budget` unchanged (that's a user-facing slider), `kActiveRadius=400`, `kMinProjectedSize=0.0015`, `stats.resolver_choice=1`.
- Anything else → `kActiveRadius=64`, `kMinProjectedSize=0.0f`, `stats.resolver_choice=0`.

Both `SectorLodResolver::set_active_radius(float)` and `set_min_projected_size(float)` already exist (`sector_resolver.h:48-49`), so no resolver API change is needed. The helper just calls both setters plus mutates `stats.resolver_choice`.

Call the helper once at startup after the first successful `connect_sequence` (replacing the current inline block) and again after every successful mid-run switch.

## Files touched

- `MatterEngine3/viewer/ui.h` — add `WorldEntry` struct, declare `draw_worlds_panel`, add `world_current` / `world_switch_requested` to `ViewerStats`.
- `MatterEngine3/viewer/ui.cpp` — implement `draw_worlds_panel`.
- `MatterEngine3/viewer/main.cpp` — scan examples at startup, seed `cfg` (respect `MATTER_WORLD`), call `draw_worlds_panel`, handle `world_switch_requested`, use `apply_world_resolver_defaults`.

No changes to `local_provider.*`, `world_composer.*`, `part_store.*`, or shader code. No new dependencies.

## Testing

Interactive only — this is a UI feature. Verify:

1. Launch viewer with no env: `Worlds` panel shows Demo (disabled), Meadow, Primitives. Click Meadow → scene switches, Meadow now disabled, Demo/Primitives clickable. Meadow resolver defaults (SectorLod, wider radius) applied.
2. Launch with `MATTER_WORLD=meadow`: Meadow starts disabled; switching to Demo relaxes resolver to PassThrough defaults.
3. Switch back and forth several times: no crashes, no GL leaks visible in the HUD's active-instance counts (they should reflect the newly-loaded world each time).
4. Existing "Reload world" button still reloads the current world in place.
5. Windows build (`make windows`) compiles cleanly and the ported viewer.exe shows the same panel.
