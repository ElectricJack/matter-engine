// MatterEngine3 world viewer: connect to a WorldProvider, reconcile a persistent
// part cache, compose with a selectable SectorResolver, raytrace, ImGui HUD.
#include "raylib.h"

#include "local_provider.h"
#include "part_store.h"
#include "world_composer.h"
#include "sector_resolver.h"
#include "renderer.h"
#include "ui.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace viewer;

int main() {
    const int W = 1280, H = 720;
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(W, H, "MatterEngine3 World Viewer");
    SetTargetFPS(60);

    Ui ui; ui.setup();

    Renderer renderer;
    std::string err;
    if (!renderer.init("shaders/raytrace_tlas_blas_processed.fs", err)) {
        printf("FATAL: %s\n", err.c_str());
        return 1;
    }

    LocalProviderConfig cfg;
    cfg.schemas_dir    = "../examples/world_demo/schemas";
    cfg.world_data_dir = "../examples/world_demo/WorldData";
    cfg.world_name     = "Demo";
    cfg.shared_lib_dir = "../shared-lib";
    cfg.cache_root     = "cache";   // persistent: viewer/cache/parts/<hash>.part

    auto provider = std::make_unique<LocalProvider>(cfg);

    // --- Connect sequence (reusable for the reload button). ---
    ViewerStats stats{};
    WorldManifest manifest;
    WorldState state;
    std::unique_ptr<PartStore> store;
    std::unique_ptr<WorldComposer> composer;
    lod_select::PartLodTable lods;

    auto connect_sequence = [&]() -> bool {
        if (!provider->connect(manifest, err)) { printf("connect: %s\n", err.c_str()); return false; }
        store = std::make_unique<PartStore>(cfg.cache_root);
        auto want = provider->reconcile(manifest, *store);
        if (!provider->fetch_parts(want, *store, err)) { printf("fetch: %s\n", err.c_str()); return false; }
        state.reset(manifest);
        composer = std::make_unique<WorldComposer>(*store, manifest.instances.size() + 16);
        lods = store->part_lod_table();
        stats.connected        = true;
        stats.parts_baked      = provider->baked_count();
        stats.cache_hits       = provider->hit_count();
        stats.last_want_count  = (int)want.size();
        stats.instances_total  = (int)manifest.instances.size();
        return true;
    };
    if (!connect_sequence()) return 1;

    PassThroughResolver pass;
    SectorLodResolver   sec(16.0f, 64.0f);

    // Populate the TLAS once, then warm up the raytrace shader so the GPU compile
    // stall happens here (with startup logging) instead of on the first real frame.
    {
        Vector3 cp0 = renderer.camera().position;
        composer->compose(state, pass, lods, make_float3(cp0.x, cp0.y, cp0.z));
        renderer.warm_up(store->blas(), composer->tlas());
    }

    bool camera_capture = false;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_TAB)) {
            camera_capture = !camera_capture;
            if (camera_capture) DisableCursor(); else EnableCursor();
        }
        if (camera_capture) renderer.update_camera_free();

        Vector3 cp = renderer.camera().position;
        float3 cam = make_float3(cp.x, cp.y, cp.z);

        SectorResolver& resolver =
            (stats.resolver_choice == 1) ? (SectorResolver&)sec : (SectorResolver&)pass;
        int active = composer->compose(state, resolver, lods, cam);

        stats.fps = (float)GetFPS();
        stats.frame_ms = GetFrameTime() * 1000.0f;
        stats.cam_pos[0] = cp.x; stats.cam_pos[1] = cp.y; stats.cam_pos[2] = cp.z;
        stats.instances_active = active;

        BeginDrawing();
            ClearBackground(BLACK);
            renderer.draw(store->blas(), composer->tlas());
            ui.begin_frame();
            ui.draw_debug_panel(stats);
            ui.end_frame();
        EndDrawing();

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
    }

    if (camera_capture) EnableCursor();
    ui.shutdown();
    renderer.shutdown();
    CloseWindow();
    return 0;
}
