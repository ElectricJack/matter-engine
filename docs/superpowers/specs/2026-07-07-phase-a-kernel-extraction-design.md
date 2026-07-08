# Phase A: Kernel Extraction — Design

**Date:** 2026-07-07
**Status:** Approved design. Parent roadmap: `2026-07-07-engine-editor-roadmap.md` (Phase A).
**Prerequisite:** `feature/autoremesher-integration` merged to `main`; work starts from post-merge main.

## Goal

Split MatterEngine3 into an engine kernel library (world/bake/render, no window, no
frame loop) and a MatterViewer app that owns the window, GL 4.6 context, and frame
loop. The viewer converts in the same phase and finishes running on the public API
only. Move-and-expose, not a rewrite.

**Exit criteria:** viewer runs Meadow through the kernel API with identical output
(screenshot-diff against pre-split reference shots), on Linux and Windows.

## Decisions

| Question | Decision |
|----------|----------|
| Math types in kernel API | Keep raylib POD types (Vector3, Matrix, Color, Camera3D). Kernel never calls raylib window/frame functions. |
| Physical layout | Viewer moves out to top-level `MatterViewer/` in this phase; `MatterEngine3/` becomes kernel-only. |
| Bake orchestration | LocalProvider moves into the kernel behind `WorldSession`; `WorldProvider` interface stays public for future remote providers. |
| Shaders | Embedded in the library via build-time codegen (`.glsl` → C-string headers); `MATTER_SHADER_DIR` env override loads from disk for live shader iteration. |
| API scope | Full final API shape ships in Phase A with synchronous internals; Phase B swaps internals to threads without signature breaks. |
| Sequencing | Facade-first: build the API over code in place, convert the viewer, screenshot-gate, then pure-rename move commits last. |

## Current state (survey, 2026-07-07)

- `src/` + `include/` ≈ 14.2k LOC: bake pipeline (script host, DSL/CSG, flatten,
  LOD, tileset), already library-shaped and GL-free. Built as `libmatter_engine3.a`.
- `viewer/` ≈ 7.7k LOC mixing app-shell (main.cpp frame loop, ui.cpp imgui, FIFO)
  with kernel material (gpu_culler, raster_composer, renderer, part_store,
  world_composer, local_provider, resolvers, tileset_provider/gl_ctx, probe_texture).
- Viewer compiles engine sources directly rather than linking the .a.
- raylib types leak into kernel-destined code (`dsl_state.h`, `tileset_bake_primary.cpp`,
  `tileset_bake_ao.cpp`) — accepted per the math-types decision.
- Shaders load from disk relative to the binary's working directory (fragile).
- No blocking globals: state is dependency-injected; the only singleton-ish pieces
  (tileset_provider slots, MaterialRegistry) stay internal to the kernel.
- `world_tracer` (include/world_tracer.h) already provides GL-free raycast over
  placed instances — the query API's backing store.

## Target layout

```
MatterEngine3/                  kernel — library only
├── include/matter/             NEW public API: engine_context.h, world_session.h,
│                               events.h, query.h — the only headers apps may include
├── src/                        bake pipeline (unchanged) + old flat include/*.h
│                               moved next to their sources (now internal)
├── src/render/                 from viewer/: gpu_culler, raster_composer, renderer,
│                               part_store, world_composer, resolvers, probe_texture,
│                               tileset_provider, tileset_gl_ctx, raster_mesh, world_state
├── src/provider/               from viewer/: local_provider, world_source, sector_resolver
├── shaders/                    from viewer/shaders{,_gpu}/; codegen → shaders_gen/*.h
├── tests/                      existing headless suites + moved GPU test binaries + new api_tests
└── Makefile                    libmatter_engine3.a now absorbs render/ + provider/

MatterViewer/                   NEW top-level app sub-project
├── main.cpp                    frame loop, raylib window, input, FIFO, screenshots
├── ui.{h,cpp}                  imgui panels
└── Makefile                    links libmatter_engine3.a + MatterSurfaceLib + raylib + imgui
```

- **One library.** No separate render .a — the kernel is one thing; no consumer
  wants culling without baking.
- **Public vs internal headers.** `-I../MatterEngine3/include` from an app reaches
  only `matter/*.h`. The existing 41 flat headers move to `src/` as implementation
  detail.
- **Dependency rule.** MatterViewer (and later ExplorerDemo/EditorLib) include only
  `matter/*.h`, raylib, imgui. Enforced mechanically (grep-gate test, below).
- **GPU tests** (gpu_cull_tests, tileset_*_tests) move to `MatterEngine3/tests/`;
  they keep creating their own hidden window (raylib is a test-only dev dependency
  of the kernel).

## Kernel public API

### matter/engine_context.h

```cpp
namespace matter {
struct EngineDesc {
    const char* cache_root;   // .part cache location
    const char* shader_dir;   // nullptr = embedded shaders (MATTER_SHADER_DIR overrides)
};
class EngineContext {
public:
    // Requires a live GL 4.6 context to be current (the app made the window).
    // One-time init: GL checks, TBB warm-up, shader compile.
    static std::unique_ptr<EngineContext> create(const EngineDesc&, std::string& err);
    std::unique_ptr<WorldSession> open_world(const WorldDesc&, std::string& err);
    ~EngineContext();         // releases all GL resources it created
};
}
```

### matter/world_session.h

```cpp
struct WorldDesc { const char* schemas_dir; const char* world_data_dir; const char* manifest; };

enum class RenderPath { GpuDriven, Raytrace };
enum class ResolverKind { SectorLod, PassThrough };

struct RenderOptions {
    RenderPath  path       = RenderPath::GpuDriven;
    ResolverKind resolver  = ResolverKind::SectorLod;
    bool wireframe         = false;
    bool hiz_occlusion     = false;   // default OFF per ROADMAP false-positive issue
    int  pixel_budget;                // resolver pixel budget (current viewer default)
};

struct FrameStats {   // everything today's HUD shows
    float resolve_ms, build_ms, draw_ms;
    uint32_t instances_resolved, instances_drawn, hiz_culled;
};

class WorldSession {
public:
    void request_bake();      // Phase A: synchronous to completion before returning.
                              // Phase B: returns instantly, worker jobs + pump.
    void tick();              // poll provider deltas, apply world state
    void render(const Camera3D& cam, int fb_width, int fb_height,
                const RenderOptions& opts);  // resolve → cull → draw into current FBO
    bool poll_event(Event& out);             // drain one; app loops until false
    const FrameStats& frame_stats() const;
    void reload();            // live-edit rebake, fail-closed semantics unchanged

    // query (matter/query.h documents the structs)
    bool raycast(const float origin[3], const float dir[3], float max_t, RayHit& out);
    uint32_t instance_count() const;
    bool instance_info(uint32_t idx, InstanceInfo& out);
};
```

`render()` draws into whatever FBO is bound — the app owns the render target.
The kernel derives view/projection/frustum from `Camera3D` internally (the
`raster_cull.h` helpers move into the kernel).

### matter/events.h

```cpp
enum class EventType { BakeStarted, BakePartDone, BakeFinished, BakeError };
struct Event {
    EventType type;
    // phase, part module name, done, total, message / error detail
};
```

Phase A emits exactly these four. Phase B extends the payload (progress
granularity, cancellation acks) without changing the drain mechanism.

### matter/query.h

```cpp
struct RayHit    { float t; float normal[3]; uint32_t instance; uint64_t part_hash; int material_id; };
struct InstanceInfo { float transform[16]; uint64_t part_hash; const char* module_name; };
```

Backed by the existing `world_tracer` BVH — a thin adapter, not new machinery.
Ships in Phase A per the API-first rule; EditorLib (Phase D) is the first consumer.

## Execution stages (facade-first)

Every stage ends with a building, rendering viewer. Files move only in Stage 5.

- **Stage 0 — Reference shots.** `tools/viewer_shots.sh` baseline (Meadow, standard
  5 poses) + FrameStats snapshot on the pre-split binary. Exit-gate references.
- **Stage 1 — Shader embedding.** Makefile codegen rule (`.comp/.vs/.fs` →
  `shaders_gen/*.h`), loading switches to embedded-with-override. Independent;
  de-risks the moves early.
- **Stage 2 — Kernel headers + facade.** Write `include/matter/*.h`; implement
  `EngineContext`/`WorldSession` as thin classes over the existing objects where
  they sit today. Events + FrameStats wired.
- **Stage 3 — Viewer converts.** main.cpp/ui.cpp rewire: connect_sequence →
  `open_world()` + `request_bake()`; per-frame resolve/cull/draw block →
  `render()`; HUD reads `frame_stats()`; FIFO/HUD toggles write `RenderOptions`.
  **Screenshot-diff must be green here** — the real exit gate, while paths are
  still stable.
- **Stage 4 — Query API.** raycast/instance_info adapter over world_tracer +
  headless test.
- **Stage 5 — The moves.** Pure `git mv` commits, no logic edits mixed in:
  (1) render/provider files → `src/render/`, `src/provider/`; flat `include/*.h` →
  `src/`; shaders → `MatterEngine3/shaders/`; (2) viewer remnants → top-level
  `MatterViewer/`; (3) GPU tests → `MatterEngine3/tests/`. Then include-path and
  Makefile fixes (kernel .a absorbs render+provider; MatterViewer links the .a),
  `build-all.sh` + CLAUDE.md project list updates.
- **Stage 6 — Verify.** `./build-all.sh test`; screenshot-diff vs Stage 0; clean
  `make windows` rebuild (full obj clear — header moves); FIFO harness smoke run.

## Error handling

- `EngineContext::create` / `open_world` return null + error string (GL version,
  shader compile, missing manifest). No exceptions cross the API boundary.
- Bake failures surface as `BakeError` events carrying today's error-string detail.
  The structured error taxonomy + bad_alloc safety net remain Phase B scope.
- `reload()` keeps live-edit fail-closed semantics: broken script → old world keeps
  rendering, error event emitted.

## Testing

- **New `api_tests` binary** (`MatterEngine3/tests/`): hidden-window GL context
  (`GALLIUM_DRIVER=d3d12`, like existing GPU tests) → create EngineContext → open
  small fixture world → `request_bake()` → assert event sequence (Started →
  PartDone×N → Finished) → `render()` to offscreen FBO → non-black pixel assert →
  `raycast` at a known instance → hit assert. Wired into `build-all.sh test`.
- **Screenshot-diff gate** at Stage 3, after Stage 5, and on Windows at Stage 6.
- **Existing 30 headless suites** pass at every stage (Stage 5 touches only their
  Makefile paths).
- **Grep-gate test**: script asserts MatterViewer sources include nothing from
  `MatterEngine3/src/`; runs in `build-all.sh test`.

## Risks

| Risk | Mitigation |
|------|------------|
| Scope creep into rewrites | Facade over code in place; moves are pure renames; screenshot gate |
| Shader embed breaks live iteration | `MATTER_SHADER_DIR` override preserved and documented |
| Windows silent staleness after header moves | Mandatory clean rebuild (standing rule) |
| Move commits conflict with concurrent work | Start only from post-merge main; Stage 5 needs a quiet tree |

## Out of scope

- Async bake, progress granularity, cancellation (Phase B — signatures already accommodate)
- EditorLib / picking UI (Phase D consumes the query API)
- OOM fixes, HiZ occlusion fix, mesher-native indexed emit (backlog)
- Rewriting bake/render internals — facade + moves only
