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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace matter {

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

    bool connected = false;

    // Event queue — capped at 4096 to prevent unbounded growth if the app
    // never drains (older events dropped when the cap is hit).
    std::deque<Event> events;

    FrameStats stats{};

    // Phase B: async bake GPU work queue.
    matter_async::GpuJobQueue gpu_jobs;

    // Lazy CPU tracer for the query API (raycast/instance_count/instance_info).
    // Built on first query after a bake. mutable so instance_count() const can build.
    mutable std::unique_ptr<world_tracer::WorldTracer> tracer;
    mutable bool tracer_dirty = true;  // true after every bake/reload

    // hash -> module name for expanded_instance info (filled from manifest)
    mutable std::unordered_map<uint64_t, std::string> module_by_hash;

    // Ensure tracer is built and up-to-date. Returns false if build failed.
    bool ensure_tracer() const;

    // The core bake/reconnect logic relocated from main.cpp's connect_sequence.
    // Returns true on success; on failure sets err and leaves connected = false.
    bool bake_once(std::string& err);
};

// ---------------------------------------------------------------------------
// WorldSession::Impl::bake_once
// Verbatim relocation of main.cpp's connect_sequence lambda (~lines 170–260).
// ---------------------------------------------------------------------------
bool WorldSession::Impl::bake_once(std::string& err) {
    if (!provider->connect(manifest, err)) {
        printf("connect: %s\n", err.c_str());
        return false;
    }
    store = std::make_unique<viewer::PartStore>(cfg.cache_root);
    auto want = provider->reconcile(manifest, *store);
    if (!provider->fetch_parts(want, *store, err)) {
        printf("fetch: %s\n", err.c_str());
        return false;
    }
    state.reset(manifest);
    // Size the TLAS to the fully child-expanded instance count, not just the
    // root count: each placed part recursively pulls in its baked children
    // (e.g. a Tree expands to hundreds of Leaf instances). Mirrors the depth
    // cap and empty-LOD skip in WorldComposer::compose.
    size_t cap = 16;
    for (const auto& e : manifest.instances) {
        viewer::walk_part_tree(e.part_hash,
            [&](uint64_t h) -> const viewer::LoadedPart* { return store->get_or_load(h); },
            [&](const viewer::LoadedPart* lp, uint64_t /*hash*/, const float /*rel*/[16], int /*depth*/) {
                // A geometry-less assembly part contributes no instance of its own
                // but its children are still visited -- mirror WorldComposer::compose.
                if (!lp->lod_blas.empty()) ++cap;
            });
    }
    composer = std::make_unique<viewer::WorldComposer>(*store, cap);
    // Recreate RasterComposer on each (re)connect so stale GL mesh caches drop.
    // Release stale probe textures before creating new ones (GL context must be live).
    viewer::release_probe_textures(probe_tex);
    raster = std::make_unique<viewer::RasterComposer>();
    if (!engine->gl46) {
        // RT path: set lights (no-op if shader not yet loaded; deferred to lazy
        // RT init in render()). Raster path skips raster init in non-gl46 mode.
        renderer.set_lights(manifest.lights);
    } else {
        // Raster path (GL 4.6).
        std::string rerr;
        if (!raster->init(rerr)) {
            printf("raster: %s\n", rerr.c_str());
            err = rerr;
            return false;
        }
        // Always upload WorldLights (defaults reproduce Phase-1 look for worlds
        // without light lines).
        raster->set_lights(manifest.lights);

        // Upload probe textures if available; fallback to Phase-1 flat ambient.
        if (manifest.probes && manifest.probes->valid()) {
            probe_tex = viewer::upload_probe_textures(*manifest.probes);
            raster->set_probes(probe_tex);
        } else {
            printf("probes unavailable - flat ambient fallback\n");
        }

        // Tone-map sky_color (c/(c+1) then pow(1/2.2)) to derive the clear color.
        // With the default lights this reproduces approximately the old (96,118,143).
        auto tonemap = [](float c) -> unsigned char {
            float mapped  = c / (c + 1.0f);                   // Reinhard
            float gamma   = std::pow(mapped, 1.0f / 2.2f);    // gamma
            float clamped = gamma < 0.0f ? 0.0f : (gamma > 1.0f ? 1.0f : gamma);
            return (unsigned char)(clamped * 255.0f + 0.5f);
        };
        // Compute exact same (unsigned char) values as main.cpp lines 215-226,
        // then store as c/255.f so glClearColor gets the same bucket.
        unsigned char r = tonemap(manifest.lights.sky_color[0]);
        unsigned char g = tonemap(manifest.lights.sky_color[1]);
        unsigned char b = tonemap(manifest.lights.sky_color[2]);
        sky_clear[0] = r / 255.f;
        sky_clear[1] = g / 255.f;
        sky_clear[2] = b / 255.f;
        printf("sky clear color: (%d,%d,%d)\n", (int)r, (int)g, (int)b);
    }
    // GPU-driven shader: load after init() so the raster shader is ready.
    // FATAL on failure — there is no CPU raster fallback (MATTER_RT=1 is
    // the escape hatch for older GL).
    if (engine->gl46) {
        std::string gerr;
        if (!raster->init_gpu_driven(gerr)) {
            fprintf(stderr, "FATAL: GPU-driven shader init failed: %s. "
                    "Set MATTER_RT=1 to fall back to the ray-traced path.\n",
                    gerr.c_str());
            err = "GPU-driven shader init failed: " + gerr;
            return false;
        }
    }
    // Upload world lights to the raytrace shader (no-op in raster mode because
    // the shader is not loaded; the raster path uses raster->set_lights above).
    renderer.set_lights(manifest.lights);
    lods = store->part_lod_table();
    stats.instances_total = (uint32_t)manifest.instances.size();
    stats.parts_baked     = (uint32_t)provider->baked_count();
    stats.cache_hits      = (uint32_t)provider->hit_count();
    // Probe grid dims for HUD (all-zero = probes unavailable/OFF)
    if (manifest.probes && manifest.probes->valid()) {
        stats.probe_dims[0] = manifest.probes->grid.nx;
        stats.probe_dims[1] = manifest.probes->grid.ny;
        stats.probe_dims[2] = manifest.probes->grid.nz;
    } else {
        stats.probe_dims[0] = stats.probe_dims[1] = stats.probe_dims[2] = 0;
    }
    connected = true;
    // Invalidate the lazy tracer: world content changed.
    tracer_dirty = true;
    tracer.reset();
    return true;
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
    // Shutdown order mirrors main.cpp lines ~549–557 (reset GL-owning objects
    // before CloseWindow; order: probe_tex -> raster -> composer -> store).
    viewer::release_probe_textures(impl_->probe_tex);
    impl_->raster.reset();
    impl_->composer.reset();
    impl_->store.reset();
    impl_->renderer.shutdown();
}

void WorldSession::request_bake() {
    // Emit BakeStarted.
    {
        Event ev;
        ev.type = EventType::BakeStarted;
        if (impl_->events.size() >= 4096) impl_->events.pop_front();
        impl_->events.push_back(std::move(ev));
    }

    // Install the on_part progress callback so BakePartDone events flow out.
    impl_->cfg.on_part = [this](const char* module, int done, int total) {
        Event ev;
        ev.type   = EventType::BakePartDone;
        ev.module = module ? module : "";
        ev.done   = done;
        ev.total  = total;
        if (impl_->events.size() >= 4096) impl_->events.pop_front();
        impl_->events.push_back(std::move(ev));
    };
    impl_->provider = std::make_unique<viewer::LocalProvider>(impl_->cfg);

    std::string err;
    if (!impl_->bake_once(err)) {
        Event ev;
        ev.type    = EventType::BakeError;
        ev.message = err;
        if (impl_->events.size() >= 4096) impl_->events.pop_front();
        impl_->events.push_back(std::move(ev));
        return;
    }

    // First success: initialize GPU culler (only when GL 4.6 available).
    if (!impl_->culler_ready && impl_->engine->gl46) {
        std::string cull_err;
        if (!impl_->gpu_culler.init(cull_err)) {
            fprintf(stderr, "FATAL: GpuCuller::init failed: %s\n", cull_err.c_str());
            // Treat init failure as a bake error so the app can surface it.
            Event ev;
            ev.type    = EventType::BakeError;
            ev.message = "GpuCuller::init failed: " + cull_err;
            if (impl_->events.size() >= 4096) impl_->events.pop_front();
            impl_->events.push_back(std::move(ev));
            return;
        }
        printf("GpuCuller: initialized\n");
        impl_->culler_ready = true;
    }

    // Emit BakeFinished.
    {
        Event ev;
        ev.type = EventType::BakeFinished;
        if (impl_->events.size() >= 4096) impl_->events.pop_front();
        impl_->events.push_back(std::move(ev));
    }
}

void WorldSession::reload() {
    // Live-edit rebake. Reset GPU culler so stale part slots from the old world
    // are cleared. Recreate provider from cfg, then run bake_once.
    // Fail-closed: on error, connected is marked false (set by bake_once on
    // failure) so render() no-ops — old world objects may dangle if bake_once
    // fails partway through tearing down store/composer, so we accept the
    // fail-closed behavior rather than trying to restore the old state.
    impl_->connected = false;

    // Emit BakeStarted.
    {
        Event ev;
        ev.type = EventType::BakeStarted;
        if (impl_->events.size() >= 4096) impl_->events.pop_front();
        impl_->events.push_back(std::move(ev));
    }

    if (impl_->culler_ready) impl_->gpu_culler.reset();

    // Install on_part callback.
    impl_->cfg.on_part = [this](const char* module, int done, int total) {
        Event ev;
        ev.type   = EventType::BakePartDone;
        ev.module = module ? module : "";
        ev.done   = done;
        ev.total  = total;
        if (impl_->events.size() >= 4096) impl_->events.pop_front();
        impl_->events.push_back(std::move(ev));
    };
    impl_->provider = std::make_unique<viewer::LocalProvider>(impl_->cfg);

    std::string err;
    if (!impl_->bake_once(err)) {
        Event ev;
        ev.type    = EventType::BakeError;
        ev.message = err;
        if (impl_->events.size() >= 4096) impl_->events.pop_front();
        impl_->events.push_back(std::move(ev));
        return;
    }

    // Emit BakeFinished.
    {
        Event ev;
        ev.type = EventType::BakeFinished;
        if (impl_->events.size() >= 4096) impl_->events.pop_front();
        impl_->events.push_back(std::move(ev));
    }
}

void WorldSession::tick() {
    // Poll provider deltas and apply to world state (main.cpp lines ~507–508).
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
