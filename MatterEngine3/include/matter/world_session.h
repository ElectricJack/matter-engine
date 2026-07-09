#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "raylib.h"   // Camera3D — POD math types are part of the API by design

#include "matter/events.h"
#include "matter/query.h"

namespace matter {

struct WorldDesc {
    const char* schemas_dir    = nullptr;
    const char* world_data_dir = nullptr;
    const char* world_name     = nullptr;  // world subdir of world_data_dir
    const char* shared_lib_dir = nullptr;  // shared .js library dir
    bool enable_live_edit = false;  // watch schemas/shared-lib dirs, cone-rebake on save (Linux inotify; no-op elsewhere)
};

enum class RenderPath { GpuDriven, Raytrace };
enum class ResolverKind { SectorLod, PassThrough };

struct RenderOptions {
    RenderPath   path     = RenderPath::GpuDriven;
    ResolverKind resolver = ResolverKind::SectorLod;
    bool  wireframe       = false;
    bool  hiz_occlusion   = false;    // default OFF (known false-positive issue)
    float pixel_budget    = 0.0f;     // 0 = default (1.0); clamped to [0.05, 4.0]
    float active_radius   = 0.0f;     // SectorLod knob; 0 = default (64.0)
    float min_projected_size = 0.0f;  // SectorLod sub-pixel cull; 0 = off
    bool  cull_backfaces  = false;    // GpuDriven path: skip backface triangles
                                      // (off by default: mesh-session winding
                                      // is not guaranteed for all part kinds)
};

struct FrameStats {
    // per-frame timings (ms)
    float resolve_ms = 0, build_ms = 0, draw_ms = 0;
    // per-frame counters
    uint32_t instances_resolved = 0;  // resolver output count
    uint32_t instances_drawn   = 0;   // clusters emitted by the GPU cull
    uint32_t clusters_culled   = 0;   // frustum-culled clusters
    uint32_t hiz_culled        = 0;   // HiZ-occlusion-culled clusters
    uint32_t triangles         = 0;   // rasterized triangle count
    // world/bake census (filled by request_bake / reload)
    uint32_t instances_total = 0;
    uint32_t parts_baked = 0;         // cache misses last bake
    uint32_t cache_hits  = 0;         // cache hits last bake
    int probe_dims[3] = {0, 0, 0};    // probe grid (all zero = unavailable)
};

class WorldSession {
public:
    ~WorldSession();   // releases session GL resources — destroy before CloseWindow

    // Phase B: asynchronous — enqueues a bake and returns immediately. Progress
    // arrives via poll_event(); GL-side work runs inside pump_gpu_jobs(). A new
    // request_bake()/reload() supersedes (cancels) an in-flight bake.
    void request_bake();

    // Poll provider deltas and apply them to world state. Call once per frame.
    void tick();

    // Resolve -> cull -> clear (kernel-derived sky color) -> draw into the
    // currently bound framebuffer. Requires a live GL context on this thread.
    void render(const Camera3D& cam, int fb_width, int fb_height,
                const RenderOptions& opts);

    // Phase B: run queued GL-thread bake work for up to ms_budget milliseconds.
    // Call once per frame on the thread that owns the GL context. Whole jobs
    // only (no mid-job slicing); always makes progress when work is queued.
    void pump_gpu_jobs(float ms_budget);

    // Phase C Task 3: set the spatial focus for the next bake pass.
    // publish_pipeline sorts parts ascending by min dist² from focus to any
    // of that part's manifest entry translations; parts with no placement sort
    // last; ties break by part hash (deterministic). Thread-safe: may be called
    // from the app thread at any time before or between bakes.
    void set_bake_focus(const float pos[3]);

    bool poll_event(Event& out);       // drain one; loop until false
    const FrameStats& frame_stats() const;

    // Phase B: asynchronous — enqueues a bake and returns immediately. Progress
    // arrives via poll_event(); GL-side work runs inside pump_gpu_jobs(). A new
    // request_bake()/reload() supersedes (cancels) an in-flight bake. Fail-closed:
    // on error a BakeError event is emitted and render() no-ops until a later
    // request_bake()/reload() succeeds (the old world is torn down before rebaking).
    void reload();

    // Phase C Task 7: enqueue a seed-driven world reroll. Stores
    // root_params_override = {"worldSeed": <world_seed>} and enqueues a Reload
    // with full supersession semantics (a newer regenerate/reload supersedes any
    // in-flight bake at the next between-parts checkpoint).
    //
    // The override is merged into each root part's params BEFORE
    // merge_params_canonical so the resolved hash changes with the seed. Terrain
    // parts that declare `static params = {worldSeed: …}` re-bake on a new seed
    // and hit cache on a repeated same seed. Scatter/vegetation parts that do NOT
    // declare worldSeed are unaffected by the override and always hit cache — a
    // reroll re-bakes terrain while vegetation variants are served from cache.
    //
    // Thread-safe: may be called from the app thread at any time; the override is
    // captured into cfg before the next LocalProvider is constructed.
    void regenerate(uint64_t world_seed);

    // Query API (backed by a lazily built CPU BVH; first call after a bake pays
    // the build cost).
    bool raycast(const float origin[3], const float dir[3], float max_t, RayHit& out);
    uint32_t instance_count() const;
    bool instance_info(uint32_t idx, InstanceInfo& out);

    // Task 7 test seam: install a per-part fault hook on the underlying provider
    // config. The hook fires once per part processed during install_graph() and the
    // publish loop; it may throw (std::bad_alloc → OutOfMemory; any other exception →
    // ScriptError/Internal). Null clears the hook.
    // NOT part of the stable public API — for kernel-internal tests only.
    void set_test_fault_hook(std::function<void(int)> hook);

    struct Impl;
    explicit WorldSession(std::unique_ptr<Impl> impl);   // internal; use open_world
    WorldSession(const WorldSession&) = delete;
    WorldSession& operator=(const WorldSession&) = delete;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace matter
