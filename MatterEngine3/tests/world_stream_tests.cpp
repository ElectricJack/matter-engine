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
#include "matter/physics.h"
#include "matter/streaming.h"
#include "ecs/physics_context.h"
#include "raylib.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
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
    const flecs::entity streaming_anchor =
        ecs_world.entity("Task4StreamingAnchor")
            .set<matter::ecs::LocalTransform>(
                {{8.0f, 0.0f, 8.0f}, {}, {1.0f, 1.0f, 1.0f}});
    const flecs::entity_t streaming_anchor_id = streaming_anchor.id();

    // Box3D Phase 2: runtime physics belongs to the session, not authored
    // content. A live body and its context therefore survive content reloads.
    matter::ecs::LocalTransform physics_transform{};
    physics_transform.translation = {0.0f, 2.0f, 0.0f};
    matter::physics::RigidBody physics_body{};
    physics_body.type = matter::physics::RigidBodyType::Dynamic;
    physics_body.gravity_scale = 0.0f;
    const flecs::entity physics_entity =
        ecs_world.entity("Task8PersistentPhysicsBody")
            .set<matter::ecs::LocalTransform>(physics_transform)
            .set<matter::physics::RigidBody>(physics_body)
            .set<matter::physics::SphereCollider>({});
    const flecs::entity_t physics_entity_id = physics_entity.id();
    const auto* const physics_context =
        &matter::physics::detail::context(session->ecs());
    session->tick({1.0f / 60.0f, 1.0f / 60.0f, 4});
    const matter::physics::PhysicsStats initial_physics_stats =
        matter::physics::physics_stats(session->ecs());
    assert(initial_physics_stats.live_bodies == 1 &&
           initial_physics_stats.bodies_created == 1 &&
           matter::physics::detail::context_world_is_valid(session->ecs()) &&
           "session creates one persistent physics body in one valid context");

    // Set initial focus at origin.
    float focus0[3] = {0, 0, 0};
    session->set_bake_focus(focus0);

    // Use a small streaming ring for fast test.
    // (env MATTER_STREAM_RINGS is set by the run target to e.g. "32:3,80:2")

    // 1. Procedural installation alone does not activate sector streaming.
    //    The install emits one BakeFinished; the first streaming completion is
    //    emitted only after an ECS entity explicitly adds SectorStreaming.
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

    // Pump a few frames to prove profile readiness cannot stream by itself.
    for (int i = 0; i < 30; ++i) {
        session->pump_gpu_jobs(16.0f);
        session->tick({0.0f, 1.0f / 60.0f, 4});
    }
    assert(session->streaming_status().state ==
               matter::streaming::SectorStreamingState::Detached &&
           session->streaming_status().resident_sectors == 0 &&
           session->frame_stats().resident_sectors == 0 &&
           "procedural profile without ECS activation streams zero sectors");

    streaming_anchor.add<matter::streaming::SectorStreaming>();
    session->tick({0.0f, 1.0f / 60.0f, 4});
    printf("waiting for BakeFinished #2 (activated first sectors)...\n");
    if (!wait_for_bake_finished(*session, 120.0)) {
        printf("FAIL: activated sector streaming did not complete within 120s\n");
        session.reset();
        CloseWindow();
        return 1;
    }
    printf("BakeFinished #2 (streaming) received\n");
    assert(runtime_state(*session).content_generation == 1 &&
           "sector streaming completion does not double-increment generation");
    for (int i = 0; i < 30; ++i) {
        session->pump_gpu_jobs(16.0f);
        session->tick({0.0f, 1.0f / 60.0f, 4});
    }

    uint32_t rs1 = session->frame_stats().resident_sectors;
    printf("resident_sectors after initial load: %u\n", rs1);
    const matter::streaming::SectorStreamingStatus active_status =
        session->streaming_status();
    assert(rs1 > 0 && active_status.state ==
               matter::streaming::SectorStreamingState::Active &&
           active_status.resident_sectors == rs1 &&
           "activation with transform and profile starts streaming");

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

    // 4. Bake focus remains a public closed-world ordering control, but no
    //    longer drives infinite sector streaming. Moving the ECS anchor does.
    const uint64_t generation_before_focus =
        session->streaming_status().generation;
    float focus_far[3] = {200.0f, 0.0f, 200.0f};
    session->set_bake_focus(focus_far);
    for (int i = 0; i < 30; ++i) {
        session->pump_gpu_jobs(16.0f);
        session->tick({0.0f, 1.0f / 60.0f, 4});
    }
    assert(session->streaming_status().generation == generation_before_focus &&
           "set_bake_focus does not restart infinite streaming");

    streaming_anchor.set<matter::ecs::LocalTransform>(
        {{200.0f, 0.0f, 200.0f}, {}, {1.0f, 1.0f, 1.0f}});
    streaming_anchor.add<matter::ecs::TransformDirty>();
    {
        double t0 = GetTime();
        while (GetTime() - t0 < 5.0) {
            session->pump_gpu_jobs(16.0f);
            session->tick({0.0f, 1.0f / 60.0f, 4});
        }
    }
    uint32_t rs2 = session->frame_stats().resident_sectors;
    printf("resident_sectors after ECS anchor move: %u (was %u)\n", rs2, rs1);
    // rs2 may be equal if the rings are large enough, but should still be > 0.
    assert(rs2 > 0 && session->streaming_status().generation ==
                          generation_before_focus &&
           "moving the ECS anchor streams without an unnecessary restart");

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
    assert(session->ecs().is_alive(streaming_anchor_id) &&
           session->ecs().entity(streaming_anchor_id)
               .has<matter::streaming::SectorStreaming>() &&
           session->streaming_status().state ==
               matter::streaming::SectorStreamingState::Active &&
           "reload preserves the same streaming entity and component");
    const matter::physics::PhysicsStats reload_physics_stats =
        matter::physics::physics_stats(session->ecs());
    assert(&matter::physics::detail::context(session->ecs()) == physics_context &&
           session->ecs().is_alive(physics_entity_id) &&
           reload_physics_stats.live_bodies == 1 &&
           reload_physics_stats.bodies_created == initial_physics_stats.bodies_created &&
           reload_physics_stats.steps == initial_physics_stats.steps &&
           "reload preserves the physics context, body, and accumulated stats");
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
    assert(session->ecs().is_alive(streaming_anchor_id) &&
           session->ecs().entity(streaming_anchor_id)
               .has<matter::streaming::SectorStreaming>() &&
           session->streaming_status().state ==
               matter::streaming::SectorStreamingState::Active &&
           "regenerate preserves the same streaming entity and component");
    const matter::physics::PhysicsStats regenerate_physics_stats =
        matter::physics::physics_stats(session->ecs());
    assert(&matter::physics::detail::context(session->ecs()) == physics_context &&
           session->ecs().is_alive(physics_entity_id) &&
           regenerate_physics_stats.live_bodies == 1 &&
           regenerate_physics_stats.bodies_created == initial_physics_stats.bodies_created &&
           regenerate_physics_stats.steps == reload_physics_stats.steps &&
           "regenerate preserves the physics context, body, and accumulated stats");
    assert(runtime_state(*session).status == matter::ecs::WorldStatus::Ready &&
           runtime_state(*session).content_generation == 3 &&
           "regenerate publishes exactly one authored generation");

    // 7. Removing activation during reload prevents the preserved entity from
    //    restarting after the replacement profile is installed.
    printf("reload() with latched streaming removal...\n");
    bool removed_at_finalize = false;
    session->set_test_fault_hook([&](int stage) {
        if (stage == -1 && !removed_at_finalize) {
            removed_at_finalize = true;
            streaming_anchor.remove<matter::streaming::SectorStreaming>();
        }
    });
    session->reload();
    if (!wait_for_bake_finished(*session, 120.0)) {
        printf("FAIL: removal-during-reload install did not complete\n");
        session.reset();
        CloseWindow();
        return 1;
    }
    for (int i = 0; i < 60; ++i) {
        session->pump_gpu_jobs(16.0f);
        session->tick({0.0f, 1.0f / 60.0f, 4});
    }
    assert(removed_at_finalize &&
           session->ecs().is_alive(streaming_anchor_id) &&
           !session->ecs().entity(streaming_anchor_id)
                .has<matter::streaming::SectorStreaming>() &&
           session->streaming_status().state ==
               matter::streaming::SectorStreamingState::Detached &&
           session->streaming_status().resident_sectors == 0 &&
           session->frame_stats().resident_sectors == 0 &&
           "removal during reload prevents residency");

    // 8. A fault in the authored finalize barrier cannot activate the staged
    //    procedural profile or allocate any sector work.
    streaming_anchor.add<matter::streaming::SectorStreaming>();
    session->set_test_fault_hook([](int stage) {
        if (stage == -1) {
            throw std::runtime_error("injected authored finalize failure");
        }
    });
    session->reload();
    assert(wait_for_fatal_bake_error(*session, 120.0) &&
           "injected finalize fault reports a fatal authored bake error");
    for (int i = 0; i < 60; ++i) {
        session->pump_gpu_jobs(16.0f);
        session->tick({0.0f, 1.0f / 60.0f, 4});
    }
    assert(session->streaming_status().state ==
               matter::streaming::SectorStreamingState::PendingProfile &&
           session->streaming_status().resident_sectors == 0 &&
           session->streaming_status().inflight_sectors == 0 &&
           session->frame_stats().resident_sectors == 0 &&
           "failed authored finalize leaves no profile, sector work, or residency");
    session->set_test_fault_hook({});

    // 9. A closed-world bake stays usable while activation reports the
    //    recoverable UnsupportedWorld error and streams zero sectors.
    matter::WorldDesc closed_wd = wd;
    closed_wd.world_name = "ClosedWorld";
    auto closed_session = engine->open_world(closed_wd, err);
    if (!closed_session) {
        printf("FAIL closed-world open_world: %s\n", err.c_str());
        session.reset();
        CloseWindow();
        return 1;
    }
    const flecs::entity closed_anchor =
        closed_session->ecs().entity("Task4ClosedWorldAnchor")
            .set<matter::ecs::LocalTransform>({})
            .add<matter::streaming::SectorStreaming>();
    closed_session->request_bake();
    if (!wait_for_bake_finished(*closed_session, 120.0)) {
        printf("FAIL: closed-world bake did not complete\n");
        closed_session.reset();
        session.reset();
        CloseWindow();
        return 1;
    }
    for (int i = 0; i < 30; ++i) {
        closed_session->pump_gpu_jobs(16.0f);
        closed_session->tick({0.0f, 1.0f / 60.0f, 4});
    }
    const auto* closed_error =
        closed_anchor.try_get<matter::streaming::SectorStreamingError>();
    assert(runtime_state(*closed_session).status ==
               matter::ecs::WorldStatus::Ready &&
           closed_error != nullptr &&
           closed_error->code ==
               matter::streaming::SectorStreamingErrorCode::UnsupportedWorld &&
           closed_session->streaming_status().resident_sectors == 0 &&
           closed_session->frame_stats().resident_sectors == 0 &&
           "closed-world activation is recoverable and non-streaming");
    closed_session.reset();

    // 10. Destroy the successful session, then open a replacement whose bake
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
    assert(!replacement->ecs().is_alive(physics_entity_id) &&
           matter::physics::physics_stats(replacement->ecs()).live_bodies == 0 &&
           matter::physics::detail::context_world_is_valid(replacement->ecs()) &&
           "replacement session owns a fresh empty physics context");
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
