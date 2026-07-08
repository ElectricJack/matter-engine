// Full public-API integration test: hidden GL window (app-owned), EngineContext,
// bake, event sequence, offscreen render, raycast. Run with GALLIUM_DRIVER=d3d12.
// Fixture: examples/primitive_demo / Primitives (smallest world, single Gallery root).
#include "matter/engine_context.h"
#include "raylib.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "api_tests");
    std::string err;
    matter::EngineDesc ed;
    ed.cache_root = "cache";   // run from MatterEngine3/viewer so the bake cache is warm
    auto engine = matter::EngineContext::create(ed, err);
    if (!engine) { printf("FAIL create: %s\n", err.c_str()); return 1; }

    matter::WorldDesc wd;
    wd.schemas_dir    = "../examples/primitive_demo/schemas";
    wd.world_data_dir = "../examples/primitive_demo/WorldData";
    wd.world_name     = "Primitives";
    wd.shared_lib_dir = "../shared-lib";
    auto session = engine->open_world(wd, err);
    if (!session) { printf("FAIL open_world: %s\n", err.c_str()); return 1; }

    session->request_bake();
    std::vector<matter::Event> evs;
    matter::Event ev;
    while (session->poll_event(ev)) evs.push_back(ev);
    if (evs.empty()) { printf("FAIL: no events\n"); return 1; }
    assert(!evs.empty());
    assert(evs.front().type == matter::EventType::BakeStarted);
    assert(evs.back().type == matter::EventType::BakeFinished);
    int part_done = 0;
    for (auto& e : evs) if (e.type == matter::EventType::BakePartDone) ++part_done;
    printf("events: %zu (%d PartDone)\n", evs.size(), part_done);

    uint32_t ic = session->instance_count();
    printf("instance_count: %u\n", ic);
    assert(ic > 0);
    matter::InstanceInfo info;
    assert(session->instance_info(0, info));
    printf("instance[0]: part_hash=%016llx module=%s\n",
           (unsigned long long)info.part_hash,
           info.module_name ? info.module_name : "(null)");

    // render into the hidden window's framebuffer; assert non-black output
    // Camera aims at the world area near instance 0's translation column.
    // Row-major: translate at [3],[7],[11].
    float tx = info.transform[3];
    float ty = info.transform[7];
    float tz = info.transform[11];
    Camera3D cam{};
    cam.position = (Vector3){ tx + 8.0f, ty + 6.0f, tz + 8.0f };
    cam.target   = (Vector3){ tx, ty, tz };
    cam.up       = (Vector3){0, 1, 0};
    cam.fovy     = 60.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    matter::RenderOptions opts;   // defaults: GpuDriven + SectorLod
    opts.resolver = matter::ResolverKind::PassThrough;
    for (int i = 0; i < 3; ++i) {
        BeginDrawing();
        session->render(cam, GetScreenWidth(), GetScreenHeight(), opts);
        EndDrawing();
    }
    Image img = LoadImageFromScreen();
    Color* px = LoadImageColors(img);
    long nonblack = 0, n = (long)img.width * img.height;
    for (long i = 0; i < n; ++i)
        if (px[i].r > 8 || px[i].g > 8 || px[i].b > 8) ++nonblack;
    printf("nonblack: %ld/%ld\n", nonblack, n);
    assert(nonblack > n / 20);
    UnloadImageColors(px);
    UnloadImage(img);

    // raycast straight down onto the world near instance 0.
    // Cast from (tx, ty+100, tz) downward — if the world geometry is at or near
    // ty=0 this will hit something.
    float origin[3] = { tx, ty + 100.0f, tz };
    float dir[3]    = { 0.0f, -1.0f, 0.0f };
    matter::RayHit hit;
    bool hit_ok = session->raycast(origin, dir, 1000.0f, hit);
    printf("raycast: hit=%d t=%.3f instance=%u\n",
           (int)hit_ok, hit.t, hit.instance);
    if (!hit_ok) {
        // Try from further offset in case geometry is displaced
        float origin2[3] = { tx + 0.5f, ty + 100.0f, tz + 0.5f };
        hit_ok = session->raycast(origin2, dir, 1000.0f, hit);
        printf("raycast (retry +0.5): hit=%d t=%.3f instance=%u\n",
               (int)hit_ok, hit.t, hit.instance);
    }
    assert(hit_ok && hit.t > 0.0f);

    session.reset();   // before CloseWindow
    engine.reset();
    CloseWindow();
    printf("api_tests: all passed\n");
    return 0;
}
