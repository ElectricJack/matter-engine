#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "matter/camera.h"
#include "matter/animation_debug.h"
#include "matter/ecs.h"
#include "matter/world_definition.h"
#include "matter/streaming.h"
#include "matter/vulkan_device.h"
#include "render/vk_gi_contract.h"
#include "part_graph_snapshot.h"

#include "matter/events.h"
#include "matter/query.h"

#include "bake_trace.h"   // bake_trace::Span — see last_bake_trace()
#include "matter/bake_observer.h"  // optional per-rung observer (W3, Lab-only)

namespace matter::evt { class Hub; }
namespace matter::scene { class SceneService; class SceneChangeTracker; }
namespace matter::props { class DynamicGroup; }

namespace matter {

struct VulkanFrame;

struct AnimationRasterRange {
    uint32_t vertex_start = 0;
    uint32_t vertex_count = 0;
    uint32_t index_start = 0;
    uint32_t index_count = 0;
};
using AnimationRasterRangeResolver =
    std::function<bool(uint64_t part_hash, AnimationRasterRange& out)>;

struct WorldDesc {
    // Preferred project layout. open_world derives objects/, worlds/,
    // optional shared-lib/, and .cache/<world>/ from this root.
    const char* project_dir = nullptr;
    const char* world_name  = nullptr;
    const char* engine_shared_lib_dir = nullptr;

    bool enable_live_edit = false;  // watch schemas/shared-lib dirs, cone-rebake on save (Linux inotify; no-op elsewhere)
};

enum class RenderPath { GpuDriven, Raytrace };
enum class ResolverKind { SectorLod, PassThrough };
enum class DlssMode : uint8_t { Native, Quality, Balanced, Performance };

const char* dlss_mode_name(DlssMode mode) noexcept;

struct VulkanLightingOverrides {
    // sun/sky are the 2026-07-30 tuning pass (both were 1.0): a hotter key
    // against a dimmer sky is what separates lit slope from shadowed slope
    // once the terrain is mostly one gray rock material. These are the values
    // the viewer's "Reset to World" button restores.
    float sun_multiplier = 1.67f;
    float sky_multiplier = 0.77f;
    float emission_multiplier = 1.0f;
    float exposure_ev = -2.0f;
    float composite_debug_view = 0.0f;
};

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
    DlssMode dlss_mode = DlssMode::Native;
    VulkanRayTracingSettings vulkan_ray_tracing{};
    VulkanGiSettings vulkan_gi{};
    VulkanLightingOverrides vulkan_lighting{};
    VulkanVolumetricsSettings vulkan_volumetrics{};
    TilesetPomSettings vulkan_tileset_pom{};

    // Bake Lab W4 (part-workbench.md SS-I.5): LOD Inspector debug overrides.
    // Lab-only — production render paths are byte-identical to pre-W4
    // behavior when left at these defaults.
    //
    // -1 (default): normal camera-based LOD selection. >=0: force every
    // cluster/part touched by this render to LOD level k, clamped to that
    // part's own level count, regardless of camera distance. Implemented by
    // squashing the selected cluster's screen-size thresholds so the existing
    // cull-shader selection math (unmodified) always resolves to level k —
    // see ensure_vulkan_part in matter_engine.cpp and pack_cluster/
    // pack_whole_part in render/gpu_cull_types.h.
    int  force_lod = -1;
    // False (default): normal rendering. True: only a part's own root-level
    // mesh renders; its baked child-instance subtrees (LoadedPart::expansion
    // nodes at depth > 0) are skipped. Coarser than the spec's per-module
    // visibility mask (hiding e.g. only "Leaf") — this is the "show root
    // only" granularity called out as an acceptable W4 fallback.
    bool hide_child_instances = false;
};

struct TickDesc {
    float frame_delta_seconds = 0.0f;
    float fixed_delta_seconds = 1.0f / 60.0f;
    uint32_t max_fixed_steps = 4;
    // Editor Edit/Pause: run the ordinary frame tick -- command drains, binding
    // lifecycle reconciliation, frame-cadence systems -- but advance no fixed
    // simulation. The accumulator is left EXACTLY as it was, so resuming
    // completes the step that was banked before the freeze rather than losing a
    // fraction or spending a catch-up burst.
    //
    // This is deliberately NOT expressible as max_fixed_steps == 0, which is a
    // malformed request that invalidates the whole tick and therefore discards
    // the frame pipeline and every reconcile that rides on it. Nor is it
    // frame_delta_seconds == 0: zeroing the delta stops new time accruing but
    // still lets an already-banked catch-up residual be spent, so a stopped
    // editor could keep stepping. Freezing is a normal tick that runs no fixed
    // step, not an error and not an arithmetic accident.
    bool advance_fixed = true;
    // Unscaled wall-clock delta for this rendered frame. Cosmetic presentation
    // cadence (the animation pose-LOD refresh clock) runs on THIS delta, so a
    // slow-motion frame_delta_seconds slows what the simulation shows, not how
    // often presentation refreshes it. 0 (default) falls back to
    // frame_delta_seconds, which is exact whenever the app applies no time
    // scaling. Never consumed by simulation state.
    float presentation_delta_seconds = 0.0f;
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
    uint32_t draw_batches      = 0;   // indirect draw buckets with >=1 instance
    // world/bake census (filled by request_bake / reload)
    uint32_t instances_total = 0;
    uint32_t parts_baked = 0;         // cache misses last bake
    uint32_t cache_hits  = 0;         // cache hits last bake
    // Phase C Task 9: world-kind sessions only; 0 for closed-world sessions.
    uint32_t resident_sectors = 0;
    // Vulkan GPU-driven path diagnostics (cumulative CPU-side counters).
    uint64_t vk_instance_cache_expansions = 0;
    uint64_t vk_vertex_uploads = 0;
    uint64_t vk_cluster_uploads = 0;
    uint64_t vk_instance_uploads = 0;
    uint64_t vk_command_layout_rebuilds = 0;
    // Static-geometry upload mode census: full recreates rewrite O(world),
    // appends write only the newly registered parts. A streaming load whose
    // full count climbs with resident parts has lost the append fast path.
    uint64_t vk_static_full_uploads = 0;
    uint64_t vk_static_append_uploads = 0;
    uint64_t vk_immediate_submits = 0;
    // WP-E (chart-space virtual texturing) residency census. All zero when the
    // VT runtime never started (no chart-bearing part in the scene).
    bool     vt_active = false;
    uint32_t vt_variants = 0;          // registered (variant, rung) layers
    uint32_t vt_max_variants = 0;      // MATTER_VT_MAX_VARIANTS, post-clamp
    uint32_t vt_pool_used = 0;         // occupied physical page slots
    uint32_t vt_pool_capacity = 0;
    uint32_t vt_pool_pinned = 0;       // always-resident tails
    uint32_t vt_fills_last_frame = 0;
    uint32_t vt_requests_last_frame = 0;
    uint32_t vt_queue_depth = 0;
    uint32_t vt_rejected_variants = 0; // fell back to legacy (budget/layers)
    uint64_t vt_fills_total = 0;
    uint64_t vt_evictions_total = 0;
    uint64_t vt_pool_bytes = 0;
    uint64_t vt_mesh_bytes = 0;        // CPU mesh copies held for the filler
    uint64_t vt_mesh_budget_bytes = 0; // MATTER_VT_MESH_BUDGET_MB, in bytes
    // Buffer-indirection census (exact-sized tables in one SSBO; replaced the
    // 2048-layer-capped image array).
    uint64_t vt_indirection_bytes = 0;          // live table blocks
    uint64_t vt_indirection_capacity_bytes = 0; // MATTER_VT_INDIRECTION_MB
    DlssMode dlss_selected_mode = DlssMode::Native;
    DlssMode dlss_active_mode = DlssMode::Native;
    uint32_t dlss_internal_width = 0;
    uint32_t dlss_internal_height = 0;
    uint32_t dlss_output_width = 0;
    uint32_t dlss_output_height = 0;
    uint64_t dlss_reset_count = 0;
    std::string dlss_reason;
    bool vk_rt_available = false;
    bool vk_rt_effective = false;
    uint32_t vk_rt_trace_dispatches = 0;
    uint32_t vk_rt_samples = 1;
    bool vk_rt_debug_view = false;
    std::string vk_rt_fallback_reason;
    // GPU-side per-pass timings (ms), smoothed EMA (α = 0.1). Values are 0
    // when the zone did not execute this frame or GPU timers are unsupported.
    float gpu_total_ms          = 0;
    float gpu_cull_ms           = 0;
    float gpu_gbuffer_ms        = 0;
    float gpu_blas_ms           = 0;
    float gpu_tlas_ms           = 0;
    float gpu_rt_ms             = 0;
    float gpu_denoise_ms        = 0;
    float gpu_dlss_ms           = 0;
    float gpu_composite_ms      = 0;
    float gpu_vol_ms            = 0;
    // WP-E: the chart-VT page-fill pass (residency uploads + tier-1
    // compositor dispatches + pool copies). 0 when VT is not active.
    float gpu_vt_ms             = 0;
    bool  gpu_timers_supported  = false;
    uint64_t ecs_fixed_steps = 0;
    uint64_t ecs_dropped_steps = 0;
    uint64_t ecs_invalid_ticks = 0;
};

class WorldSession {
public:
    ~WorldSession();   // releases session GL resources — destroy before CloseWindow

    // Phase B: asynchronous — enqueues a bake and returns immediately. Progress
    // arrives via poll_event(); GL-side work runs inside pump_gpu_jobs(). A new
    // request_bake()/reload() supersedes (cancels) an in-flight bake.
    void request_bake();

    // Poll provider deltas and apply them to world state. Call once per frame.
    void tick(const TickDesc& tick);

    // The session-owned Flecs world. Runtime entity IDs are local to this
    // session and remain alive across authored-content reload/regeneration.
    flecs::world& ecs();
    const flecs::world& ecs() const;

    // Resolve -> cull -> clear (kernel-derived sky color) -> draw into the
    // currently bound framebuffer. Requires a live GL context on this thread.
    void render(const CameraDesc& cam, int fb_width, int fb_height,
                const RenderOptions& opts);

    bool render(const CameraDesc& cam, const VulkanFrame& frame,
                const RenderOptions& opts, std::string& err);

    // Apply an optional camera spawn authored by the active World JavaScript.
    // Returns false when the world did not provide static camera settings.
    bool apply_authored_camera(CameraDesc& camera) const;

    // Resolve the temporal candidate recorded by render(). Call exactly once
    // with the result returned by VulkanDevice::end_frame.
    void finish_vulkan_frame(uint64_t frame_serial, bool presented);

    bool readback_swapchain_rgba8(const VulkanFrame& frame,
                                  std::vector<uint8_t>& rgba,
                                  std::string& err);

    // Phase B: run queued GL-thread bake work for up to ms_budget milliseconds.
    // Call once per frame on the thread that owns the GL context. Whole jobs
    // only (no mid-job slicing); always makes progress when work is queued.
    void pump_gpu_jobs(float ms_budget);

    // ---- Streaming LOD configuration (editor LOD Settings panel) ----------
    // A ring maps an anchor radius to a value: the scatter tier (0..2) for
    // scatter_rings, the terrain LOD (0 coarsest .. 5 native voxel) for
    // terrain_bands. Innermost first.
    struct StreamingLodRing { float radius = 0.0f; int value = 0; };
    struct StreamingLodConfig {
        std::vector<StreamingLodRing> scatter_rings;
        std::vector<StreamingLodRing> terrain_bands;
        // Matches make_streaming_profile's default (on since 2026-07-30). Only
        // the value the LOD Settings panel shows before its first live-mirror
        // of the active profile, but a `false` here read as "the ladder is off"
        // while the engine had it on.
        bool terrain_lod_enabled = true;
        // Informational (filled by streaming_lod_config, ignored by
        // set_streaming_lod_overrides): the world's sector size, for UI
        // spacing hints.
        float sector_size = 64.0f;
    };
    // The ACTIVE resolved profile of the current world (world JS + env +
    // overrides + engine defaults). False before a world-kind connect.
    bool streaming_lod_config(StreamingLodConfig& out) const;
    // World-authored volumetrics defaults (World.volumetrics static),
    // available once a world-kind connect completes. The editor adopts these
    // into its live volumetrics controls on world load.
    bool world_volumetrics(VulkanVolumetricsSettings& out) const;

    // The world's script-declared runtime tunables (`static props`), as a live
    // property group the editor can bind into its registry -- null when the
    // world declares none (property-system spec S9).
    //
    // OWNED BY THE SESSION and rebuilt on every world-kind connect, including a
    // reload of the same world. A caller that binds it into a props::Registry
    // MUST unbind before triggering the reconnect that replaces it; the editor
    // does exactly that at its set_world seam. The values start at the script's
    // declared defaults; nothing in the engine reads them back today (see the
    // Phase-6 seam note in the spec), so this is the editor's surface for
    // showing and persisting them.
    props::DynamicGroup* world_props();
    // Override applied at the NEXT world (re)connect — pair with a world
    // reload to take effect. Empty ring/band lists fall back to the world's
    // own values / engine defaults; the enabled flag always applies.
    void set_streaming_lod_overrides(const StreamingLodConfig& overrides);
    void clear_streaming_lod_overrides();

    // True when no GL-thread job is queued. Lets the caller widen the pump
    // budget only while a streaming backlog exists (sector publishes are
    // ~1 ms each; a fixed small budget drains a 5,000-sector fill at a
    // handful per frame).
    bool gpu_jobs_idle() const;

    // Phase C Task 3: set the spatial focus for the next bake pass.
    // publish_pipeline sorts parts ascending by min dist² from focus to any
    // of that part's manifest entry translations; parts with no placement sort
    // last; ties break by part hash (deterministic). Thread-safe: may be called
    // from the app thread at any time before or between bakes.
    void set_bake_focus(const float pos[3]);

    // Single-consumer: drain only from the app (main/UI) thread — the same
    // thread that owns the app command lane and pumps the session per frame
    // (event-system.md S I.11 / S II.4 item 6; E4b SessionBinding runs the
    // world-switch teardown on this thread). Drain one; loop until false.
    bool poll_event(Event& out);

    // The per-session event hub (event-system.md S I.13: one hub per
    // WorldSession, lifetime = session lifetime — the returned reference is
    // valid only until this session is closed/replaced). Bake/stream
    // progress is emitted here as typed events (matter/events/*.h); the
    // legacy poll_event() above is a compat shim over a private
    // lane::legacy_poll subscription set (S I.11 / S II.4 item 6).
    // New subscribers register directly against this hub.
    evt::Hub& events();
    const evt::Hub& events() const;

    // E5b (event-system.md S I.14): the session-owned scene-graph model layer.
    // scene_service() is the ONE supported path for create/duplicate/delete/
    // reparent/rename/component edits (validation + the Flecs mutation, returns
    // a typed SceneEditResult). scene_change_tracker() publishes the canonical
    // sequenced scene-row deltas (scene.rows_upserted / scene.rows_removed on
    // events()) at end-of-tick flush and serves the (rows, sequence) recovery
    // snapshot. Both are app-thread-affine; the returned references are valid
    // only until this session is closed/replaced (same as events()). Wired for
    // the E5c SessionBinding adapter.
    scene::SceneService& scene_service();
    scene::SceneChangeTracker& scene_change_tracker();

    const FrameStats& frame_stats() const;
    // Copies committed animation metadata and the latest immutable runtime
    // presentation state for every live ECS animation binding. An empty result
    // is a valid "no live animator data" state; no cache load or evaluation is
    // performed by this observational viewer seam.
    bool animation_debug_snapshots(
        std::vector<AnimationDebugInstanceSnapshot>& out) const;
    // Value-owned runtime accounting for lifecycle diagnostics and editor
    // tooling. In particular, active_assets must remain bounded across entity
    // removal and animated-part replacement.
    AnimationRuntimeStats animation_runtime_stats() const;

    // --- editor-driven external target writes -----------------------------
    // The narrow write surface an editor gizmo needs, resolved by animator +
    // authored target name so no AnimationService handle crosses the boundary.
    //
    // These are ORDINARY EXTERNAL WRITES: they are staged and sampled at the
    // target's declared cadence exactly like a gameplay write, and they obey
    // one-driver arbitration. A controller-driven target therefore returns
    // false here -- the caller is expected to have disabled the affordance
    // already (see the editor's Targets tab), and this is the enforcement that
    // makes that disabled state real rather than cosmetic.
    //
    // Returns false when the animator or target name is unknown, the target is
    // controller-driven, or the transform is non-finite.
    bool set_animation_target_transform(AnimatorInstanceHandle instance,
                                        const char* target_name,
                                        const AnimationTransform& desired);
    // Requests that the target skip its smoothing and adopt the desired
    // transform on the next evaluation. Same arbitration as above.
    bool snap_animation_target(AnimatorInstanceHandle instance,
                               const char* target_name);
    // Test-only production seam: substitutes only the immutable renderer range
    // lookup. Skin validation and ECS binding still execute through the exact
    // runtime reconciliation used by Vulkan.
    void set_test_animation_raster_range_resolver(
        AnimationRasterRangeResolver resolver);
    // Copied coordinator state; no streamer or render-resource state crosses
    // the worker/app boundary.
    streaming::SectorStreamingStatus streaming_status() const;

    // Phase B: asynchronous — enqueues a bake and returns immediately. Progress
    // arrives via poll_event(); GL-side work runs inside pump_gpu_jobs(). A new
    // request_bake()/reload() supersedes (cancels) an in-flight bake. Fail-closed:
    // on error a BakeError event is emitted and render() no-ops until a later
    // request_bake()/reload() succeeds (the old world is torn down before rebaking).
    void reload();

    // Phase C Task 9: world-kind sessions: the sea level from the world definition.
    // Returns true and sets `out` for world-kind sessions; returns false for
    // closed-world (expand/tileset) sessions (no water plane in those worlds).
    bool sea_level(float& out) const;

    // Copy of the provider's part-graph snapshot. Refreshed after connect and
    // live-edit reresolve. Returns false if no provider is connected.
    bool graph_snapshot(part_graph_snapshot::Snapshot& out) const;

    // Generation counter bumped on install/reresolve; callers cache the snapshot
    // and re-query only when generation changes.
    uint64_t graph_generation() const;

    // Bake Lab: snapshot of the hierarchical span trace recorded by the most
    // recent (or in-flight) bake. Valid after BakeFinished; calling during a
    // bake is safe and yields a consistent partial tree (open spans keep
    // end_ms == bake_trace::kOpenEndMs). Root children are the execute_bake
    // stages (install/compose/publish; a resolve-cache hit skips the first two).
    void last_bake_trace(bake_trace::Span& out) const;

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
    bool part_bounds(uint64_t part_hash, PartBounds& out) const;

    // Bake Lab W4: LOD Inspector grid data source (part-workbench.md SS-I.5).
    // Pure PartStore reads, no bake/render side effects — mirrors
    // instance_info/part_bounds above. Return 0/false when part_hash has no
    // loaded LoadedPart (not yet baked, or released from CPU memory) so
    // callers can tell "no data yet" apart from "zero levels/children".
    uint32_t part_lod_level_count(uint64_t part_hash) const;
    bool part_lod_level_info(uint64_t part_hash, uint32_t level, PartLodLevelInfo& out) const;
    // Children aggregated by hash: repeated placements of the same module
    // collapse into one PartChildSummary entry (see query.h).
    uint32_t part_child_summary_count(uint64_t part_hash) const;
    bool part_child_summary(uint64_t part_hash, uint32_t idx, PartChildSummary& out) const;

    // Bake Lab W3: install an optional per-rung bake observer (Lab-only; not
    // part of the stable public API). Null clears it. Applied to the next
    // request_bake()/reload() — see matter/bake_observer.h for the full
    // seam contract (thread discipline, null = zero cost). Callers that
    // register a non-null observer are expected to be Lab/tooling code
    // (e.g. PartWorkbench's private isolation session), never a production
    // world session.
    void set_bake_observer(BakeObserver* observer);

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
