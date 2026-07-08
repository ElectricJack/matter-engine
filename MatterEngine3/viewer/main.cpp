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
#include "gl46.h"
#include "gpu_culler.h"
#include "raster_cull.h"

#include <algorithm>   // std::transform
#include <cctype>      // std::tolower
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>     // strcmp (FIFO `hiz on|off`)
#include <functional>
#include <memory>
#include <string>

#include <fcntl.h>      // open, O_RDWR/O_NONBLOCK (live-command FIFO)
#include <sys/stat.h>   // mkfifo
#include <unistd.h>     // read, close, unlink

using namespace viewer;

static void apply_world_resolver_defaults(const std::string& world_name,
                                          SectorLodResolver& sec,
                                          ViewerStats& stats) {
    // Per-world resolver knobs: the Meadow spans ~256x256 units and needs a
    // wider active radius plus sub-pixel culling; every other world uses the
    // tight defaults.
    if (world_name == "Meadow") {
        sec.set_active_radius(400.0f);
        sec.set_min_projected_size(0.0015f);   // ~1 px at 720p (fov/height)
        stats.resolver_choice = 1;             // SectorLod by default
    } else {
        sec.set_active_radius(64.0f);
        sec.set_min_projected_size(0.0f);
        stats.resolver_choice = 0;             // PassThrough by default
    }
}

int main() {
    const bool use_rt = getenv("MATTER_RT") != nullptr;

    const int W = 1280, H = 720;
    // GL 4.6 is a hard requirement for the raster path (MATTER_RT=1 is the
    // ray-traced fallback for older GL). MSAA is incompatible with the HiZ
    // occlusion path: build_hiz blits the default framebuffer depth, which is
    // undefined for a multisampled FB. The raster path therefore runs without
    // the MSAA hint. gpu_cull_requested() is a pure env read, safe before
    // InitWindow — it defaults ON and is only disabled by MATTER_GPU_CULL=0
    // (typically paired with MATTER_RT=1 on GL < 4.6 hardware).
    unsigned cfg_flags = FLAG_WINDOW_RESIZABLE;
    if (use_rt || !viewer::gpu_cull_requested()) cfg_flags |= FLAG_MSAA_4X_HINT;
    SetConfigFlags(cfg_flags);
    InitWindow(W, H, "MatterEngine3 World Viewer");
    SetTargetFPS(60);

    Ui ui; ui.setup();

    auto worlds = scan_worlds("../examples");
    printf("worlds available (%d):\n", (int)worlds.size());
    for (size_t i = 0; i < worlds.size(); ++i) {
        printf("  [%zu] %s  (%s / %s)\n",
               i, worlds[i].label.c_str(),
               worlds[i].schemas_dir.c_str(), worlds[i].world_data_dir.c_str());
    }
    if (worlds.empty()) {
        printf("FATAL: no worlds found under ../examples\n");
        return 1;
    }

    Renderer renderer;
    renderer.init_camera();   // always: sets camera defaults used in both modes
    std::string err;
    if (use_rt) {
        if (!renderer.init_shader("shaders/raytrace_tlas_blas_processed.fs", err)) {
            printf("FATAL: %s\n", err.c_str());
            return 1;
        }
    }

    // Raster path is GPU-driven only. RT path bypasses the GL 4.6 check.
    bool gpu_cull = false;
    if (!use_rt) {
        if (!viewer::gpu_cull_requested()) {
            fprintf(stderr, "FATAL: MATTER_GPU_CULL=0 requires MATTER_RT=1 "
                    "(the CPU raster path has been removed). Unset MATTER_GPU_CULL "
                    "for the default GPU-driven raster path, or set MATTER_RT=1 "
                    "to fall back to the software ray tracer.\n");
            return 1;
        }
        std::string why;
        if (!viewer::gl46_available(why)) {
            fprintf(stderr, "FATAL: GL 4.6 required for raster path (%s). "
                    "Set MATTER_GPU_CULL=0 with MATTER_RT=1 for the ray-traced fallback.\n",
                    why.c_str());
            return 1;
        }
        gpu_cull = true;
        printf("GPU cull path: enabled (GL 4.6 ok)\n");
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

    // Pick the initial world from the scanned list. MATTER_WORLD (case-
    // insensitive match against world_name) overrides the default; unknown
    // value falls through to index 0.
    int initial_world = 0;
    if (const char* world_env = getenv("MATTER_WORLD")) {
        std::string want = world_env;
        std::transform(want.begin(), want.end(), want.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        for (size_t i = 0; i < worlds.size(); ++i) {
            std::string have = worlds[i].world_name;
            std::transform(have.begin(), have.end(), have.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (have == want) { initial_world = (int)i; break; }
        }
    }

    LocalProviderConfig cfg;
    cfg.schemas_dir    = worlds[initial_world].schemas_dir;
    cfg.world_data_dir = worlds[initial_world].world_data_dir;
    cfg.world_name     = worlds[initial_world].world_name;
    cfg.shared_lib_dir = "../shared-lib";
    cfg.cache_root     = "cache";   // persistent: viewer/cache/parts/<hash>.part

    auto provider = std::make_unique<LocalProvider>(cfg);

    // --- Connect sequence (reusable for the reload button). ---
    ViewerStats stats{};
    stats.world_current = initial_world;
    // MATTER_HIZ=0|1 overrides the HiZ occlusion default (off) at startup, so
    // A/B runs are scriptable without the FIFO. Runtime toggles: HUD checkbox
    // + FIFO `hiz on|off`. Only meaningful when the GPU cull path is active.
    if (const char* hz = getenv("MATTER_HIZ")) stats.hiz_enabled = (hz[0] != '0');
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
        // GPU-driven shader: load after init() so the raster shader is ready.
        // FATAL on failure — there is no CPU raster fallback (MATTER_RT=1 is
        // the escape hatch for older GL).
        if (!use_rt) {
            std::string gerr;
            if (!raster->init_gpu_driven(gerr)) {
                fprintf(stderr, "FATAL: GPU-driven shader init failed: %s. "
                        "Set MATTER_RT=1 to fall back to the ray-traced path.\n",
                        gerr.c_str());
                return false;
            }
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

    // GPU culler: constructed and initialized ONLY when the GL 4.6 gate passed.
    // Must be initialized after InitWindow (GL context live) and after connect_sequence
    // (GL state settled). FATAL on init failure per task spec.
    GpuCuller gpu_culler;
    if (gpu_cull) {
        std::string cull_err;
        if (!gpu_culler.init(cull_err)) {
            fprintf(stderr, "FATAL: GpuCuller::init failed: %s\n", cull_err.c_str());
            return 1;
        }
        printf("GpuCuller: initialized\n");
        stats.gpu_cull_active = true;
    }

    PassThroughResolver pass;
    // Constructor radius is overwritten immediately by apply_world_resolver_defaults;
    // the placeholder 64.0f keeps the resolver in a valid state before the first call.
    SectorLodResolver sec(16.0f, 64.0f);
    apply_world_resolver_defaults(cfg.world_name, sec, stats);

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
        if (IsKeyPressed(KEY_F9) && raster) {
            raster->set_wireframe(!raster->wireframe());
            printf("wireframe %s\n", raster->wireframe() ? "on" : "off");
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
                } else if (sscanf(line.c_str(), "hiz %15s", labelbuf) == 1) {
                    stats.hiz_enabled = (strcmp(labelbuf, "on") == 0);
                    printf("hiz %s\n", stats.hiz_enabled ? "on" : "off");
                } else if (sscanf(line.c_str(), "wireframe %15s", labelbuf) == 1) {
                    bool w = (strcmp(labelbuf, "on") == 0) ||
                             (strcmp(labelbuf, "toggle") == 0 && raster && !raster->wireframe());
                    if (raster) raster->set_wireframe(w);
                    printf("wireframe %s\n", w ? "on" : "off");
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
        if (use_rt) {
            active = composer->compose(state, resolver, lods, cam);
        } else {
            auto t0 = std::chrono::steady_clock::now();
            auto resolved = resolver.resolve(state, lods, cam);
            auto t1 = std::chrono::steady_clock::now();
            // Raster path is GPU-driven only (guaranteed by the startup FATAL gate).
            float eye[3]     = {cp.x, cp.y, cp.z};
            Vector3 tgt      = renderer.camera().target;
            float target3[3] = {tgt.x, tgt.y, tgt.z};
            float up3[3]     = {0, 1, 0};
            float aspect     = (float)GetScreenWidth() / (float)GetScreenHeight();
            // Build view/proj/vp explicitly (same near/far as
            // camera_frustum_planes_raw) so the HiZ path gets the exact vp
            // the frustum planes came from.
            const float near_z = 0.05f, far_z = 4000.0f;
            float view[16], proj[16], vp[16];
            viewer::make_lookat(eye, target3, up3, view);
            viewer::make_perspective(renderer.camera().fovy, aspect, near_z, far_z, proj);
            viewer::mul16(view, proj, vp);
            float planes[6][4];
            viewer::extract_frustum_planes(vp, planes);
            // Propagate the runtime HiZ toggle every frame (HUD/FIFO/env).
            gpu_culler.set_hiz_enabled(stats.hiz_enabled);
            gpu_culler.cull(resolved, *store, eye, planes, vp, stats.pixel_budget);
            auto t2 = std::chrono::steady_clock::now();
            stats.resolve_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
            stats.build_ms   = std::chrono::duration<float, std::milli>(t2 - t1).count();
            active = (int)resolved.size();   // resolved count; emitted shown via gpu_emitted
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
                stats.raster_tris = raster->draw_gpu_driven(
                        gpu_culler, *store, renderer.camera());
                stats.gpu_emitted    = (int)gpu_culler.emitted();
                stats.gpu_culled     = (int)gpu_culler.culled_clusters();
                stats.gpu_culled_hiz = (int)gpu_culler.culled_hiz();
                stats.raster_batches  = 0;   // not meaningful for indirect path
                stats.culled_clusters = stats.gpu_culled;
                stats.batch_cache_hit = false;
                stats.draw_ms = std::chrono::duration<float, std::milli>(
                                    std::chrono::steady_clock::now() - d0).count();
            }
            // Build HiZ depth max-pyramid for next-frame occlusion culling.
            // No-op when the HiZ toggle is off (set_hiz_enabled above).
            if (gpu_cull) gpu_culler.build_hiz(GetScreenWidth(), GetScreenHeight());
            ui.begin_frame();
            ui.draw_debug_panel(stats);
            ui.draw_worlds_panel(worlds, stats);
            ui.draw_camera_panel(renderer.camera());
            ui.end_frame();
        EndDrawing();

        if (!stats_label.empty()) {
            // Append-only format (scripts parse by position): the trailing
            // field is the HiZ-occlusion-culled cluster count (Task 10).
            printf("STATS,%s,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d\n",
                   stats_label.c_str(), stats.frame_ms,
                   stats.resolve_ms, stats.build_ms, stats.draw_ms,
                   stats.instances_active, stats.raster_batches,
                   stats.raster_tris, stats.culled_clusters,
                   stats.gpu_culled_hiz);
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
            // raylib's TakeScreenshot silently prepends GetWorkingDirectory()
            // to the path, so absolute paths land at `<cwd><path>` which is
            // nonsense. Do the screen-capture + PNG export ourselves so
            // absolute paths work verbatim.
            Image screen = LoadImageFromScreen();
            if (!ExportImage(screen, shot_path.c_str())) {
                printf("shot FAILED %s\n", shot_path.c_str());
            } else {
                printf("shot %s\n", shot_path.c_str());
            }
            UnloadImage(screen);
            std::string done = shot_path + ".done";
            if (FILE* f = fopen(done.c_str(), "w")) fclose(f);
        }

        WorldDelta d;
        if (provider->poll_deltas(d)) state.apply(d);

        if (stats.reload_requested) {
            stats.reload_requested = false;
            // Re-enable the cursor before reload so a failure can't strand it.
            if (camera_capture) { camera_capture = false; EnableCursor(); }
            // Reset GpuCuller so stale part slots from the old world are cleared.
            if (gpu_cull) gpu_culler.reset();
            // connect_sequence replaces `store`/`composer`; if it fails partway the
            // old composer would dangle, so bail the loop and shut down cleanly.
            if (!connect_sequence()) { printf("reload failed; exiting\n"); break; }
        }

        if (stats.world_switch_requested >= 0) {
            int idx = stats.world_switch_requested;
            stats.world_switch_requested = -1;
            if (idx < (int)worlds.size()) {
                const auto& w = worlds[idx];
                printf("world switch -> [%d] %s\n", idx, w.label.c_str());
                cfg.schemas_dir    = w.schemas_dir;
                cfg.world_data_dir = w.world_data_dir;
                cfg.world_name     = w.world_name;
                // LocalProvider takes cfg by value at construction — mutating cfg
                // alone doesn't reach the existing provider instance.
                provider = std::make_unique<LocalProvider>(cfg);
                // Re-enable the cursor before rebuilding so a failure can't strand it
                // (mirrors the reload path).
                if (camera_capture) { camera_capture = false; EnableCursor(); }
                // Reset GpuCuller so stale part slots from the old world are cleared.
                if (gpu_cull) gpu_culler.reset();
                if (!connect_sequence()) { printf("world switch failed; exiting\n"); break; }
                stats.world_current = idx;
                apply_world_resolver_defaults(cfg.world_name, sec, stats);
            }
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
