// MatterEngine3 world viewer: connect to a WorldProvider, reconcile a persistent
// part cache, compose with a selectable SectorResolver, raytrace or rasterize,
// ImGui HUD. Default: raster path (no ~60s shader warm-up). MATTER_RT=1 uses
// the full raytrace path.
#include "raylib.h"

#include "local_provider.h"
#include "part_store.h"
#include "world_composer.h"
#include "sector_resolver.h"
#include "renderer.h"
#include "raster_composer.h"
#include "probe_texture.h"
#include "ui.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

#include <fcntl.h>      // open, O_RDWR/O_NONBLOCK (live-command FIFO)
#include <sys/stat.h>   // mkfifo
#include <unistd.h>     // read, close, unlink

using namespace viewer;

int main() {
    const bool use_rt = getenv("MATTER_RT") != nullptr;

    const int W = 1280, H = 720;
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(W, H, "MatterEngine3 World Viewer");
    SetTargetFPS(60);

    Ui ui; ui.setup();

    Renderer renderer;
    renderer.init_camera();   // always: sets camera defaults used in both modes
    std::string err;
    if (use_rt) {
        if (!renderer.init_shader("shaders/raytrace_tlas_blas_processed.fs", err)) {
            printf("FATAL: %s\n", err.c_str());
            return 1;
        }
    }

    // MATTER_CAM="px,py,pz,tx,ty,tz" overrides the initial camera (eye + target),
    // so a headless screenshot can frame an arbitrarily sized scene without a
    // rebuild. Missing/garbage -> keep the renderer default.
    if (const char* cam_env = getenv("MATTER_CAM")) {
        float c[6];
        if (sscanf(cam_env, "%f,%f,%f,%f,%f,%f",
                   &c[0],&c[1],&c[2],&c[3],&c[4],&c[5]) == 6) {
            renderer.camera().position = (Vector3){ c[0], c[1], c[2] };
            renderer.camera().target   = (Vector3){ c[3], c[4], c[5] };
            printf("MATTER_CAM: eye(%.1f,%.1f,%.1f) target(%.1f,%.1f,%.1f)\n",
                   c[0],c[1],c[2],c[3],c[4],c[5]);
        }
    }

    LocalProviderConfig cfg;
    cfg.schemas_dir    = "../examples/world_demo/schemas";
    cfg.world_data_dir = "../examples/world_demo/WorldData";
    cfg.world_name     = "Demo";
    cfg.shared_lib_dir = "../shared-lib";
    cfg.cache_root     = "cache";   // persistent: viewer/cache/parts/<hash>.part

    // MATTER_WORLD=primitives switches to the primitive-demo gallery (every DSL
    // op) instead of the default tree scene; local_provider scatters it by name.
    const char* world_env = getenv("MATTER_WORLD");
    if (world_env && std::string(world_env) == "primitives") {
        cfg.schemas_dir    = "../examples/primitive_demo/schemas";
        cfg.world_data_dir = "../examples/primitive_demo/WorldData";
        cfg.world_name     = "Primitives";
    }
    // MATTER_WORLD=meadow loads the dense meadow benchmark world (same
    // world_demo schemas; the Meadow manifest root carries the expand flag).
    const bool meadow = world_env && std::string(world_env) == "meadow";
    if (meadow) cfg.world_name = "Meadow";

    auto provider = std::make_unique<LocalProvider>(cfg);

    // --- Connect sequence (reusable for the reload button). ---
    ViewerStats stats{};
    WorldManifest manifest;
    WorldState state;
    std::unique_ptr<PartStore> store;
    std::unique_ptr<WorldComposer> composer;
    std::unique_ptr<RasterComposer> raster;
    lod_select::PartLodTable lods;
    ProbeTextures probe_tex{};   // current probe 3D textures; released on reload/shutdown
    Color sky_clear = (Color){ 96, 118, 143, 255 };   // updated from tone-mapped sky_color

    auto connect_sequence = [&]() -> bool {
        if (!provider->connect(manifest, err)) { printf("connect: %s\n", err.c_str()); return false; }
        store = std::make_unique<PartStore>(cfg.cache_root);
        auto want = provider->reconcile(manifest, *store);
        if (!provider->fetch_parts(want, *store, err)) { printf("fetch: %s\n", err.c_str()); return false; }
        state.reset(manifest);
        // Size the TLAS to the fully child-expanded instance count, not just the
        // root count: each placed part recursively pulls in its baked children
        // (e.g. a Tree expands to hundreds of Leaf instances). Mirrors the depth
        // cap and empty-LOD skip in WorldComposer::compose.
        std::function<size_t(uint64_t, int)> expanded_count =
            [&](uint64_t h, int depth) -> size_t {
                if (depth > 8) return 0;
                const LoadedPart* lp = store->get_or_load(h);
                if (!lp) return 0;
                // A geometry-less assembly part contributes no instance of its own
                // but still expands its children -- mirror WorldComposer::compose.
                size_t n = lp->lod_blas.empty() ? 0 : 1;
                for (const auto& c : lp->children)
                    n += expanded_count(c.child_resolved_hash, depth + 1);
                return n;
            };
        size_t cap = 16;
        for (const auto& e : manifest.instances) cap += expanded_count(e.part_hash, 0);
        composer = std::make_unique<WorldComposer>(*store, cap);
        // Recreate RasterComposer on each (re)connect so stale GL mesh caches drop.
        // Release stale probe textures before creating new ones (GL context must be live).
        release_probe_textures(probe_tex);
        raster = std::make_unique<RasterComposer>();
        if (!use_rt) {
            std::string rerr;
            if (!raster->init(rerr)) {
                printf("raster: %s\n", rerr.c_str());
                return false;
            }
            // Always upload WorldLights (defaults reproduce Phase-1 look for worlds
            // without light lines).
            raster->set_lights(manifest.lights);

            // Upload probe textures if available; fallback to Phase-1 flat ambient.
            if (manifest.probes && manifest.probes->valid()) {
                probe_tex = upload_probe_textures(*manifest.probes);
                raster->set_probes(probe_tex);
            } else {
                printf("probes unavailable - flat ambient fallback\n");
            }

            // Tone-map sky_color (c/(c+1) then pow(1/2.2)) to derive the clear color.
            // With the default lights this reproduces approximately the old (96,118,143).
            auto tonemap = [](float c) -> unsigned char {
                float mapped = c / (c + 1.0f);                    // Reinhard
                float gamma  = std::pow(mapped, 1.0f / 2.2f);     // gamma
                float clamped = gamma < 0.0f ? 0.0f : (gamma > 1.0f ? 1.0f : gamma);
                return (unsigned char)(clamped * 255.0f + 0.5f);
            };
            sky_clear = (Color){
                tonemap(manifest.lights.sky_color[0]),
                tonemap(manifest.lights.sky_color[1]),
                tonemap(manifest.lights.sky_color[2]),
                255
            };
            printf("sky clear color: (%d,%d,%d)\n",
                   (int)sky_clear.r, (int)sky_clear.g, (int)sky_clear.b);
        }
        // Upload world lights to the raytrace shader (no-op in raster mode because
        // the shader is not loaded; the raster path uses raster->set_lights above).
        renderer.set_lights(manifest.lights);
        lods = store->part_lod_table();
        stats.connected        = true;
        stats.parts_baked      = provider->baked_count();
        stats.cache_hits       = provider->hit_count();
        stats.last_want_count  = (int)want.size();
        stats.instances_total  = (int)manifest.instances.size();
        // Probe grid dims for HUD (all-zero = probes unavailable/OFF)
        if (manifest.probes && manifest.probes->valid()) {
            stats.probe_dims[0] = manifest.probes->grid.nx;
            stats.probe_dims[1] = manifest.probes->grid.ny;
            stats.probe_dims[2] = manifest.probes->grid.nz;
        } else {
            stats.probe_dims[0] = stats.probe_dims[1] = stats.probe_dims[2] = 0;
        }
        return true;
    };
    if (!connect_sequence()) return 1;

    PassThroughResolver pass;
    // Per-world resolver config: the Meadow spans ~256x256 units, so activate
    // sectors across the whole world and floor-cull sub-pixel parts (grass/
    // pebbles self-cull at distance; their epsilon ladders stop well above 1 px).
    const float kActiveRadius     = meadow ? 400.0f : 64.0f;
    const float kMinProjectedSize = meadow ? 0.0015f : 0.0f;   // ~1 px at 720p (fov/height)
    SectorLodResolver sec(16.0f, kActiveRadius);
    sec.set_min_projected_size(kMinProjectedSize);
    if (meadow) stats.resolver_choice = 1;   // SectorLod by default for the benchmark

    // RT mode only: populate the TLAS and warm up the raytrace shader so the GPU
    // compile stall happens here (with startup logging) instead of on the first
    // real frame. Raster mode skips this entirely — no ~60s warm-up.
    if (use_rt) {
        Vector3 cp0 = renderer.camera().position;
        composer->compose(state, pass, lods, make_float3(cp0.x, cp0.y, cp0.z));
        renderer.warm_up(store->blas(), composer->tlas());
    }

    bool camera_capture = false;

    // Headless capture: if MATTER_SCREENSHOT is set, render a few frames so the
    // raytrace output is stable, dump a PNG to that path, then exit cleanly.
    const char* screenshot_path = getenv("MATTER_SCREENSHOT");
    int frames_drawn = 0;

    // --- Live command FIFO (optional) ---------------------------------------
    // MATTER_CMD_FIFO names a named pipe; when set, the viewer stays alive and
    // executes one-line commands so an external driver can move the camera,
    // capture screenshots, and hot-reload schemas WITHOUT paying the (~60s)
    // raytrace shader warm-up again. Commands (one per line):
    //   cam <px> <py> <pz> <tx> <ty> <tz>   move eye + look-at target
    //   shot <path>                          capture a PNG once the image settles
    //   reload                               re-bake changed schemas + repopulate
    //   quit                                 exit cleanly
    // After each shot the viewer touches "<path>.done" so the driver can poll for
    // a fully-written file instead of racing the PNG encode.
    int cmd_fd = -1;
    std::string cmd_buf;
    const char* fifo_path = getenv("MATTER_CMD_FIFO");
#ifndef _WIN32
    if (fifo_path) {
        mkfifo(fifo_path, 0600);   // harmless if it already exists
        // O_RDWR holds a writer fd open on our side so reads never see EOF
        // between separate external writers; O_NONBLOCK keeps the loop running.
        cmd_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (cmd_fd < 0) printf("MATTER_CMD_FIFO: failed to open %s\n", fifo_path);
        else            printf("MATTER_CMD_FIFO: listening on %s\n", fifo_path);
    }
#else
    // mkfifo/O_NONBLOCK are POSIX-only; the live-command FIFO is a Linux dev aid.
    if (fifo_path) printf("MATTER_CMD_FIFO not supported on Windows; ignoring\n");
#endif
    std::string shot_path;   // pending screenshot target
    std::string stats_label;   // pending `stats <label>` FIFO request
    int  shot_frames = 0;    // frames to let the image settle before capture
    bool quit_requested = false;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_TAB)) {
            camera_capture = !camera_capture;
            if (camera_capture) DisableCursor(); else EnableCursor();
        }
        if (camera_capture) renderer.update_camera_free();

        if (cmd_fd >= 0) {
            char rb[512];
            ssize_t n;
            while ((n = read(cmd_fd, rb, sizeof rb)) > 0) cmd_buf.append(rb, (size_t)n);
            size_t nl;
            while ((nl = cmd_buf.find('\n')) != std::string::npos) {
                std::string line = cmd_buf.substr(0, nl);
                cmd_buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                float c[6];
                char pathbuf[256];
                char labelbuf[64];
                if (sscanf(line.c_str(), "cam %f %f %f %f %f %f",
                           &c[0],&c[1],&c[2],&c[3],&c[4],&c[5]) == 6) {
                    renderer.camera().position = (Vector3){ c[0], c[1], c[2] };
                    renderer.camera().target   = (Vector3){ c[3], c[4], c[5] };
                } else if (sscanf(line.c_str(), "shot %255s", pathbuf) == 1) {
                    shot_path = pathbuf;
                    shot_frames = 4;   // RT-derived settle count; applied harmlessly in raster mode too
                } else if (sscanf(line.c_str(), "stats %63s", labelbuf) == 1) {
                    stats_label = labelbuf;   // printed after this frame's stats fill
                } else if (sscanf(line.c_str(), "budget %f", &c[0]) == 1) {
                    // Clamp to [0.05, 4.0]: wider than the HUD slider for scripted sweeps,
                    // but guards against 0/negative values that break LOD selection math.
                    if (c[0] < 0.05f) c[0] = 0.05f;
                    if (c[0] > 4.0f)  c[0] = 4.0f;
                    stats.pixel_budget = c[0];
                } else if (line == "reload") {
                    stats.reload_requested = true;
                } else if (line == "quit") {
                    quit_requested = true;
                } else {
                    printf("cmd: unrecognized '%s'\n", line.c_str());
                }
            }
        }

        Vector3 cp = renderer.camera().position;
        float3 cam = make_float3(cp.x, cp.y, cp.z);

        SectorResolver& resolver =
            (stats.resolver_choice == 1) ? (SectorResolver&)sec : (SectorResolver&)pass;

        sec.set_pixel_budget(stats.pixel_budget);
        raster->set_pixel_budget(stats.pixel_budget);

        int active = 0;
        std::vector<RasterBatch> batches;
        if (use_rt) {
            active = composer->compose(state, resolver, lods, cam);
        } else {
            auto t0 = std::chrono::steady_clock::now();
            auto resolved = resolver.resolve(state, lods, cam);
            auto t1 = std::chrono::steady_clock::now();
            batches = raster->build_batches(resolved, *store, renderer.camera(), state.version());
            auto t2 = std::chrono::steady_clock::now();
            stats.resolve_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
            stats.build_ms   = std::chrono::duration<float, std::milli>(t2 - t1).count();
            for (const auto& b : batches) active += (int)b.transforms.size();
        }

        stats.fps = (float)GetFPS();
        stats.frame_ms = GetFrameTime() * 1000.0f;
        stats.cam_pos[0] = cp.x; stats.cam_pos[1] = cp.y; stats.cam_pos[2] = cp.z;
        stats.instances_active = active;

        BeginDrawing();
            ClearBackground(sky_clear);
            if (use_rt) {
                renderer.draw(store->blas(), composer->tlas());
            } else {
                auto d0 = std::chrono::steady_clock::now();
                stats.raster_tris     = raster->draw(batches, *store, renderer.camera());
                stats.draw_ms = std::chrono::duration<float, std::milli>(
                                    std::chrono::steady_clock::now() - d0).count();
                stats.raster_batches  = (int)raster->batches();
                stats.culled_clusters = (int)raster->culled_clusters();
                stats.batch_cache_hit = raster->cache_hit();
            }
            ui.begin_frame();
            ui.draw_debug_panel(stats);
            ui.draw_camera_panel(renderer.camera());
            ui.end_frame();
        EndDrawing();

        if (!stats_label.empty()) {
            printf("STATS,%s,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d\n",
                   stats_label.c_str(), stats.frame_ms,
                   stats.resolve_ms, stats.build_ms, stats.draw_ms,
                   stats.instances_active, stats.raster_batches,
                   stats.raster_tris, stats.culled_clusters);
            fflush(stdout);
            stats_label.clear();
        }

        if (screenshot_path) {
            // Settle count of 3 is RT-derived (waits for raytrace to stabilize);
            // harmless in raster mode where the image is stable on frame 1.
            if (++frames_drawn >= 3) {
                TakeScreenshot(screenshot_path);
                printf("screenshot written to %s\n", screenshot_path);
                break;
            }
        }

        // FIFO-driven capture: shoot once the image has settled, then touch a
        // "<path>.done" marker so the driver polls a complete file, not a
        // half-encoded PNG.
        if (shot_frames > 0 && --shot_frames == 0) {
            TakeScreenshot(shot_path.c_str());
            std::string done = shot_path + ".done";
            if (FILE* f = fopen(done.c_str(), "w")) fclose(f);
            printf("shot %s\n", shot_path.c_str());
        }

        WorldDelta d;
        if (provider->poll_deltas(d)) state.apply(d);

        if (stats.reload_requested) {
            stats.reload_requested = false;
            // Re-enable the cursor before reload so a failure can't strand it.
            if (camera_capture) { camera_capture = false; EnableCursor(); }
            // connect_sequence replaces `store`/`composer`; if it fails partway the
            // old composer would dangle, so bail the loop and shut down cleanly.
            if (!connect_sequence()) { printf("reload failed; exiting\n"); break; }
        }

        if (quit_requested) break;
    }

    if (cmd_fd >= 0) { close(cmd_fd); if (fifo_path) unlink(fifo_path); }
    if (camera_capture) EnableCursor();
    // Reset GL-owning objects before CloseWindow; order matters — all
    // UnloadMesh/UnloadShader calls must complete while the GL context is live.
    release_probe_textures(probe_tex);
    raster.reset();
    composer.reset();
    store.reset();
    ui.shutdown();
    renderer.shutdown();
    CloseWindow();
    return 0;
}
