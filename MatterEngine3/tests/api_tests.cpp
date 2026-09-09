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
    // Unbuffered: every failure here is an assert(), and abort() discards
    // whatever printf left in stdio's buffer -- which is exactly the
    // diagnostic printout that says why the assertion failed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "api_tests");
    std::string err;
    matter::EngineDesc ed;
    ed.cache_root = "cache";   // run from MatterEditor/ so the bake cache is warm
    // Headless kernel session: no interactive renderer. Despite its legacy
    // name, allow_gl_lt_46 means exactly "this caller does not need a render
    // device" (see EngineContext::create in matter_engine.cpp), and that is
    // the truth here -- everything below is bake, ECS and raycast, and
    // WorldSession::render() is the MATTER_VULKAN_ONLY no-op stub this
    // binary is compiled against, so a VulkanDevice would draw nothing.
    // Without this flag create() correctly refuses with "an interactive
    // session requires a Vulkan render device".
    ed.allow_gl_lt_46 = true;
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
    //
    // The path is NOT EngineDesc::cache_root. Each world gets its own cache
    // root, `<project_dir>/.cache/<world_name>` (LocalProviderConfig, see
    // local_provider.h), and resolve_cache::save writes
    // `<that>/cache/<world_name>.resolve`. This used to point at
    // "cache/cache/Primitives.resolve" under MatterEditor/, which no longer
    // exists -- so the remove() silently did nothing, every run after the
    // first took the resolve-cache fast path, and the trace below had one
    // child ("publish") instead of three.
    std::remove("../projects/primitive_demo/.cache/Primitives/cache/"
                "Primitives.resolve");

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
        // BakeFinished is emitted from the finalize GPU job, which runs on THIS
        // thread inside pump_gpu_jobs -- while the bake worker is still unwinding
        // out of publish_pipeline and has not yet closed its "publish" span. So
        // the event arrives a hair before the trace is complete and a snapshot
        // taken the instant BakeFinished is polled catches publish still open
        // (end_ms == kOpenEndMs == -1). Keep pumping until every root child is
        // closed, bounded, rather than asserting on a torn tree.
        {
            const double t0 = GetTime();
            for (;;) {
                session->last_bake_trace(trace);
                bool all_closed = !trace.children.empty();
                for (auto& c : trace.children)
                    if (c.end_ms == bake_trace::kOpenEndMs) all_closed = false;
                if (all_closed || GetTime() - t0 > 10.0) break;
                session->pump_gpu_jobs(4.0f);
            }
        }
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
    printf("instance[0]: translation=(%.3f, %.3f, %.3f)\n", tx, ty, tz);
    matter::CameraDesc cam{{tx + 8.0f, ty + 6.0f, tz + 8.0f},
                           {tx, ty, tz}, {0, 1, 0},
                           60.0f * 3.14159265358979323846f / 180.0f,
                           1.0f, 5000.0f};
    matter::RenderOptions opts;   // defaults: GpuDriven + SectorLod
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

    // raycast down onto the world above instance 0.
    //
    // What the query API actually traces matters here, and it is NOT the mesh
    // the renderer draws: WorldTracer selects the COARSEST ladder rung of each
    // part (see the LOD-choice note on load_part in world_tracer.cpp), so on
    // this fixture the whole 27 m Gallery is a few dozen triangles. A single
    // ray down the instance origin therefore proves nothing -- the primitive
    // that stands there at LOD 0 has been decimated away at the rung the
    // tracer holds, and asserting on it made this test a fixture-geometry
    // test rather than an API test.
    //
    // So sweep the instance footprint on a 0.5 m grid and require that the
    // world is hittable at all, then check the API contract on every hit:
    // t inside the ray bound, and a resolved instance index / part hash.
    int hit_count = 0;
    matter::RayHit first_hit{};
    float first_hit_x = 0.0f, first_hit_z = 0.0f;
    for (int ix = -16; ix <= 48; ++ix) {
        for (int iz = -20; iz <= 4; ++iz) {
            const float x = tx + ix * 0.5f;
            const float z = tz + iz * 0.5f;
            float origin[3] = { x, ty + 100.0f, z };
            float dir[3]    = { 0.0f, -1.0f, 0.0f };
            matter::RayHit hit;
            if (!session->raycast(origin, dir, 1000.0f, hit)) continue;
            assert(hit.t > 0.0f && hit.t < 1000.0f);
            assert(hit.instance != 0xffffffffu);
            assert(hit.part_hash == info.part_hash);
            if (hit_count == 0) { first_hit = hit; first_hit_x = x; first_hit_z = z; }
            ++hit_count;
        }
    }
    printf("raycast: %d hits over the footprint; first at (%.2f, %.2f) "
           "t=%.3f instance=%u material=%d\n",
           hit_count, first_hit_x, first_hit_z, first_hit.t,
           first_hit.instance, first_hit.material_id);
    assert(hit_count > 0 && "a downward sweep over the instance footprint hits it");
    // A downward ray that hits a surface from above must come back with an
    // upward-facing normal -- the tracer flips the geometric normal toward the
    // ray origin, so this also pins that convention.
    printf("raycast: first-hit normal=(%.3f, %.3f, %.3f)\n",
           first_hit.normal[0], first_hit.normal[1], first_hit.normal[2]);
    assert(first_hit.normal[1] > 0.0f);

    // A ray pointed away from the world misses, and leaves the caller's Hit
    // untouched (world_tracer.h's Hit contract).
    {
        float origin[3] = { tx, ty + 100.0f, tz };
        float dir[3]    = { 0.0f, 1.0f, 0.0f };
        matter::RayHit up{};
        up.t = -42.0f;
        const bool up_hit = session->raycast(origin, dir, 1000.0f, up);
        printf("raycast (upward): hit=%d t=%.3f\n", (int)up_hit, up.t);
        assert(!up_hit && up.t == -42.0f);
    }

    session.reset();   // before CloseWindow
    engine.reset();
    CloseWindow();
    printf("api_tests: all passed\n");
    return 0;
}
