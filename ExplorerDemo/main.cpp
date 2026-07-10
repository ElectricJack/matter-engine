// ExplorerDemo — minimal standalone app that consumes only the MatterEngine3
// public API (matter/*.h) to let a user fly through the Meadow Valley world.
//
// Env vars:
//   EXPLORER_DATA_DIR=<path>
//       Root directory for world data. Expected layout:
//           <dir>/schemas/          — world schema .js files
//           <dir>/shared-lib/       — shared DSL library .js files
//           <dir>/worlds/Meadow/    — Meadow world data
//       Defaults to ./WorldData (packaged layout, relative to the exe's cwd).
//       Dev builds fall back to ../MatterEngine3/examples/world_demo/... if
//       ./WorldData does not exist and EXPLORER_DATA_DIR is unset.
//
//   EXPLORER_SMOKE="secs=<n>[,shot=<path>]"
//       Smoke-test mode: run for n seconds, optionally capture screenshot to
//       <path>, print "explorer: ready" on the first rendered frame after
//       BakeStarted, then exit 0.
//
// Run from the ExplorerDemo/ directory so that cache/ resolves correctly.

#include "raylib.h"
#include "raymath.h"

#include "matter/engine_context.h"
#include "matter/world_session.h"
#include "matter/events.h"

#include "camera_rig.h"
#include "hud.h"
#include "staged_camera.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#ifdef _WIN32
#  include <sys/stat.h>
#else
#  include <sys/stat.h>
#endif

// ---------------------------------------------------------------------------
// Data-directory resolution: EXPLORER_DATA_DIR override with dev-path fallback.
//
// Packaged layout (./WorldData/):
//   schemas/          — world schema .js files
//   shared-lib/       — shared DSL library .js files
//   worlds/Meadow/    — Meadow world data
//
// Dev layout (../MatterEngine3/...):
//   examples/world_demo/schemas/
//   examples/world_demo/WorldData/Meadow/
//   shared-lib/
// ---------------------------------------------------------------------------
static bool dir_exists(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

// WorldPaths holds the three directory arguments for matter::WorldDesc.
// world_data_dir is the PARENT directory containing the world subdirectory
// (engine appends "/" + world_name to find world.manifest etc.).
struct WorldPaths {
    std::string schemas_dir;
    std::string world_data_dir;   // parent of worlds; engine appends /Meadow
    std::string shared_lib_dir;
};

static WorldPaths resolve_world_paths() {
    // 1. Explicit override always wins.
    const char* env = getenv("EXPLORER_DATA_DIR");
    if (env && env[0] != '\0') {
        std::string base = env;
        // Packaged layout under the override dir:
        //   <base>/schemas/, <base>/worlds/<world_name>/, <base>/shared-lib/
        return {
            base + "/schemas",
            base + "/worlds",
            base + "/shared-lib"
        };
    }

    // 2. Packaged layout: ./WorldData exists in cwd (i.e. next to the exe).
    if (dir_exists("WorldData")) {
        return {
            "WorldData/schemas",
            "WorldData/worlds",
            "WorldData/shared-lib"
        };
    }

    // 3. Dev fallback: ../MatterEngine3/... layout (original hardcoded paths).
    return {
        "../MatterEngine3/examples/world_demo/schemas",
        "../MatterEngine3/examples/world_demo/WorldData",
        "../MatterEngine3/shared-lib"
    };
}

// ---------------------------------------------------------------------------
// Smoke-mode helpers: parse EXPLORER_SMOKE="secs=<n>[,shot=<path>]"
// ---------------------------------------------------------------------------
struct SmokeOpts {
    bool   active       = false;
    int    secs         = 0;
    char   shot[512]    = {};
    bool   ready_printed = false;
};

static SmokeOpts parse_smoke_env() {
    SmokeOpts opts{};
    const char* env = getenv("EXPLORER_SMOKE");
    if (!env) return opts;
    opts.active = true;
    // Parse "secs=<n>" and optional ",shot=<path>".
    const char* p = strstr(env, "secs=");
    if (p) opts.secs = atoi(p + 5);
    p = strstr(env, "shot=");
    if (p) {
        p += 5;
        int i = 0;
        while (*p && *p != ',' && i < (int)(sizeof(opts.shot) - 1))
            opts.shot[i++] = *p++;
        opts.shot[i] = '\0';
    }
    return opts;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    SmokeOpts smoke = parse_smoke_env();

    // --- Window init ---
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "ExplorerDemo — Meadow Valley");
    SetTargetFPS(60);

    // --- Engine setup ---
    matter::EngineDesc edesc;
    edesc.cache_root    = "cache";
    edesc.shader_dir    = nullptr;    // use embedded shaders
    edesc.allow_gl_lt_46 = false;

    std::string err;
    auto engine = matter::EngineContext::create(edesc, err);
    if (!engine) {
        fprintf(stderr, "FATAL: EngineContext::create failed: %s\n", err.c_str());
        CloseWindow();
        return 1;
    }

    // --- World session (Meadow Valley) ---
    // Data paths are resolved from EXPLORER_DATA_DIR, ./WorldData (packaged),
    // or the dev ../MatterEngine3/... layout (fallback when ./WorldData absent).
    WorldPaths wp = resolve_world_paths();
    matter::WorldDesc wd;
    wd.schemas_dir    = wp.schemas_dir.c_str();
    wd.world_data_dir = wp.world_data_dir.c_str();
    wd.world_name     = "Meadow";
    wd.shared_lib_dir = wp.shared_lib_dir.c_str();
    wd.enable_live_edit = false;

    auto session = engine->open_world(wd, err);
    if (!session) {
        fprintf(stderr, "FATAL: open_world failed: %s\n", err.c_str());
        CloseWindow();
        return 1;
    }

    // Kick off the first bake (demand-driven; camera focus guides tile order).
    session->request_bake();

    // --- Camera rig ---
    CameraRig rig;
    rig.init();

    // --- Loading HUD + staged camera (Task 9) ---
    Hud          hud;
    StagedCamera staged;

    // Resolver knobs for Meadow Valley (same as MatterViewer's Meadow defaults).
    matter::RenderOptions render_opts;
    render_opts.path             = matter::RenderPath::GpuDriven;
    render_opts.resolver         = matter::ResolverKind::SectorLod;
    render_opts.wireframe        = false;
    render_opts.hiz_occlusion    = false;
    render_opts.active_radius    = 400.0f;
    render_opts.min_projected_size = 0.0015f;
    render_opts.cull_backfaces   = true;

    bool bake_started = false;   // true once BakeStarted event is seen
    bool bake_done    = false;   // true once BakeFinished event is seen

    double t_start = GetTime();
    int    frames  = 0;

    // Screenshot capture: in smoke mode, take the shot 3s before the deadline
    // so that as much terrain as possible has assembled before the capture.
    // (BakeStarted fires within the first frame; taking a shot 3 frames after
    // BakeStarted always yields a black frame on a cold bake.)
    bool smoke_shot_written = false;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        float now = (float)GetTime();

        // --- Staged camera or user input ---
        // staged.update() returns true while it's still driving the rig.
        // If user has given input (rig.has_user_input()), staged camera exits.
        // In smoke mode the staged camera still runs (that's how screenshots verify it).
        bool staged_active = staged.update(rig, dt, bake_done);

        // Only call rig.update() when staged camera isn't active, OR always call
        // it to capture input state (but suppress movement when staged is active).
        // We always call it so input detection works; rig.update() moves the camera
        // only when not overridden by set_staged_pose().
        if (!staged_active) {
            rig.update(dt);
        } else {
            // Still poll input state so has_user_input() fires on first key press.
            // We pass dt=0 to suppress any actual movement; staged pose is set by
            // staged.update() above via set_staged_pose().
            rig.update(0.0f);
        }

        // --- Set bake focus to camera position every frame (before tick). ---
        {
            const float focus[3] = {
                rig.cam.position.x,
                rig.cam.position.y,
                rig.cam.position.z
            };
            session->set_bake_focus(focus);
        }

        // --- Tick world state ---
        session->tick();

        // --- Execute queued GL bake work (up to 4 ms per frame) ---
        session->pump_gpu_jobs(4.0f);

        // --- Drain events; feed HUD ---
        {
            matter::Event ev;
            while (session->poll_event(ev)) {
                // Feed every event to the HUD.
                hud.feed(ev);

                if (ev.type == matter::EventType::BakeStarted) {
                    bake_started = true;
                    printf("explorer: bake started\n");
                    fflush(stdout);
                } else if (ev.type == matter::EventType::BakePartDone) {
                    printf("bake %d/%d %s\n", ev.done, ev.total, ev.module.c_str());
                } else if (ev.type == matter::EventType::BakeFinished) {
                    bake_done = true;
                    staged.notify_bake_finished();
                    printf("bake finished (%d errors)\n", ev.errors);
                    fflush(stdout);
                } else if (ev.type == matter::EventType::BakeError) {
                    fprintf(stderr, "bake error [%s]: %s\n",
                            ev.module.c_str(), ev.message.c_str());
                } else if (ev.type == matter::EventType::RefineTileDone) {
                    // Refine events are frequent; log at low verbosity.
                    // printf("refine tile %d/%d  tx=%d tz=%d\n",
                    //        ev.done, ev.total, ev.tile_tx, ev.tile_tz);
                }
            }
        }

        // --- Render ---
        BeginDrawing();
            // session->render clears to the kernel sky color once connected,
            // but early-returns before that — clear here so the install/load
            // window isn't left with an uncleared framebuffer.
            ClearBackground((Color){ 96, 118, 143, 255 });
            session->render(rig.cam, GetScreenWidth(), GetScreenHeight(), render_opts);
            const matter::FrameStats& fs = session->frame_stats();
            // Task 9: draw HUD (replaces old draw_hud free function).
            hud.draw(GetScreenWidth(), GetScreenHeight(), fs, (float)GetFPS(), now);
        EndDrawing();

        ++frames;

        // --- Smoke-mode ready signal: first rendered frame after BakeStarted ---
        if (smoke.active && bake_started && !smoke.ready_printed) {
            printf("explorer: ready\n");
            fflush(stdout);
            smoke.ready_printed = true;
        }

        // --- Smoke-mode time limit + screenshot ---
        if (smoke.active) {
            double elapsed = GetTime() - t_start;
            // Capture screenshot 3 seconds before the deadline so the image shows
            // as much assembled terrain as possible (cold bake takes the full window).
            if (!smoke_shot_written && smoke.shot[0] != '\0' &&
                elapsed >= (double)(smoke.secs - 3)) {
                smoke_shot_written = true;
                Image screen = LoadImageFromScreen();
                if (ExportImage(screen, smoke.shot)) {
                    printf("explorer: screenshot written to %s\n", smoke.shot);
                } else {
                    printf("explorer: screenshot FAILED %s\n", smoke.shot);
                }
                UnloadImage(screen);
                fflush(stdout);
            }
            if ((int)elapsed >= smoke.secs) {
                printf("explorer: smoke done (%.1fs, %d frames)\n", elapsed, frames);
                fflush(stdout);
                break;
            }
        }
    }

    // Destroy session before closing the GL context.
    session.reset();
    engine.reset();

    CloseWindow();
    return 0;
}
