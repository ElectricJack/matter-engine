// Full public-API integration test: hidden GL window (app-owned), EngineContext,
// bake, event sequence, offscreen render, raycast. Run with GALLIUM_DRIVER=d3d12.
// Fixture: projects/primitive_demo / Primitives (smallest world, single Gallery root).
#include "matter/engine_context.h"
#include "bake_trace.h"
#include "bake_trace_names.h"
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
    ed.cache_root = "cache";   // run from MatterEditor/ so the bake cache is warm
    auto engine = matter::EngineContext::create(ed, err);
    if (!engine) { printf("FAIL create: %s\n", err.c_str()); return 1; }

    matter::WorldDesc wd;
    wd.project_dir           = "../projects/primitive_demo";
    wd.world_name            = "Primitives";
    wd.engine_shared_lib_dir = "../MatterEngine3/shared-lib";
    auto session = engine->open_world(wd, err);
    if (!session) { printf("FAIL open_world: %s\n", err.c_str()); return 1; }

    // Bake Lab (task 1.2): remove the resolve cache so this bake runs the full
    // install+compose+publish path. A warm-cache bake legitimately takes the
    // fast path (publish span only), which would make the trace check below
    // nondeterministic across runs.
    std::remove("cache/cache/Primitives.resolve");

    session->request_bake();
    // Phase B (Task 6): bake now runs on a worker thread and marshals GL work
    // back to this (app/GL) thread via pump_gpu_jobs. Drive the event stream
    // to BakeFinished/BakeError or a 60-second timeout.
    std::vector<matter::Event> evs;
    {
        double t0 = GetTime();
        bool finished = false;
        while (!finished && GetTime() - t0 < 60.0) {
            session->pump_gpu_jobs(4.0f);
            matter::Event ev;
            while (session->poll_event(ev)) {
                evs.push_back(ev);
                if (ev.type == matter::EventType::BakeFinished) { finished = true; break; }
                if (ev.type == matter::EventType::BakeError) {
                    printf("FAIL bake error: phase=%s code=%d msg=%s\n",
                           ev.phase.c_str(), (int)ev.code, ev.message.c_str());
                    return 1;
                }
            }
        }
        if (!finished) { printf("FAIL: bake timeout after 60s\n"); return 1; }
    }
    if (evs.empty()) { printf("FAIL: no events\n"); return 1; }
    assert(!evs.empty());
    assert(evs.front().type == matter::EventType::BakeStarted);
    assert(evs.back().type == matter::EventType::BakeFinished);
    int part_done = 0;
    for (auto& e : evs) if (e.type == matter::EventType::BakePartDone) ++part_done;
    printf("events: %zu (%d PartDone)\n", evs.size(), part_done);

    // Bake Lab (task 1.2): after BakeFinished, last_bake_trace returns the
    // stage-span tree. The cache was cleared above, so this was a full bake:
    // the root's children are exactly install, compose, publish, in order,
    // and all spans are closed (end_ms >= begin_ms >= 0).
    {
        bake_trace::Span trace;
        session->last_bake_trace(trace);
        assert(trace.name && std::strcmp(trace.name, bake_trace::kRootName) == 0);
        printf("bake trace: %zu root children\n", trace.children.size());
        for (auto& c : trace.children)
            printf("  span %s: %.1f..%.1f ms\n",
                   c.name ? c.name : "(null)", c.begin_ms, c.end_ms);
        assert(trace.children.size() == 3);
        assert(std::strcmp(trace.children[0].name, bake_trace::kSpanInstall) == 0);
        assert(std::strcmp(trace.children[1].name, bake_trace::kSpanCompose) == 0);
        assert(std::strcmp(trace.children[2].name, bake_trace::kSpanPublish) == 0);
        for (auto& c : trace.children) {
            assert(c.begin_ms >= 0.0);
            assert(c.end_ms >= c.begin_ms);   // closed, not kOpenEndMs
        }
    }

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
    matter::CameraDesc cam{{tx + 8.0f, ty + 6.0f, tz + 8.0f},
                           {tx, ty, tz}, {0, 1, 0},
                           60.0f * 3.14159265358979323846f / 180.0f,
                           1.0f, 5000.0f};
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
#ifndef MATTER_VULKAN_ONLY
    assert(nonblack > n / 20);
#else
    // Phase 5a (tech-debt.md S6) deleted the GL renderer/raster-composer/
    // GpuCuller path this assertion exercised. WorldSession::render() is the
    // no-op MATTER_VULKAN_ONLY stub, so nothing draws and the hidden
    // window's framebuffer stays black (nonblack == 0) -- not a bug, just no
    // render path to assert on until a Vulkan uploader exists.
#endif
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
