#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "matter/camera.h"
#include "matter/animation_debug.h"
#include "matter/render_debug.h"
#include "matter/ecs.h"
#include "matter/world_definition.h"
#include "matter/streaming.h"
#include "matter/sun_angles.h"  // kSunAngularDiameterDefaultDeg + the convention
#include "matter/atmosphere_lighting.h"
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
// (ResolverKind is gone. There was a PassThrough resolver -- every world entry
// emitted at LOD 0, no binning, no culling -- kept as a correctness reference
// and selected by default for every world except Meadow. It meant the engine
// had two resolve paths that had to agree, and every knob added to one had to
// be remembered for the other; the sub-pixel floor was routed only to SectorLod
// for a while, which made the editor's slider silently inert on the one world
// anyone was tuning. SectorLod is a superset -- it emits the same instances,
// then adds per-sector rung selection and inline-cutover child expansion -- so
// there is nothing PassThrough could express that survives its removal.)
enum class DlssMode : uint8_t { Native, Quality, Balanced, Performance };

// Public, renderer-type-free copy-out of the atmosphere snapshot that actually
// committed. The Vulkan renderer's richer viewer::ResolvedAtmosphereStatus
// remains private to the render implementation; editor automation must read
// this copy instead of reconstructing status from requested controls.
struct ResolvedAtmospherePresentationStatus {
    uint64_t generation_serial = 0;
    float resolved_elevation_deg = 0.0f;
    Float3 atmospheric_direct_base_rgb{};
    Float3 atmospheric_noon_direct_base_rgb{};
    float direct_world_ratio = 0.0f;
    Float3 direct_base_rgb{};
    Float3 direct_world_sun_rgb{};
    float sky_ambient_ratio = 0.0f;
    Float3 sky_display_modifier_rgb{};
    Float3 sky_irradiance_modifier_rgb{};
};

const char* dlss_mode_name(DlssMode mode) noexcept;

struct RenderOptions {
    RenderPath   path     = RenderPath::GpuDriven;
    // Geometry-stage diagnostic. Applied while the G-buffer is written, unlike
    // VulkanLightingOverrides::composite_debug_view; None is inert.
    GeometryDebugView geometry_debug_view = GeometryDebugView::None;
    bool  wireframe       = false;
    // Intra-cell impostor parallax: the two extra atlas taps in gbuffer.frag
    // that offset the sample by the stored displacement so a card fakes depth
    // as the view moves off its baked axis. Default ON (the shipped look);
    // turn off to measure what those taps buy. Distinct from tileset POM,
    // which already excludes impostors entirely. The card's gl_FragDepth
    // push-back is NOT affected -- that places the card at the depth it
    // depicts and is a correctness term, not an appearance one.
    bool  impostor_parallax = true;
    // HZB OCCLUSION CULLING (M4 Phase B, design §5.1). Tests each cluster's
    // screen AABB against a min-reduced depth pyramid, so geometry behind a
    // wall is rejected before it is drawn and `hiz_culled` finally has a
    // non-zero meaning.
    //
    // Default OFF, and the reason is latency rather than doubt about the test:
    // the pyramid is built from the depth the PREVIOUS frame finished, so while
    // the camera moves something just revealed can be rejected for one frame.
    // It is exact whenever the cull camera is not moving, which includes the
    // frozen-cull-camera inspection mode this was checked with.
    bool  hiz_occlusion   = false;
    // CULL DRAWS against the occlusion ID pass's mask (M4). This is the
    // one that stops occluded sectors being rasterised at all, as opposed
    // to the streaming cap, which only makes them coarser.
    //
    // Off by default and a separate switch from the cap for a reason that
    // is not caution for its own sake: a wrong bit in the cap costs detail,
    // a wrong bit here removes geometry from the picture.
    bool  occlusion_draw_cull = false;
    // FREEZE THE CULL CAMERA (M4, an inspection aid rather than a feature).
    //
    // Pins the frustum planes and the eye position the cull dispatch tests
    // against, and leaves world_to_clip following the live camera. That split
    // is the entire point: the frame is DRAWN from wherever you have flown to,
    // and CULLED as though you were still where you froze -- so flying out and
    // looking back shows you exactly the set the cull pass kept, from outside
    // it. There is no other way to see a culling decision; on-screen, a correct
    // cull and a broken one look the same until something pops.
    //
    // LOD selection is pinned with it, because cull.comp picks the rung from
    // the same eye. That is wanted here (the frozen view's detail is what you
    // came to inspect) and is the reason this is not called "freeze frustum".
    bool  freeze_cull_camera = false;
    // OCCLUSION DETAIL CAP (M4). Ticks a tile may go undrawn before it is
    // desired one level coarser; 0 disables the whole feature.
    //
    // LIVE, and it had to become live. It was launch-time only
    // (MATTER_OCCLUSION_GRACE) and that made it undiscoverable: an issue
    // report arrived reading "not enough is being occluded", captured with
    // both freeze toggles switched on -- so the UI had been found -- and the
    // cap silently at zero, because the one control that turns the feature on
    // was an env var. Nothing about the cap needs a restart: it changes which
    // level the descent stops at and nothing else.
    //
    // -1 means "do not override", which is how the env var and a world's own
    // profile survive a session that never touches the editor control.
    int   occlusion_grace_ticks = -1;
    int   occlusion_cap_levels  = 1;
    float pixel_budget    = 0.0f;     // 0 = default (1.0); clamped to [0.05, 4.0]
    // (No active_radius. It is DERIVED from the world's outermost terrain LOD
    // band -- the distance past which the streamer keeps nothing resident
    // anyway -- so the two can no longer disagree. It used to be a caller knob
    // defaulting to 64 m against a 16 m sector pitch, which is about four
    // sectors: on a world whose fog wall is 2.5 km that empties the view, and
    // it was the reason SectorLod could not be evaluated on StreamMountain at
    // all until a slider was added for it. A world with no bands is unbounded,
    // which is what a closed (non-streamed) world needs.)
    float min_projected_size = 0.0f;  // sub-pixel cull; 0 = off
    bool  cull_backfaces  = false;    // GpuDriven path: skip backface triangles
                                      // (off by default: mesh-session winding
                                      // is not guaranteed for all part kinds)
    DlssMode dlss_mode = DlssMode::Native;
    VulkanRayTracingSettings vulkan_ray_tracing{};
    VulkanGiSettings vulkan_gi{};
    VulkanLightingOverrides vulkan_lighting{};
    AtmosphereSettings atmosphere{};
    VulkanVolumetricsSettings volumetrics{};
    CloudShadowSettings cloud_shadows{};
    TilesetPomSettings vulkan_tileset_pom{};
    // Phase 0: the chart-VT near band, decoupled from the POM distances
    // beside it. Compiled defaults (50/10 m) are what a caller that never
    // sets it gets, so headless tests and the replay harness run the same
    // band the editor does.
    VtNearBandSettings vulkan_vt_near_band{};
    // Live fog override (property-system "render.fog"). The world-authored
    // FogSettings the connect captured is consumed PER FRAME — WorldSession::
    // render hands it to VkSceneRenderer::set_volumetrics_settings on every
    // call — so an editor copy of it can simply ride the per-frame options the
    // way vulkan_lighting/volumetrics already do.
    //
    // Opt-in rather than unconditional: every non-editor caller (headless
    // tests, the replay harness, the Part Workbench's isolation session) leaves
    // this false and keeps consuming the session's own authored fog, so the
    // default RenderOptions is bit-identical to the pre-override behavior.
    bool use_fog_override = false;
    FogSettings fog_override{};

    // Live sun orientation/size override (property-system "render.lighting").
    // Exactly the opt-in shape use_fog_override above has, and for the same
    // reason: VulkanLightingOverrides::sun_azimuth_deg / _elevation_deg /
    // _angular_diameter_deg are only meaningful once someone has SEEDED them
    // from the connected world (the editor does, at BakeFinished). A caller
    // that leaves this false gets the world's authored sun_direction copied
    // through untouched — no trig, no renormalization, no pixel drift.
    bool use_sun_override = false;

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
    // Split of draw_ms into the four spans that make it up. These were already
    // measured for the periodic [draw.zones] log line; surfacing them per-frame
    // is what lets a hitch report say WHICH span ate the frame instead of only
    // that the frame was long. A hitch capture showed draw dominating a 400 ms
    // frame with no way to attribute it further.
    float draw_vt_requests_ms = 0;   // service_vt_rung_requests
    float draw_cull_render_ms = 0;   // record_cull_and_render
    float draw_skin_seal_ms = 0;     // finish_animation_skinning_frame
    float draw_composite_ms = 0;     // record_composite_to_swapchain
    // per-frame counters
    uint32_t instances_resolved = 0;  // resolver output count
    uint32_t instances_drawn   = 0;   // clusters emitted by the GPU cull
    uint32_t clusters_culled   = 0;   // frustum-culled clusters
    uint32_t hiz_culled        = 0;   // HiZ-occlusion-culled clusters
    uint32_t triangles         = 0;   // rasterized triangle count
    uint32_t draw_batches      = 0;   // indirect draw buckets with >=1 instance
    // M2.5: terminal impostors holding an atlas slot right now. On the stats
    // overlay because "are any drawing?" is the question the abandoned
    // impostor tier could not answer -- it was absent for a whole generation
    // of artifacts and reported nothing.
    uint32_t resident_impostors = 0;
    // world/bake census (filled by request_bake / reload)
    uint32_t instances_total = 0;
    uint32_t parts_baked = 0;         // cache misses last bake
    uint32_t cache_hits  = 0;         // cache hits last bake
    // Phase C Task 9: world-kind sessions only; 0 for closed-world sessions.
    uint32_t resident_sectors = 0;
    // ---- occlusion streaming (M4) -----------------------------------------
    // What the cap is doing RIGHT NOW, surfaced because the feature is
    // otherwise invisible by design: it never hides a tile, it streams the
    // unseen ones coarser, so the frame looks identical whether it is on or
    // off. An issue report ("doesn't appear to hide sectors, it looks the same
    // when toggled") arrived from a session where it was working correctly and
    // had cut residency from 703 to 242 -- with nothing on screen to say so.
    //
    // occlusion_visible_sectors: sectors that owned a pixel in the last
    //                            harvested frame.
    // occlusion_capped_tiles:    octree nodes the cap is currently refusing to
    //                            split. This is the number that moves the
    //                            instant the toggle flips.
    uint32_t occlusion_visible_sectors = 0;
    uint32_t occlusion_capped_tiles = 0;
    bool     occlusion_active = false;
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
    // M6: registrations that reused an existing layer because it had the same
    // parameterisation, and rebuilds a finer rung forced. shared_refs is the
    // count of duplicate page sets NOT being fetched.
    uint64_t vt_shared_refs_total = 0;
    uint64_t vt_finer_rebuilds_total = 0;
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
    float gpu_rt_ms             = 0;   // primary/shadow trace only
    float gpu_rt_gi_ms          = 0;   // GI/reflection trace (0 when GI off)
    float gpu_denoise_ms        = 0;
    float gpu_dlss_ms           = 0;
    float gpu_composite_ms      = 0;
    float gpu_vol_ms            = 0;
    // Append-only detail lanes. gpu_vol_ms remains the combined froxel span.
    float gpu_atmosphere_ms     = 0;
    float gpu_cloud_shadows_ms  = 0;
    float gpu_vol_density_ms    = 0;
    float gpu_vol_scatter_ms    = 0;
    float gpu_vol_integrate_ms  = 0;
    uint32_t vol_grid_w = 0, vol_grid_h = 0, vol_grid_d = 0;
    uint64_t vol_memory_bytes = 0;
    uint64_t cloud_shadow_memory_bytes = 0;
    FroxelXyScale vol_effective_xy_scale = FroxelXyScale::X1_0;
    FroxelDepthSlices vol_effective_depth_slices = FroxelDepthSlices::D128;
    uint64_t vol_resource_generation = 0;
    bool vol_allocation_rejected = false;
    std::string vol_allocation_error;
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

    bool resolved_atmosphere_status(
        ResolvedAtmospherePresentationStatus& out) const;
    // Queues one one-shot reset of diffuse-GI, reflection/miss and volumetric
    // histories without changing the committed atmosphere generation.
    void request_atmosphere_history_reset();

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
        // Nested sector LOD. Informational in the panel: level L tiles are
        // sector_size << L across, so the SAME terrain_bands above are read as
        // per-level annuli rather than as per-sector LODs, and the outermost
        // band bounds residency instead of the outermost scatter ring. Read
        // back from the active profile; not settable as an override, because
        // switching tiling mid-session would strand every resident tile under
        // a keyspace the streamer no longer uses.
        bool nested_sectors = false;
        // Volumetric sectors (M3). Informational for the same reason and with
        // the same force: tiles are cubes on an octree rather than columns on a
        // quadtree, which changes the keyspace, so it is read back from the
        // active profile and never settable as a live override. Implies
        // nested_sectors -- the engine forces it off without one.
        bool volumetric_sectors = false;
        // Informational (filled by streaming_lod_config, ignored by
        // set_streaming_lod_overrides): the world's sector size, for UI
        // spacing hints.
        float sector_size = 64.0f;
        // Streamer pacing (matter_stream::Config's own knobs). Unlike the ring
        // lists above these have no "empty means keep the world's" encoding —
        // the world script cannot author them — so the defaults here ARE the
        // engine defaults and applying them unconditionally is a no-op.
        // MATTER_STREAM_HYSTERESIS / _INFLIGHT / _FAIL_COOLDOWN still apply in
        // make_streaming_profile for runs with no editor.
        float hysteresis = 16.0f;
        int   max_inflight = 64;
        int   fail_cooldown_updates = 64;
    };
    // The ACTIVE resolved profile of the current world (world JS + env +
    // overrides + engine defaults). False before a world-kind connect.
    bool streaming_lod_config(StreamingLodConfig& out) const;
    // World-authored volumetrics defaults (World.volumetrics static),
    // available once a world-kind connect completes. The editor adopts these
    // into its live volumetrics controls on world load.
    bool world_volumetrics(VulkanVolumetricsSettings& out) const;
    // World-authored physical atmosphere and cloud-shadow defaults, captured
    // at the same world-install seams as volumetrics.
    bool world_atmosphere(AtmosphereSettings& out) const;
    bool world_cloud_shadows(CloudShadowSettings& out) const;
    // World-authored fog (World.fog static), captured at every install /
    // compose / cone-rebake. False until the first successful connect. The
    // editor seeds its live render.fog override from this so the group's
    // layer-2 baseline is what the world script authored.
    bool world_fog(FogSettings& out) const;

    // World-authored sun, as the angles the Lighting panel edits: the inverse
    // of WorldSettings::sun_direction plus WorldSettings::
    // sun_angular_diameter_deg. Captured at the same seams world_fog is, and
    // false until the first successful connect.
    //
    // The editor MUST seed VulkanLightingOverrides from this before the
    // property registry captures the render.lighting baseline; that is what
    // makes layer 2 equal what the world script authored. It is also what lets
    // the engine recognise an untouched override and pass the authored
    // direction through unconverted — see RenderOptions::use_sun_override.
    bool world_sun(SunAngles& out) const;

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

    // ---- Per-module draw overrides (view-time filter) ---------------------
    // The modules whose instances this world can draw, sorted and deduplicated
    // — the row list of the editor's Draw Overrides section. Filled at connect
    // from the provider's bake plan (closed worlds) and the streaming asset
    // install (world-kind sessions). False before the first connect.
    //
    // Deliberately NOT included: the per-sector `WorldSector` container itself.
    // Its geometry is the terrain heightfield/volume the sector script emits
    // directly, and its content hash is per sector rather than per module, so
    // it has no stable module identity to hang an override on. Terrain is
    // therefore outside this control (see draw_overrides() below).
    bool world_modules(std::vector<std::string>& out) const;

    // The per-module draw-override group (hide / max_draw_distance / lod_bias
    // per module), as a live property group the editor binds into its registry
    // exactly like world_props(). Null when the world has no modules.
    //
    // OWNED BY THE SESSION, and — like world_props() — rebuilt at a world-kind
    // connect ONLY when the module set actually changed, so a reload or a
    // live-edit rebake never swaps the group out from under a mid-tune editor
    // binding. A caller that binds it MUST unbind before triggering the
    // reconnect that could replace it.
    //
    // Unlike world_props(), the ENGINE reads this group back: render() samples
    // its value buffer once per frame and, when it changed, re-resolves the
    // module-keyed overrides onto part hashes. All-default values (the state
    // every headless/replay/test run stays in, since nothing binds or edits
    // the group there) are a bit-exact no-op: no instance is skipped and the
    // GPU cull lane stays at its neutral one-entry table.
    props::DynamicGroup* draw_overrides();

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

    // ---- Seam welding (volumetric-sectors M0-WP3b) --------------------------
    //
    // Accounting for the runtime cross-level seam welder
    // (docs/volumetric-sectors-design-2026-08-10.md §4.1). The geometry figures
    // describe the pool; the draw figures below describe what M0-WP8 put on
    // screen from it (resolution R3: one content-addressed part per cross-level
    // face pair through the existing ensure_part path).
    //
    // App thread. Pool figures are recomputed from the live pool on each call;
    // the counters are cumulative since the last world teardown.
    struct SeamWeldStatus {
        // ---- live pool ----
        int      pairs = 0;         // cross-level face pairs currently welded
        uint64_t triangles = 0;     // triangles across the pool
        // seam::WeldStats summed over the pool. The identity
        // crossings == quads + tris + missing_landing + degenerate holds here
        // too, because it holds per weld_face call and all four are sums.
        int crossings = 0;
        int quads = 0;
        int tris = 0;
        int missing_landing = 0;
        int sign_conflicts = 0;
        int degenerate = 0;

        // ---- cumulative counters ----
        //
        // R4: a MISSING RECORD is not a missing weld's fault. A sector staged
        // off disk (the staged_load->ok == false -> get_or_load fallback) has no
        // boundary record at all, so it welds nothing and draws with today's
        // overlap behaviour. Counted apart from missing_landing precisely so
        // that a future bisect does not read "no weld here" as a welder bug.
        uint64_t drawn_without_record = 0;
        // Pairs built while fewer fine tiles were drawn than the 2:1 pairing
        // expects. A FINE-side gap is a transient the publish gate is meant to
        // shrink; a coarse-side one is irreducible geometry. weld_face reports
        // one missing_landing number and cannot tell them apart, so these two
        // fields carry the attribution the pair's shape can prove.
        int      fine_side_incomplete = 0;   // live pairs, not cumulative
        // R11: these two used to be one field that said "live pairs" and held a
        // sum of lookups -- 9,054 against 72 pairs, which as a pair count is
        // nonsense and which a bisect would misread. They are now separate and
        // each says what it is.
        //   coarse_side_nulls   LIVE PAIRS on which at least one coarse-side
        //                       landing lookup failed. A pair count, comparable
        //                       against `pairs`.
        //   coarse_null_lookups the raw total those pairs recorded, summed over
        //                       the pool. `side_at` increments it once per
        //                       failed lookup, so a busy plane contributes
        //                       thousands on its own; it is a magnitude, not a
        //                       population, and it is refreshed on each pair's
        //                       last rebuild rather than accumulated over time.
        int      coarse_side_nulls = 0;      // live pairs, not cumulative
        uint64_t coarse_null_lookups = 0;    // live pairs' lookup total
        // Face pairs rejected because the two sides were more than one level
        // apart. Non-zero means the drawn +-1 invariant broke (§4.4/§4.5).
        uint64_t level_gap_pairs = 0;
        uint64_t build_errors = 0;
        // §4.5 half 2. `level_holds` counts tiles the gate withheld;
        // `drawn_level_violations` counts tiles that became drawn anyway with a
        // >= 2-level drawn face neighbour -- i.e. what the gate could not
        // safely stop, which is the number that says whether the streamer's
        // staged refinement is doing its job.
        uint64_t level_holds = 0;
        uint64_t drawn_level_violations = 0;
        // The MERGE direction's mechanism, and deliberately a separate number
        // from `level_holds` so a soak can tell the two apart.
        //
        // `level_holds` is a PARK count: it counts the tile being withheld, at
        // the publish/unpark sites, under the coverage-qualified +-1 gate.
        // `merge_coverage_holds` counts EVICTIONS DEFERRED -- a different tile
        // (a drawn fine tile), at a different site (the eviction batch), to
        // keep a parked coarse newcomer's footprint covered so that it stays
        // parked under the existing coverage clause. Nothing new is ever
        // parked by it, which is what preserves `parked => covered`.
        //
        // Counted once per eviction on first deferral, never on the per-batch
        // retries, so it is a population and not a frame counter. A hold that
        // outlives the 30 s valve prints one line naming the condition that
        // survived; a surviving hold means the neighbouring footprint's coarse
        // target never became resident, i.e. a bake/streamer failure rather
        // than an accepted race.
        uint64_t merge_coverage_holds = 0;

        // ---- the drawn representation (M0-WP8) ----
        //
        // `drawn_pairs` <= `pairs`: a pair carries geometry but draws nothing
        // in a headless session, under MATTER_NO_SEAM_WELD_DRAW=1, or when a
        // registration was refused. That gap is fail-soft by design -- the
        // tiles' own baked border bands still overlap the seam (§4.1
        // consequence 4) -- so a non-zero difference is a diagnostic, not an
        // error.
        int      drawn_pairs = 0;        // live pairs with a registered part
        int      registered_parts = 0;   // renderer parts the welder holds
        uint64_t drawn_triangles = 0;    // triangles across the drawn pairs
        // Cumulative.
        //
        // `parts_registered` on its own IS NOT A CHURN MEASURE, and reading it
        // as one is what made the M0 soak's "noop_rebuilds : parts_registered"
        // of ~0.9 : 1 look like a defect. It counts three different events, and
        // only one of them is churn. Use the breakdown:
        //
        //     parts_registered == pairs_created + pairs_recontented
        //
        //   pairs_created      a cross-level face pair APPEARED in the drawn
        //                      set and got its first part. Irreducible -- the
        //                      seam did not exist a moment ago -- and it
        //                      dominates on a moving camera, because every band
        //                      boundary the view sweeps past creates and
        //                      retires pairs. 196 registrations against 97 live
        //                      pairs is roughly two per pair, which is the pair
        //                      LIFECYCLE, not repeated churn on one pair.
        //   pairs_recontented  a LIVE pair's geometry changed and its renderer
        //                      part was swapped. THIS is the number to compare
        //                      against `noop_rebuilds`. Its irreducible core is
        //                      the 2:1 fine side arriving one sibling at a
        //                      time: the first sibling drawn gets a half-plane
        //                      weld and the second re-welds the whole plane.
        //
        // And note what `noop_rebuilds` does and does not measure. The R2
        // fan-out is PRECISE -- rebuild_welds_for(key) enumerates only pairs
        // that `key` is itself a side of, never a neighbour's unrelated pairs
        // -- so a rebuild nearly always coincides with a real change to one of
        // the pair's own sides. "Almost every rebuild is a no-op in the steady
        // state" describes a coarser fan-out than the one that was built; the
        // no-ops that do occur are the side events that did not move geometry
        // (a parked publish, a same-level variant republish, a second sibling
        // rebuilt in a batch that already welded the pair, the eviction of a
        // tile that was never shown). A ratio near 1 : 1 is therefore normal,
        // and the thing to watch is `pairs_recontented` growing faster than the
        // drawn set moves.
        uint64_t parts_registered = 0;
        uint64_t parts_released = 0;
        uint64_t noop_rebuilds = 0;
        uint64_t pairs_created = 0;
        uint64_t pairs_recontented = 0;
        // MUST BE ZERO. A live pair whose input fingerprint did not move but
        // whose geometry did -- i.e. the welder is not a function of its
        // inputs. That is a determinism bug, not a performance one: the no-op
        // path above and R3's ensure_part(new)/release_part(old) swap both rest
        // on identical geometry re-deriving an identical content hash. The
        // engine also prints the first occurrence to stderr.
        uint64_t content_changed_inputs_stable = 0;
        uint64_t register_failures = 0;
        // A weld part hash that already named a baked part, or a weld instance
        // id that was already live. Both are ~2^-64 events that would corrupt
        // an unrelated instance if acted on, so they are refused and counted
        // rather than assumed away.
        uint64_t hash_collisions = 0;
        uint64_t id_collisions = 0;
        // High-water mark of `pairs`. The structural bound is 4 x drawn tiles;
        // see kSeamWeldPairAlarm in matter_engine.cpp for the ceiling survey
        // this number is judged against.
        int      pairs_peak = 0;
    };
    SeamWeldStatus seam_weld_status() const;

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
