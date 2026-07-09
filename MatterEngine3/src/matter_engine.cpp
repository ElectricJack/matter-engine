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

// Raylib must come before glad to avoid double-definition of GL types.
#include "raylib.h"
#include "external/glad.h"   // glClearColor / glClear (same as gpu_culler.cpp)

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace matter {

// Classify a provider-returned error string into a structured code. The
// executor doesn't get typed errors back from the pipeline (yet — Task 7 wires
// finer-grained tags), so we bucket by substring for now. Task 7 will overlay
// the real code plumbing; keeping it here makes it easy to hoist later.
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
    if (err.find("manifest") != std::string::npos ||
        err.find("not found") != std::string::npos ||
        err.find("load") != std::string::npos)
        return BakeErrorCode::IoError;
    if (err.find("bake ") != std::string::npos ||
        err.find("script") != std::string::npos ||
        err.find("install") != std::string::npos ||
        err.find("resolve hash") != std::string::npos ||
        err.find("evaluate") != std::string::npos)
        return BakeErrorCode::ScriptError;
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
    viewer::LocalProviderConfig cfg;
    std::unique_ptr<viewer::LocalProvider> provider;

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
    for (;;) {
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
                    // Task 9 will implement RebakeCone; treat as a full BakeAll
                    // for now so a stray push does something sane instead of
                    // silently dropping.
                    execute_bake(cmd, /*is_reload=*/false);
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

    provider = std::make_unique<viewer::LocalProvider>(cfg);

    // install_graph on the worker (script eval + per-part bake; no GL).
    // Task 7: skip-and-continue — install_graph() returns true even with partial
    // failures; emit one BakeError per failed part, then continue the pipeline
    // with the parts that succeeded. count_errors accumulates the total.
    int count_errors = 0;
    {
        std::string err;
        if (!provider->install_graph(err)) {
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
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "install", "cancelled"); return; }

    // 3) compose_world on the worker (scatter/place; tileset GL marshaled) ----
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
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "compose", "cancelled"); return; }

    // 4) GL reset job — recreate raster + composer + PartStore on the GL thread
    // Also creates the fresh PartStore inside the job so the old store (still
    // holding GL-owned BLAS textures) is destroyed on the GL thread, matching
    // today's teardown ordering.
    struct ResetOutput {
        std::unique_ptr<viewer::PartStore>     new_store;
        std::unique_ptr<viewer::WorldComposer> new_composer;
        std::unique_ptr<viewer::RasterComposer> new_raster;
        viewer::ProbeTextures                   new_probe_tex{};
    };
    auto reset_out = std::make_shared<ResetOutput>();

    // Capture manifest snapshot by value into the reset job — it needs the
    // lights/probes to program the raster composer, and the worker will
    // overwrite `manifest` when publishing per-part deltas below.
    matter_async::GpuJob reset_job;
    reset_job.name  = "bake.reset";
    reset_job.token = token;
    reset_job.fn = [this, reset_out, new_manifest](std::string& err) -> bool {
        matter_async::assert_gl_thread("bake.reset");
        // Fail-close: mark disconnected on the app/GL thread before tearing
        // down the old world. This write and the matching `connected = true`
        // below are both on the GL thread — no data race with the atomic.
        // Old world stops rendering from this point until the new one is ready.
        connected.store(false, std::memory_order_release);

        // --- relocated from the old bake_once (GL setup block) ---------------
        // Release stale probe textures before creating new ones.
        viewer::release_probe_textures(probe_tex);
        reset_out->new_raster = std::make_unique<viewer::RasterComposer>();
        auto& raster_local = reset_out->new_raster;
        if (!engine->gl46) {
            // RT path: set lights (no-op if shader not yet loaded; the lazy RT
            // init in render() will re-apply after loading). Raster path skips
            // raster init in non-gl46 mode.
            renderer.set_lights(new_manifest.lights);
        } else {
            std::string rerr;
            if (!raster_local->init(rerr)) {
                printf("raster: %s\n", rerr.c_str());
                err = rerr;
                return false;
            }
            raster_local->set_lights(new_manifest.lights);

            if (new_manifest.probes && new_manifest.probes->valid()) {
                reset_out->new_probe_tex = viewer::upload_probe_textures(*new_manifest.probes);
                raster_local->set_probes(reset_out->new_probe_tex);
            } else {
                printf("probes unavailable - flat ambient fallback\n");
            }

            // Tone-map sky_color (c/(c+1) then pow(1/2.2)) to derive the clear
            // color. With the default lights this reproduces approximately
            // (96,118,143).
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
            printf("sky clear color: (%d,%d,%d)\n", (int)r, (int)g, (int)b);

            // GPU-driven shader init after set_lights (mirrors old bake_once).
            std::string gerr;
            if (!raster_local->init_gpu_driven(gerr)) {
                fprintf(stderr, "FATAL: GPU-driven shader init failed: %s. "
                        "Set MATTER_RT=1 to fall back to the ray-traced path.\n",
                        gerr.c_str());
                err = "GPU-driven shader init failed: " + gerr;
                return false;
            }
        }
        // Upload world lights to the raytrace shader (no-op in raster mode).
        renderer.set_lights(new_manifest.lights);

        // First-success GpuCuller init (relocated from the old request_bake tail).
        if (!culler_ready && engine->gl46) {
            std::string cull_err;
            if (!gpu_culler.init(cull_err)) {
                fprintf(stderr, "FATAL: GpuCuller::init failed: %s\n", cull_err.c_str());
                err = "GpuCuller::init failed: " + cull_err;
                return false;
            }
            printf("GpuCuller: initialized\n");
            culler_ready = true;
        }

        // Fresh PartStore inside the GL job so the OLD store (BLAS textures)
        // is destroyed on the GL thread.
        reset_out->new_store = std::make_unique<viewer::PartStore>(cfg.cache_root);
        // Small initial cap; grows in the publish jobs; final exact cap on
        // the finalize job.
        reset_out->new_composer = std::make_unique<viewer::WorldComposer>(
            *reset_out->new_store, /*tlas_capacity=*/16);

        // Clear world state (entries only — lights/probes are the raster/renderer's
        // concern and already programmed above). apply() of per-hash deltas below
        // grows it back up.
        state.reset(viewer::WorldManifest{});

        // Swap ownership from old objects to the fresh ones. The old objects
        // destruct here on the GL thread (raster shader/texture cleanup).
        raster.swap(reset_out->new_raster);
        composer.swap(reset_out->new_composer);
        store.swap(reset_out->new_store);
        probe_tex = reset_out->new_probe_tex;
        reset_out->new_probe_tex = viewer::ProbeTextures{}; // moved

        // Publish the new manifest snapshot on the app/GL thread and mark
        // connected. tracer is stale from now on.
        manifest    = new_manifest;
        connected.store(true, std::memory_order_release);
        tracer_dirty = true;
        tracer.reset();
        return true;
    };
    {
        std::string rerr;
        if (!gpu_jobs.run_blocking(std::move(reset_job), rerr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : BakeErrorCode::GpuError,
                       "gl", rerr.empty() ? "reset job failed" : rerr);
            return;
        }
    }
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "gl", "cancelled"); return; }

    // 5) Reconcile job (store swap already done in reset; reconcile now reads
    //    the new store from the app thread and returns the want-list to us).
    struct ReconcileOutput { std::vector<uint64_t> want; };
    auto reco_out = std::make_shared<ReconcileOutput>();
    matter_async::GpuJob reco_job;
    reco_job.name  = "bake.reconcile";
    reco_job.token = token;
    reco_job.fn    = [this, reco_out](std::string& /*err*/) -> bool {
        matter_async::assert_gl_thread("bake.reconcile");
        reco_out->want = provider->reconcile(manifest, *store);
        return true;
    };
    {
        std::string rerr;
        if (!gpu_jobs.run_blocking(std::move(reco_job), rerr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : BakeErrorCode::GpuError,
                       "gl", rerr.empty() ? "reconcile failed" : rerr);
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
    const int total_parts = (int)publish_order.size();
    const auto& want = publish_order;

    // module_by_hash for the BakePartDone label — same source as fetch_parts.
    // Kept per-command so a cache-warm run still labels events correctly.
    std::unordered_map<uint64_t, std::string> mod_by_hash;
    for (const auto& e : manifest.instances)
        if (!e.module.empty()) mod_by_hash[e.part_hash] = e.module;

    // Shared cap-growth state across all publish jobs. Both fields are read and
    // written only on the GL thread (all publish jobs are FIFO on the pump), so
    // no mutex is needed. Initial cap matches the reset job's WorldComposer(16).
    // Task 7 fix: load_fail_count accumulates per-part load failures across all
    // publish jobs (GL thread only, FIFO — no mutex needed). Read by worker after
    // run_blocking(finalize_job) guarantees all publish jobs have completed.
    struct CapState { size_t needed = 0; size_t current = 16; int load_fail_count = 0; };
    auto cap_state = std::make_shared<CapState>();

    // Capture the fault hook once at post-time (by value) so each publish job
    // holds its own copy. Safe: hook is set before request_bake() and not
    // modified until the next bake command, which cannot start until after
    // execute_bake returns and run_blocking(finalize_job) has completed.
    auto fault_hook = cfg.test_fault_hook;

    for (int i = 0; i < total_parts; ++i) {
        if (is_cancelled()) {
            emit_error(BakeErrorCode::Cancelled, "parts", "cancelled between parts");
            return;
        }
        uint64_t h = want[i];

        // Deterministic post-time emit (unchanged — order must not change).
        {
            Event ev;
            ev.type   = EventType::BakePartDone;
            auto it   = mod_by_hash.find(h);
            ev.module = (it != mod_by_hash.end()) ? it->second : "";
            ev.done   = i + 1;
            ev.total  = total_parts;
            ev.phase  = "parts";
            emit_event(std::move(ev));
        }

        // Capture module name at post-time so the publish job lambda can label
        // BakeError events without accessing mod_by_hash on the GL thread.
        std::string part_module;
        {
            auto it = mod_by_hash.find(h);
            if (it != mod_by_hash.end()) part_module = it->second;
        }

        // Snapshot manifest entries whose part_hash == h. Copied by value so
        // the job doesn't race with a future manifest swap (though a supersede
        // cancels the token before that happens).
        std::vector<viewer::WorldManifestEntry> added;
        for (const auto& e : manifest.instances)
            if (e.part_hash == h) added.push_back(e);
        const size_t entry_count = added.size();

        matter_async::GpuJob pj;
        pj.name  = "bake.publish";
        pj.token = token;
        pj.fn = [this, i, h, part_module, added_moved = std::move(added),
                 entry_count, cap_state, fault_hook](std::string& /*err*/) -> bool {
            matter_async::assert_gl_thread("bake.publish");

            // Task 7 fix: per-part skip-and-continue in the publish (load) phase.
            // Fire the test fault hook before get_or_load; catch exceptions to
            // inject deterministic faults. On any failure: emit a BakeError,
            // increment load_fail_count, and return true-with-skip (do NOT
            // publish the delta; do not abort the job pipeline).
            // emit_event is mutex-guarded so calling from the GL thread is safe.
            BakeErrorCode fail_code = BakeErrorCode::IoError;
            std::string   fail_msg;
            bool          part_failed = false;

            try {
                if (fault_hook) fault_hook(i);
                if (!store->get_or_load(h)) {
                    fail_code = BakeErrorCode::IoError;
                    fail_msg  = "load failed for part " + part_module +
                                " (hash " + std::to_string(h) + ")";
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

            // Composer cap growth (spec step 6): count drawable nodes in this
            // part's tree, accumulate needed_cap, and recreate the composer
            // when the cap is exceeded. TLAS recomposes every frame so recreate
            // is cheap; we add headroom (max(needed, current*2)) to avoid
            // recreating on every part.
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

    // 7) Finalize job (blocking): part_lod_table + census + exact-cap
    //    composer recreate using the original bake_once cap walk.
    matter_async::GpuJob finalize_job;
    finalize_job.name  = "bake.finalize";
    finalize_job.token = token;
    finalize_job.fn    = [this](std::string& /*err*/) -> bool {
        matter_async::assert_gl_thread("bake.finalize");
        lods = store->part_lod_table();

        // Census (relocated from the old bake_once tail).
        stats.instances_total = (uint32_t)manifest.instances.size();
        stats.parts_baked     = (uint32_t)provider->baked_count();
        stats.cache_hits      = (uint32_t)provider->hit_count();
        if (manifest.probes && manifest.probes->valid()) {
            stats.probe_dims[0] = manifest.probes->grid.nx;
            stats.probe_dims[1] = manifest.probes->grid.ny;
            stats.probe_dims[2] = manifest.probes->grid.nz;
        } else {
            stats.probe_dims[0] = stats.probe_dims[1] = stats.probe_dims[2] = 0;
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
                       "gl", ferr.empty() ? "finalize failed" : ferr);
            return;
        }
    }

    // Task 7 fix: merge load-phase (publish-job) failures into count_errors.
    // run_blocking(finalize_job) above guarantees all publish jobs completed
    // before we reach here, so load_fail_count is final and safe to read on
    // the worker thread (GL-thread writes are sequenced before the barrier).
    count_errors += cap_state->load_fail_count;

    // 8) BakeFinished ---------------------------------------------------------
    {
        Event ev;
        ev.type   = EventType::BakeFinished;
        ev.errors = count_errors;   // Task 7: install + load failure count.
        emit_event(std::move(ev));
    }
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

    // Construct provider. No bake here — caller must call request_bake().
    simpl->provider = std::make_unique<viewer::LocalProvider>(simpl->cfg);

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
}

void WorldSession::render(const Camera3D& cam, int fb_width, int fb_height,
                          const RenderOptions& opts) {
    if (!impl_->connected) return;

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
