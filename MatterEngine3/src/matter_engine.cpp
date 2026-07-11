// matter_engine.cpp — Stage 2b facade: EngineContext / WorldSession over the
// in-process viewer pipeline. Implements the public API in matter/engine_context.h
// and matter/world_session.h. The logic is relocated verbatim from viewer/main.cpp
// (same constants, same ordering, same comments) so that switching main.cpp to this
// facade in Task 6 produces pixel-identical screenshots.
//
// Task 7 will implement raycast/instance_count/instance_info (currently stubs).

#include "matter/engine_context.h"
#include "matter/world_session.h"

#include "async_bake.h"
#include "local_provider.h"
#include "part_store.h"   // LoadedPart, walk_part_tree
#include "world_composer.h"
#include "sector_resolver.h"
#include "renderer.h"
#include "raster_composer.h"
#include "probe_texture.h"
#include "gpu_culler.h"
#include "raster_cull.h"
#include "gl46.h"
#include "shader_source.h"   // matter::set_shader_override_dir (Task 1 header)
#include "world_tracer.h"    // WorldTracer — lazy CPU BVH for query API

// Task 10: live-edit watcher + production seams.
#include "live_edit.h"
#include "live_edit_prod.h"
#include "part_graph_snapshot.h"

// Phase C Task 6: camera-driven refine loop.
#include "refine_controller.h"

// Phase C Task 9: sector streamer.
#include "sector_streamer.h"
#include "terrain_field.h"

// Phase C Task 17: resolve/manifest cache for instant warm relaunch.
#include "resolve_cache.h"
#ifdef __linux__
#include "inotify_watcher.h"
#endif

// Raylib must come before glad to avoid double-definition of GL types.
#include "raylib.h"
#include "external/glad.h"   // glClearColor / glClear (same as gpu_culler.cpp)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace matter {

// Task 10: NullWatcher — a FileWatcher that never yields events.
// Used as the FileWatcher argument for LiveEditSession constructed on the
// worker thread (which uses rebuild(paths) directly, never tick()).
namespace {
class NullWatcher : public live_edit::FileWatcher {
public:
    void add_watch(const std::string&) override {}
    int poll(std::vector<live_edit::FileEvent>&) override { return 0; }
    long long now_ms() override {
        auto t = std::chrono::steady_clock::now().time_since_epoch();
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
    }
};
} // anonymous namespace

// Classify a provider-returned error string into a structured code. The
// executor doesn't get typed errors back from the pipeline (yet — Task 7 wires
// finer-grained tags), so we bucket by substring for now. Task 7 will overlay
// the real code plumbing; keeping it here makes it easy to hoist later.
//
// Priority order (highest → lowest):
//   1. Cancelled / shutdown   — always terminal; must not misclassify as I/O
//   2. OOM / bad_alloc        — memory exhaustion
//   3. GPU errors             — shader/GL failures
//   4. Script errors          — evaluated BEFORE I/O: a script message may
//      contain "load" or "not found" (e.g. "failed to load module"), so the
//      script bucket must win over the I/O bucket.
//   5. I/O errors             — missing files / manifest failures
//   6. Internal               — catch-all
static BakeErrorCode classify_error(const std::string& err) {
    if (err.find("cancel") != std::string::npos ||
        err.find("shutdown") != std::string::npos)
        return BakeErrorCode::Cancelled;
    if (err.find("out of memory") != std::string::npos ||
        err.find("bad_alloc") != std::string::npos)
        return BakeErrorCode::OutOfMemory;
    if (err.find("GPU") != std::string::npos ||
        err.find("gpu") != std::string::npos ||
        err.find("shader") != std::string::npos ||
        err.find("GL") != std::string::npos)
        return BakeErrorCode::GpuError;
    if (err.find("bake ") != std::string::npos ||
        err.find("script") != std::string::npos ||
        err.find("install") != std::string::npos ||
        err.find("resolve hash") != std::string::npos ||
        err.find("evaluate") != std::string::npos)
        return BakeErrorCode::ScriptError;
    if (err.find("manifest") != std::string::npos ||
        err.find("not found") != std::string::npos ||
        err.find("load") != std::string::npos)
        return BakeErrorCode::IoError;
    return BakeErrorCode::Internal;
}

// ---------------------------------------------------------------------------
// EngineContext::Impl — minimal engine-level state shared by all sessions.
// ---------------------------------------------------------------------------
struct EngineContext::Impl {
    std::string cache_root;
    bool gl46 = false;
};

// ---------------------------------------------------------------------------
// WorldSession::Impl — per-world session state (mirrors main.cpp locals).
// ---------------------------------------------------------------------------
struct WorldSession::Impl {
    EngineContext::Impl* engine = nullptr;   // non-owning

    // Provider config and live provider instance.
    // Phase C Task 14: shared_ptr so publish job lambdas can capture a strong
    // reference, ensuring the provider outlives all in-flight publish jobs even
    // if a superseded bake creates a new provider before the old jobs complete.
    viewer::LocalProviderConfig cfg;
    std::shared_ptr<viewer::LocalProvider> provider;

    // World data.
    viewer::WorldManifest manifest;
    viewer::WorldState    state;

    // Compositor objects.
    std::unique_ptr<viewer::PartStore>      store;
    std::unique_ptr<viewer::WorldComposer>  composer;
    std::unique_ptr<viewer::RasterComposer> raster;
    lod_select::PartLodTable                lods;

    // Probe textures (released before recreating on reconnect/shutdown).
    viewer::ProbeTextures probe_tex{};

    // Sky clear color: derived from tone-mapped sky_color in bake_once().
    // Three separate floats kept as gamma-mapped floats passed to glClearColor
    // (same value as main.cpp's `sky_clear` Color struct, but stored as floats
    // pre-divided by 255 to avoid 8-bit quantization before the GL clear).
    // Values are: (unsigned char)(gamma * 255.f + 0.5f) / 255.f so the
    // fractional part rounds to exactly the same 8-bit bucket as
    // ClearBackground(sky_clear) — pixel-identical output guaranteed.
    float sky_clear[3] = {96 / 255.f, 118 / 255.f, 143 / 255.f};

    // GPU culler (per-session; initialized once per session after first bake).
    viewer::GpuCuller gpu_culler;
    bool              culler_ready = false;

    // RT renderer (camera synced per render; shader lazily initialized on first
    // Raytrace render to avoid the ~60s shader warm-up until it's actually needed).
    viewer::Renderer renderer;
    bool             rt_shader_ready = false;
    bool             rt_warmed       = false;

    // Resolvers — mirrors main.cpp's pass/sec locals.
    viewer::PassThroughResolver pass;
    // Constructor radius overwritten immediately by opts.active_radius in render();
    // 64.0f keeps it valid before first render call.
    viewer::SectorLodResolver sec{16.0f, 64.0f};

    std::atomic<bool> connected{false};

    // Event queue — capped at 4096 to prevent unbounded growth if the app
    // never drains (older events dropped when the cap is hit).
    // Phase B: worker + app thread both touch this, so every access is
    // guarded by events_mutex.
    std::deque<Event> events;
    std::mutex        events_mutex;

    // Phase C Task 3: bake focus point (app thread writes, worker reads).
    // Uses its own mutex (separate from events_mutex) so a set_bake_focus call
    // never contends with event fan-out. Initialized to (0,0,0).
    std::mutex focus_mutex;
    float      focus[3] = {0.f, 0.f, 0.f};

    // Phase C Task 7: root-params override for regenerate(seed). App thread writes
    // under seed_mutex; execute_bake() reads a snapshot under the same mutex and
    // installs it into cfg.root_params_json before constructing the provider.
    // Empty string = no override (default). Set by WorldSession::regenerate().
    std::mutex  seed_mutex;
    std::string seed_root_params_json;

    FrameStats stats{};

    // Phase B: async bake GPU work queue + command queue + worker thread.
    matter_async::GpuJobQueue gpu_jobs;
    matter_async::CommandQueue commands;
    std::thread                worker;
    // Set to true while the worker is inside a command (BakeAll/Reload); read
    // by tick() to skip provider->poll_deltas() while the provider is being
    // torn down and rebuilt. Cheap insurance: LocalProvider::poll_deltas
    // currently always returns false, but the flag also fences future providers.
    std::atomic<bool> bake_active{false};

    // Lazy CPU tracer for the query API (raycast/instance_count/instance_info).
    // Built on first query after a bake. mutable so instance_count() const can build.
    mutable std::unique_ptr<world_tracer::WorldTracer> tracer;
    mutable bool tracer_dirty = true;  // true after every bake/reload

    // hash -> module name for expanded_instance info (filled from manifest)
    mutable std::unordered_map<uint64_t, std::string> module_by_hash;

    // Ensure tracer is built and up-to-date. Returns false if build failed.
    bool ensure_tracer() const;

    // --- Phase B: async bake worker helpers (defined below) ------------------
    // Emit an event onto the queue (thread-safe). Applies the 4096 cap.
    void emit_event(Event ev);
    // Start the worker thread if not already running.
    void ensure_worker_started();
    // Worker thread entry point.
    void worker_loop();
    // Execute one BakeAll/Reload command. Called only on the worker thread.
    void execute_bake(matter_async::Command& cmd, bool is_reload);
    // Execute a RebakeCone command. Called only on the worker thread.
    void execute_rebake_cone(matter_async::Command& cmd);
    // Phase C Task 6: execute one camera-driven refine step.
    // Called by worker_loop in the pop_wait timeout path when refine_ctrl is live.
    void execute_refine_step();
    // Phase C Task 9: execute one sector-streamer step (world-kind sessions).
    // Called by worker_loop in the pop_wait timeout path when sector_streamer is live.
    void execute_sector_stream_step();
    // Phase C Task 9: install world-kind field, set world binding on host_baker,
    // install sector child assets. Called from execute_bake after install_graph
    // succeeds when provider->world_module() is non-empty.
    bool install_world(const std::shared_ptr<matter_async::CancelToken>& token,
                       std::string& err);
    // Phase C Task 9: drain sector evictions — release resources for evicted sectors.
    // Called on the worker thread (within a gpu_jobs.run_blocking context or inline).
    void drain_sector_evictions();

    // Parameters for publish_pipeline — the genuine differences between the
    // BakeAll/Reload and RebakeCone publish flows.
    struct PublishPipelineParams {
        // Job name prefix: "bake" or "cone" (used in GpuJob::name strings and
        // assert_gl_thread markers so tracing distinguishes the two executors).
        std::string job_prefix;
        // Initial error count. execute_bake seeds this from install-phase
        // failures; execute_rebake_cone passes 0 (cone install errors are
        // emitted but not counted in BakeFinished.errors).
        int count_errors_seed = 0;
        // Whether to attempt first-success GpuCuller init inside the reset job.
        // True for BakeAll/Reload (first bake arms the culler); false for
        // RebakeCone (culler already initialized by the prior full bake).
        bool init_culler = false;
        // Whether to update stats.probe_dims in the finalize job.
        // True for BakeAll/Reload (full world manifest); false for RebakeCone
        // (cone is a partial rebuild; probe layout is unchanged).
        bool update_probe_dims = false;
        // Whether to emit verbose diagnostic prints inside the reset job
        // (raster init error, probe-unavailable warning, sky-clear color).
        // True for BakeAll/Reload; false for RebakeCone.
        bool verbose_reset_log = false;
        // Test fault hook, forwarded into each publish job lambda.
        // Non-null only in test builds that call set_test_fault_hook().
        // execute_rebake_cone does not wire the hook (cone is not injection-tested).
        std::function<void(int)> fault_hook;
        // Whether to append " (hash N)" to the load-failure message.
        // True for BakeAll/Reload (matches existing message format);
        // false for RebakeCone (existing message omits the hash).
        bool load_msg_include_hash = false;
        // Phase C Task 14: demand-driven bake. When non-null, each publish job
        // calls provider_ref->ensure_part_baked() + ensure_part_flattened()
        // before the GPU load, and streams newly discovered FlatInstanceRefs
        // onto the tail of publish_order. Null for cone (cone re-bake via
        // LiveEditSession; all artifacts pre-exist).
        std::shared_ptr<viewer::LocalProvider> provider_ref;
    };

    // Shared publish flow: steps 4-8 (reset job → reconcile → per-part publish
    // jobs → finalize job → BakeFinished). Called only on the worker thread.
    // `new_manifest` is consumed (moved in) by the reset job.
    void publish_pipeline(const std::shared_ptr<matter_async::CancelToken>& token,
                          viewer::WorldManifest new_manifest,
                          const PublishPipelineParams& p);

    // --- Task 10: live-edit watcher state (app thread only) ------------------
    bool enable_live_edit = false;

#ifdef __linux__
    // Inotify watcher, lazily created in tick() when enable_live_edit is true.
    std::unique_ptr<live_edit::InotifyWatcher> inotify_watcher;
    bool inotify_watching = false;  // dirs added to the watcher?
#endif

    // Debounce state (mirrors LiveEditSession::tick debounce, ~15 lines).
    // App thread only — no mutex needed.
    std::set<std::string> le_pending_paths_;
    long long le_last_event_ms_ = 0;
    bool le_have_pending_ = false;
    static constexpr long long k_debounce_ms = 150;

    // --- Phase C Task 6: camera-driven refine loop ----------------------------
    // RefineController: built from the graph snapshot + world instances after each
    // full publish finishes; rebuilt on supersession (BakeAll/Reload). Null when no
    // world is live or the bake has not yet finished.
    // Accessed ONLY on the worker thread — no mutex needed.
    std::unique_ptr<matter_refine::RefineController> refine_ctrl;

    // Provider kept alive through the refine phase so ensure_part_baked/flattened
    // remain callable without new machinery. Set at the end of publish_pipeline
    // (same shared_ptr as pp.provider_ref); cleared when the refine phase ends or
    // a new bake supersedes this session.
    std::shared_ptr<viewer::LocalProvider> refine_provider;

    // Refine radius (XZ eviction distance in world units). Read ONCE at open_world
    // time from MATTER_REFINE_RADIUS env var; default 160.0f (≈ 10 tiles of 10 m).
    // Exposed as an env override so tests can set a small radius without recompile.
    float refine_radius = 160.0f;

    // Tile count from the most-recent RefineController::build() (used in emitted events).
    // Worker thread only.
    size_t refine_tile_count = 0;

    // --- Phase C Task 9: SectorStreamer world-kind loop ----------------------
    // Non-null when the current session opened a world-kind manifest.
    // Owned by the worker thread (created in publish_pipeline tail, cleared
    // on supersession / shutdown). Null for closed-world sessions.
    std::unique_ptr<matter_stream::SectorStreamer> sector_streamer;

    // World-kind field runtime (owned; lives for the session generation).
    // Null for closed-world sessions or before install completes.
    std::unique_ptr<terrain_field::FieldRuntime> world_field;

    // Sea level from the most recent eval_world result (-inf = not a world).
    float world_sea_level = std::numeric_limits<float>::lowest();

    // Biomes JSON forwarded to every sector bake.
    std::string world_biomes_json;

    // worldSeed from the most recent install (forwarded to sector params).
    uint64_t world_seed = 0;

    // fieldHash = 16-hex-digit string of FieldRuntime::hash().
    std::string world_field_hash;

    // child_hashes for every WorldSector bake (the fixed 28-variant asset set).
    // Populated after the asset install in publish_pipeline; the order matches
    // child_modules/child_params and is identical for every sector.
    std::vector<uint64_t>    sector_child_hashes;
    std::vector<std::string> sector_child_modules;
    std::vector<std::string> sector_child_params;

    // Resident-sector map: (tx,tz,rung) → {instance_id, part_hash}.
    // Worker thread writes at publish; GL thread writes at evict (but the
    // eviction apply also happens on the GL thread, so no data race if we
    // restrict all sector_map writes/reads to the GL thread via the gpu_jobs
    // queue).  To keep things simple we guard with sector_map_mutex.
    struct SectorEntry { uint32_t instance_id; uint64_t part_hash; };
    struct SectorKey {
        int64_t tx, tz; int rung;
        bool operator==(const SectorKey& o) const { return tx==o.tx && tz==o.tz && rung==o.rung; }
    };
    struct SectorKeyHash {
        size_t operator()(const SectorKey& k) const {
            uint64_t h = (uint64_t(uint32_t(int32_t(k.tx))) << 32) | uint32_t(int32_t(k.tz));
            h ^= h >> 33;
            h ^= uint64_t(k.rung) * 0xbf58476d1ce4e5b9ull;
            return (size_t)h;
        }
    };
    std::unordered_map<SectorKey, SectorEntry, SectorKeyHash> sector_map;
    std::mutex sector_map_mutex;

    // Next instance id for sector placements (separate from the publish-pipeline
    // next_manifest_id so the two don't collide; starts at 0x10000000 for sectors).
    uint32_t sector_next_id = 0x10000000u;

    // Sector size from the eval_world result (world units per sector tile).
    float world_sector_size = 16.0f;

    // Cached sector source text (WorldSector.js read once at install_world time).
    std::string world_sector_source;

    // True once the first streaming cycle has completed with no remaining holes.
    // Reset on BakeAll/Reload/regenerate. Used to emit BakeFinished for world-kind.
    bool world_initial_load_done = false;

    // I1 fix: deferred upgrade results from GL jobs.
    // Worker-thread-only. Each entry records a pending refine.upgrade outcome:
    //   tile_idx — index in refine_ctrl to mark Full (success) or Coarse (failure)
    //   tile_tx / tile_tz — for the RefineTileDone event
    //   result  — shared_ptr<atomic<int>>: 0=in-flight, 1=success, 2=failure
    // The GL job writes the result; the worker drains at the START of each step
    // and calls mark() + emits the event. Worker owns ALL mark() calls.
    struct PendingUpgrade {
        uint32_t tile_idx;
        int      tile_tx;
        int      tile_tz;
        std::shared_ptr<std::atomic<int>> result; // 0=in-flight, 1=success, 2=fail
    };
    std::vector<PendingUpgrade> refine_pending_upgrades_;
};

// ---------------------------------------------------------------------------
// WorldSession::Impl::emit_event / ensure_worker_started / worker_loop
// Phase B: worker command loop and event fan-out.
// ---------------------------------------------------------------------------

void WorldSession::Impl::emit_event(Event ev) {
    std::lock_guard<std::mutex> lk(events_mutex);
    if (events.size() >= 4096) events.pop_front();
    events.push_back(std::move(ev));
}

void WorldSession::Impl::ensure_worker_started() {
    if (worker.joinable()) return;
    worker = std::thread([this] { worker_loop(); });
}

void WorldSession::Impl::worker_loop() {
    // Phase C Task 6: refine loop.
    // After a bake finishes (bake_active = false) and a RefineController is live,
    // the worker uses pop_wait(50ms) instead of pop() so it can service one refine
    // step per timeout slot.  Commands always win: a bake/reload command cancels and
    // rebuilds the refine controller.
    //
    // Invariant: a refine step never runs while a command is pending or a bake is in
    // flight — commands always win the pop.

    for (;;) {
        // When a refine controller OR sector streamer is live, use timed pop so
        // idle slots drive refine/streaming steps.  Commands always win the pop.
        if (refine_ctrl || sector_streamer) {
            matter_async::Command cmd;
            bool timed_out = false;
            bool got_cmd = commands.pop_wait(cmd, /*ms=*/50, timed_out);

            if (!got_cmd && !timed_out) {
                // Shutdown + drained. Reset state and exit.
                refine_ctrl.reset();
                refine_provider.reset();
                refine_pending_upgrades_.clear();
                sector_streamer.reset();
                return;
            }

            if (got_cmd) {
                // A command arrived — execute it (supersedes refine/stream).
                if (cmd.kind == matter_async::CommandKind::Shutdown) {
                    refine_ctrl.reset();
                    refine_provider.reset();
                    refine_pending_upgrades_.clear();
                    sector_streamer.reset();
                    return;
                }
                // BakeAll/Reload: reset refine controller (will be rebuilt post-publish).
                if (cmd.kind == matter_async::CommandKind::BakeAll ||
                    cmd.kind == matter_async::CommandKind::Reload) {
                    refine_ctrl.reset();
                    refine_provider.reset();
                    refine_pending_upgrades_.clear();
                    // Phase C Task 9: drain sector evictions before destroying streamer.
                    if (sector_streamer) {
                        sector_streamer->clear();
                        drain_sector_evictions();
                    }
                    sector_streamer.reset();
                    world_field.reset();
                    world_initial_load_done = false;
                }
                bake_active.store(true, std::memory_order_release);
                try {
                    switch (cmd.kind) {
                        case matter_async::CommandKind::BakeAll:
                            execute_bake(cmd, /*is_reload=*/false);
                            break;
                        case matter_async::CommandKind::Reload:
                            execute_bake(cmd, /*is_reload=*/true);
                            break;
                        case matter_async::CommandKind::RebakeCone:
                            execute_rebake_cone(cmd);
                            break;
                        case matter_async::CommandKind::Shutdown:
                            bake_active.store(false, std::memory_order_release);
                            refine_ctrl.reset();
                            refine_provider.reset();
                            refine_pending_upgrades_.clear();
                            return;
                    }
                } catch (std::bad_alloc&) {
                    Event ev;
                    ev.type    = EventType::BakeError;
                    ev.code    = BakeErrorCode::OutOfMemory;
                    ev.message = "std::bad_alloc";
                    emit_event(std::move(ev));
                } catch (std::exception& e) {
                    Event ev;
                    ev.type    = EventType::BakeError;
                    ev.code    = BakeErrorCode::Internal;
                    ev.message = e.what();
                    emit_event(std::move(ev));
                }
                bake_active.store(false, std::memory_order_release);
                continue;
            }

            // Timed out with no command — take ONE refine/stream step.
            if (refine_ctrl) {
                execute_refine_step();
            } else if (sector_streamer) {
                execute_sector_stream_step();
            }
            continue;
        }

        // No refine controller or sector streamer: block until next command.
        matter_async::Command cmd;
        if (!commands.pop(cmd)) return;   // shutdown + drained
        if (cmd.kind == matter_async::CommandKind::Shutdown) return;

        bake_active.store(true, std::memory_order_release);
        try {
            switch (cmd.kind) {
                case matter_async::CommandKind::BakeAll:
                    execute_bake(cmd, /*is_reload=*/false);
                    break;
                case matter_async::CommandKind::Reload:
                    execute_bake(cmd, /*is_reload=*/true);
                    break;
                case matter_async::CommandKind::RebakeCone:
                    execute_rebake_cone(cmd);
                    break;
                case matter_async::CommandKind::Shutdown:
                    bake_active.store(false, std::memory_order_release);
                    return;
            }
        } catch (std::bad_alloc&) {
            Event ev;
            ev.type    = EventType::BakeError;
            ev.code    = BakeErrorCode::OutOfMemory;
            ev.message = "std::bad_alloc";
            emit_event(std::move(ev));
        } catch (std::exception& e) {
            Event ev;
            ev.type    = EventType::BakeError;
            ev.code    = BakeErrorCode::Internal;
            ev.message = e.what();
            emit_event(std::move(ev));
        }
        bake_active.store(false, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::execute_bake
// The worker-side command executor. Runs the BakeAll/Reload pipeline on the
// worker thread, marshaling GL work to the app/GL thread via gpu_jobs.
// Steps mirror the Task 6 brief: emit BakeStarted -> install -> compose ->
// GL reset job -> reconcile job (store swap) -> per-part publish jobs
// (interleaved with cancellation checks) -> finalize job -> BakeFinished.
// ---------------------------------------------------------------------------
void WorldSession::Impl::execute_bake(matter_async::Command& cmd, bool is_reload) {
    auto& token = cmd.token;
    auto is_cancelled = [&] { return token && token->is_cancelled(); };

    // 1) BakeStarted -----------------------------------------------------------
    {
        Event ev;
        ev.type = EventType::BakeStarted;
        emit_event(std::move(ev));
    }

    // Emit-a-BakeError helper (worker-side, so all call sites just tag phase).
    auto emit_error = [&](BakeErrorCode code, const char* phase, const std::string& msg) {
        Event ev;
        ev.type    = EventType::BakeError;
        ev.code    = code;
        ev.phase   = phase;
        ev.message = msg;
        emit_event(std::move(ev));
    };

    // 2) Build a fresh provider and install the part graph --------------------
    // The provider is per-command: on_part is wired to emit BakePartDone with
    // the appropriate phase, and gpu_run marshals tileset GL to the app thread
    // via gpu_jobs.run_blocking (this Task 6 seam; Task 4 added the field).
    // The reload variant additionally resets the GPU culler before we begin.
    if (is_reload && culler_ready) {
        // GpuCuller state lives on the GL thread. Marshal the reset.
        matter_async::GpuJob rj;
        rj.name  = "gpu_culler.reset";
        rj.token = token;
        rj.fn    = [this](std::string& /*err*/) {
            matter_async::assert_gl_thread("gpu_culler.reset");
            gpu_culler.reset();
            return true;
        };
        std::string rerr;
        if (!gpu_jobs.run_blocking(std::move(rj), rerr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : BakeErrorCode::GpuError,
                       "gl", rerr.empty() ? "gpu_culler.reset failed" : rerr);
            return;
        }
    }
    // In reload mode, marking `connected = false` is done inside the GL reset
    // job below (same thread that will set it back to true), so old-world
    // rendering continues until the GL reset actually runs (fail-closed matches
    // today's reload behavior, and the write is on the app/GL thread — no race).

    // Install-phase on_part carries phase="install" (total==0, indeterminate).
    cfg.on_part = [this](const char* module, int done, int total) {
        Event ev;
        ev.type   = EventType::BakePartDone;
        ev.module = module ? module : "";
        ev.done   = done;
        ev.total  = total;
        // total==0 => install phase; total>0 => fetch/parts phase.
        // Distinguish by total: install fires with 0, per-part with want.size().
        ev.phase  = (total == 0) ? "install" : "parts";
        emit_event(std::move(ev));
    };
    // Bind gpu_run to marshal tileset GL work to the app thread via gpu_jobs.
    cfg.gpu_run = [this, token](const char* name,
                                std::function<bool(std::string&)> fn,
                                std::string& err) -> bool {
        matter_async::GpuJob j;
        j.name  = name ? name : "gpu";
        j.fn    = std::move(fn);
        j.token = token;
        return gpu_jobs.run_blocking(std::move(j), err);
    };
    // Signal whether GLAD function pointers are loaded (i.e., a window exists)
    // so the tileset phase can choose between headless settle-only and full GPU
    // atlas bake. GLAD pointers are process-global and written once at window
    // init before any bake, so reading them from the worker thread is safe.
    // Using gl_loaded() rather than engine->gl46 so that RT/viewer contexts
    // (allow_gl_lt_46=true) still attempt the atlas bake and get a proper
    // success-or-clear-error from gl46_available(), instead of a silent skip.
    cfg.gl_available = viewer::gl_loaded();

    // Phase C Task 7: snapshot the root-params override (set by regenerate()).
    // Read under seed_mutex so a concurrent regenerate() on the app thread is
    // safe. Installed into cfg so the fresh LocalProvider sees it in install_graph().
    {
        std::lock_guard<std::mutex> lk(seed_mutex);
        cfg.root_params_json = seed_root_params_json;
    }

    provider = std::make_shared<viewer::LocalProvider>(cfg);

    // Instrumentation rider (Phase C): coarse phase timing for large-world
    // attribution. Tileset phase is embedded inside compose_world (not
    // separable without surgery into LocalProvider), so compose_ms covers
    // scatter + flatten + tileset bake. Summary emitted at BakeFinished time.
    using clk_t = std::chrono::steady_clock;
    auto t_bake_start = clk_t::now();

    // Phase C Task 17: resolve/manifest cache — skip JS eval on warm launches.
    // Live-edit sessions bypass the cache (they need a live script host graph).
    // Fail-closed: any anomaly (bad key, truncated file, probe miss) falls through
    // to the full install+compose path.
    bool resolve_cache_hit = false;
    uint64_t rc_cache_key  = 0;
    if (!enable_live_edit && !cfg.schemas_dir.empty() && !cfg.world_data_dir.empty()) {
        const std::string world_manifest_path =
            cfg.world_data_dir + "/" + cfg.world_name + "/world.manifest";
        rc_cache_key = resolve_cache::compute_key(
            world_manifest_path,
            cfg.root_params_json,
            cfg.schemas_dir,
            cfg.shared_lib_dir);
        if (rc_cache_key != 0) {
            resolve_cache::ResolveCachePayload rc_payload;
            if (resolve_cache::load(engine->cache_root, cfg.world_name,
                                    rc_cache_key, rc_payload)) {
                // Cache header valid — try to restore provider state.
                std::string rc_err;
                if (provider->restore_from_cache(rc_payload.snapshot,
                                                 rc_payload.bake_plan,
                                                 rc_payload.root_hashes,
                                                 rc_err)) {
                    // Populate new_manifest from cached instances + lights.
                    viewer::WorldManifest cached_manifest;
                    cached_manifest.instances = std::move(rc_payload.instances);
                    cached_manifest.lights    = rc_payload.lights;

                    // Try to load probes from existing probe cache.
                    // Miss → treat as resolve-cache miss (full path).
                    if (provider->try_load_cached_probes(cached_manifest)) {
                        fprintf(stderr, "resolve cache: hit %016llx\n",
                                (unsigned long long)rc_cache_key);

                        // Proceed directly to publish_pipeline with cached data.
                        auto t_publish_start_rc = clk_t::now();
                        double total_ms_rc = std::chrono::duration<double, std::milli>(
                            clk_t::now() - t_bake_start).count();
                        fprintf(stderr,
                            "[bake-timing] install=0ms compose=0ms (resolve-cache-hit) "
                            "publish=...ms total(pre-publish)=%.0fms\n", total_ms_rc);

                        PublishPipelineParams pp_rc;
                        pp_rc.job_prefix            = "bake";
                        pp_rc.count_errors_seed     = 0;
                        pp_rc.init_culler           = true;
                        pp_rc.update_probe_dims     = true;
                        pp_rc.verbose_reset_log     = true;
                        pp_rc.fault_hook            = cfg.test_fault_hook;
                        pp_rc.load_msg_include_hash = true;
                        pp_rc.provider_ref          = provider;
                        publish_pipeline(token, std::move(cached_manifest), pp_rc);
                        double publish_ms_rc = std::chrono::duration<double, std::milli>(
                            clk_t::now() - t_publish_start_rc).count();
                        double total_ms2 = std::chrono::duration<double, std::milli>(
                            clk_t::now() - t_bake_start).count();
                        fprintf(stderr,
                            "[bake-timing] install=0ms compose=0ms publish=%.0fms "
                            "total=%.0fms (resolve-cache-hit)\n",
                            publish_ms_rc, total_ms2);
                        return;  // done — no full install+compose
                    } else {
                        fprintf(stderr,
                            "resolve cache: hit key but probe cache miss — full resolve\n");
                    }
                } else {
                    fprintf(stderr, "resolve cache: restore_from_cache failed (%s) — full resolve\n",
                            rc_err.c_str());
                }
            }
        }
    }

    // install_graph on the worker (script eval + per-part bake; no GL).
    // Phase C Task 14: RootsOnly — only root nodes are baked at install.
    // Children bake on demand in the publish loop (ensure_part_baked).
    // Task 7: skip-and-continue — install_graph() returns true even with partial
    // failures; emit one BakeError per failed part, then continue the pipeline
    // with the parts that succeeded. count_errors accumulates the total.
    int count_errors = 0;
    auto t_install_start = clk_t::now();
    {
        std::string err;
        if (!provider->install_graph(err, part_graph::BakePolicy::RootsOnly)) {
            printf("install: %s\n", err.c_str());
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : classify_error(err),
                       "install", err);
            return;
        }
        // Emit per-part errors for any parts that failed during install.
        for (const auto& fp : provider->install_result().failed) {
            BakeErrorCode code = classify_error(fp.error);
            Event ev;
            ev.type    = EventType::BakeError;
            ev.code    = code;
            ev.phase   = "install";
            ev.module  = fp.module;
            ev.message = fp.error;
            emit_event(std::move(ev));
            ++count_errors;
        }
    }
    double install_ms = std::chrono::duration<double, std::milli>(
        clk_t::now() - t_install_start).count();
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "install", "cancelled"); return; }

    // --- Phase C Task 9: world-kind divergence --------------------------------
    // If the manifest contained a `world` flag, skip compose_world and instead
    // eval the world definition, install the field + sector assets, and proceed
    // to publish_pipeline with an empty manifest. The SectorStreamer (built in
    // publish_pipeline's tail) will stream sectors on demand.
    if (!provider->world_module().empty()) {
        auto t_world_start = clk_t::now();
        std::string werr;
        world_initial_load_done = false;  // reset for this generation
        if (!install_world(token, werr)) {
            fprintf(stderr, "install_world: %s\n", werr.c_str());
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : classify_error(werr),
                       "install", werr);
            return;
        }
        double world_ms = std::chrono::duration<double, std::milli>(
            clk_t::now() - t_world_start).count();
        if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "install", "cancelled"); return; }

        // World-kind sessions use an empty manifest; sectors are streamed.
        viewer::WorldManifest empty_manifest;
        auto t_publish_start = clk_t::now();
        PublishPipelineParams pp;
        pp.job_prefix            = "bake";
        pp.count_errors_seed     = count_errors;
        pp.init_culler           = true;
        pp.update_probe_dims     = false;
        pp.verbose_reset_log     = true;
        pp.fault_hook            = cfg.test_fault_hook;
        pp.load_msg_include_hash = true;
        pp.provider_ref          = provider;
        publish_pipeline(token, std::move(empty_manifest), pp);
        // publish_pipeline replaces PartStore — re-apply transient scratch dir.
        if (store) store->set_scratch_dir(provider->transient_dir());
        double publish_ms = std::chrono::duration<double, std::milli>(
            clk_t::now() - t_publish_start).count();
        double total_ms = std::chrono::duration<double, std::milli>(
            clk_t::now() - t_bake_start).count();
        fprintf(stderr, "[bake-timing] install=%.0fms world=%.0fms publish=%.0fms total=%.0fms (world-kind)\n",
                install_ms, world_ms, publish_ms, total_ms);
        return;  // world-kind path complete — SectorStreamer built in publish_pipeline tail
    }

    // 3) compose_world on the worker (scatter/place; tileset GL marshaled) ----
    // NOTE: tileset phase runs inside compose_world and is not separable without
    // surgery into LocalProvider — compose_ms includes scatter + flatten + tileset.
    auto t_compose_start = clk_t::now();
    viewer::WorldManifest new_manifest;
    {
        std::string err;
        if (!provider->compose_world(new_manifest, err)) {
            printf("compose: %s\n", err.c_str());
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : classify_error(err),
                       "compose", err);
            return;
        }
    }
    double compose_ms = std::chrono::duration<double, std::milli>(
        clk_t::now() - t_compose_start).count();
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "compose", "cancelled"); return; }

    // Phase C Task 17: save resolve cache after a successful full install+compose.
    // Write to temp + rename (atomic). Non-fatal on failure (no cache next warm launch).
    if (!enable_live_edit && rc_cache_key != 0 && !is_cancelled()) {
        resolve_cache::ResolveCachePayload rc_save;
        rc_save.instances   = new_manifest.instances;
        rc_save.lights      = new_manifest.lights;
        rc_save.snapshot    = provider->graph_snapshot();
        rc_save.bake_plan   = provider->install_result().bake_plan;
        rc_save.root_hashes = provider->install_result().root_hashes;
        if (!resolve_cache::save(engine->cache_root, cfg.world_name,
                                 rc_cache_key, rc_save)) {
            fprintf(stderr, "resolve cache: save failed (non-fatal)\n");
        } else {
            fprintf(stderr, "resolve cache: saved key %016llx\n",
                    (unsigned long long)rc_cache_key);
        }
    }

    // 4-8) Shared publish flow: reset → reconcile → per-part publish → finalize
    //      → BakeFinished. count_errors is seeded with install-phase failures.
    // Phase C Task 14: pass provider_ref so publish jobs can bake on demand.
    auto t_publish_start = clk_t::now();
    PublishPipelineParams pp;
    pp.job_prefix            = "bake";
    pp.count_errors_seed     = count_errors;
    pp.init_culler           = true;
    pp.update_probe_dims     = true;
    pp.verbose_reset_log     = true;
    pp.fault_hook            = cfg.test_fault_hook;
    pp.load_msg_include_hash = true;
    pp.provider_ref          = provider;  // shared_ptr extends lifetime through publish
    publish_pipeline(token, std::move(new_manifest), pp);
    double publish_ms = std::chrono::duration<double, std::milli>(
        clk_t::now() - t_publish_start).count();
    double total_ms = std::chrono::duration<double, std::milli>(
        clk_t::now() - t_bake_start).count();
    fprintf(stderr, "[bake-timing] install=%.0fms compose=%.0fms publish=%.0fms total=%.0fms\n",
            install_ms, compose_ms, publish_ms, total_ms);
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::publish_pipeline
// Steps 4-8 of the bake/cone publish flow, shared by execute_bake and
// execute_rebake_cone. Caller has already emitted BakeStarted, run
// install_graph (accumulating errors into p.count_errors_seed), and produced
// new_manifest via compose_world. This function takes ownership of new_manifest.
//
// Genuine differences parametrized via PublishPipelineParams:
//   job_prefix          — "bake" vs "cone" in GpuJob names + assert markers
//   count_errors_seed   — install-phase errors (0 for cone)
//   init_culler         — first-success GpuCuller init (bake only)
//   update_probe_dims   — stats.probe_dims census in finalize (bake only)
//   verbose_reset_log   — raster/probe/sky diagnostic prints in reset (bake only)
//   fault_hook          — test injection hook in publish job (bake only)
//   load_msg_include_hash — append "(hash N)" to load-failure message (bake only)
// ---------------------------------------------------------------------------
void WorldSession::Impl::publish_pipeline(
    const std::shared_ptr<matter_async::CancelToken>& token,
    viewer::WorldManifest new_manifest,
    const PublishPipelineParams& p)
{
    auto is_cancelled = [&] { return token && token->is_cancelled(); };

    auto emit_error = [&](BakeErrorCode code, const char* phase, const std::string& msg) {
        Event ev;
        ev.type    = EventType::BakeError;
        ev.code    = code;
        ev.phase   = phase;
        ev.message = msg;
        emit_event(std::move(ev));
    };

    const std::string& pfx = p.job_prefix;

    // 4) GL reset job: recreate raster + composer + PartStore on the GL thread.
    struct ResetOutput {
        std::unique_ptr<viewer::PartStore>      new_store;
        std::unique_ptr<viewer::WorldComposer>  new_composer;
        std::unique_ptr<viewer::RasterComposer> new_raster;
        viewer::ProbeTextures                   new_probe_tex{};
    };
    auto reset_out = std::make_shared<ResetOutput>();

    matter_async::GpuJob reset_job;
    reset_job.name  = pfx + ".reset";
    reset_job.token = token;
    reset_job.fn = [this, reset_out, new_manifest, p, pfx](std::string& err) -> bool {
        matter_async::assert_gl_thread((pfx + ".reset").c_str());
        connected.store(false, std::memory_order_release);

        viewer::release_probe_textures(probe_tex);
        reset_out->new_raster = std::make_unique<viewer::RasterComposer>();
        auto& raster_local = reset_out->new_raster;
        if (!engine->gl46) {
            renderer.set_lights(new_manifest.lights);
        } else {
            std::string rerr;
            if (!raster_local->init(rerr)) {
                if (p.verbose_reset_log) printf("raster: %s\n", rerr.c_str());
                err = rerr;
                return false;
            }
            raster_local->set_lights(new_manifest.lights);
            if (new_manifest.probes && new_manifest.probes->valid()) {
                reset_out->new_probe_tex = viewer::upload_probe_textures(*new_manifest.probes);
                raster_local->set_probes(reset_out->new_probe_tex);
            } else if (p.verbose_reset_log) {
                printf("probes unavailable - flat ambient fallback\n");
            }
            auto tonemap = [](float c) -> unsigned char {
                float mapped  = c / (c + 1.0f);
                float gamma   = std::pow(mapped, 1.0f / 2.2f);
                float clamped = gamma < 0.0f ? 0.0f : (gamma > 1.0f ? 1.0f : gamma);
                return (unsigned char)(clamped * 255.0f + 0.5f);
            };
            unsigned char r = tonemap(new_manifest.lights.sky_color[0]);
            unsigned char g = tonemap(new_manifest.lights.sky_color[1]);
            unsigned char b = tonemap(new_manifest.lights.sky_color[2]);
            sky_clear[0] = r / 255.f;
            sky_clear[1] = g / 255.f;
            sky_clear[2] = b / 255.f;
            if (p.verbose_reset_log)
                printf("sky clear color: (%d,%d,%d)\n", (int)r, (int)g, (int)b);
            std::string gerr;
            if (!raster_local->init_gpu_driven(gerr)) {
                if (p.verbose_reset_log)
                    fprintf(stderr, "FATAL: GPU-driven shader init failed: %s. "
                            "Set MATTER_RT=1 to fall back to the ray-traced path.\n",
                            gerr.c_str());
                err = "GPU-driven shader init failed: " + gerr;
                return false;
            }
        }
        renderer.set_lights(new_manifest.lights);

        if (p.init_culler && !culler_ready && engine->gl46) {
            std::string cull_err;
            if (!gpu_culler.init(cull_err)) {
                fprintf(stderr, "FATAL: GpuCuller::init failed: %s\n", cull_err.c_str());
                err = "GpuCuller::init failed: " + cull_err;
                return false;
            }
            printf("GpuCuller: initialized\n");
            culler_ready = true;
        }

        reset_out->new_store = std::make_unique<viewer::PartStore>(cfg.cache_root);
        reset_out->new_composer = std::make_unique<viewer::WorldComposer>(
            *reset_out->new_store, /*tlas_capacity=*/16);

        state.reset(viewer::WorldManifest{});
        raster.swap(reset_out->new_raster);
        composer.swap(reset_out->new_composer);
        store.swap(reset_out->new_store);
        probe_tex = reset_out->new_probe_tex;
        reset_out->new_probe_tex = viewer::ProbeTextures{};

        manifest = new_manifest;
        connected.store(true, std::memory_order_release);
        tracer_dirty = true;
        tracer.reset();
        return true;
    };
    {
        std::string rerr;
        if (!gpu_jobs.run_blocking(std::move(reset_job), rerr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : BakeErrorCode::GpuError,
                       "gl", rerr.empty() ? pfx + " reset job failed" : rerr);
            return;
        }
    }
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "gl", "cancelled"); return; }

    // 5) Reconcile job.
    struct ReconcileOutput { std::vector<uint64_t> want; };
    auto reco_out = std::make_shared<ReconcileOutput>();
    matter_async::GpuJob reco_job;
    reco_job.name  = pfx + ".reconcile";
    reco_job.token = token;
    reco_job.fn    = [this, reco_out, pfx](std::string& /*err*/) -> bool {
        matter_async::assert_gl_thread((pfx + ".reconcile").c_str());
        reco_out->want = provider->reconcile(manifest, *store);
        return true;
    };
    {
        std::string rerr;
        if (!gpu_jobs.run_blocking(std::move(reco_job), rerr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : BakeErrorCode::GpuError,
                       "gl", rerr.empty() ? pfx + " reconcile failed" : rerr);
            return;
        }
    }
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "gl", "cancelled"); return; }

    // 6) Per-part publish jobs (fire-and-forget, FIFO).
    //    - Emit BakePartDone at POST time on the worker (deterministic order).
    //    - Check cancellation between posts (the between-parts checkpoint).
    //    - The job loads the part into store, applies the delta of entries
    //      matching this part_hash, invalidates the tracer, and grows the
    //      composer cap when needed.
    //
    // Publish order: `want` first (parts we know need loading + drive
    // BakePartDone progress), then any manifest hashes NOT in want (warm-cache
    // reload: parts already on disk that reconcile skipped — the fresh store
    // still needs to load them and state still needs their entries applied).
    // The trailing hashes get BakePartDone events too so the caller sees a
    // consistent (done, total) progression regardless of cache state.
    std::vector<uint64_t> publish_order = reco_out->want;
    {
        std::set<uint64_t> in_want(publish_order.begin(), publish_order.end());
        std::set<uint64_t> seen(publish_order.begin(), publish_order.end());
        for (const auto& e : manifest.instances) {
            if (in_want.count(e.part_hash)) continue;
            if (!seen.insert(e.part_hash).second) continue;
            publish_order.push_back(e.part_hash);
        }
    }

    // Phase C Task 3: distance-ordered publish.
    // Snapshot focus ONCE so the entire pass uses one consistent value
    // (deterministic; app thread may call set_bake_focus at any time).
    // Build hash → min squared distance to any of its manifest entries.
    // WorldManifestEntry.transform is a row-major float[16]: translation lives
    // at indices [3]=tx, [7]=ty, [11]=tz (matching set_translate and the DSL
    // placeChild/translate output; NOT at [12],[13],[14] which is the last row).
    // Parts with no manifest entry sort last (dist = +infinity).
    // Stable sort ascending by dist², tie-break by part hash (deterministic).
    //
    // Phase C Task 6 (carried-in follow-up a): snap_focus is hoisted outside the
    // block so the ref-streaming section of the publish loop can apply the same
    // comparator to newly-appended tail entries (stable sort of the appended segment
    // after each ref-streaming batch).
    float snap_focus[3];
    {
        std::lock_guard<std::mutex> lk(focus_mutex);
        snap_focus[0] = focus[0];
        snap_focus[1] = focus[1];
        snap_focus[2] = focus[2];
    }
    {
        // Build min dist² map: hash → smallest dist² across all manifest entries.
        std::unordered_map<uint64_t, float> min_dist2;
        for (const uint64_t h : publish_order)
            min_dist2[h] = std::numeric_limits<float>::infinity();
        for (const auto& e : manifest.instances) {
            auto it = min_dist2.find(e.part_hash);
            if (it == min_dist2.end()) continue;
            // Row-major 4×4: translation at [3]=tx, [7]=ty, [11]=tz.
            float dx = e.transform[3]  - snap_focus[0];
            float dy = e.transform[7]  - snap_focus[1];
            float dz = e.transform[11] - snap_focus[2];
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < it->second) it->second = d2;
        }
        std::stable_sort(publish_order.begin(), publish_order.end(),
            [&](uint64_t a, uint64_t b) {
                float da = min_dist2.count(a) ? min_dist2[a]
                                              : std::numeric_limits<float>::infinity();
                float db = min_dist2.count(b) ? min_dist2[b]
                                              : std::numeric_limits<float>::infinity();
                if (da != db) return da < db;
                return a < b;   // tie-break: ascending part hash (deterministic)
            });
    }

    // module_by_hash for the BakePartDone label — seeded from manifest.instances
    // entries that already carry a module name (graph-known roots set e.module).
    // Ref-streamed children appended during the publish loop have no module name
    // available without new provider plumbing, so their BakePartDone.module = "".
    std::unordered_map<uint64_t, std::string> mod_by_hash;
    for (const auto& e : manifest.instances)
        if (!e.module.empty()) mod_by_hash[e.part_hash] = e.module;

    // Shared cap-growth state across all publish jobs. Both fields are read and
    // written only on the GL thread (all publish jobs are FIFO on the pump), so
    // no mutex is needed. Initial cap matches the reset job's WorldComposer(16).
    // load_fail_count accumulates per-part load failures (GL thread only, FIFO).
    // Read by worker after run_blocking(finalize_job) guarantees all publish
    // jobs have completed.
    struct CapState { size_t needed = 0; size_t current = 16; int load_fail_count = 0; };
    auto cap_state = std::make_shared<CapState>();

    // Capture the fault hook once at post-time (by value) so each publish job
    // holds its own copy.
    auto fault_hook = p.fault_hook;
    bool include_hash = p.load_msg_include_hash;

    // Phase C Task 14: demand-driven bake tracking.
    // queued_hashes: dedup set for ref-streamed hashes appended to publish_order.
    // bake_fail_count: worker-side bake failures (separate from load_fail_count).
    std::set<uint64_t> queued_hashes(publish_order.begin(), publish_order.end());
    int bake_fail_count = 0;

    // next_manifest_id: for appending new WorldManifestEntry for streamed refs.
    // Start after the last existing id so new entries don't collide.
    uint32_t next_manifest_id = 1;
    for (const auto& e : manifest.instances)
        if (e.instance_id >= next_manifest_id) next_manifest_id = e.instance_id + 1;

    // Demand-bake provider: set when provider_ref is supplied (BakeAll/Reload).
    // Null for cone (artifacts pre-exist).
    auto demand_provider = p.provider_ref;

    // Dynamic loop: publish_order may grow as FlatInstanceRefs are discovered.
    // total grows too — BakePartDone.total may increase between events (documented
    // in events.h: HUD consumers must not assume constant total).
    for (size_t i = 0; i < publish_order.size(); ++i) {
        if (is_cancelled()) {
            emit_error(BakeErrorCode::Cancelled, "parts", "cancelled between parts");
            return;
        }
        uint64_t h = publish_order[i];

        // Phase C Task 14 step 2: ensure part is baked (worker thread, CPU-only).
        // For demand-bake path (demand_provider set): bake this part's subtree on
        // demand before the GPU upload. On failure: emit BakeError, skip GPU job.
        bool part_bake_failed = false;
        if (demand_provider) {
            std::string berr;
            if (!demand_provider->ensure_part_baked(h, berr)) {
                // "not in bake plan": only top-level unknown hashes trigger this;
                // initial publish_order comes from graph-known roots; tileset roots
                // are excluded from manifest.instances (local_provider.cpp:558-559)
                // so they never reach publish_order. Deeper bake failures produce
                // distinct wording and fall through to the BakeError branch below.
                if (berr.find("not in bake plan") == std::string::npos) {
                    std::string part_module_b;
                    { auto it = mod_by_hash.find(h); if (it != mod_by_hash.end()) part_module_b = it->second; }
                    Event bev;
                    bev.type    = EventType::BakeError;
                    bev.code    = classify_error(berr);
                    bev.phase   = "parts";
                    bev.module  = part_module_b;
                    bev.message = berr;
                    emit_event(std::move(bev));
                    ++bake_fail_count;
                }
                // Skip GPU job for this part (artifact missing)
                part_bake_failed = true;
            }
        }

        // Phase C Task 14 step 3: flatten (non-fatal, same as compose_world's flatten_one).
        if (!part_bake_failed && demand_provider) {
            demand_provider->ensure_part_flattened(h);
        }

        // Phase C Task 14 step 4: ref streaming.
        // Read the flat.part for this hash and append any FlatInstanceRefs to
        // manifest.instances + publish_order tail (dedup via queued_hashes).
        // Runs on the worker before posting the GPU job, so the new entries are
        // visible to the upcoming GPU job's `added` snapshot build.
        if (!part_bake_failed) {
            // Only attempt ref streaming when the flat is available (demand or eager).
            const std::string flat_abs = engine->cache_root + "/" +
                                         part_asset::cache_path_flat(h);
            if (part_asset::peek_format_version(flat_abs) ==
                    part_asset::kFormatVersionFlat) {
                BLASManager scratch_blas;
                TLASManager scratch_tlas(4);
                std::vector<part_asset::FlatCluster> clusters_ignored;
                std::vector<part_asset::FlatInstanceRef> refs;
                if (part_asset::load_flat_v3(flat_abs, h, scratch_blas,
                                              scratch_tlas, clusters_ignored, refs)
                    && !refs.empty()) {
                    size_t tail_start = publish_order.size();
                    for (const auto& r : refs) {
                        uint64_t rh = r.child_resolved_hash;
                        if (!queued_hashes.insert(rh).second) continue; // already queued
                        // Append a WorldManifestEntry for this ref child.
                        viewer::WorldManifestEntry we;
                        we.instance_id = next_manifest_id++;
                        we.part_hash   = rh;
                        std::memcpy(we.transform, r.transform, sizeof(we.transform));
                        // Worker-thread append: safe because all GpuJob publish
                        // lambdas consume a moved `added` snapshot and never read
                        // manifest.instances directly; run_blocking(finalize_job)
                        // provides the acquire barrier before finalize reads it.
                        // (Invariant: publish-job bodies must NOT read manifest.)
                        // Module name is not available for ref-streamed children
                        // without new provider plumbing; BakePartDone.module = "".
                        manifest.instances.push_back(we);
                        publish_order.push_back(rh);
                    }
                    // Phase C Task 6 (carried-in follow-up a): sort the newly-appended
                    // tail of publish_order by focus distance — same comparator as the
                    // initial sort applied to the full list before the loop.
                    // Only the new entries [tail_start, end) are unsorted; indices
                    // [0, tail_start) are already committed (GPU jobs posted); sorting
                    // the tail ensures ref-streamed children publish in focus order.
                    if (publish_order.size() > tail_start + 1) {
                        // Build min dist² for just the new tail entries from manifest.
                        std::unordered_map<uint64_t, float> tail_dist2;
                        for (size_t ti = tail_start; ti < publish_order.size(); ++ti)
                            tail_dist2[publish_order[ti]] = std::numeric_limits<float>::infinity();
                        for (const auto& e : manifest.instances) {
                            auto it = tail_dist2.find(e.part_hash);
                            if (it == tail_dist2.end()) continue;
                            float dx = e.transform[3]  - snap_focus[0];
                            float dy = e.transform[7]  - snap_focus[1];
                            float dz = e.transform[11] - snap_focus[2];
                            float d2 = dx*dx + dy*dy + dz*dz;
                            if (d2 < it->second) it->second = d2;
                        }
                        std::stable_sort(
                            publish_order.begin() + (ptrdiff_t)tail_start,
                            publish_order.end(),
                            [&](uint64_t a, uint64_t b) {
                                float da = tail_dist2.count(a)
                                    ? tail_dist2[a]
                                    : std::numeric_limits<float>::infinity();
                                float db = tail_dist2.count(b)
                                    ? tail_dist2[b]
                                    : std::numeric_limits<float>::infinity();
                                if (da != db) return da < db;
                                return a < b;  // tie-break: ascending hash (deterministic)
                            });
                    }
                }
            }
        }

        // Deterministic post-time emit (order must not change).
        // total = publish_order.size() at emit time — may grow between events
        // as FlatInstanceRefs are discovered (see events.h).
        {
            Event ev;
            ev.type   = EventType::BakePartDone;
            // mod_by_hash is seeded from graph-known roots only; ref-streamed
            // children publish with module="" (no provider API to look them up).
            auto it   = mod_by_hash.find(h);
            ev.module = (it != mod_by_hash.end()) ? it->second : "";
            ev.done   = (int)(i + 1);
            ev.total  = (int)publish_order.size();
            ev.phase  = "parts";
            emit_event(std::move(ev));
        }

        if (part_bake_failed) continue;  // skip GPU job for this part

        // Capture module name at post-time so the publish job lambda can label
        // BakeError events without accessing mod_by_hash on the GL thread.
        std::string part_module;
        {
            auto it = mod_by_hash.find(h);
            if (it != mod_by_hash.end()) part_module = it->second;
        }

        // Snapshot manifest entries whose part_hash == h.
        std::vector<viewer::WorldManifestEntry> added;
        for (const auto& e : manifest.instances)
            if (e.part_hash == h) added.push_back(e);
        const size_t entry_count = added.size();

        matter_async::GpuJob pj;
        pj.name  = pfx + ".publish";
        pj.token = token;
        pj.fn = [this, i, h, part_module, added_moved = std::move(added),
                 entry_count, cap_state, fault_hook, include_hash,
                 pfx](std::string& /*err*/) -> bool {
            matter_async::assert_gl_thread((pfx + ".publish").c_str());

            // Per-part skip-and-continue in the publish (load) phase.
            // Fire the test fault hook before get_or_load; catch exceptions to
            // inject deterministic faults. On any failure: emit a BakeError,
            // increment load_fail_count, and return true-with-skip (do NOT
            // publish the delta; do not abort the job pipeline).
            // emit_event is mutex-guarded so calling from the GL thread is safe.
            BakeErrorCode fail_code = BakeErrorCode::IoError;
            std::string   fail_msg;
            bool          part_failed = false;

            try {
                if (fault_hook) fault_hook((int)i);
                if (!store->get_or_load(h)) {
                    fail_code = BakeErrorCode::IoError;
                    fail_msg  = "load failed for part " + part_module;
                    if (include_hash)
                        fail_msg += " (hash " + std::to_string(h) + ")";
                    part_failed = true;
                }
            } catch (std::bad_alloc&) {
                fail_code  = BakeErrorCode::OutOfMemory;
                fail_msg   = "std::bad_alloc loading part " + part_module;
                part_failed = true;
            } catch (std::exception& ex) {
                fail_code  = BakeErrorCode::IoError;
                fail_msg   = ex.what();
                part_failed = true;
            }

            if (part_failed) {
                Event bev;
                bev.type    = EventType::BakeError;
                bev.code    = fail_code;
                bev.phase   = "parts";
                bev.module  = part_module;
                bev.message = fail_msg;
                emit_event(std::move(bev));
                ++cap_state->load_fail_count;
                return true;   // skip-and-continue: pipeline keeps running
            }

            viewer::WorldDelta d;
            d.added = added_moved;
            state.apply(d);
            tracer_dirty = true;
            tracer.reset();

            // Composer cap growth: count drawable nodes in this part's tree,
            // accumulate needed_cap, and recreate the composer when the cap is
            // exceeded. TLAS recomposes every frame so recreate is cheap; we add
            // headroom (max(needed, current*2)) to avoid recreating on every part.
            size_t drawable_nodes = 0;
            viewer::walk_part_tree(h,
                [this](uint64_t hh) -> const viewer::LoadedPart* { return store->get_or_load(hh); },
                [&](const viewer::LoadedPart* lp, uint64_t, const float[16], int) {
                    if (!lp->lod_blas.empty()) ++drawable_nodes;
                });
            cap_state->needed += entry_count * drawable_nodes;
            if (cap_state->needed > cap_state->current) {
                size_t new_cap = cap_state->needed > cap_state->current * 2
                                     ? cap_state->needed
                                     : cap_state->current * 2;
                composer = std::make_unique<viewer::WorldComposer>(*store, new_cap);
                cap_state->current = new_cap;
            }
            return true;
        };
        gpu_jobs.post(std::move(pj));
    }

    // 7) Finalize job (blocking): part_lod_table + census + exact-cap composer.
    matter_async::GpuJob finalize_job;
    finalize_job.name  = pfx + ".finalize";
    finalize_job.token = token;
    finalize_job.fn    = [this, p, pfx](std::string& /*err*/) -> bool {
        matter_async::assert_gl_thread((pfx + ".finalize").c_str());
        lods = store->part_lod_table();

        stats.instances_total = (uint32_t)manifest.instances.size();
        stats.parts_baked     = (uint32_t)provider->baked_count();
        stats.cache_hits      = (uint32_t)provider->hit_count();
        if (p.update_probe_dims) {
            if (manifest.probes && manifest.probes->valid()) {
                stats.probe_dims[0] = manifest.probes->grid.nx;
                stats.probe_dims[1] = manifest.probes->grid.ny;
                stats.probe_dims[2] = manifest.probes->grid.nz;
            } else {
                stats.probe_dims[0] = stats.probe_dims[1] = stats.probe_dims[2] = 0;
            }
        }

        // Final exact-cap composer recreate. Walk the (now fully loaded) part
        // tree of every manifest instance and count drawable nodes exactly, as
        // the old bake_once did. Everything is already in the store from the
        // publish jobs, so this is cheap.
        size_t cap = 16;
        for (const auto& e : manifest.instances) {
            viewer::walk_part_tree(e.part_hash,
                [this](uint64_t hh) -> const viewer::LoadedPart* { return store->get_or_load(hh); },
                [&](const viewer::LoadedPart* lp, uint64_t, const float[16], int) {
                    if (!lp->lod_blas.empty()) ++cap;
                });
        }
        composer = std::make_unique<viewer::WorldComposer>(*store, cap);
        return true;
    };
    {
        std::string ferr;
        if (!gpu_jobs.run_blocking(std::move(finalize_job), ferr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : BakeErrorCode::GpuError,
                       "gl", ferr.empty() ? pfx + " finalize failed" : ferr);
            return;
        }
    }

    // Merge load-phase (publish-job) failures and bake-phase failures into count_errors.
    // run_blocking(finalize_job) above guarantees all publish jobs completed
    // before we reach here, so load_fail_count is final and safe to read on
    // the worker thread (GL-thread writes are sequenced before the barrier).
    // bake_fail_count was accumulated on the worker thread (sequenced before finalize).
    int count_errors = p.count_errors_seed + bake_fail_count + cap_state->load_fail_count;

    // 8) BakeFinished.
    {
        Event ev;
        ev.type   = EventType::BakeFinished;
        ev.errors = count_errors;
        emit_event(std::move(ev));
    }

    // 9) Deferred tileset phase (Task 15): runs after BakeFinished so silhouette
    //    is never blocked by the ~350s box3d settle wall.
    //    BakePartDone{phase="tileset"} events may follow BakeFinished — documented
    //    in events.h.
    //    NON-FATAL here: the silhouette already published; a tileset failure must
    //    not kill a session whose geometry is already rendering. (Fatal on the
    //    connect() sync path — LocalProvider::connect — where callers need the full world.)
    if (p.provider_ref) {
        auto tileset_on_part = [this, &pfx](int done, int total, const char* module) {
            Event ev;
            ev.type   = EventType::BakePartDone;
            ev.phase  = "tileset";
            ev.module = module ? module : "";
            ev.done   = done;
            ev.total  = total;
            emit_event(std::move(ev));
        };
        std::string terr;
        if (!p.provider_ref->run_tileset_deferred(tileset_on_part, is_cancelled, terr)) {
            if (!is_cancelled()) {
                // Non-fatal: emit BakeError but the world is already rendering.
                Event ev;
                ev.type    = EventType::BakeError;
                ev.code    = classify_error(terr);
                ev.phase   = "tileset";
                ev.message = terr;
                emit_event(std::move(ev));
            }
        }
    }

    // 10) Phase C Task 6: build RefineController from graph snapshot + world instances.
    //     Only for BakeAll/Reload (provider_ref set); cone rebakes don't update the
    //     terrain tile set.  Runs after the tileset tail so the world is fully settled.
    //     Reset then rebuild so supersession starts fresh.
    if (p.provider_ref && !is_cancelled()) {
        refine_provider = p.provider_ref;   // extend provider lifetime into refine phase

        // Build GraphNodes for RefineController from the retained bake_plan, which
        // covers ALL parametric variants (one entry per (module, params) pair keyed
        // by resolved_hash). The graph_snapshot_.nodes map has only one entry per
        // module name (the first-seen variant per the "module-identity contract" in
        // part_graph.cpp), so it cannot distinguish Terrain(tx=0,tz=0) from
        // Terrain(tx=1,tz=0).  The bake_plan has BakeInputs{module, params} for each
        // distinct resolved_hash — exactly what RefineController needs.
        const auto& bake_plan = p.provider_ref->install_result().bake_plan;
        std::vector<matter_refine::GraphNode> gnodes;
        gnodes.reserve(bake_plan.size());
        for (const auto& kv : bake_plan) {
            matter_refine::GraphNode gn;
            gn.module        = kv.second.module;
            gn.params_json   = part_graph::params_to_json(kv.second.params);
            gn.resolved_hash = kv.first;   // key IS the resolved_hash
            gnodes.push_back(std::move(gn));
        }

        // Build instance refs from manifest.instances (row-major: tx=[3], ty=[7], tz=[11]).
        std::vector<matter_refine::InstanceRef> irefs;
        irefs.reserve(manifest.instances.size());
        for (uint32_t i = 0; i < (uint32_t)manifest.instances.size(); ++i) {
            const auto& e = manifest.instances[i];
            matter_refine::InstanceRef ir;
            ir.hash             = e.part_hash;
            ir.translation[0]   = e.transform[3];
            ir.translation[1]   = e.transform[7];
            ir.translation[2]   = e.transform[11];
            ir.manifest_idx     = i;
            irefs.push_back(ir);
        }

        auto new_ctrl = std::make_unique<matter_refine::RefineController>();
        new_ctrl->build(
            matter_refine::span<const matter_refine::GraphNode>(gnodes.data(), gnodes.size()),
            matter_refine::span<const matter_refine::InstanceRef>(irefs.data(), irefs.size()));

        refine_tile_count = new_ctrl->tile_count();
        refine_ctrl = std::move(new_ctrl);
        printf("[refine] controller built: %zu tiles\n", refine_tile_count);
    }

    // --- Phase C Task 9: world-kind session — SectorStreamer ------------------
    // If install_graph set world_field (world-kind), build the streamer instead
    // of (in addition to — world manifests have no asset-placed instances) refine.
    // closed-world sessions: world_field == null → skip.
    if (p.provider_ref && !is_cancelled() && world_field) {
        refine_ctrl.reset();   // world sessions don't use refine

        matter_stream::Config scfg;
        // Default ring table: 48/120/300/800; env override for tests.
        const char* rings_env = std::getenv("MATTER_STREAM_RINGS");
        if (rings_env) {
            // Simple format: "r0:rung0,r1:rung1,..." e.g. "32:3,80:2,160:1,400:0"
            scfg.rings.clear();
            const char* p2 = rings_env;
            while (*p2) {
                float r = std::atof(p2);
                while (*p2 && *p2 != ':') ++p2;
                if (*p2 == ':') ++p2;
                int rg = std::atoi(p2);
                while (*p2 && *p2 != ',') ++p2;
                if (*p2 == ',') ++p2;
                if (r > 0) scfg.rings.push_back({r, rg});
            }
        }
        sector_streamer = std::make_unique<matter_stream::SectorStreamer>(scfg);
        refine_provider = p.provider_ref;  // extend provider lifetime
        printf("[stream] SectorStreamer built for world-kind session\n");
    }
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::execute_refine_step
// Phase C Task 6: one camera-driven refine step. Called by worker_loop in the
// pop_wait timeout slot when refine_ctrl is live and no command is pending.
//
// Per step:
//   1. Evict full tiles beyond refine_radius (farthest-first): post GpuJob to
//      swap instance full→coarse + release_part(full) + store.release(full),
//      then mark(Coarse) and emit RefineTileDone with decremented done.
//   2. Take the highest-priority Coarse tile nearest to the focus.
//   3. ensure_part_baked(full_hash) + ensure_part_flattened(full_hash) on the worker.
//      On error: log, mark(Coarse) (skip this tile), continue.
//   4. Post a GpuJob (fire-and-forget) that loads the full variant and swaps
//      manifest.instances[manifest_idx].part_hash coarse→full, then applies a
//      WorldDelta so state reflects the new hash.
//      GL-thread invariant: assert_gl_thread inside the job.
//   5. mark(Full) + emit RefineTileDone{module="Terrain", done=full_count,
//      total=tile_count, phase="refine"}.
//
// Focus is projected to y=0 before passing to RefineController (carried-in
// follow-up b from Task 4 review): tile centers sit at y=0; camera height H
// would otherwise shrink the effective XZ eviction radius to sqrt(r²−H²).
// Projecting to y=0 makes refine_radius a clean XZ ground-plane distance,
// which matches what the user intends when tuning the radius in world units.
// ---------------------------------------------------------------------------
void WorldSession::Impl::execute_refine_step() {
    if (!refine_ctrl || !refine_provider) return;

    const size_t tile_count = refine_tile_count;

    // 0) Drain pending upgrade results written by GL jobs in prior steps.
    //    Worker owns all mark() calls; GL jobs write to an atomic slot and return —
    //    the worker applies the outcome here so state transitions are sequenced
    //    entirely on the worker thread with no data race.
    //    Handoff: GL job writes result atomic (1=success, 2=failure); worker reads
    //    it here, calls mark(Full) on success or mark(Coarse)+log on failure, then
    //    emits the deferred RefineTileDone (upgrade events are emitted at drain time,
    //    one step after the job was posted).
    {
        auto it = refine_pending_upgrades_.begin();
        while (it != refine_pending_upgrades_.end()) {
            int r = it->result->load(std::memory_order_acquire);
            if (r == 0) {
                ++it; // still in-flight — check again next step
                continue;
            }
            uint32_t ti    = it->tile_idx;
            int      ev_tx = it->tile_tx;
            int      ev_tz = it->tile_tz;
            if (r == 1) {
                // Swap succeeded — promote tile to Full and report progress.
                refine_ctrl->mark(ti, matter_refine::TileRecord::State::Full);
                Event ev;
                ev.type    = EventType::RefineTileDone;
                ev.module  = "Terrain";
                ev.done    = (int)refine_ctrl->full_count();
                ev.total   = (int)tile_count;
                ev.phase   = "refine";
                ev.tile_tx = ev_tx;
                ev.tile_tz = ev_tz;
                emit_event(std::move(ev));
            } else {
                // Swap failed (get_or_load returned null) — leave Coarse for retry.
                fprintf(stderr, "[refine] upgrade GL job failed for tile (%d,%d) — "
                        "will retry on next refine step\n", ev_tx, ev_tz);
                refine_ctrl->mark(ti, matter_refine::TileRecord::State::Coarse);
            }
            it = refine_pending_upgrades_.erase(it);
        }
    }

    // Snapshot focus; project to y=0 (design note: see above).
    float focus_yz0[3];
    {
        std::lock_guard<std::mutex> lk(focus_mutex);
        focus_yz0[0] = focus[0];
        focus_yz0[1] = 0.0f;  // project to ground plane — tiles sit at y=0
        focus_yz0[2] = focus[2];
    }

    // 1) Evict full tiles beyond radius (farthest-first).
    {
        std::vector<uint32_t> to_evict = refine_ctrl->evict_beyond(focus_yz0, refine_radius);
        for (uint32_t ti : to_evict) {
            const matter_refine::TileRecord& rec = refine_ctrl->tile_at(ti);
            // Capture fields for the GpuJob lambda.
            uint64_t full_hash_e   = rec.full_hash;
            uint64_t coarse_hash_e = rec.coarse_hash;
            uint32_t midx_e        = rec.manifest_idx;
            int      ev_tx_e       = rec.tile_tx;
            int      ev_tz_e       = rec.tile_tz;

            // I3 fix: skip eviction job for tiles with no real full variant
            // (null-hash tiles were marked Full as "skip forever"; releasing hash 0
            // is a no-op today but would regress if release-callers assume live hashes).
            if (full_hash_e == 0) {
                refine_ctrl->mark(ti, matter_refine::TileRecord::State::Coarse);
                continue;
            }

            // Mark Coarse first (worker thread); GpuJob runs later on GL thread.
            refine_ctrl->mark(ti, matter_refine::TileRecord::State::Coarse);

            // full_count after eviction.
            size_t done_after = refine_ctrl->full_count();

            // Post eviction job: swap instance full→coarse + GPU release.
            matter_async::GpuJob ej;
            ej.name = "refine.evict";
            ej.fn   = [this, full_hash_e, coarse_hash_e, midx_e](std::string& /*err*/) -> bool {
                matter_async::assert_gl_thread("refine.evict");
                // Swap instance hash back to coarse in the manifest.
                if (midx_e < (uint32_t)manifest.instances.size()) {
                    manifest.instances[midx_e].part_hash = coarse_hash_e;
                    // Apply delta so WorldState reflects the swap.
                    viewer::WorldDelta d;
                    d.added.push_back(manifest.instances[midx_e]);
                    state.apply(d);
                    tracer_dirty = true;
                    tracer.reset();
                }
                // Release full-res GPU and CPU resources.
                if (culler_ready) gpu_culler.release_part(full_hash_e);
                if (store)        store->release(full_hash_e);
                return true;
            };
            gpu_jobs.post(std::move(ej));

            // Emit RefineTileDone with the new (decremented) done count.
            Event ev;
            ev.type    = EventType::RefineTileDone;
            ev.module  = "Terrain";
            ev.done    = (int)done_after;
            ev.total   = (int)tile_count;
            ev.phase   = "refine";
            ev.tile_tx = ev_tx_e;
            ev.tile_tz = ev_tz_e;
            emit_event(std::move(ev));
        }
    }

    // 2) Pick the nearest Coarse tile to focus.
    matter_refine::TileRecord* tr = nullptr;
    if (!refine_ctrl->next(focus_yz0, &tr)) return;  // all tiles Full or Queued

    uint32_t ti_next       = refine_ctrl->tile_index_of(tr);
    uint64_t full_hash_n   = tr->full_hash;
    uint64_t coarse_hash_n = tr->coarse_hash;
    uint32_t midx_n        = tr->manifest_idx;
    int      next_tx       = tr->tile_tx;
    int      next_tz       = tr->tile_tz;

    if (full_hash_n == 0) {
        // No full variant — treat as permanently done (never emits RefineTileDone).
        refine_ctrl->mark(ti_next, matter_refine::TileRecord::State::Full);
        return;
    }

    // Mark Queued so worker_loop doesn't re-pick it on the next timeout.
    refine_ctrl->mark(ti_next, matter_refine::TileRecord::State::Queued);

    // 3) Bake the full variant on the worker thread.
    {
        std::string berr;
        if (!refine_provider->ensure_part_baked(full_hash_n, berr)) {
            // Bake failure: log and mark Coarse so the tile stays eligible for retry.
            fprintf(stderr, "[refine] ensure_part_baked failed for full hash %016llx: %s\n",
                    (unsigned long long)full_hash_n, berr.c_str());
            refine_ctrl->mark(ti_next, matter_refine::TileRecord::State::Coarse);
            return;
        }
        if (!refine_provider->ensure_part_flattened(full_hash_n)) {
            // Flatten failure: log and mark Coarse so the tile stays eligible for retry.
            fprintf(stderr, "[refine] ensure_part_flattened failed for full hash %016llx\n",
                    (unsigned long long)full_hash_n);
            refine_ctrl->mark(ti_next, matter_refine::TileRecord::State::Coarse);
            return;
        }
    }

    // 4) Post GPU job: load full variant + swap instance hash + apply delta.
    //    I1 fix: the GL job writes its outcome to a shared atomic so the worker can
    //    call mark(Full/Coarse) at the START of the next step (drain in step 0 above).
    //    Worker owns ALL mark() calls; GL job only writes the result atomic.
    (void)coarse_hash_n;  // coarse_hash for eviction; not needed for upgrade swap

    auto result_slot = std::make_shared<std::atomic<int>>(0); // 0=in-flight
    refine_pending_upgrades_.push_back({ti_next, next_tx, next_tz, result_slot});

    matter_async::GpuJob uj;
    uj.name = "refine.upgrade";
    uj.fn   = [this, full_hash_n, midx_n, result_slot](std::string& /*err*/) -> bool {
        matter_async::assert_gl_thread("refine.upgrade");
        // Load the full part into the store.
        if (!store->get_or_load(full_hash_n)) {
            // Load failure — manifest unchanged; tile will be retried (marked Coarse
            // by the worker when it drains this result at the next step).
            result_slot->store(2, std::memory_order_release); // 2 = failure
            return true;  // skip-and-continue; worker handles the state transition
        }
        // Ensure part registered in culler.
        if (culler_ready) gpu_culler.ensure_part(full_hash_n, *store);

        // Swap manifest entry's part_hash to the full variant.
        if (midx_n < (uint32_t)manifest.instances.size()) {
            manifest.instances[midx_n].part_hash = full_hash_n;
            viewer::WorldDelta d;
            d.added.push_back(manifest.instances[midx_n]);
            state.apply(d);
            tracer_dirty = true;
            tracer.reset();
        }
        result_slot->store(1, std::memory_order_release); // 1 = success
        return true;
    };
    gpu_jobs.post(std::move(uj));
    // mark(Full) and RefineTileDone are deferred to the drain in step 0 of the next
    // execute_refine_step call, after the GL job has written result_slot.
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::install_world
// Phase C Task 9: called from execute_bake after install_graph when
// provider->world_module() is non-empty. Evaluates the world definition,
// parses the field program, sets the world binding on the host_baker, marks
// WorldSector as transient, and pre-bakes the sector child asset set.
// Returns false + err on failure (caller emits BakeError).
// ---------------------------------------------------------------------------
bool WorldSession::Impl::install_world(
    const std::shared_ptr<matter_async::CancelToken>& token,
    std::string& err)
{
    const std::string& wmod = provider->world_module();
    if (wmod.empty()) { err = "install_world: no world module"; return false; }

    // 1. Read world source: <schemas_dir>/<world_module>.js
    std::string world_source;
    {
        const std::string world_js = cfg.schemas_dir + "/" + wmod + ".js";
        std::ifstream in(world_js, std::ios::binary);
        if (!in) { err = "install_world: cannot read " + world_js; return false; }
        std::ostringstream ss; ss << in.rdbuf();
        world_source = ss.str();
    }

    // 2. eval_world with the root_params_json override.
    script_host::ScriptHost host;
    host.set_shared_lib_root(cfg.shared_lib_dir);
    std::string params_json = cfg.root_params_json.empty() ? "{}" : cfg.root_params_json;
    script_host::WorldEvalResult r = host.eval_world(world_source, params_json);
    if (!r.ok) { err = "install_world: eval_world failed: " + r.message; return false; }

    // 3. Parse field program -> FieldRuntime.
    terrain_field::FieldProgram prog;
    std::string perr;
    if (!terrain_field::FieldProgram::parse(r.field_program, prog, perr)) {
        err = "install_world: FieldProgram::parse failed: " + perr;
        return false;
    }
    world_field = std::make_unique<terrain_field::FieldRuntime>(std::move(prog));

    // 4. Store world constants.
    world_sea_level    = world_field->sea_level();
    world_biomes_json  = r.biomes_json;
    world_sector_size  = r.sector_size;
    {
        char hbuf[32];
        std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                      (unsigned long long)world_field->hash());
        world_field_hash = hbuf;
    }
    // Extract worldSeed from merged params. eval_world applies class defaults,
    // so the seed is embedded in the field program's hash. The caller may also
    // have set it via regenerate(). Check root_params_json first.
    world_seed = 0;
    {
        // Simple extraction: look for "worldSeed" key in the params_json.
        // params_from_json returns a Params map; if it has worldSeed, use it.
        auto ps = part_graph::params_from_json(params_json);
        auto it = ps.find("worldSeed");
        if (it != ps.end() && it->second.kind == part_graph::ParamValue::Kind::Number) {
            world_seed = (uint64_t)it->second.num;
        }
    }

    // 5. Set world binding on the provider's host_baker so sector bakes get
    //    the field (terrainVolume / heightAt / biomeAt etc.).
    dsl::WorldBinding wb;
    wb.field       = world_field.get();
    wb.sector_size = r.sector_size;
    wb.y_min       = r.y_min;
    wb.y_max       = r.y_max;
    provider->host_baker().set_world(wb);

    // 6. Mark WorldSector as transient so its artifacts go to scratch.
    provider->set_transient_modules({"WorldSector"});
    if (store) store->set_scratch_dir(provider->transient_dir());

    // 7. Read sector source: <schemas_dir>/WorldSector.js
    {
        const std::string sector_js = cfg.schemas_dir + "/WorldSector.js";
        std::ifstream in(sector_js, std::ios::binary);
        if (!in) { err = "install_world: cannot read " + sector_js; return false; }
        std::ostringstream ss; ss << in.rdbuf();
        world_sector_source = ss.str();
    }

    // 8. Asset install: eval_requires on WorldSector to discover its child variants,
    //    then resolve_hash + ensure_part_baked + ensure_part_flattened for each child.
    std::vector<script_host::RequiredChild> children =
        host.eval_requires(world_sector_source, "{}");
    fprintf(stderr, "[stream] WorldSector requires %zu child variants\n", children.size());

    sector_child_hashes.clear();
    sector_child_modules.clear();
    sector_child_params.clear();
    sector_child_hashes.reserve(children.size());
    sector_child_modules.reserve(children.size());
    sector_child_params.reserve(children.size());

    // Resolve + bake each child asset variant.
    for (const auto& child : children) {
        if (token && token->is_cancelled()) {
            err = "install_world: cancelled during asset install";
            return false;
        }
        // Read child source
        std::string child_source;
        {
            const std::string child_js = cfg.schemas_dir + "/" + child.module_specifier + ".js";
            std::ifstream in(child_js, std::ios::binary);
            if (!in) {
                fprintf(stderr, "install_world: cannot read child %s (skipping)\n", child_js.c_str());
                Event ev;
                ev.type = EventType::BakeError;
                ev.code = BakeErrorCode::ScriptError;
                ev.phase = "install";
                ev.message = "cannot read child " + child_js;
                emit_event(std::move(ev));
                continue;
            }
            std::ostringstream ss; ss << in.rdbuf();
            child_source = ss.str();
        }
        // Resolve hash for this child variant (no children of its own for leaf schemas).
        uint64_t child_hash = host.resolve_hash(child_source, child.params_json);

        // Bake via the provider's demand path (checks cache first).
        // We need this child in the provider's bake_plan. Since it might not be
        // there (world-kind manifests have no graph roots for asset schemas), we
        // use the host_baker directly.
        if (!provider->host_baker().cached(child_hash)) {
            provider->host_baker().set_baking_module(child.module_specifier);
            if (!provider->host_baker().bake(child_source,
                    part_graph::params_from_json(child.params_json),
                    {}, {}, {}, child_hash)) {
                fprintf(stderr, "install_world: bake failed for %s (skipping)\n",
                        child.module_specifier.c_str());
                Event ev;
                ev.type = EventType::BakeError;
                ev.code = BakeErrorCode::ScriptError;
                ev.phase = "install";
                ev.message = "bake failed for " + child.module_specifier +
                             " params=" + child.params_json;
                emit_event(std::move(ev));
                continue;
            }
            provider->host_baker().bake_lod_variants(
                child_source, part_graph::params_from_json(child.params_json),
                {}, child_hash);
        }

        sector_child_hashes.push_back(child_hash);
        sector_child_modules.push_back(child.module_specifier);
        sector_child_params.push_back(child.params_json);
    }

    fprintf(stderr, "[stream] asset install complete: %zu child hashes, "
            "field_hash=%s, sector_size=%.1f, sea_level=%.2f\n",
            sector_child_hashes.size(), world_field_hash.c_str(),
            world_sector_size, world_sea_level);
    return true;
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::drain_sector_evictions
// Phase C Task 9: process pending evictions from the sector streamer.
// Removes instances from world state, releases GPU and PartStore resources,
// releases transient artifacts. Posts a blocking GL job to ensure all state
// mutations happen on the GL thread. Called on the worker thread.
// ---------------------------------------------------------------------------
void WorldSession::Impl::drain_sector_evictions() {
    if (!sector_streamer) return;
    auto evs = sector_streamer->take_evictions();
    if (evs.empty()) return;

    auto evs_shared = std::make_shared<std::vector<matter_stream::Eviction>>(std::move(evs));
    matter_async::GpuJob ej;
    ej.name = "stream.drain_evict";
    ej.fn   = [this, evs_shared](std::string& /*err*/) -> bool {
        matter_async::assert_gl_thread("stream.drain_evict");
        for (const auto& ev : *evs_shared) {
            SectorKey sk{ev.tx, ev.tz, ev.rung};
            std::lock_guard<std::mutex> lk(sector_map_mutex);
            auto it = sector_map.find(sk);
            if (it == sector_map.end()) continue;

            uint32_t inst_id   = it->second.instance_id;
            uint64_t part_hash = it->second.part_hash;

            viewer::WorldDelta d;
            d.removed.push_back(inst_id);
            state.apply(d);
            tracer_dirty = true;
            tracer.reset();

            if (culler_ready) gpu_culler.release_part(part_hash);
            if (store) store->release(part_hash);
            if (provider) provider->release_transient(part_hash);

            sector_map.erase(it);
        }
        return true;
    };
    std::string eerr;
    gpu_jobs.run_blocking(std::move(ej), eerr);
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::execute_sector_stream_step
// Phase C Task 9: one sector-streaming step. Called by worker_loop in the
// pop_wait timeout slot when sector_streamer is live and no command is pending.
//
// Per step:
//   1. Update camera position on the streamer.
//   2. Drain evictions (sectors camera left behind).
//   3. Service bake requests (holes and upgrades, nearest first).
//   4. On bake success → GL publish job.
//   5. After first cycle with no remaining holes → emit BakeFinished.
// ---------------------------------------------------------------------------
void WorldSession::Impl::execute_sector_stream_step() {
    if (!sector_streamer) return;
    if (!provider) return;

    // 1. Update camera position.
    float fx, fz;
    {
        std::lock_guard<std::mutex> lk(focus_mutex);
        fx = focus[0];
        fz = focus[2];
    }
    sector_streamer->update(fx, fz);

    // 2. Drain evictions on a blocking GL job so world state is updated on the
    //    GL thread (matches the pattern used by refine evictions).
    {
        auto evs = sector_streamer->take_evictions();
        if (!evs.empty()) {
            // Package eviction work into a blocking GL job.
            auto evs_shared = std::make_shared<std::vector<matter_stream::Eviction>>(std::move(evs));
            matter_async::GpuJob ej;
            ej.name = "stream.evict";
            ej.fn   = [this, evs_shared](std::string& /*err*/) -> bool {
                matter_async::assert_gl_thread("stream.evict");
                for (const auto& ev : *evs_shared) {
                    SectorKey sk{ev.tx, ev.tz, ev.rung};
                    std::lock_guard<std::mutex> lk(sector_map_mutex);
                    auto it = sector_map.find(sk);
                    if (it == sector_map.end()) continue;

                    uint32_t inst_id   = it->second.instance_id;
                    uint64_t part_hash = it->second.part_hash;

                    viewer::WorldDelta d;
                    d.removed.push_back(inst_id);
                    state.apply(d);
                    tracer_dirty = true;
                    tracer.reset();

                    if (culler_ready) gpu_culler.release_part(part_hash);
                    if (store) store->release(part_hash);
                    if (provider) provider->release_transient(part_hash);

                    sector_map.erase(it);
                }
                return true;
            };
            std::string eerr;
            gpu_jobs.run_blocking(std::move(ej), eerr);
        }
    }

    // 3. Service bake requests.
    matter_stream::SectorRequest req;
    while (sector_streamer->next_request(req)) {
        // Build params JSON for WorldSector bake.
        // biomes_json needs to be escaped for embedding in a JSON string value.
        std::string biomes_escaped;
        biomes_escaped.reserve(world_biomes_json.size() + 16);
        for (char c : world_biomes_json) {
            if (c == '"') biomes_escaped += "\\\"";
            else if (c == '\\') biomes_escaped += "\\\\";
            else biomes_escaped += c;
        }

        char params_buf[1024];
        std::snprintf(params_buf, sizeof(params_buf),
            R"({"tx":%lld,"tz":%lld,"rung":%d,"worldSeed":%llu,"fieldHash":"%s","biomes":"%s"})",
            (long long)req.tx, (long long)req.tz, req.rung,
            (unsigned long long)world_seed, world_field_hash.c_str(),
            biomes_escaped.c_str());
        std::string sector_params(params_buf);

        // Bake this sector.
        script_host::BakeOptions opts;
        opts.parts_dir = provider->transient_dir();
        opts.world.field       = world_field.get();
        opts.world.sector_size = world_sector_size;
        opts.world.y_min       = -64.0f;
        opts.world.y_max       = 192.0f;

        script_host::ScriptHost bake_host;
        bake_host.set_shared_lib_root(cfg.shared_lib_dir);
        auto br = bake_host.bake_source(
            world_sector_source, sector_params, opts,
            sector_child_hashes.data(), sector_child_hashes.size(),
            sector_child_modules.data(), sector_child_params.data());

        if (!br.error.ok) {
            fprintf(stderr, "[stream] sector bake failed (%lld,%lld r%d): %s\n",
                    (long long)req.tx, (long long)req.tz, req.rung,
                    br.error.message.c_str());
            sector_streamer->on_failed(req.tx, req.tz, req.rung);

            Event ev;
            ev.type    = EventType::BakeError;
            ev.code    = BakeErrorCode::ScriptError;
            ev.phase   = "stream";
            ev.message = br.error.message;
            emit_event(std::move(ev));
            continue;
        }

        uint64_t sector_hash = br.resolved_hash;

        // 4. GL publish job: add this sector to the world.
        int64_t pub_tx = req.tx, pub_tz = req.tz;
        int pub_rung = req.rung;
        float S = world_sector_size;

        matter_async::GpuJob pj;
        pj.name = "stream.publish";
        pj.fn = [this, pub_tx, pub_tz, pub_rung, sector_hash, S](std::string& /*err*/) -> bool {
            matter_async::assert_gl_thread("stream.publish");

            if (!sector_streamer->on_published(pub_tx, pub_tz, pub_rung)) {
                // No longer desired — discard.
                if (provider) provider->release_transient(sector_hash);
                return true;
            }

            // Load the part.
            if (store && !store->get_or_load(sector_hash)) {
                fprintf(stderr, "[stream] get_or_load failed for sector (%lld,%lld r%d)\n",
                        (long long)pub_tx, (long long)pub_tz, pub_rung);
                return true;
            }

            // Assign instance id.
            uint32_t inst_id;
            {
                std::lock_guard<std::mutex> lk(sector_map_mutex);
                inst_id = sector_next_id++;
            }

            // Build transform: ROW-major, translate at [3],[7],[11].
            viewer::WorldManifestEntry we;
            we.instance_id = inst_id;
            we.part_hash   = sector_hash;
            we.module      = "WorldSector";
            // Identity + translation.
            std::memset(we.transform, 0, sizeof(we.transform));
            we.transform[0]  = 1.0f;
            we.transform[5]  = 1.0f;
            we.transform[10] = 1.0f;
            we.transform[15] = 1.0f;
            we.transform[3]  = (float)pub_tx * S;  // tx
            we.transform[7]  = 0.0f;                // ty
            we.transform[11] = (float)pub_tz * S;  // tz

            viewer::WorldDelta d;
            d.added.push_back(we);
            state.apply(d);
            tracer_dirty = true;
            tracer.reset();

            // Ensure GPU culler knows about this part.
            if (culler_ready && store)
                gpu_culler.ensure_part(sector_hash, *store);

            // Record in sector_map.
            {
                std::lock_guard<std::mutex> lk(sector_map_mutex);
                SectorKey sk{pub_tx, pub_tz, pub_rung};
                sector_map[sk] = SectorEntry{inst_id, sector_hash};
            }

            // Grow composer cap if needed.
            if (composer && store) {
                size_t cap_needed = state.entries().size() + 16;
                composer = std::make_unique<viewer::WorldComposer>(*store, cap_needed);
            }

            return true;
        };
        gpu_jobs.post(std::move(pj));
    }

    // 5. After the first update+drain cycle with no remaining holes, emit BakeFinished.
    if (!world_initial_load_done && sector_streamer->resident_count() > 0 &&
        sector_streamer->inflight_count() == 0) {
        world_initial_load_done = true;
        Event ev;
        ev.type   = EventType::BakeFinished;
        ev.errors = 0;
        emit_event(std::move(ev));
    }
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::execute_rebake_cone
// Worker-side RebakeCone executor. Builds the upward cone from the provider's
// current snapshot + a fresh ScriptHost, runs LiveEditSession::rebuild(paths),
// then (on success) compose_world + the same publish flow as BakeAll (steps 4-8).
// Fail-closed: on rebuild failure emit errors and DO NOTHING — old world keeps
// rendering (last-good artifacts intact).
// ---------------------------------------------------------------------------
void WorldSession::Impl::execute_rebake_cone(matter_async::Command& cmd) {
    auto& token = cmd.token;
    auto is_cancelled = [&] { return token && token->is_cancelled(); };

    const std::set<std::string> paths(cmd.changed_files.begin(),
                                      cmd.changed_files.end());
    if (paths.empty()) return;

    if (is_cancelled()) return;

    // Emit-error helper.
    auto emit_error = [&](BakeErrorCode code, const char* phase, const std::string& msg) {
        Event ev;
        ev.type    = EventType::BakeError;
        ev.code    = code;
        ev.phase   = phase;
        ev.message = msg;
        emit_event(std::move(ev));
    };

    // 0) Announce start.
    {
        Event ev;
        ev.type = EventType::BakeStarted;
        emit_event(std::move(ev));
    }

    if (is_cancelled()) {
        emit_error(BakeErrorCode::Cancelled, "cone", "cancelled");
        return;
    }

    // 1) Build production seams over the provider's current snapshot.
    //    provider is valid on the worker thread (bake_active guard in tick() fences app).
    //    cfg_ fields are set before open_world and not modified concurrently.
    part_graph_snapshot::Snapshot& snap = provider->graph_snapshot();

    // Fresh ScriptHost for the rebake (shared-lib root from cfg).
    script_host::ScriptHost host;
    host.set_shared_lib_root(cfg.shared_lib_dir);

    live_edit_prod::ProdGraphResolver gr(snap, host,
                                         cfg.schemas_dir,
                                         cfg.shared_lib_dir);
    live_edit_prod::ProdBaker         pb(snap, host, engine->cache_root);
    live_edit_prod::ProdFlattener     pf(snap, host, engine->cache_root);

    // NullSink: we convert errors to BakeError events below.
    struct NullSink : live_edit::ErrorSink {
        void report(const live_edit::LiveEditError&) override {}
    } null_sink;

    NullWatcher nw;
    live_edit::LiveEditSession sess(nw, gr, pb, pf, null_sink,
                                   live_edit::LiveEditConfig{/*debounce_ms=*/0,
                                                             /*bake_budget_ms=*/0});

    // 2) Run the cone rebuild.
    live_edit::RebuildReport rep = sess.rebuild(paths);

    if (!rep.succeeded) {
        // Fail-closed: emit structured errors, do NOT touch the rendered world.
        for (const auto& e : rep.errors) {
            BakeErrorCode code;
            if (e.cause == live_edit::LiveEditError::Cause::FlattenFailed)
                code = BakeErrorCode::Internal;
            else
                code = BakeErrorCode::ScriptError;
            Event ev;
            ev.type    = EventType::BakeError;
            ev.code    = code;
            ev.phase   = "cone";
            ev.module  = e.part;
            ev.message = e.message;
            emit_event(std::move(ev));
        }
        return;  // old world keeps rendering
    }

    if (is_cancelled()) {
        emit_error(BakeErrorCode::Cancelled, "cone", "cancelled");
        return;
    }

    // 3) Re-install the graph so ir_.root_hashes picks up the new cone hashes.
    //    The cone rebuild wrote new .part artifacts to the content-addressed
    //    cache; re-install is cheap because all parts are now cache hits.
    //    Refresh cfg_ callbacks with the cone command's token before re-installing.
    cfg.on_part = [this](const char* module, int done, int total) {
        Event ev;
        ev.type   = EventType::BakePartDone;
        ev.module = module ? module : "";
        ev.done   = done;
        ev.total  = total;
        ev.phase  = (total == 0) ? "install" : "parts";
        emit_event(std::move(ev));
    };
    cfg.gpu_run = [this, token](const char* name,
                                std::function<bool(std::string&)> fn,
                                std::string& err) -> bool {
        matter_async::GpuJob j;
        j.name  = name ? name : "gpu";
        j.fn    = std::move(fn);
        j.token = token;
        return gpu_jobs.run_blocking(std::move(j), err);
    };
    cfg.gl_available = viewer::gl_loaded();  // same semantics as execute_bake: loaded ≠ 4.6-confirmed
    // Phase C Task 7: cfg.root_params_json is intentionally NOT reset here.
    // The cone path operates within the current world's seed context: whatever
    // worldSeed was set by the last execute_bake() (via regenerate() or a plain
    // reload()) stays in cfg so the cone's install_graph() resolves hashes
    // consistently with the live world. A cone triggered after regenerate(seed)
    // will re-install with the same seed override — the cone only touches the
    // changed files' subtree, so this is correct and consistent.
    // Re-create the provider so it picks up the updated cfg_ callbacks.
    provider = std::make_shared<viewer::LocalProvider>(cfg);

    {
        std::string ierr;
        if (!provider->install_graph(ierr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : classify_error(ierr),
                       "cone", ierr.empty() ? "install_graph failed" : ierr);
            return;
        }
        // Emit per-part errors for any partial install failures.
        for (const auto& fp : provider->install_result().failed) {
            BakeErrorCode code = classify_error(fp.error);
            Event ev;
            ev.type    = EventType::BakeError;
            ev.code    = code;
            ev.phase   = "cone";
            ev.module  = fp.module;
            ev.message = fp.error;
            emit_event(std::move(ev));
        }
    }

    if (is_cancelled()) {
        emit_error(BakeErrorCode::Cancelled, "cone", "cancelled");
        return;
    }

    // 4) compose_world using the re-installed provider.
    viewer::WorldManifest new_manifest;
    {
        std::string cerr;
        if (!provider->compose_world(new_manifest, cerr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : classify_error(cerr),
                       "cone", cerr.empty() ? "compose_world failed" : cerr);
            return;
        }
    }

    if (is_cancelled()) {
        emit_error(BakeErrorCode::Cancelled, "cone", "cancelled");
        return;
    }

    // 4-8) Shared publish flow. Cone install errors are emitted above but not
    //      seeded into count_errors_seed (cone does not report them in
    //      BakeFinished.errors, matching the original execute_rebake_cone behavior).
    PublishPipelineParams pp;
    pp.job_prefix            = "cone";
    pp.count_errors_seed     = 0;
    pp.init_culler           = false;
    pp.update_probe_dims     = false;
    pp.verbose_reset_log     = false;
    pp.fault_hook            = {};
    pp.load_msg_include_hash = false;
    publish_pipeline(token, std::move(new_manifest), pp);
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::ensure_tracer
// Build the lazy CPU BVH from the current world state. Returns false if the
// tracer could not be built (e.g. no parts loaded yet). Const so that
// instance_count() const can trigger it via the mutable members.
// ---------------------------------------------------------------------------
bool WorldSession::Impl::ensure_tracer() const {
    if (!tracer_dirty && tracer) return true;

    // Build TraceInstance list from state root entries.
    const auto& entries = state.entries();
    if (entries.empty()) return false;

    std::vector<world_tracer::TraceInstance> trace_instances;
    trace_instances.reserve(entries.size());
    for (const auto& e : entries) {
        world_tracer::TraceInstance ti;
        ti.part_hash = e.part_hash;
        std::memcpy(ti.transform, e.transform, sizeof(ti.transform));
        trace_instances.push_back(ti);
    }

    tracer = std::make_unique<world_tracer::WorldTracer>();
    std::string err;
    if (!tracer->build(engine->cache_root, trace_instances, err)) {
        std::fprintf(stderr, "WorldSession: tracer build failed: %s\n", err.c_str());
        tracer.reset();
        return false;
    }

    // Build hash -> module name from manifest entries (best-effort; only root entries
    // that came from named manifest roots will have a non-empty module field).
    module_by_hash.clear();
    for (const auto& e : manifest.instances) {
        if (!e.module.empty())
            module_by_hash[e.part_hash] = e.module;
    }

    tracer_dirty = false;
    return true;
}

// ---------------------------------------------------------------------------
// EngineContext
// ---------------------------------------------------------------------------

EngineContext::EngineContext(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

EngineContext::~EngineContext() = default;

std::unique_ptr<EngineContext> EngineContext::create(const EngineDesc& desc,
                                                     std::string& err) {
    // Register the calling thread as the GL thread so assert_gl_thread guards
    // are armed in debug builds. Must be called before any GL work begins.
    matter_async::register_gl_thread();

    // Plumb shader override dir (nullptr clears to env/embedded).
    matter::set_shader_override_dir(desc.shader_dir);

    auto impl = std::make_unique<Impl>();
    impl->cache_root = desc.cache_root ? desc.cache_root : "cache";

    if (!desc.allow_gl_lt_46) {
        // GL 4.6 gate (main.cpp lines ~95–113). When allow_gl_lt_46 is set
        // (RT path) we skip the check and leave impl->gl46 = false.
        std::string why;
        if (!viewer::gl46_available(why)) {
            err = "GL 4.6 required for raster path (" + why + "). "
                  "Set allow_gl_lt_46=true with MATTER_RT=1 for the ray-traced fallback.";
            return nullptr;
        }
        impl->gl46 = true;
        printf("GPU cull path: enabled (GL 4.6 ok)\n");
    }

    return std::unique_ptr<EngineContext>(new EngineContext(std::move(impl)));
}

std::unique_ptr<WorldSession> EngineContext::open_world(const WorldDesc& desc,
                                                        std::string& err) {
    auto simpl = std::make_unique<WorldSession::Impl>();
    simpl->engine = impl_.get();

    // Fill provider config from WorldDesc + engine cache_root.
    simpl->cfg.schemas_dir    = desc.schemas_dir    ? desc.schemas_dir    : "";
    simpl->cfg.world_data_dir = desc.world_data_dir ? desc.world_data_dir : "";
    simpl->cfg.world_name     = desc.world_name     ? desc.world_name     : "";
    simpl->cfg.shared_lib_dir = desc.shared_lib_dir ? desc.shared_lib_dir : "";
    simpl->cfg.cache_root     = impl_->cache_root;

    // Task 10: live-edit opt-in. On Linux, eagerly create and register the
    // inotify watcher so events during and after the initial bake are captured.
#ifdef __linux__
    simpl->enable_live_edit = desc.enable_live_edit;
    if (desc.enable_live_edit && !simpl->cfg.schemas_dir.empty()) {
        simpl->inotify_watcher = std::make_unique<live_edit::InotifyWatcher>();
        simpl->inotify_watcher->add_watch(simpl->cfg.schemas_dir);
        if (!simpl->cfg.shared_lib_dir.empty())
            simpl->inotify_watcher->add_watch(simpl->cfg.shared_lib_dir);
        simpl->inotify_watching = true;
        printf("live-edit: watching %s\n", simpl->cfg.schemas_dir.c_str());
    }
#else
    if (desc.enable_live_edit)
        printf("live-edit: MATTER_LIVE_EDIT=1 ignored on non-Linux (inotify not available)\n");
#endif

    // Phase C Task 6: read MATTER_REFINE_RADIUS override ONCE at init time.
    // Default 160.0f (≈ 10 tiles × 10 m/tile). Tests can set a small value
    // via the env var to exercise eviction without a large world.
    {
        const char* rr_env = std::getenv("MATTER_REFINE_RADIUS");
        if (rr_env) {
            float rr = std::atof(rr_env);
            if (rr > 0.0f) simpl->refine_radius = rr;
        }
    }

    // Construct provider. No bake here — caller must call request_bake().
    simpl->provider = std::make_shared<viewer::LocalProvider>(simpl->cfg);

    // Always init the camera; sets defaults used in both RT and raster modes.
    simpl->renderer.init_camera();

    return std::unique_ptr<WorldSession>(new WorldSession(std::move(simpl)));
}

// ---------------------------------------------------------------------------
// WorldSession
// ---------------------------------------------------------------------------

WorldSession::WorldSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

WorldSession::~WorldSession() {
    // Phase B destructor protocol (Task 6 brief):
    //   1) commands.shut_down() — cancels in-flight token, wakes worker.
    //   2) gpu_jobs.shut_down() — unblocks any run_blocking waiter on the
    //      worker so it can observe the cancel and return.
    //   3) worker.join() — the worker thread returns from execute_bake.
    //   4) gpu_jobs.pump(1e9) — drain stragglers on the GL thread so any
    //      posted publish jobs run their destructors here (not deferred).
    //   5) existing GL teardown (release probe_tex -> raster -> composer
    //      -> store, then renderer.shutdown()) mirrors main.cpp lines
    //      ~549–557.
    impl_->commands.shut_down();
    impl_->gpu_jobs.shut_down();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->gpu_jobs.pump(1e9);

    viewer::release_probe_textures(impl_->probe_tex);
    impl_->raster.reset();
    impl_->composer.reset();
    impl_->store.reset();
    impl_->renderer.shutdown();
}

void WorldSession::set_test_fault_hook(std::function<void(int)> hook) {
    // Task 7 test seam: stored on cfg_ so it's picked up when the next bake
    // command builds a fresh LocalProvider(cfg_). Thread-safe: only call this
    // before request_bake() or between bakes on the same thread as the caller.
    impl_->cfg.test_fault_hook = std::move(hook);
}

void WorldSession::set_bake_focus(const float pos[3]) {
    // Phase C Task 3: thread-safe write of the focus point.
    // Copied under focus_mutex; the worker reads a snapshot at the top of
    // publish_pipeline so the whole pass uses one consistent focus value.
    std::lock_guard<std::mutex> lk(impl_->focus_mutex);
    impl_->focus[0] = pos[0];
    impl_->focus[1] = pos[1];
    impl_->focus[2] = pos[2];
}

void WorldSession::request_bake() {
    // Phase B: enqueue a BakeAll command and return immediately. The worker
    // executes the pipeline; progress arrives via poll_event() and GL work
    // runs in pump_gpu_jobs() on the app/GL thread. Supersession is handled
    // inside CommandQueue::push (cancels in-flight token + clears pending).
    impl_->ensure_worker_started();
    matter_async::Command c;
    c.kind = matter_async::CommandKind::BakeAll;
    impl_->commands.push(std::move(c));
}

void WorldSession::reload() {
    // Phase B: identical shape to request_bake(), but kind = Reload so the
    // worker will additionally reset the GPU culler at the top of execute_bake
    // (mirroring old reload() semantics).
    impl_->ensure_worker_started();
    matter_async::Command c;
    c.kind = matter_async::CommandKind::Reload;
    impl_->commands.push(std::move(c));
}

void WorldSession::regenerate(uint64_t world_seed) {
    // Phase C Task 7: store the root-params override and enqueue a Reload.
    // The override JSON is built here on the app thread (cheap sprintf), stored
    // under seed_mutex, and snapshotted by execute_bake() just before it
    // constructs the LocalProvider. Full supersession semantics: if a bake is
    // already in flight, the Reload cancels it at the next between-parts
    // checkpoint and starts fresh with the new seed.
    {
        std::lock_guard<std::mutex> lk(impl_->seed_mutex);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"worldSeed\":%llu}",
                      (unsigned long long)world_seed);
        impl_->seed_root_params_json = buf;
    }
    impl_->ensure_worker_started();
    matter_async::Command c;
    c.kind = matter_async::CommandKind::Reload;
    impl_->commands.push(std::move(c));
}

bool WorldSession::sea_level(float& out) const {
    if (impl_->world_sea_level == std::numeric_limits<float>::lowest()) return false;
    out = impl_->world_sea_level;
    return true;
}

void WorldSession::tick() {
    // Poll provider deltas and apply to world state (main.cpp lines ~507–508).
    // Phase B: the worker may be tearing down or rebuilding provider under us;
    // skip poll_deltas while a bake command is active. LocalProvider::poll_deltas
    // returns false today, so this atomic-flag guard suffices without a
    // shared_ptr/mutex promotion of `provider`. When a future provider grows a
    // live delta stream (e.g. NetworkProvider), promote provider under a small
    // mutex so tick() can hold a strong ref for the duration of poll_deltas.
    if (impl_->bake_active.load(std::memory_order_acquire)) return;
    if (!impl_->provider) return;
    viewer::WorldDelta d;
    if (impl_->provider->poll_deltas(d)) {
        impl_->state.apply(d);
        // World content changed — lazy tracer is stale.
        impl_->tracer_dirty = true;
        impl_->tracer.reset();
    }

#ifdef __linux__
    // Task 10: inotify watcher + 150 ms debounce (app thread only).
    // Only active when enable_live_edit was set in WorldDesc.
    // The watcher is created eagerly in open_world; we only poll here.
    if (impl_->enable_live_edit && impl_->inotify_watching) {
        // Drain newly observed events into the pending debounce set.
        std::vector<live_edit::FileEvent> evs;
        impl_->inotify_watcher->poll(evs);
        for (const auto& e : evs) {
            impl_->le_pending_paths_.insert(e.path);
            if (e.t_ms > impl_->le_last_event_ms_)
                impl_->le_last_event_ms_ = e.t_ms;
            impl_->le_have_pending_ = true;
        }

        // Fire once the quiet window has elapsed since the last event.
        if (impl_->le_have_pending_) {
            long long now = impl_->inotify_watcher->now_ms();
            if (now - impl_->le_last_event_ms_ >= impl_->k_debounce_ms) {
                // Quiet window elapsed: push a RebakeCone command.
                std::vector<std::string> paths(impl_->le_pending_paths_.begin(),
                                               impl_->le_pending_paths_.end());
                impl_->le_pending_paths_.clear();
                impl_->le_have_pending_ = false;
                impl_->le_last_event_ms_ = 0;

                impl_->ensure_worker_started();
                matter_async::Command c;
                c.kind          = matter_async::CommandKind::RebakeCone;
                c.changed_files = std::move(paths);
                impl_->commands.push(std::move(c));
            }
        }
    }
#endif // __linux__
}

void WorldSession::render(const Camera3D& cam, int fb_width, int fb_height,
                          const RenderOptions& opts) {
    if (!impl_->connected) return;

    // Phase C Task 9: update resident_sectors stat each frame.
    impl_->stats.resident_sectors = impl_->sector_streamer
        ? (uint32_t)impl_->sector_streamer->resident_count() : 0;

    // Apply option defaults.
    float budget = opts.pixel_budget;
    if (budget == 0.0f) budget = 1.0f;
    if (budget < 0.05f) budget = 0.05f;
    if (budget > 4.0f)  budget = 4.0f;

    float active_radius = opts.active_radius;
    if (active_radius == 0.0f) active_radius = 64.0f;

    impl_->sec.set_active_radius(active_radius);
    // Unconditional: 0 = off (matches pre-facade main.cpp, which always set it).
    impl_->sec.set_min_projected_size(opts.min_projected_size);
    impl_->sec.set_pixel_budget(budget);
    impl_->raster->set_pixel_budget(budget);

    // Select resolver.
    viewer::SectorResolver& resolver =
        (opts.resolver == ResolverKind::SectorLod)
            ? (viewer::SectorResolver&)impl_->sec
            : (viewer::SectorResolver&)impl_->pass;

    const Vector3 cp = cam.position;
    const float3  cam_pos = make_float3(cp.x, cp.y, cp.z);

    if (opts.path == RenderPath::Raytrace) {
        // --- Raytrace path (mirrors main.cpp lines ~389–391 + warm-up ~286–290) ---
        // Lazy RT shader init: defers the ~60s warm-up until first Raytrace render.
        if (!impl_->rt_shader_ready) {
            std::string serr;
            if (!impl_->renderer.init_shader("shaders/raytrace_tlas_blas_processed.fs", serr)) {
                printf("RT shader init failed: %s\n", serr.c_str());
                return;
            }
            impl_->rt_shader_ready = true;
            // set_lights after init_shader so uniforms land on the loaded shader.
            impl_->renderer.set_lights(impl_->manifest.lights);
        }
        int active = impl_->composer->compose(impl_->state, resolver, impl_->lods, cam_pos);
        impl_->stats.instances_resolved = (uint32_t)active;

        if (!impl_->rt_warmed) {
            impl_->renderer.warm_up(impl_->store->blas(), impl_->composer->tlas());
            impl_->rt_warmed = true;
        }

        // Sync camera (the facade owns its own Renderer, not main.cpp's).
        impl_->renderer.camera() = cam;

        // Clear + draw.
        glClearColor(impl_->sky_clear[0], impl_->sky_clear[1], impl_->sky_clear[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        impl_->renderer.draw(impl_->store->blas(), impl_->composer->tlas());

    } else {
        // --- GpuDriven path (mirrors main.cpp lines ~392–431 + ~436–451) ---
        auto t0 = std::chrono::steady_clock::now();
        auto resolved = resolver.resolve(impl_->state, impl_->lods, cam_pos);
        auto t1 = std::chrono::steady_clock::now();

        float eye[3]     = {cp.x, cp.y, cp.z};
        Vector3 tgt      = cam.target;
        float target3[3] = {tgt.x, tgt.y, tgt.z};
        float up3[3]     = {0, 1, 0};
        float aspect     = (float)fb_width / (float)fb_height;
        // Build view/proj/vp explicitly (same near/far as
        // camera_frustum_planes_raw) so the HiZ path gets the exact vp
        // the frustum planes came from.
        const float near_z = 0.05f, far_z = 4000.0f;
        float view[16], proj[16], vp[16];
        viewer::make_lookat(eye, target3, up3, view);
        viewer::make_perspective(cam.fovy, aspect, near_z, far_z, proj);
        viewer::mul16(view, proj, vp);
        float planes[6][4];
        viewer::extract_frustum_planes(vp, planes);
        // Propagate the runtime HiZ toggle every frame.
        impl_->gpu_culler.set_hiz_enabled(opts.hiz_occlusion);
        impl_->raster->set_wireframe(opts.wireframe);
        impl_->raster->set_cull_backfaces(opts.cull_backfaces);
        // Enable stats readback (same as main.cpp line 418 — viewer always shows counters).
        impl_->gpu_culler.set_stats_readback(true);
        impl_->gpu_culler.cull(resolved, *impl_->store, eye, planes, vp, budget);
        auto t2 = std::chrono::steady_clock::now();

        impl_->stats.resolve_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
        impl_->stats.build_ms   = std::chrono::duration<float, std::milli>(t2 - t1).count();
        impl_->stats.instances_resolved = (uint32_t)resolved.size();

        // Clear + draw (main.cpp lines ~432–447).
        glClearColor(impl_->sky_clear[0], impl_->sky_clear[1], impl_->sky_clear[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        auto d0 = std::chrono::steady_clock::now();
        impl_->stats.triangles = (uint32_t)impl_->raster->draw_gpu_driven(
                impl_->gpu_culler, *impl_->store, cam);
        impl_->stats.instances_drawn    = (uint32_t)impl_->gpu_culler.emitted();
        impl_->stats.clusters_culled    = (uint32_t)impl_->gpu_culler.culled_clusters();
        impl_->stats.hiz_culled         = (uint32_t)impl_->gpu_culler.culled_hiz();
        impl_->stats.draw_ms = std::chrono::duration<float, std::milli>(
                                   std::chrono::steady_clock::now() - d0).count();

        // Build HiZ depth max-pyramid for next-frame occlusion culling.
        // No-op when HiZ toggle is off.
        if (impl_->culler_ready)
            impl_->gpu_culler.build_hiz(fb_width, fb_height);
    }
}

bool WorldSession::poll_event(Event& out) {
    // Phase B: worker thread pushes into `events` under events_mutex; the
    // consumer (app thread) drains here under the same lock.
    std::lock_guard<std::mutex> lk(impl_->events_mutex);
    if (impl_->events.empty()) return false;
    out = std::move(impl_->events.front());
    impl_->events.pop_front();
    return true;
}

const FrameStats& WorldSession::frame_stats() const {
    impl_->stats.resident_sectors = impl_->sector_streamer
        ? (uint32_t)impl_->sector_streamer->resident_count() : 0;
    return impl_->stats;
}

void WorldSession::pump_gpu_jobs(float ms_budget) {
    impl_->gpu_jobs.pump((double)ms_budget);
}

// ---------------------------------------------------------------------------
// Query API — backed by a lazily built CPU BVH (WorldTracer).
// ---------------------------------------------------------------------------
bool WorldSession::raycast(const float origin[3], const float dir[3],
                           float max_t, RayHit& out) {
    if (!impl_->connected) return false;
    if (!impl_->ensure_tracer()) return false;

    world_tracer::Hit hit;
    if (!impl_->tracer->trace(origin, dir, max_t, hit)) return false;

    out.t           = hit.t;
    out.normal[0]   = hit.normal[0];
    out.normal[1]   = hit.normal[1];
    out.normal[2]   = hit.normal[2];
    out.material_id = hit.material_id;
    out.instance    = hit.instance;

    // Resolve part_hash via expanded_instance table.
    out.part_hash = 0;
    if (hit.instance != 0xffffffffu) {
        float xf[16];
        impl_->tracer->expanded_instance(hit.instance, out.part_hash, xf);
    }
    return true;
}

uint32_t WorldSession::instance_count() const {
    if (!impl_->connected) return 0;
    if (!impl_->ensure_tracer()) return 0;
    return (uint32_t)impl_->tracer->expanded_instance_count();
}

bool WorldSession::instance_info(uint32_t idx, InstanceInfo& out) {
    if (!impl_->connected) return false;
    if (!impl_->ensure_tracer()) return false;

    uint64_t part_hash = 0;
    if (!impl_->tracer->expanded_instance(idx, part_hash, out.transform)) return false;

    out.part_hash = part_hash;

    // Module name: look up in hash->module map built from manifest entries.
    out.module_name = nullptr;
    auto it = impl_->module_by_hash.find(part_hash);
    if (it != impl_->module_by_hash.end() && !it->second.empty())
        out.module_name = it->second.c_str();

    return true;
}

} // namespace matter
