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

Five units under `MatterEngine3/viewer/`, each with one responsibility and a narrow interface:

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
└─────────────────────────────────────────────────────────────┘
```

Data flows downward each frame: `world_source` holds the authoritative-from-the-client's-view instance set, `part_library` supplies the geometry for each part hash, `world_composer` selects+transforms instances into the TLAS, and `renderer` traces it.

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
Backs `connect()` with the `example_world` pipeline output — it runs the same install → bake → flatten path (or reads its committed `parts/` dir + flattened instance list) and returns a manifest where every `part_hash` is already present in the local `PartStore`. So `reconcile` yields an empty want-list, `fetch_parts` is a no-op, and `poll_deltas` returns empty. The network path and the local path are identical code in the renderer.

---

## Module responsibilities and interfaces

### `world_source` (`world_source.h` / `local_provider.cpp`)
- Defines `WorldProvider`, `WorldManifest`, `WorldDelta`, and the live `WorldState` (a `std::vector<WorldManifestEntry>` keyed/replaced by `instance_id`, plus apply-delta helpers).
- `LocalProvider` produces the manifest from the example-world bake.
- Depends on: `part_library` (for the `PartStore&` reconcile arg), the SP-3/SP-4 composition path for `LocalProvider` only.

### `part_library` (`part_store.h` / `part_store.cpp`)
- `PartStore`: maps `part_hash -> BLASHandle` via an owned `BLASManager`. `has(hash)`, `get_or_load(hash, dir)` (calls `part_asset::load_v2` → `BLASManager::register_triangles`), and exposes the `BLASManager&` for the renderer to bind.
- Content-addressed: a hash loaded once is reused for every instance (terrain/tree/grass dedup is automatic).
- Depends on: SP-1 `part_asset_v2` (`load_v2`, `cache_path_resolved`), MSL `BLASManager` (read-only).

### `world_composer` (`world_composer.h` / `world_composer.cpp`)
- Per frame: takes `WorldState` + `PartStore` + camera, decides which instances are active and at which LOD, and records them into the `TLASManager` (`tlas.clear()` → `tlas.draw(blas_handle, material_id)` under each instance transform → `tlas.build(blas_manager)`).
- **`SectorResolver` seam** — a strategy object that answers "given the camera, which instances render this frame and at which LOD level?"
  - Minimal viewer: `PassThroughResolver` — all instances active, LOD 0 (finest), no culling.
  - Future drop-in: `SectorLodResolver` backed by `sector_grid::bin_instances` + `lod_select::select_sector_lods` + distance-sphere activation + far-proxy (the `2026-06-24-sector-lod-instanced-world-design.md` pipeline). The composer interface does not change.
- Depends on: MSL `TLASManager` (read-only), `part_library`.

### `renderer` (`renderer.h` / `renderer.cpp`)
- Owns the raytrace shader (`LoadShader("shaders/raytrace_tlas_blas_processed.fs")`), camera state, and screen-size/camera uniforms.
- Each frame: `blas.bind_to_shader(shader)`, `tlas.bind_to_shader(shader, blas)`, set camera + screen uniforms, `BeginShaderMode` → fullscreen `DrawRectangle` → `EndShaderMode` (mirrors MSL `main.cpp`'s render path).
- One-time shader warm-up at startup (compile via a 1×1 offscreen draw + `glFinish`), as MSL does, to move the GPU compile stall out of the first real frame.
- Depends on: MSL `BLASManager`, `TLASManager`, raylib.

### `main` (`main.cpp`)
- `InitWindow`, construct the units, run the connect sequence, then the frame loop: free-fly camera input → `world_composer` updates the TLAS → `renderer` draws → `poll_deltas` applies changes.

---

## Build

A new `viewer` target in `MatterEngine3/`'s build (its own Makefile or a target in the tests Makefile, but **NOT** in the headless `build-all.sh test` sweep — it needs a GL context). Links:
- MSL render sources: `../../MatterSurfaceLib/src/{blas_manager,tlas_manager,bvh,bvh_analyzer,bvh_visualizer}.cpp` plus material/tri deps, via `-I../../MatterSurfaceLib/include`.
- SP-1/SP-4 sources for `LocalProvider`: the exact source set the `example_world` target already links (`part_asset_v2`, `world_flatten`, `sector_grid`, `lod_select`, `lod_bake`, plus the script-host/QuickJS set under `-DMATTER_HAVE_SCRIPT_HOST`) — reuse that recipe verbatim from `MatterEngine3/tests/Makefile`.
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

- The composition/provider units are GL-free and unit-testable: a headless test that constructs a `LocalProvider`, runs `connect` → `reconcile` (asserts empty want-list when parts are pre-baked) → builds `WorldState`, and applies a synthetic `WorldDelta` (assert add/remove/move semantics). This can join the headless sweep.
- The renderer + `main` are GL and not in the headless sweep; verified by launching the viewer and confirming the world draws (the user launches GUI apps via `!` or their own terminal — the harness reaps backgrounded GUI children).

---

## Backlog (future extension seams — explicitly out of scope now)

These are the drop-in points the architecture preserves but does not build in the minimal viewer:

- **Network `WorldProvider` drop-in (SP-8 / SP-9).** A `NetworkProvider` implementing the same `WorldProvider` interface over a socket: `connect()` opens the session and streams the server's manifest; `reconcile()` runs the hash have/want diff client-side; `fetch_parts()` requests only missing `.part` blobs from the server (content-addressed dedup); `poll_deltas()` receives server-broadcast world deltas. **No renderer, composer, or part_store changes** — `main` selects `LocalProvider` (single-player / embedded server) vs `NetworkProvider` (connect to dedicated server) at startup. This is the primary growth seam.
- **`SectorLodResolver` drop-in.** Replace `PassThroughResolver` with the sector-LOD pipeline (`bin_instances` → `select_sector_lods`, distance-sphere activation, merged far-proxy) behind the unchanged `SectorResolver` interface.
- **Live edit / hot-reload (SP-5).** The `poll_deltas` channel already carries instance changes; a future provider can also signal part-hash re-bakes so the `PartStore` reloads a changed BLAS and the world refreshes without restart.
- **Materials / lighting / temporal rendering.** The viewer binds whatever the MSL shader supports today; the temporal-rendering-foundation work is orthogonal and plugs into the renderer later.

---

## Goals / non-goals

**Goals**
- One GL window rendering the committed example world via MSL's raytraced TLAS/BLAS path.
- Free-fly camera.
- The `WorldProvider` + `PartStore` + `SectorResolver` boundaries, with `LocalProvider` + `PassThroughResolver` as the minimal concrete impls.
- A headless unit test for the GL-free provider/composer logic.

**Non-goals (deferred)**
- Networking (interface is ready; `NetworkProvider` is backlog).
- Sector-LOD activation / far-proxy (interface is ready; `SectorLodResolver` is backlog).
- Live editing, in-world part placement, UI/HUD.
- Any modification to MatterSurfaceLib.
