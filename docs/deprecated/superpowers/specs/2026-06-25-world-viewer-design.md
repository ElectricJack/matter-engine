# World Viewer Design

**Goal:** A GL window that renders the baked example world (terrain + trees + grass) through MatterSurfaceLib's existing raytraced TLAS/BLAS pipeline, architected as the foundation for the final game engine's client.

**Status:** Design approved 2026-06-25. Minimal feature scope, growth-minded module boundaries.

---

## Context and constraints

- **MatterSurfaceLib (MSL) is consumed READ-ONLY.** The viewer links MSL's render path (`BLASManager`, `TLASManager`, `raytrace_tlas_blas_processed.fs`, `bvh_tlas_common.glsl`) and reuses it unchanged. All new code lives under `MatterEngine3/viewer/`.
- **The client is always a pure client.** Per the dedicated-server authority model (see `2026-06-24-procedural-part-authoring-design.md`), the viewer never owns authoritative world state. It consumes a `WorldState` + `PartStore` obtained from a `WorldProvider`. Single-player wires that provider to a local in-process source; networked play swaps in a network-backed provider with no renderer changes.
- **This is the engine's seed, not a throwaway demo.** Feature scope is minimal (one window, free camera, the committed example world) but the module boundaries and extension seams are the real thing: the `WorldProvider` lifecycle, the `PartStore` content-addressed cache, and the `SectorResolver` seam where the spec's future sector-LOD pipeline plugs in.

---

## Architecture

Six units under `MatterEngine3/viewer/`, each with one responsibility and a narrow interface:

```
┌─ main ─────────────────────────────────────────────────────┐
│  GL window + input + frame loop; wires the units together   │
├─ world_source ─────────────────────────────────────────────┤
│  WorldProvider interface (connect/reconcile/fetch/deltas)   │
│  + LocalProvider concrete impl                              │
│  owns: WorldState (live placed-instance set)                │
├─ part_library ─────────────────────────────────────────────┤
│  PartStore: content-addressed cache of .part -> BLAS        │
│  (load_v2 each part hash into a BLASManager once)           │
├─ world_composer ───────────────────────────────────────────┤
│  WorldState + PartStore -> TLAS instances per frame         │
│  SectorResolver seam: which instances + which LOD this frame│
├─ renderer ─────────────────────────────────────────────────┤
│  reuses MSL BLASManager + TLASManager + raytrace shader     │
│  camera uniforms; fullscreen raytrace draw                  │
├─ ui (Dear ImGui) ──────────────────────────────────────────┤
│  debug/HUD overlay drawn after the raytrace pass            │
│  stats + camera/provider state; seam for future tools       │
└─────────────────────────────────────────────────────────────┘
```

Data flows downward each frame: `world_source` holds the authoritative-from-the-client's-view instance set, `part_library` supplies the geometry for each part hash, `world_composer` selects+transforms instances into the TLAS, and `renderer` traces it. The `ui` overlay draws last, on top of the traced frame.

---

## The `WorldProvider` lifecycle (approved)

The client never assumes its data is current — it reconciles against the source's hashes on connect. Because parts are content-addressed, "is anything out of date?" is a set-difference of hashes; no versioning or timestamps.

```cpp
// world_source.h
struct WorldManifestEntry {
    uint32_t instance_id;
    uint64_t part_hash;       // resolved hash of the placed part
    mat4     transform;       // world placement
};
struct WorldManifest {
    uint64_t world_root_hash;
    std::vector<WorldManifestEntry> instances;
};
struct WorldDelta {
    std::vector<WorldManifestEntry> added;    // new or moved (replace by instance_id)
    std::vector<uint32_t>           removed;  // instance_ids to drop
};

class WorldProvider {
public:
    virtual ~WorldProvider() = default;

    // Open a session; return the authoritative placed-instance set, hashes only (cheap).
    virtual bool connect(WorldManifest& out, std::string& err) = 0;

    // Given the manifest and what the client already has on disk, return the
    // set of part hashes the client is missing or has stale.
    virtual std::vector<uint64_t>
        reconcile(const WorldManifest& manifest, const PartStore& store) = 0;

    // Fetch only the missing .part blobs; client writes them into its PartStore.
    virtual bool fetch_parts(const std::vector<uint64_t>& want,
                             PartStore& store, std::string& err) = 0;

    // Stream of incremental world changes after the initial snapshot.
    // Returns false (empty) when no changes are pending.
    virtual bool poll_deltas(WorldDelta& out) = 0;
};
```

**Connect sequence (run once at startup, reusable on reconnect):**

1. `connect()` → `WorldManifest` (hashes + transforms only).
2. `reconcile(manifest, store)` → walks entries, asks `PartStore::has(part_hash)`, returns the "want" list.
3. `fetch_parts(want, store)` → source returns only the missing blobs; `PartStore` loads each into its `BLASManager`.
4. Build the initial `WorldState` from the manifest entries.
5. Each frame, `poll_deltas()` applies `added`/`removed` to the live `WorldState`.

**`LocalProvider` (the only concrete impl in the minimal viewer):**
Backs `connect()` with the SP-3 install path against a **persistent** content-addressed cache dir (e.g. `MatterEngine3/viewer/cache/parts/`, **not** a throwaway `/tmp` sandbox — this is the key difference from the `example_world` driver, which `rm -rf`s its sandbox each run). It runs `read_manifest` → `install`, then returns a `WorldManifest` whose entries carry each part's resolved hash. Because install is content-addressed:

- **First run (cold cache):** install bakes every part, writing `parts/<hash>.part`. `reconcile` reports all hashes as wanted; `fetch_parts` loads them.
- **Second run, nothing changed (warm cache):** install is an all-hits no-op (`InstallResult.baked.empty()`), every `.part` is already on disk, `reconcile` yields an **empty want-list**, `fetch_parts` is a no-op — the world reloads immediately, no rebake. (This is exactly the `example_world` "second install bakes nothing" behavior, made persistent.)
- **Source changed:** only the parts whose resolved hash changed (and their dependents) re-bake; `reconcile` wants only those.

This is the same content-addressed reconciliation the network path uses — "build once, then hash-check and skip" — so the local and network providers are identical in the renderer. The persistent on-disk `parts/` dir IS the cache the client carries between runs and between server sessions.

---

## Module responsibilities and interfaces

### `world_source` (`world_source.h` / `local_provider.cpp`)
- Defines `WorldProvider`, `WorldManifest`, `WorldDelta`, and the live `WorldState` (a `std::vector<WorldManifestEntry>` keyed/replaced by `instance_id`, plus apply-delta helpers).
- `LocalProvider` produces the manifest from the example-world bake.
- Depends on: `part_library` (for the `PartStore&` reconcile arg), the SP-3/SP-4 composition path for `LocalProvider` only.

### `part_library` (`part_store.h` / `part_store.cpp`)
- `PartStore`: maps `part_hash -> BLASHandle` via an owned `BLASManager`, backed by a **persistent on-disk cache dir** (`cache_path_resolved(hash)` → `parts/<hash>.part`). `has(hash)` (in-memory **or** on-disk presence — drives `reconcile`), `get_or_load(hash)` (`part_asset::load_v2` → `BLASManager::register_triangles`, memoized), and exposes the `BLASManager&` for the renderer to bind.
- Content-addressed and **durable across runs**: a hash loaded once is reused for every instance this run (terrain/tree/grass dedup is automatic), and a `.part` baked on a prior run is found on disk so the next launch skips the bake entirely.
- Depends on: SP-1 `part_asset_v2` (`load_v2`, `cache_path_resolved`), MSL `BLASManager` (read-only).

### `world_composer` (`world_composer.h` / `world_composer.cpp`)
- Per frame: takes `WorldState` + `PartStore` + camera, decides which instances are active and at which LOD, and records them into the `TLASManager` (`tlas.clear()` → `tlas.draw(blas_handle, material_id)` under each instance transform → `tlas.build(blas_manager)`).
- **`SectorResolver` seam** — a strategy object that answers "given the camera, which instances render this frame and at which LOD level?" Two impls ship in the viewer, **selectable live from the HUD** so they can be A/B'd against the same world:
  - `PassThroughResolver` — all instances active, LOD 0 (finest), no culling. The baseline / correctness reference.
  - `SectorLodResolver` — bins instances with `sector_grid::bin_instances`, chooses per-sector LOD via `lod_select::select_sector_lods` against the camera, and activates sectors by distance sphere (the `2026-06-24-sector-lod-instanced-world-design.md` pipeline). This is what exercises the `lod_bake` LOD levels and the sector/LOD-select composition at render time. Merged far-proxy is the one piece left to backlog (see below).
  - Both satisfy the same `SectorResolver` interface; swapping them changes no composer/renderer code.
- The per-part LOD table (`lod_select::PartLodTable`: bound radius + screen-size thresholds) is built once at load from the `.part` LOD levels and handed to the resolver.
- Depends on: MSL `TLASManager` (read-only), `part_library`, SP-4 `sector_grid` + `lod_select`.

### `renderer` (`renderer.h` / `renderer.cpp`)
- Owns the raytrace shader (`LoadShader("shaders/raytrace_tlas_blas_processed.fs")`), camera state, and screen-size/camera uniforms.
- Each frame: `blas.bind_to_shader(shader)`, `tlas.bind_to_shader(shader, blas)`, set camera + screen uniforms, `BeginShaderMode` → fullscreen `DrawRectangle` → `EndShaderMode` (mirrors MSL `main.cpp`'s render path).
- One-time shader warm-up at startup (compile via a 1×1 offscreen draw + `glFinish`), as MSL does, to move the GPU compile stall out of the first real frame.
- Depends on: MSL `BLASManager`, `TLASManager`, raylib.

### `ui` (`ui.h` / `ui.cpp`) — Dear ImGui overlay
- Mirrors MSL's integration exactly: Dear ImGui core (`Libraries/imgui`) with the **GLFW + OpenGL3 backends** (`imgui_impl_glfw`, `imgui_impl_opengl3`), bound to raylib's GLFW window via `glfwGetCurrentContext()`. raylib uses GLFW under `PLATFORM_DESKTOP`, so ImGui shares the same window/GL context.
- `Ui::setup()` — `ImGui::CreateContext()` → `StyleColorsDark()` → `ImGui_ImplGlfw_InitForOpenGL(glfwGetCurrentContext(), true)` → `ImGui_ImplOpenGL3_Init("#version 330")`. Called once after `InitWindow`.
- `Ui::begin_frame()` / `Ui::end_frame()` — wrap `ImGui_ImplOpenGL3_NewFrame()` + `ImGui_ImplGlfw_NewFrame()` + `ImGui::NewFrame()` and `ImGui::Render()` + `ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData())`. Called after the raytrace pass so the overlay composites on top.
- `Ui::draw_debug_panel(ViewerStats&)` — the HUD: FPS/frame time, camera position + facing, instance count (total / active this frame), occupied-sector count, provider connection state, **cache stats** (parts baked vs. cache hits on the last connect, i.e. cold-vs-warm reload + last reconcile want-count), a **resolver selector** (`PassThrough` vs `SectorLod`) that switches the active `SectorResolver` live, and a "reload world" button (re-runs the connect sequence). `ViewerStats` is a plain struct the renderer/composer/provider fill in each frame; the resolver selection is the one writable field the panel sets back — it owns no other engine state.
- `Ui::shutdown()` — `ImGui_ImplOpenGL3_Shutdown()` → `ImGui_ImplGlfw_Shutdown()` → `ImGui::DestroyContext()`.
- This panel is the seam for future in-engine tools (part placement, material pickers, live-edit controls); minimal viewer ships only the read-only debug panel plus the reload button.
- Depends on: Dear ImGui (`Libraries/imgui`), raylib's bundled GLFW.

### `main` (`main.cpp`)
- `InitWindow` → `Ui::setup()`, construct the units, run the connect sequence, then the frame loop: free-fly camera input → `world_composer` updates the TLAS → `renderer` draws the traced frame → `Ui::begin_frame()` / `draw_debug_panel` / `end_frame()` → `poll_deltas` applies changes. On exit, `Ui::shutdown()`.
- Camera vs. UI input: start in cursor-enabled (UI) mode like MSL; a key (e.g. right-mouse or Tab) toggles between free-fly camera capture and ImGui interaction so mouse-look and the panel don't fight.

---

## Build

A new `viewer` target in `MatterEngine3/`'s build (its own Makefile or a target in the tests Makefile, but **NOT** in the headless `build-all.sh test` sweep — it needs a GL context). Links:
- MSL render sources: `../../MatterSurfaceLib/src/{blas_manager,tlas_manager,bvh,bvh_analyzer,bvh_visualizer}.cpp` plus material/tri deps, via `-I../../MatterSurfaceLib/include`.
- SP-1/SP-4 sources for `LocalProvider`: the exact source set the `example_world` target already links (`part_asset_v2`, `world_flatten`, `sector_grid`, `lod_select`, `lod_bake`, plus the script-host/QuickJS set under `-DMATTER_HAVE_SCRIPT_HOST`) — reuse that recipe verbatim from `MatterEngine3/tests/Makefile`.
- Dear ImGui (reuse MSL's wiring exactly): `IMGUI_PATH = ../../Libraries/imgui`; compile `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `backends/imgui_impl_glfw.cpp`, `backends/imgui_impl_opengl3.cpp`; add `-I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends -I$(RAYLIB_PATH)/src/external/glfw/include` (the GLFW headers ship inside raylib). `imgui_demo.cpp` is optional (handy during bring-up).
- raylib + GL (reuse MSL's `WSL_LINUX=1` link flags).
- Copies/symlinks MSL's `shaders/` dir next to the binary so the shader path resolves.

---

## Error handling

- **Missing shader / GL init failure:** fatal at startup, print and exit non-zero (a viewer with no GL is useless).
- **`connect()` / `fetch_parts()` failure:** surface the provider's error string and exit; for the LocalProvider this means the bake failed (same failure modes as `example_world`).
- **`load_v2` failure for a part hash:** skip that instance, log once per hash; the rest of the world still renders (a single bad part shouldn't black-screen the world).
- **TLAS capacity:** size the `TLASManager` to the instance count from the manifest (the example world is ~153 instances; MSL's default cap is 100, so the viewer constructs `TLASManager(manifest.instances.size() + headroom)`).

---

## Testing

- The composition/provider units are GL-free and unit-testable: a headless test that constructs a `LocalProvider` over a temp cache dir and asserts the **two-run cache behavior** — first `connect` bakes and `reconcile` wants every hash; a second `connect` against the same cache bakes nothing (`InstallResult.baked.empty()`) and `reconcile` returns an **empty want-list** (the "instant reload, nothing changed" path). It also builds `WorldState` and applies a synthetic `WorldDelta` (assert add/remove/move semantics). This joins the headless sweep.
- `SectorLodResolver` is GL-free too: a headless test asserts that a far camera selects coarser LOD levels and deactivates distant sectors versus a near camera, over a fixed instance set.
- The renderer + `main` are GL and not in the headless sweep; verified by launching the viewer and confirming the world draws (the user launches GUI apps via `!` or their own terminal — the harness reaps backgrounded GUI children).

---

## Backlog (future extension seams — explicitly out of scope now)

These are the drop-in points the architecture preserves but does not build in the minimal viewer:

- **Network `WorldProvider` drop-in (SP-8 / SP-9).** A `NetworkProvider` implementing the same `WorldProvider` interface over a socket: `connect()` opens the session and streams the server's manifest; `reconcile()` runs the hash have/want diff client-side; `fetch_parts()` requests only missing `.part` blobs from the server (content-addressed dedup); `poll_deltas()` receives server-broadcast world deltas. **No renderer, composer, or part_store changes** — `main` selects `LocalProvider` (single-player / embedded server) vs `NetworkProvider` (connect to dedicated server) at startup. This is the primary growth seam.
- **Merged far-proxy.** `SectorLodResolver` ships with binning + per-sector LOD-select + distance-sphere activation (in scope); collapsing far sectors into a single merged proxy mesh is the remaining optimization from the sector-LOD spec, left to backlog behind the unchanged `SectorResolver` interface.
- **Live edit / hot-reload (SP-5).** The `poll_deltas` channel already carries instance changes; a future provider can also signal part-hash re-bakes so the `PartStore` reloads a changed BLAS and the world refreshes without restart — the incremental-rebake counterpart to the bake-once cache.
- **Materials / lighting / temporal rendering.** The viewer binds whatever the MSL shader supports today; the temporal-rendering-foundation work is orthogonal and plugs into the renderer later.

---

## Goals / non-goals

**Goals**
- One GL window rendering the committed example world via MSL's raytraced TLAS/BLAS path.
- Free-fly camera.
- The `WorldProvider` + `PartStore` + `SectorResolver` boundaries, with `LocalProvider` plus **both** `PassThroughResolver` and `SectorLodResolver` as concrete impls.
- A **persistent content-addressed cache**: build the world once, and on a subsequent run with nothing changed reload immediately (no rebake) via hash reconciliation.
- Live sector-LOD: `bin_instances` + `select_sector_lods` + distance-sphere activation, exercising the baked LOD levels at render time.
- A Dear ImGui debug/HUD overlay (stats + cache hit/bake counts + a live resolver `PassThrough`/`SectorLod` toggle + reload button), wired as the seam for future in-engine tools.
- A headless unit test for the GL-free provider/composer/resolver logic, including the two-run cache-hit path.

**Non-goals (deferred)**
- Networking (interface is ready; `NetworkProvider` is backlog).
- Merged far-proxy (sector binning + LOD-select + activation are in scope; the far-sector merge is backlog).
- Live editing / hot-reload and in-world part placement (the ImGui panel and `poll_deltas` channel are the seams, but editing tools are backlog).
- Any modification to MatterSurfaceLib.
