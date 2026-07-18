// world_stream_tests.cpp — Phase C Task 9: world-kind session end-to-end test.
// GPU test (requires GALLIUM_DRIVER=d3d12). Runs from MatterViewer/ directory.
//
// Assertions:
//   1. Every session owns one ECS world with matter.ecs runtime state.
//   2. Successful authored publishes advance content_generation exactly once.
//   3. Runtime entities survive reload() and regenerate().
//   4. Existing world streaming, sea-level, and render behavior remains intact.
//   5. A fatal bake sets Failed without deleting runtime entities.
//   6. A replacement session cannot resolve the prior session's entity ID.

#include "matter/engine_context.h"
#include "matter/ecs.h"
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
        s.tick({0.0f, 1.0f / 60.0f, 4});
        matter::Event ev;
        while (s.poll_event(ev)) {
            if (ev.type == matter::EventType::BakeFinished) {
                // Bake-state commands are worker-produced plain data and become
                // visible only when the session progresses its ECS runtime.
                s.tick({0.0f, 1.0f / 60.0f, 4});
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

static bool wait_for_fatal_bake_error(matter::WorldSession& s,
                                      double timeout_s = 120.0) {
    const double t0 = GetTime();
    while (GetTime() - t0 < timeout_s) {
        s.pump_gpu_jobs(4.0f);
        s.tick({0.0f, 1.0f / 60.0f, 4});
        matter::Event ev;
        while (s.poll_event(ev)) {
            if (ev.type == matter::EventType::BakeError &&
                ev.code != matter::BakeErrorCode::Cancelled &&
                ev.phase != "stream") {
                s.tick({0.0f, 1.0f / 60.0f, 4});
                return true;
            }
        }
    }
    return false;
}

static const matter::ecs::WorldRuntimeState& runtime_state(
    const matter::WorldSession& session) {
    return session.ecs().get<matter::ecs::WorldRuntimeState>();
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

    flecs::world& ecs_world = session->ecs();
    assert(ecs_world.lookup("matter::ecs").is_alive() &&
           "session ECS world resolves matter.ecs module scope");
    assert(runtime_state(*session).status == matter::ecs::WorldStatus::Loading &&
           "new session begins Loading");
    assert(runtime_state(*session).content_generation == 0 &&
           "new session begins before any authored generation");
    const flecs::entity runtime_entity =
        ecs_world.entity("Task6PersistentRuntimeEntity");
    const flecs::entity_t runtime_entity_id = runtime_entity.id();

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
    assert(runtime_state(*session).status == matter::ecs::WorldStatus::Ready &&
           "successful authored publish sets Ready");
    assert(runtime_state(*session).content_generation == 1 &&
           "initial authored publish increments content generation once");

    printf("waiting for BakeFinished #2 (first sectors)...\n");
    if (!wait_for_bake_finished(*session, 120.0)) {
        printf("FAIL: sector streaming did not complete within 120s\n");
        session.reset();
        CloseWindow();
        return 1;
    }
    printf("BakeFinished #2 (streaming) received\n");
    assert(runtime_state(*session).content_generation == 1 &&
           "sector streaming completion does not double-increment generation");

    // Pump a few more frames to let GL publish jobs land.
    for (int i = 0; i < 30; ++i) {
        session->pump_gpu_jobs(16.0f);
        session->tick({0.0f, 1.0f / 60.0f, 4});
    }

    uint32_t rs1 = session->frame_stats().resident_sectors;
    printf("resident_sectors after initial load: %u\n", rs1);
    assert(rs1 > 0 && "resident_sectors > 0 after first stream cycle");

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
            session->tick({0.0f, 1.0f / 60.0f, 4});
        }
    }
    uint32_t rs2 = session->frame_stats().resident_sectors;
    printf("resident_sectors after focus move: %u (was %u)\n", rs2, rs1);
    // rs2 may be equal if the rings are large enough, but should still be > 0.
    assert(rs2 > 0 && "resident_sectors > 0 after focus move");

    // 5. reload() preserves the ECS world and publishes one new generation.
    printf("reload()...\n");
    session->reload();
    if (!wait_for_bake_finished(*session, 120.0) ||
        !wait_for_bake_finished(*session, 120.0)) {
        printf("FAIL: reload did not complete install + streaming\n");
        session.reset();
        CloseWindow();
        return 1;
    }
    assert(session->ecs().is_alive(runtime_entity_id) &&
           session->ecs().lookup("Task6PersistentRuntimeEntity").id() == runtime_entity_id &&
           "runtime entity survives reload with the same ID");
    assert(runtime_state(*session).status == matter::ecs::WorldStatus::Ready &&
           runtime_state(*session).content_generation == 2 &&
           "reload publishes exactly one authored generation");

    // 6. regenerate(7) -> two BakeFinished events again (install + streaming)
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
        session->tick({0.0f, 1.0f / 60.0f, 4});
    }
    uint32_t rs3 = session->frame_stats().resident_sectors;
    printf("resident_sectors after regenerate: %u\n", rs3);
    assert(rs3 > 0 && "resident_sectors > 0 after regenerate");
    assert(session->ecs().is_alive(runtime_entity_id) &&
           session->ecs().lookup("Task6PersistentRuntimeEntity").id() == runtime_entity_id &&
           "runtime entity survives regenerate with the same ID");
    assert(runtime_state(*session).status == matter::ecs::WorldStatus::Ready &&
           runtime_state(*session).content_generation == 3 &&
           "regenerate publishes exactly one authored generation");

    // 7. Destroy the successful session, then open a replacement whose bake
    // deterministically fails because its world directory does not exist.
    printf("destroying session...\n");
    session.reset();
    printf("session destroyed cleanly\n");

    matter::WorldDesc bad_wd = wd;
    bad_wd.world_name = "Task6MissingWorldForFatalBake";
    auto replacement = engine->open_world(bad_wd, err);
    if (!replacement) {
        printf("FAIL replacement open_world: %s\n", err.c_str());
        CloseWindow();
        return 1;
    }
    assert(!replacement->ecs().is_alive(runtime_entity_id) &&
           "replacement session cannot resolve the old runtime entity ID");
    const flecs::entity failed_runtime_entity =
        replacement->ecs().entity("Task6FailedBakeRuntimeEntity");
    const flecs::entity_t failed_runtime_entity_id = failed_runtime_entity.id();
    replacement->request_bake();
    assert(wait_for_fatal_bake_error(*replacement) &&
           "missing world produces a fatal bake error");
    assert(runtime_state(*replacement).status == matter::ecs::WorldStatus::Failed &&
           "fatal bake error sets Failed");
    assert(runtime_state(*replacement).content_generation == 0 &&
           "fatal bake does not publish a content generation");
    assert(replacement->ecs().is_alive(failed_runtime_entity_id) &&
           replacement->ecs().lookup("Task6FailedBakeRuntimeEntity").id() ==
               failed_runtime_entity_id &&
           "fatal bake does not delete runtime entities");
    replacement.reset();

    printf("ALL TESTS PASSED\n");
    CloseWindow();
    return 0;
}
