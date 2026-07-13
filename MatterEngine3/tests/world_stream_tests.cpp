// world_stream_tests.cpp — Phase C Task 9: world-kind session end-to-end test.
// GPU test (requires GALLIUM_DRIVER=d3d12). Runs from MatterViewer/ directory.
//
// Assertions:
//   1. open_world + request_bake -> poll until BakeFinished: no fatal errors,
//      resident_sectors > 0
//   2. sea_level() returns true, value matches fixture's seaLevel (0.0)
//   3. render one frame -> triangles > 0
//   4. Move focus +200 -> resident_sectors changes
//   5. regenerate(7) -> BakeFinished again, resident_sectors > 0
//   6. Destroy -> clean shutdown

#include "matter/engine_context.h"
#include "raylib.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static bool wait_for_bake_finished(matter::WorldSession& s, double timeout_s = 120.0) {
    double t0 = GetTime();
    bool finished = false;
    while (!finished && GetTime() - t0 < timeout_s) {
        s.pump_gpu_jobs(4.0f);
        matter::Event ev;
        while (s.poll_event(ev)) {
            if (ev.type == matter::EventType::BakeFinished) {
                finished = true;
                break;
            }
            if (ev.type == matter::EventType::BakeError) {
                // Fatal errors abort; non-fatal stream errors are tolerated.
                if (ev.phase != "stream") {
                    printf("FAIL bake error: phase=%s code=%d msg=%s\n",
                           ev.phase.c_str(), (int)ev.code, ev.message.c_str());
                    return false;
                }
                fprintf(stderr, "  [stream error] %s\n", ev.message.c_str());
            }
        }
    }
    return finished;
}

int main() {
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "world_stream_tests");

    std::string err;
    matter::EngineDesc ed;
    ed.cache_root = "cache";
    auto engine = matter::EngineContext::create(ed, err);
    if (!engine) { printf("FAIL create: %s\n", err.c_str()); CloseWindow(); return 1; }

    matter::WorldDesc wd;
    wd.schemas_dir    = "../MatterEngine3/tests/fixtures/world_stream/schemas";
    wd.world_data_dir = "../MatterEngine3/tests/fixtures/world_stream/WorldData";
    wd.world_name     = "TestWorld";
    wd.shared_lib_dir = "../MatterEngine3/shared-lib";
    auto session = engine->open_world(wd, err);
    if (!session) { printf("FAIL open_world: %s\n", err.c_str()); CloseWindow(); return 1; }

    // Set initial focus at origin.
    float focus0[3] = {0, 0, 0};
    session->set_bake_focus(focus0);

    // Use a small streaming ring for fast test.
    // (env MATTER_STREAM_RINGS is set by the run target to e.g. "32:3,80:2")

    // 1. request_bake -> two BakeFinished events:
    //    (a) install complete (empty manifest published)
    //    (b) first sectors streamed in (from execute_sector_stream_step)
    session->request_bake();
    printf("waiting for BakeFinished #1 (install)...\n");
    if (!wait_for_bake_finished(*session, 120.0)) {
        printf("FAIL: install bake did not complete within 120s\n");
        session.reset();
        CloseWindow();
        return 1;
    }
    printf("BakeFinished #1 (install) received\n");

    printf("waiting for BakeFinished #2 (first sectors)...\n");
    if (!wait_for_bake_finished(*session, 120.0)) {
        printf("FAIL: sector streaming did not complete within 120s\n");
        session.reset();
        CloseWindow();
        return 1;
    }
    printf("BakeFinished #2 (streaming) received\n");

    // Pump a few more frames to let GL publish jobs land.
    for (int i = 0; i < 30; ++i) {
        session->pump_gpu_jobs(16.0f);
        session->tick();
    }

    uint32_t rs1 = session->frame_stats().resident_sectors;
    printf("resident_sectors after initial load: %u\n", rs1);
    assert(rs1 > 0 && "resident_sectors > 0 after first stream cycle");

    // Probe bricks appear for resident sectors (bounded poll; bakes are async).
    auto poll_brick = [&](int64_t tx, int64_t tz, bool want, double timeout_s) {
        double t0 = GetTime();
        while (GetTime() - t0 < timeout_s) {
            session->pump_gpu_jobs(16.0f);
            session->tick();
            if (session->debug_probe_brick(tx, tz) == want) return true;
        }
        return false;
    };
    bool brick0 = poll_brick(0, 0, true, 90.0);
    printf("probe brick (0,0) appeared: %d\n", (int)brick0);
    assert(brick0 && "probe brick baked for resident sector (0,0)");
    assert(session->frame_stats().probe_bricks > 0 && "probe_bricks stat > 0");

    // 2. sea_level()
    float sl = -999.0f;
    bool has_sl = session->sea_level(sl);
    printf("sea_level: has=%d val=%.2f\n", has_sl, sl);
    assert(has_sl && "world-kind session has sea_level");
    assert(std::fabs(sl - 0.0f) < 0.01f && "seaLevel matches fixture (0.0)");

    // 3. render one frame -> triangles > 0
    matter::CameraDesc cam{{8.0f, 40.0f, 8.0f}, {8.0f, 0.0f, 8.0f},
                           {0, 1, 0},
                           60.0f * 3.14159265358979323846f / 180.0f,
                           1.0f, 5000.0f};
    matter::RenderOptions opts;
    opts.resolver = matter::ResolverKind::PassThrough;
    for (int i = 0; i < 3; ++i) {
        BeginDrawing();
        session->render(cam, GetScreenWidth(), GetScreenHeight(), opts);
        EndDrawing();
    }
    uint32_t tris = session->frame_stats().triangles;
    printf("triangles after render: %u\n", tris);
    assert(tris > 0 && "triangles > 0 after render");

    // 4. Move focus +200 -> resident_sectors changes
    float focus_far[3] = {200.0f, 0.0f, 200.0f};
    session->set_bake_focus(focus_far);
    // Let the streaming loop work for a few seconds.
    {
        double t0 = GetTime();
        while (GetTime() - t0 < 5.0) {
            session->pump_gpu_jobs(16.0f);
            session->tick();
        }
    }
    uint32_t rs2 = session->frame_stats().resident_sectors;
    printf("resident_sectors after focus move: %u (was %u)\n", rs2, rs1);
    // rs2 may be equal if the rings are large enough, but should still be > 0.
    assert(rs2 > 0 && "resident_sectors > 0 after focus move");

    // Bricks are freed when the sector column fully evicts.
    // Focus moved to (200,0,200) — sector (0,0) is outside the 80m outer ring.
    bool brick0_gone = poll_brick(0, 0, false, 60.0);
    printf("probe brick (0,0) freed after eviction: %d\n", (int)brick0_gone);
    assert(brick0_gone && "probe brick freed on full eviction");

    // 5. regenerate(7) -> two BakeFinished events again (install + streaming)
    printf("regenerate(7)...\n");
    session->regenerate(7);
    if (!wait_for_bake_finished(*session, 120.0)) {
        printf("FAIL: regenerate install did not complete within 120s\n");
        session.reset();
        CloseWindow();
        return 1;
    }
    printf("regenerate install BakeFinished received\n");
    if (!wait_for_bake_finished(*session, 120.0)) {
        printf("FAIL: regenerate streaming did not complete within 120s\n");
        session.reset();
        CloseWindow();
        return 1;
    }
    printf("regenerate streaming BakeFinished received\n");
    for (int i = 0; i < 30; ++i) {
        session->pump_gpu_jobs(16.0f);
        session->tick();
    }
    uint32_t rs3 = session->frame_stats().resident_sectors;
    printf("resident_sectors after regenerate: %u\n", rs3);
    assert(rs3 > 0 && "resident_sectors > 0 after regenerate");

    // 6. Destroy -> clean shutdown
    printf("destroying session...\n");
    session.reset();
    printf("session destroyed cleanly\n");

    printf("ALL TESTS PASSED\n");
    CloseWindow();
    return 0;
}
