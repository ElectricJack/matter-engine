// MatterEngine3 world viewer — Stage 3: consumes only the matter:: public API.
// Engine interaction is fully delegated to matter::EngineContext / WorldSession;
// main.cpp retains window init, FIFO/screenshot plumbing, UI, and camera policy.
// MATTER_RT=1   — ray-traced path (no GL 4.6 required, ~60s shader warm-up)
// MATTER_CAM    — "px,py,pz,tx,ty,tz" initial camera override
// MATTER_WORLD  — case-insensitive world name override
// MATTER_HIZ    — "0"|"1" initial HiZ occlusion override
// MATTER_SCREENSHOT — path; render 3 frames then write PNG and exit
// MATTER_CMD_FIFO   — named pipe; commands: cam/shot/stats/budget/hiz/reload/quit/wireframe
#include "raylib.h"

#include "matter/engine_context.h"
#include "matter/world_session.h"
#include "ui.h"

#include <algorithm>   // std::transform
#include <cctype>      // std::tolower
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>     // strcmp (FIFO `hiz on|off`)
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>      // open, O_RDWR/O_NONBLOCK
#include <sys/stat.h>   // mkfifo
#include <unistd.h>     // read, close, unlink

// ---------------------------------------------------------------------------
// App-side camera policy (free-cam = raylib CAMERA_FREE)
// Copied verbatim from Renderer::init_camera + Renderer::update_camera_free.
// ---------------------------------------------------------------------------
static void init_camera(Camera3D& cam) {
    cam.position   = (Vector3){ 20.0f, 16.0f, 34.0f };
    cam.target     = (Vector3){ 0.0f, 9.0f, 0.0f };
    cam.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    cam.fovy       = 45.0f;
    cam.projection = CAMERA_PERSPECTIVE;
}

static void update_camera_free(Camera3D& cam) {
    UpdateCamera(&cam, CAMERA_FREE);
}

// ---------------------------------------------------------------------------
// Per-world resolver defaults (Meadow: wider radius + sub-pixel culling).
// Now writes into app-side floats rather than a SectorLodResolver reference.
// ---------------------------------------------------------------------------
static void apply_world_resolver_defaults(const std::string& world_name,
                                          float& active_radius,
                                          float& min_projected_size,
                                          viewer::ViewerStats& stats) {
    if (world_name == "Meadow") {
        active_radius      = 400.0f;
        min_projected_size = 0.0015f;
        stats.resolver_choice = 1;   // SectorLod by default
    } else {
        active_radius      = 64.0f;
        min_projected_size = 0.0f;
        stats.resolver_choice = 0;   // PassThrough by default
    }
}

// ---------------------------------------------------------------------------
// Inline gpu_cull_requested() — avoids pulling in gl46.h / gpu_culler.h.
// ---------------------------------------------------------------------------
static bool gpu_cull_requested() {
    const char* v = getenv("MATTER_GPU_CULL");
    return v == nullptr || v[0] != '0';
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
    if (use_rt || !gpu_cull_requested()) cfg_flags |= FLAG_MSAA_4X_HINT;
    SetConfigFlags(cfg_flags);
    InitWindow(W, H, "MatterEngine3 World Viewer");
    SetTargetFPS(60);

    viewer::Ui ui; ui.setup();

    auto worlds = viewer::scan_worlds("../MatterEngine3/examples");
    printf("worlds available (%d):\n", (int)worlds.size());
    for (size_t i = 0; i < worlds.size(); ++i) {
        printf("  [%zu] %s  (%s / %s)\n",
               i, worlds[i].label.c_str(),
               worlds[i].schemas_dir.c_str(), worlds[i].world_data_dir.c_str());
    }
    if (worlds.empty()) {
        printf("FATAL: no worlds found under ../MatterEngine3/examples\n");
        return 1;
    }

    // MATTER_GPU_CULL=0 requires MATTER_RT=1 (same FATAL gate as before).
    if (!use_rt && !gpu_cull_requested()) {
        fprintf(stderr, "FATAL: MATTER_GPU_CULL=0 requires MATTER_RT=1 "
                "(the CPU raster path has been removed). Unset MATTER_GPU_CULL "
                "for the default GPU-driven raster path, or set MATTER_RT=1 "
                "to fall back to the software ray tracer.\n");
        CloseWindow();
        return 1;
    }

    // --- Engine setup ---
    matter::EngineDesc edesc;
    edesc.cache_root    = "cache";
    edesc.allow_gl_lt_46 = use_rt;
    std::string err;
    auto engine = matter::EngineContext::create(edesc, err);
    if (!engine) { fprintf(stderr, "FATAL: %s\n", err.c_str()); CloseWindow(); return 1; }

    // Camera (app-owned; passed to session->render every frame).
    Camera3D camera{};
    init_camera(camera);

    // MATTER_CAM="px,py,pz,tx,ty,tz" overrides the initial camera.
    if (const char* cam_env = getenv("MATTER_CAM")) {
        float c[6];
        if (sscanf(cam_env, "%f,%f,%f,%f,%f,%f",
                   &c[0],&c[1],&c[2],&c[3],&c[4],&c[5]) == 6) {
            camera.position = (Vector3){ c[0], c[1], c[2] };
            camera.target   = (Vector3){ c[3], c[4], c[5] };
            printf("MATTER_CAM: eye(%.1f,%.1f,%.1f) target(%.1f,%.1f,%.1f)\n",
                   c[0],c[1],c[2],c[3],c[4],c[5]);
        }
    }

    // Pick initial world (MATTER_WORLD, case-insensitive).
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

    viewer::ViewerStats stats{};
    stats.world_current = initial_world;
    // MATTER_HIZ=0|1 overrides HiZ default at startup.
    if (const char* hz = getenv("MATTER_HIZ")) stats.hiz_enabled = (hz[0] != '0');

    // App-side resolver knobs (written by apply_world_resolver_defaults, read each frame).
    float active_radius      = 64.0f;
    float min_projected_size = 0.0f;

    // App-side wireframe toggle (F9 / FIFO); no-op in rendering today (no set_wireframe).
    bool wireframe = false;

    // --- open_and_bake helper (used for initial connect, reload, world switch) ---
    auto open_and_bake = [&](const viewer::WorldEntry& w) -> std::unique_ptr<matter::WorldSession> {
        matter::WorldDesc wd;
        wd.schemas_dir    = w.schemas_dir.c_str();
        wd.world_data_dir = w.world_data_dir.c_str();
        wd.world_name     = w.world_name.c_str();
        wd.shared_lib_dir = "../MatterEngine3/shared-lib";
        std::string werr;
        auto s = engine->open_world(wd, werr);
        if (!s) { printf("open_world: %s\n", werr.c_str()); return nullptr; }
        s->request_bake();
        matter::Event ev; bool ok = true;
        while (s->poll_event(ev)) {
            if (ev.type == matter::EventType::BakePartDone)
                printf("bake %d/%d %s\n", ev.done, ev.total, ev.module.c_str());
            if (ev.type == matter::EventType::BakeError) {
                printf("bake error: %s\n", ev.message.c_str()); ok = false;
            }
        }
        return ok ? std::move(s) : nullptr;
    };

    auto session = open_and_bake(worlds[initial_world]);
    if (!session) { ui.shutdown(); CloseWindow(); return 1; }

    stats.gpu_cull_active = !use_rt;
    apply_world_resolver_defaults(worlds[initial_world].world_name,
                                  active_radius, min_projected_size, stats);

    // Fill initial stats from the first bake.
    {
        const matter::FrameStats& fs = session->frame_stats();
        stats.parts_baked     = (int)fs.parts_baked;
        stats.cache_hits      = (int)fs.cache_hits;
        stats.instances_total = (int)fs.instances_total;
        memcpy(stats.probe_dims, fs.probe_dims, sizeof stats.probe_dims);
        stats.connected = true;
    }

    bool camera_capture = false;

    // Headless capture: if MATTER_SCREENSHOT is set, render a few frames, dump PNG, exit.
    const char* screenshot_path = getenv("MATTER_SCREENSHOT");
    int frames_drawn = 0;

    // --- Live command FIFO (optional) ---
    // MATTER_CMD_FIFO names a named pipe; commands: cam/shot/stats/budget/hiz/reload/quit/wireframe
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
    if (fifo_path) printf("MATTER_CMD_FIFO not supported on Windows; ignoring\n");
#endif
    std::string shot_path;     // pending screenshot target
    std::string stats_label;   // pending `stats <label>` FIFO request
    int  shot_frames    = 0;   // frames to let the image settle before capture
    bool quit_requested = false;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_TAB)) {
            camera_capture = !camera_capture;
            if (camera_capture) DisableCursor(); else EnableCursor();
        }
        if (IsKeyPressed(KEY_F9)) wireframe = !wireframe;
        if (camera_capture) update_camera_free(camera);

        // FIFO command pump.
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
                    camera.position = (Vector3){ c[0], c[1], c[2] };
                    camera.target   = (Vector3){ c[3], c[4], c[5] };
                } else if (sscanf(line.c_str(), "shot %255s", pathbuf) == 1) {
                    shot_path   = pathbuf;
                    shot_frames = 4;   // settle count
                } else if (sscanf(line.c_str(), "stats %63s", labelbuf) == 1) {
                    stats_label = labelbuf;
                } else if (sscanf(line.c_str(), "budget %f", &c[0]) == 1) {
                    if (c[0] < 0.05f) c[0] = 0.05f;
                    if (c[0] > 4.0f)  c[0] = 4.0f;
                    stats.pixel_budget = c[0];
                } else if (sscanf(line.c_str(), "hiz %15s", labelbuf) == 1) {
                    stats.hiz_enabled = (strcmp(labelbuf, "on") == 0);
                    printf("hiz %s\n", stats.hiz_enabled ? "on" : "off");
                } else if (line == "wireframe" || line == "wireframe toggle") {
                    wireframe = !wireframe;
                    printf("wireframe %s\n", wireframe ? "on" : "off");
                } else if (line == "reload") {
                    stats.reload_requested = true;
                } else if (line == "quit") {
                    quit_requested = true;
                } else {
                    printf("cmd: unrecognized '%s'\n", line.c_str());
                }
            }
        }

        // Tick world state (poll provider deltas).
        session->tick();

        // Build render options.
        matter::RenderOptions opts;
        opts.path     = use_rt ? matter::RenderPath::Raytrace : matter::RenderPath::GpuDriven;
        opts.resolver = stats.resolver_choice == 1 ? matter::ResolverKind::SectorLod
                                                   : matter::ResolverKind::PassThrough;
        opts.wireframe         = wireframe;
        opts.hiz_occlusion     = stats.hiz_enabled;
        opts.pixel_budget      = stats.pixel_budget;
        opts.active_radius     = active_radius;
        opts.min_projected_size = min_projected_size;

        stats.fps      = (float)GetFPS();
        stats.frame_ms = GetFrameTime() * 1000.0f;
        stats.cam_pos[0] = camera.position.x;
        stats.cam_pos[1] = camera.position.y;
        stats.cam_pos[2] = camera.position.z;

        BeginDrawing();
            session->render(camera, GetScreenWidth(), GetScreenHeight(), opts);
            const matter::FrameStats& fs = session->frame_stats();
            stats.resolve_ms      = fs.resolve_ms;
            stats.build_ms        = fs.build_ms;
            stats.draw_ms         = fs.draw_ms;
            stats.instances_active = (int)fs.instances_resolved;
            stats.gpu_emitted     = (int)fs.instances_drawn;
            stats.gpu_culled      = (int)fs.clusters_culled;
            stats.gpu_culled_hiz  = (int)fs.hiz_culled;
            stats.culled_clusters = stats.gpu_culled;
            stats.raster_tris     = (int)fs.triangles;
            stats.raster_batches  = 0;
            stats.batch_cache_hit = false;
            stats.instances_total = (int)fs.instances_total;
            stats.parts_baked     = (int)fs.parts_baked;
            stats.cache_hits      = (int)fs.cache_hits;
            stats.connected       = true;
            memcpy(stats.probe_dims, fs.probe_dims, sizeof stats.probe_dims);
            ui.begin_frame();
            ui.draw_debug_panel(stats);
            ui.draw_worlds_panel(worlds, stats);
            ui.draw_camera_panel(camera);
            ui.end_frame();
        EndDrawing();

        if (!stats_label.empty()) {
            // Append-only format (scripts parse by position): trailing field
            // is the HiZ-occlusion-culled cluster count (Task 10).
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
            if (++frames_drawn >= 3) {
                Image screen = LoadImageFromScreen();
                if (!ExportImage(screen, screenshot_path)) {
                    printf("screenshot FAILED %s\n", screenshot_path);
                } else {
                    printf("screenshot written to %s\n", screenshot_path);
                }
                UnloadImage(screen);
                break;
            }
        }

        // FIFO-driven capture: shoot once the image has settled.
        if (shot_frames > 0 && --shot_frames == 0) {
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

        if (stats.reload_requested) {
            stats.reload_requested = false;
            if (camera_capture) { camera_capture = false; EnableCursor(); }
            // Drain events from the old session before reload.
            session->reload();
            matter::Event ev;
            bool reload_ok = true;
            while (session->poll_event(ev)) {
                if (ev.type == matter::EventType::BakePartDone)
                    printf("bake %d/%d %s\n", ev.done, ev.total, ev.module.c_str());
                if (ev.type == matter::EventType::BakeError) {
                    printf("bake error: %s\n", ev.message.c_str());
                    reload_ok = false;
                }
            }
            if (!reload_ok) {
                // Fail-closed: keep running (session render() will no-op until
                // a later reload succeeds). Matches brief "on BakeError keep running".
                printf("reload failed; continuing\n");
            }
        }

        if (stats.world_switch_requested >= 0) {
            int idx = stats.world_switch_requested;
            stats.world_switch_requested = -1;
            if (idx < (int)worlds.size()) {
                const auto& w = worlds[idx];
                printf("world switch -> [%d] %s\n", idx, w.label.c_str());
                if (camera_capture) { camera_capture = false; EnableCursor(); }
                session.reset();
                session = open_and_bake(w);
                if (!session) { printf("world switch failed; exiting\n"); break; }
                stats.world_current = idx;
                apply_world_resolver_defaults(w.world_name, active_radius,
                                              min_projected_size, stats);
                {
                    const matter::FrameStats& fs = session->frame_stats();
                    stats.parts_baked     = (int)fs.parts_baked;
                    stats.cache_hits      = (int)fs.cache_hits;
                    stats.instances_total = (int)fs.instances_total;
                    memcpy(stats.probe_dims, fs.probe_dims, sizeof stats.probe_dims);
                    stats.connected = true;
                }
            }
        }

        if (quit_requested) break;
    }

    if (cmd_fd >= 0) { close(cmd_fd); if (fifo_path) unlink(fifo_path); }
    if (camera_capture) EnableCursor();
    // Session destructor releases GL; must precede CloseWindow.
    session.reset();
    ui.shutdown();
    CloseWindow();
    return 0;
}
