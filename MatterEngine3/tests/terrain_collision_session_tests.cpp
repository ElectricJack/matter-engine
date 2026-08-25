#include "check.h"

#include "matter/engine_context.h"
#include "matter/ecs.h"
#include "matter/log.h"
#include "matter/world_session.h"
#include "ecs/physics_context.h"
#include "bake_mode.h"
#include "terrain_collision/terrain_collision_artifact.h"
#include "terrain_mesher.h"
#include "terrain_river_overlay.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using matter::terrain_collision::CanonicalDefinition;
using matter::terrain_collision::TerrainCollisionCandidate;
using matter::terrain_collision::TileCandidate;

bool write_file(const fs::path& path, const std::string& body) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream << body;
    return stream.good();
}

void remove_tree(const fs::path& path) {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

std::string hex_key(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << value;
    return stream.str();
}

fs::path manifest_path(const fs::path& cache_root,
                       std::uint64_t installation_key) {
    return cache_root / "terrain_collision" / "v1" / "generations" /
           (hex_key(installation_key) + ".mtcm");
}

bool write_world_object(const fs::path& root,
                        bool collision,
                        float friction = 0.55f,
                        const char* region_max_x = "32") {
    std::ostringstream source;
    source << "class TerrainSession extends World {\n"
           << "  static roots=[{module:'TerrainRoot',transform:[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}];\n"
           << "  static params = { worldSeed: 9 };\n"
           << "  static world = { sectorSize: 16, yMin: -16, yMax: 32 };\n"
           << "  field(p) {\n"
           << "    const h = noise2(p.worldSeed, 1/64, 2).mul(2).add(2);\n"
           << "    return { density: heightToDensity(h), moisture: h, relief: h, seaLevel: -8 };\n"
           << "  }\n"
           << "  biomes() { return { meadow: { grass: 1 } }; }\n";
    if (collision) {
        source << "  collision() {\n"
               << "    const c = terrainCollision({cellSize:0.5,friction:"
               << friction << ",restitution:0.03});\n"
               << "    c.region('gameplay',{min:[0,0,0],max:["
               << region_max_x << ",16,16]});\n"
               << "    c.build();\n"
               << "  }\n";
    }
    source << "}\n";
    return write_file(root / "worlds" / "TerrainSession.js", source.str());
}

bool build_project(const fs::path& root,
                   bool collision = true,
                   float friction = 0.55f,
                   const char* region_max_x = "32") {
    remove_tree(root);
    std::error_code error;
    fs::create_directories(root / ".cache" / "TerrainSession" / "parts", error);
    if (error) return false;
    if (!write_world_object(root, collision, friction, region_max_x)) return false;
    if (!write_file(root / "objects" / "TerrainRoot.js",
        "class TerrainRoot extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone); this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(0,0,0); this.vertex(1,0,0); this.vertex(0,0,1);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;
    return write_file(root / "objects" / "WorldSector.js",
        "class WorldSector extends Part {\n"
        "  static params={tx:0,ty:0,tz:0,rung:0,volumetric:0,worldSeed:0,fieldHash:'',biomes:''};\n"
        "  static requires=[];\n"
        "  build(p) { this.terrainVolumeTiled(p.tx,p.ty|0,p.tz,p.rung,[MAT.grass,MAT.dirt,MAT.rock,MAT.snow]); }\n"
        "}\n");
}

struct SessionFixture {
    fs::path root;
    std::string cache_root;
    std::string project_root;
    std::unique_ptr<matter::EngineContext> engine;
    std::unique_ptr<matter::WorldSession> session;

    explicit SessionFixture(const char* label,
                            bool collision = true,
                            float friction = 0.55f,
                            const char* region_max_x = "32") {
        static std::atomic<std::uint64_t> serial{0};
        root = fs::temp_directory_path() /
            (std::string("me3_terrain_session_") + label + "_" +
             std::to_string(serial.fetch_add(1, std::memory_order_relaxed)));
        CHECK(build_project(root, collision, friction, region_max_x),
              "terrain collision session fixture is created");
        cache_root = (root / ".cache").string();
        project_root = root.string();
        matter::EngineDesc engine_desc{};
        engine_desc.cache_root = cache_root.c_str();
        engine_desc.allow_gl_lt_46 = true;
        std::string error;
        engine = matter::EngineContext::create(engine_desc, error);
        CHECK(engine != nullptr, error.c_str());
        if (!engine) return;
        matter::WorldDesc world_desc{};
        world_desc.project_dir = project_root.c_str();
        world_desc.world_name = "TerrainSession";
        world_desc.engine_shared_lib_dir = "../shared-lib";
        session = engine->open_world(world_desc, error);
        CHECK(session != nullptr, error.c_str());
    }

    ~SessionFixture() {
        session.reset();
        engine.reset();
        remove_tree(root);
    }
};

TerrainCollisionCandidate candidate_for(const CanonicalDefinition& definition) {
    TerrainCollisionCandidate candidate{};
    candidate.geometry_key = definition.geometry_key;
    candidate.installation_key = definition.installation_key;
    candidate.friction = definition.friction;
    candidate.restitution = definition.restitution;
    std::uint64_t tile_key = 100;
    for (const auto& coordinate : definition.sectors) {
        TileCandidate tile{};
        tile.coordinate = coordinate;
        tile.origin_m = {
            static_cast<float>(coordinate.x) * definition.sector_size_m,
            static_cast<float>(coordinate.y) * definition.sector_size_m,
            static_cast<float>(coordinate.z) * definition.sector_size_m,
        };
        tile.tile_key = tile_key++;
        tile.digest = tile.tile_key ^ 0x71c0111510ULL;
        tile.vertices = {
            {1.0f, 0.0f, 1.0f},
            {definition.sector_size_m - 1.0f, 0.0f, 1.0f},
            {definition.sector_size_m - 1.0f, 0.0f,
             definition.sector_size_m - 1.0f},
            {1.0f, 0.0f, definition.sector_size_m - 1.0f},
        };
        tile.indices = {0u, 2u, 1u, 0u, 3u, 2u};
        candidate.tiles.push_back(std::move(tile));
    }
    candidate.stats.built_tiles =
        static_cast<std::uint32_t>(candidate.tiles.size());
    candidate.stats.triangle_count = candidate.tiles.size() * 2u;
    candidate.stats.unique_vertex_count = candidate.tiles.size() * 4u;
    candidate.stats.artifact_bytes = candidate.tiles.size() * 192u;
    candidate.stats.cold_build_ms = 1.25;
    candidate.stats.validation_ms = 0.25;
    return candidate;
}

struct ThrowingCopyBuilderState {
    std::atomic<bool> armed{false};
    std::atomic<int> copies{0};
    std::atomic<int> invocations{0};
};

struct ThrowingCopyBuilder {
    std::shared_ptr<ThrowingCopyBuilderState> state;

    explicit ThrowingCopyBuilder(
        std::shared_ptr<ThrowingCopyBuilderState> shared_state)
        : state(std::move(shared_state)) {}

    ThrowingCopyBuilder(const ThrowingCopyBuilder& other)
        : state(other.state) {
        state->copies.fetch_add(1, std::memory_order_acq_rel);
        if (state->armed.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "injected terrain builder callback copy failure");
        }
    }

    ThrowingCopyBuilder(ThrowingCopyBuilder&&) noexcept = default;

    bool operator()(const terrain_field::FieldRuntime&,
                    const CanonicalDefinition&,
                    const std::string&,
                    const std::function<bool()>&,
                    TerrainCollisionCandidate&,
                    std::string&) const {
        state->invocations.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
};

struct EmptyWhatException final : std::exception {
    const char* what() const noexcept override { return ""; }
};

std::atomic<bool> throw_terrain_install_log{false};

void throwing_terrain_install_log_sink(matter::log::Level level,
                                       const char* tag,
                                       const char*,
                                       void*) {
    if (level == matter::log::Level::Info && tag != nullptr &&
        std::strcmp(tag, "terrain-collision") == 0 &&
        throw_terrain_install_log.exchange(false,
                                           std::memory_order_acq_rel)) {
        throw EmptyWhatException{};
    }
}

struct ThrowingTerrainInstallLogGuard {
    ThrowingTerrainInstallLogGuard() {
        matter::log::add_sink(throwing_terrain_install_log_sink);
    }
    ~ThrowingTerrainInstallLogGuard() {
        throw_terrain_install_log.store(false, std::memory_order_release);
        matter::log::remove_sink(throwing_terrain_install_log_sink);
    }
};

terrain_mesher::SectorMesh one_triangle_mesh() {
    terrain_mesher::SectorMesh mesh{};
    terrain_mesher::MaterialBucket bucket{};
    bucket.positions = {0.0f, 0.0f, 0.0f,
                        0.0f, 0.0f, 1.0f,
                        1.0f, 0.0f, 0.0f};
    bucket.normals = {0.0f, 1.0f, 0.0f,
                      0.0f, 1.0f, 0.0f,
                      0.0f, 1.0f, 0.0f};
    mesh.buckets.push_back(std::move(bucket));
    return mesh;
}

struct TerminalEvents {
    int finished = 0;
    int errors = 0;
    int cancelled = 0;
    matter::BakeErrorCode error_code = matter::BakeErrorCode::Internal;
    std::string phase;
    std::string message;
};

void poll_terminal_events(matter::WorldSession& session,
                          TerminalEvents& events) {
    matter::Event event{};
    while (session.poll_event(event)) {
        if (event.type == matter::EventType::BakeFinished) {
            ++events.finished;
        } else if (event.type == matter::EventType::BakeError) {
            if (event.code == matter::BakeErrorCode::Cancelled) {
                ++events.cancelled;
            } else {
                ++events.errors;
                events.error_code = event.code;
                events.phase = event.phase;
                events.message = event.message;
            }
        }
    }
}

bool pump_until(matter::WorldSession& session,
                const std::function<bool()>& done,
                TerminalEvents& events,
                int timeout_seconds = 60) {
    const auto deadline = Clock::now() + std::chrono::seconds(timeout_seconds);
    while (Clock::now() < deadline) {
        session.pump_gpu_jobs(4.0f);
        session.tick({0.0f, 1.0f / 60.0f, 4});
        poll_terminal_events(session, events);
        if (done()) {
            session.tick({0.0f, 1.0f / 60.0f, 4});
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool status_snapshot_is_consistent(const matter::TerrainCollisionStatus& status) {
    switch (status.state) {
        case matter::TerrainCollisionState::Disabled:
            return status.generation_key == 0 && status.geometry_key == 0 &&
                   status.cell_size_m == 0.0f && status.region_count == 0 &&
                   status.sector_count == 0 &&
                   status.non_empty_tile_count == 0 &&
                   status.empty_tile_count == 0 &&
                   status.triangle_count == 0 &&
                   status.unique_vertex_count == 0 &&
                   status.artifact_bytes == 0 &&
                   status.box3d_retained_bytes == 0 &&
                   status.failure_code.empty() && status.failure_message.empty();
        case matter::TerrainCollisionState::Building:
            return status.failure_code.empty() && status.failure_message.empty() &&
                   (status.generation_key == 0 ||
                    (status.geometry_key != 0 && status.cell_size_m > 0.0f &&
                     status.region_count != 0 && status.sector_count != 0));
        case matter::TerrainCollisionState::CandidateReady:
        case matter::TerrainCollisionState::Installed:
            return status.generation_key != 0 && status.geometry_key != 0 &&
                   status.cell_size_m > 0.0f && status.region_count != 0 &&
                   status.sector_count != 0 &&
                   status.non_empty_tile_count + status.empty_tile_count ==
                       status.sector_count &&
                   status.triangle_count <=
                       status.unique_vertex_count * 2u &&
                   (status.state != matter::TerrainCollisionState::Installed ||
                    status.non_empty_tile_count == 0 ||
                    status.box3d_retained_bytes != 0) &&
                   status.failure_code.empty() &&
                   status.failure_message.empty();
        case matter::TerrainCollisionState::Failed:
            return !status.failure_code.empty() && !status.failure_message.empty();
    }
    return false;
}

void test_worker_build_app_install_ready_gate_and_identity() {
    std::printf("-- worker_build_app_install_ready_gate_and_identity\n");
    SessionFixture fixture("gate");
    if (!fixture.session) return;
    const std::thread::id app_thread = std::this_thread::get_id();
    std::thread::id build_thread{};
    std::thread::id install_thread{};
    std::atomic<bool> identity_ok{false};
    std::atomic<bool> candidate_visible_at_install{false};
    std::atomic<int> lifetime_observations{0};
    std::atomic<bool> candidate_alive_on_worker{false};
    std::atomic<bool> candidate_released_before_reset{false};
    std::atomic<bool> stop_reader{false};
    std::atomic<bool> bad_snapshot{false};
    fixture.session->set_test_terrain_collision_build_callback(
        [&](const terrain_field::FieldRuntime& field,
            const CanonicalDefinition& definition,
            const std::string& cache_root,
            const std::function<bool()>&,
            TerrainCollisionCandidate& candidate,
            std::string&) {
            build_thread = std::this_thread::get_id();
            identity_ok.store(
                definition.source.field_hash == field.hash() &&
                definition.source.overlay_hash ==
                    (field.height_overlay() ? field.height_overlay()->hash() : 0u) &&
                definition.source.bake_mode_salt == bake_mode::salt() &&
                definition.source.mesher_semantic_version ==
                    terrain_mesher::kSemanticVersion &&
                definition.source.geometry_format_version == 1u &&
                definition.sector_size_m == 16.0f &&
                definition.cell_size_m == 0.5f && definition.rung == 2 &&
                definition.regions.size() == 1u &&
                definition.sectors.size() == 2u &&
                cache_root ==
                    (fixture.root / ".cache" / "TerrainSession").string(),
                std::memory_order_release);
            candidate = candidate_for(definition);
            return true;
        });
    fixture.session->set_test_terrain_collision_publication_hook([&] {
        install_thread = std::this_thread::get_id();
        candidate_visible_at_install.store(
            fixture.session->terrain_collision_status().state ==
                matter::TerrainCollisionState::CandidateReady,
            std::memory_order_release);
    });
    fixture.session->set_test_terrain_collision_candidate_observer(
        [&](std::weak_ptr<
                const matter::terrain_collision::TerrainCollisionCandidate>
                candidate) {
            const int observation = lifetime_observations.fetch_add(
                1, std::memory_order_acq_rel);
            if (observation == 0) {
                candidate_alive_on_worker.store(
                    !candidate.expired(), std::memory_order_release);
            } else if (observation == 1) {
                candidate_released_before_reset.store(
                    candidate.expired(), std::memory_order_release);
            }
        });
    std::thread status_reader([&] {
        while (!stop_reader.load(std::memory_order_acquire)) {
            if (!status_snapshot_is_consistent(
                    fixture.session->terrain_collision_status())) {
                bad_snapshot.store(true, std::memory_order_release);
                break;
            }
        }
    });
    fixture.session->request_bake();
    TerminalEvents events{};
    const bool finished = pump_until(
        *fixture.session,
        [&] { return events.finished == 1 || events.errors != 0; }, events);
    stop_reader.store(true, std::memory_order_release);
    status_reader.join();
    const auto status = fixture.session->terrain_collision_status();
    const auto physics = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    const auto runtime = fixture.session->ecs().get<matter::ecs::WorldRuntimeState>();
    CHECK(finished && events.finished == 1 && events.errors == 0,
          "collision-authored session reaches one successful terminal event");
    CHECK(build_thread != app_thread && install_thread == app_thread,
          "candidate build runs on the worker and Box3D install on the app thread");
    CHECK(identity_ok.load(std::memory_order_acquire),
          "session canonicalizes the exact field, overlay, bake, mesher, sector, rung, and cache identity");
    CHECK(candidate_visible_at_install.load(std::memory_order_acquire),
          "app publication observes CandidateReady before replacing physics");
    CHECK(lifetime_observations.load(std::memory_order_acquire) == 2 &&
              candidate_alive_on_worker.load(std::memory_order_acquire) &&
              candidate_released_before_reset.load(std::memory_order_acquire),
          "candidate ownership expires immediately after collision publication and before visual reset");
    CHECK(status.state == matter::TerrainCollisionState::Installed &&
              status.generation_key == physics.installation_key &&
              status.box3d_retained_bytes == physics.retained_bytes &&
              physics.shape_count == 2u && runtime.status == matter::ecs::WorldStatus::Ready,
          "terrain is Installed before the same generation becomes Ready");
    CHECK(!bad_snapshot.load(std::memory_order_acquire),
          "racing status readers see only self-consistent snapshots");
}

void test_omitted_collision_clears_before_ready() {
    std::printf("-- omitted_collision_clears_before_ready\n");
    SessionFixture fixture("clear", true, 0.51f);
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition& definition,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate& candidate, std::string&) {
            candidate = candidate_for(definition);
            return true;
        });
    fixture.session->request_bake();
    TerminalEvents first{};
    CHECK(pump_until(*fixture.session, [&] { return first.finished == 1; }, first),
          "collision generation A finishes");
    CHECK(matter::physics::detail::context(fixture.session->ecs())
              .terrain_collision_stats().shape_count != 0u,
          "generation A has live terrain physics");
    CHECK(write_world_object(fixture.root, false),
          "generation B removes collision authoring");
    fixture.session->reload();
    TerminalEvents second{};
    CHECK(pump_until(*fixture.session, [&] {
              return second.finished == 1 || second.errors != 0;
          }, second), "collision-free generation B finishes");
    const auto status = fixture.session->terrain_collision_status();
    const auto physics = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    CHECK(second.finished == 1 && second.errors == 0 &&
              status.state == matter::TerrainCollisionState::Disabled &&
              physics.installation_key == 0 && physics.shape_count == 0,
          "omitted collision clears old terrain before B reports Ready");
}

void test_builder_failure_reports_once_and_never_ready() {
    std::printf("-- builder_failure_reports_once_and_never_ready\n");
    SessionFixture fixture("build_failure");
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition&,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate&, std::string& error) {
            error = "injected cache validation failure";
            return false;
        });
    fixture.session->request_bake();
    TerminalEvents events{};
    CHECK(pump_until(*fixture.session, [&] { return events.errors == 1; }, events),
          "builder failure reaches its terminal event");
    for (int i = 0; i != 30; ++i) {
        fixture.session->pump_gpu_jobs(2.0f);
        fixture.session->tick({0.0f, 1.0f / 60.0f, 4});
        matter::Event event{};
        while (fixture.session->poll_event(event)) {
            if (event.type == matter::EventType::BakeFinished) ++events.finished;
            if (event.type == matter::EventType::BakeError &&
                event.code != matter::BakeErrorCode::Cancelled) ++events.errors;
        }
    }
    const auto status = fixture.session->terrain_collision_status();
    const auto runtime = fixture.session->ecs().get<matter::ecs::WorldRuntimeState>();
    CHECK(events.errors == 1 && events.finished == 0 &&
              events.error_code == matter::BakeErrorCode::Internal &&
              events.phase == "terrain-collision",
          "build/cache/validation failure emits one Internal terrain-collision error and no Ready event");
    CHECK(status.state == matter::TerrainCollisionState::Failed &&
              status.failure_code == "build-failed" &&
              status.failure_message.find("injected cache validation failure") !=
                  std::string::npos &&
              runtime.status == matter::ecs::WorldStatus::Failed,
          "builder failure publishes stable failed status and Failed world state");
}

void test_prebuild_exception_uses_collision_failure_path() {
    std::printf("-- prebuild_exception_uses_collision_failure_path\n");
    SessionFixture fixture("prebuild_exception");
    if (!fixture.session) return;
    std::atomic<bool> builder_called{false};
    fixture.session->set_test_terrain_collision_before_build_hook([] {
        throw std::runtime_error("injected terrain prebuild exception");
    });
    fixture.session->set_test_terrain_collision_build_callback(
        [&](const terrain_field::FieldRuntime&, const CanonicalDefinition&,
            const std::string&, const std::function<bool()>&,
            TerrainCollisionCandidate&, std::string&) {
            builder_called.store(true, std::memory_order_release);
            return true;
        });
    fixture.session->request_bake();
    TerminalEvents events{};
    CHECK(pump_until(*fixture.session, [&] { return events.errors == 1; }, events),
          "prebuild exception reaches a collision-specific terminal event");
    for (int i = 0; i != 20; ++i) {
        fixture.session->pump_gpu_jobs(2.0f);
        fixture.session->tick({0.0f, 1.0f / 60.0f, 4});
        matter::Event event{};
        while (fixture.session->poll_event(event)) {
            if (event.type == matter::EventType::BakeFinished) ++events.finished;
            if (event.type == matter::EventType::BakeError &&
                event.code != matter::BakeErrorCode::Cancelled) ++events.errors;
        }
    }
    const auto status = fixture.session->terrain_collision_status();
    const auto runtime = fixture.session->ecs().get<matter::ecs::WorldRuntimeState>();
    CHECK(!builder_called.load(std::memory_order_acquire),
          "prebuild exception occurs before the candidate builder is invoked");
    CHECK(events.errors == 1 && events.finished == 0 &&
              events.error_code == matter::BakeErrorCode::Internal &&
              events.phase == "terrain-collision",
          "prebuild exception emits exactly one phased Internal error and no Ready");
    CHECK(status.state == matter::TerrainCollisionState::Failed &&
              status.failure_code == "build-exception" &&
              status.failure_message.find("injected terrain prebuild exception") !=
                  std::string::npos &&
              runtime.status == matter::ecs::WorldStatus::Failed,
          "prebuild exception records precise collision failure status and Failed world state");
}

void test_throwing_builder_copy_failure_is_atomic_and_disconnects() {
    std::printf("-- throwing_builder_copy_failure_is_atomic_and_disconnects\n");
    SessionFixture fixture("throwing_builder_copy", true, 0.41f);
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition& definition,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate& candidate, std::string&) {
            candidate = candidate_for(definition);
            return true;
        });
    fixture.session->request_bake();
    TerminalEvents baseline_events{};
    CHECK(pump_until(*fixture.session, [&] {
              return baseline_events.finished == 1 ||
                     baseline_events.errors != 0;
          }, baseline_events), "throwing-copy baseline reaches Ready");
    CHECK(fixture.session->connected_for_test() &&
              fixture.session->ecs().get<matter::ecs::WorldRuntimeState>().status ==
                  matter::ecs::WorldStatus::Ready,
          "throwing-copy fixture begins connected and Ready");

    auto copy_state = std::make_shared<ThrowingCopyBuilderState>();
    matter::TerrainCollisionBuildTestCallback throwing_builder{
        ThrowingCopyBuilder{copy_state}};
    fixture.session->set_test_terrain_collision_build_callback(
        std::move(throwing_builder));
    copy_state->armed.store(true, std::memory_order_release);
    fixture.session->set_test_terrain_collision_failure_record_bad_alloc(true);
    CHECK(write_world_object(fixture.root, true, 0.68f),
          "throwing-copy reload changes collision identity");
    fixture.session->reload();
    TerminalEvents events{};
    CHECK(pump_until(*fixture.session, [&] { return events.errors == 1; }, events),
          "throwing callback copy reaches collision-specific terminal failure");
    for (int i = 0; i != 20; ++i) {
        fixture.session->pump_gpu_jobs(2.0f);
        fixture.session->tick({0.0f, 1.0f / 60.0f, 4});
        poll_terminal_events(*fixture.session, events);
    }
    const auto status = fixture.session->terrain_collision_status();
    const auto runtime = fixture.session->ecs().get<matter::ecs::WorldRuntimeState>();
    CHECK(copy_state->copies.load(std::memory_order_acquire) >= 1 &&
              copy_state->invocations.load(std::memory_order_acquire) == 0,
          "failure is raised by copying the callback target before invocation");
    CHECK(events.errors == 1 && events.finished == 0 &&
              events.error_code == matter::BakeErrorCode::Internal &&
              events.phase == "terrain-collision",
          "throwing copy emits exactly one phased Internal error and no Ready");
    CHECK(status.state == matter::TerrainCollisionState::Failed &&
              !status.failure_code.empty() &&
              !status.failure_message.empty() &&
              runtime.status == matter::ecs::WorldStatus::Failed &&
              !fixture.session->connected_for_test(),
          "bad-allocation fallback publishes one coherent Failed snapshot and disconnects prior Ready");
}

void test_app_job_exception_before_install_preserves_prior_and_fails_coherently() {
    std::printf("-- app_job_exception_before_install_preserves_prior_and_fails_coherently\n");
    SessionFixture fixture("app_job_preinstall_exception", true, 0.41f);
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition& definition,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate& candidate, std::string&) {
            candidate = candidate_for(definition);
            return true;
        });
    fixture.session->request_bake();
    TerminalEvents baseline_events{};
    CHECK(pump_until(*fixture.session, [&] {
              return baseline_events.finished == 1 ||
                     baseline_events.errors != 0;
          }, baseline_events), "pre-install exception baseline reaches Ready");
    auto* retained_context = &matter::physics::detail::context(
        fixture.session->ecs());
    const auto baseline = retained_context->terrain_collision_stats();
    CHECK(baseline.installation_key != 0 && baseline.shape_count != 0 &&
              fixture.session->connected_for_test(),
          "pre-install exception baseline collision is installed and connected");

    std::atomic<bool> context_hidden{false};
    fixture.session->set_test_terrain_collision_publication_hook([&] {
        fixture.session->ecs().set<matter::physics::detail::PhysicsContextRef>(
            {nullptr});
        context_hidden.store(true, std::memory_order_release);
    });
    CHECK(write_world_object(fixture.root, true, 0.68f),
          "pre-install exception reload changes collision identity");
    fixture.session->reload();

    const auto candidate_deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < candidate_deadline &&
           fixture.session->terrain_collision_status().state !=
               matter::TerrainCollisionState::CandidateReady) {
        if (fixture.session->has_pending_gpu_jobs_for_test()) {
            fixture.session->pump_gpu_jobs(0.0f);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto queued_deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < queued_deadline &&
           !fixture.session->has_pending_gpu_jobs_for_test()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(fixture.session->terrain_collision_status().state ==
              matter::TerrainCollisionState::CandidateReady &&
              fixture.session->has_pending_gpu_jobs_for_test(),
          "replacement candidate waits in the queued app-thread collision job");

    fixture.session->pump_gpu_jobs(0.0f);
    fixture.session->ecs().set<matter::physics::detail::PhysicsContextRef>(
        {retained_context});
    CHECK(context_hidden.load(std::memory_order_acquire),
          "publication hook removes the physics lookup before the later context access");

    TerminalEvents events{};
    CHECK(pump_until(*fixture.session, [&] { return events.errors == 1; }, events),
          "exception outside the publication-hook catches reaches a terminal error");
    for (int i = 0; i != 20; ++i) {
        fixture.session->pump_gpu_jobs(2.0f);
        fixture.session->tick({0.0f, 1.0f / 60.0f, 4});
        poll_terminal_events(*fixture.session, events);
    }
    const auto after_failure = retained_context->terrain_collision_stats();
    const auto status = fixture.session->terrain_collision_status();
    const auto runtime = fixture.session->ecs().get<matter::ecs::WorldRuntimeState>();
    CHECK(after_failure.installation_key == baseline.installation_key &&
              after_failure.shape_count == baseline.shape_count &&
              after_failure.replacements == baseline.replacements,
          "pre-install app-job exception preserves the prior Box3D generation");
    CHECK(events.errors == 1 && events.finished == 0 &&
              events.error_code == matter::BakeErrorCode::Internal &&
              events.phase == "terrain-collision" && !events.message.empty(),
          "pre-install app-job exception emits one nonempty phased error and no Ready");
    CHECK(status.state == matter::TerrainCollisionState::Failed &&
              !status.failure_code.empty() &&
              !status.failure_message.empty() &&
              runtime.status == matter::ecs::WorldStatus::Failed &&
              !fixture.session->connected_for_test(),
          "pre-install app-job exception atomically publishes Failed and disconnects");
}

void test_empty_publication_exception_uses_nonempty_diagnostic() {
    std::printf("-- empty_publication_exception_uses_nonempty_diagnostic\n");
    SessionFixture fixture("empty_publication_exception");
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition& definition,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate& candidate, std::string&) {
            candidate = candidate_for(definition);
            return true;
        });
    fixture.session->set_test_terrain_collision_publication_hook([] {
        throw EmptyWhatException{};
    });
    fixture.session->request_bake();
    TerminalEvents events{};
    CHECK(pump_until(*fixture.session, [&] { return events.errors == 1; }, events),
          "empty-what publication exception reaches collision failure routing");
    for (int i = 0; i != 20; ++i) {
        fixture.session->pump_gpu_jobs(2.0f);
        fixture.session->tick({0.0f, 1.0f / 60.0f, 4});
        poll_terminal_events(*fixture.session, events);
    }
    const auto status = fixture.session->terrain_collision_status();
    const auto runtime = fixture.session->ecs().get<matter::ecs::WorldRuntimeState>();
    CHECK(events.errors == 1 && events.finished == 0 &&
              events.phase == "terrain-collision" && !events.message.empty(),
          "empty-what exception emits one nonempty terrain-collision error");
    CHECK(status.state == matter::TerrainCollisionState::Failed &&
              !status.failure_code.empty() &&
              !status.failure_message.empty() &&
              runtime.status == matter::ecs::WorldStatus::Failed,
          "empty-what exception leaves a coherent nonempty Failed snapshot");
}

void test_post_commit_log_exception_cannot_reject_installed_generation() {
    std::printf("-- post_commit_log_exception_cannot_reject_installed_generation\n");
    SessionFixture fixture("post_commit_log_exception", true, 0.41f);
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition& definition,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate& candidate, std::string&) {
            candidate = candidate_for(definition);
            return true;
        });
    fixture.session->request_bake();
    TerminalEvents baseline_events{};
    CHECK(pump_until(*fixture.session, [&] {
              return baseline_events.finished == 1 ||
                     baseline_events.errors != 0;
          }, baseline_events), "post-commit exception baseline reaches Ready");
    auto& context = matter::physics::detail::context(fixture.session->ecs());
    const auto baseline = context.terrain_collision_stats();

    ThrowingTerrainInstallLogGuard log_guard;
    throw_terrain_install_log.store(true, std::memory_order_release);
    CHECK(write_world_object(fixture.root, true, 0.68f),
          "post-commit exception reload changes collision identity");
    fixture.session->reload();
    TerminalEvents events{};
    CHECK(pump_until(*fixture.session, [&] {
              return events.finished == 1 || events.errors != 0;
          }, events), "post-commit logging exception reaches a terminal outcome");
    for (int i = 0; i != 20; ++i) {
        fixture.session->pump_gpu_jobs(2.0f);
        fixture.session->tick({0.0f, 1.0f / 60.0f, 4});
        poll_terminal_events(*fixture.session, events);
    }
    const auto installed = context.terrain_collision_stats();
    const auto status = fixture.session->terrain_collision_status();
    const auto runtime = fixture.session->ecs().get<matter::ecs::WorldRuntimeState>();
    CHECK(installed.installation_key != 0 &&
              installed.installation_key != baseline.installation_key &&
              installed.replacements == baseline.replacements + 1u,
          "Box3D replacement commits before the injected install-summary exception");
    CHECK(!throw_terrain_install_log.load(std::memory_order_acquire),
          "throwing install-summary sink is invoked after Box3D commit");
    CHECK(events.errors == 0 && events.finished == 1 &&
              status.state == matter::TerrainCollisionState::Installed &&
              status.generation_key == installed.installation_key &&
              runtime.status == matter::ecs::WorldStatus::Ready &&
              fixture.session->connected_for_test(),
          "post-commit diagnostic failure cannot report the committed generation as failed");
}

void test_transactional_replacement_failure_retains_prior() {
    std::printf("-- transactional_replacement_failure_retains_prior\n");
    SessionFixture fixture("replace", true, 0.41f);
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition& definition,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate& candidate, std::string&) {
            candidate = candidate_for(definition);
            return true;
        });
    fixture.session->request_bake();
    TerminalEvents first{};
    CHECK(pump_until(*fixture.session, [&] { return first.finished == 1; }, first),
          "replacement generation A finishes");
    CHECK(write_world_object(fixture.root, true, 0.62f),
          "replacement generation B changes material identity");
    fixture.session->reload();
    TerminalEvents second{};
    CHECK(pump_until(*fixture.session, [&] { return second.finished == 1; }, second),
          "replacement generation B finishes");
    auto& context = matter::physics::detail::context(fixture.session->ecs());
    const auto retained_b = context.terrain_collision_stats();
    CHECK(retained_b.replacements == 1u && retained_b.installation_key != 0,
          "A to B performs exactly one successful transactional replacement");
    matter::physics::detail::fail_terrain_collision_mesh_create_on_tile_for_test(
        context, 1u);
    CHECK(write_world_object(fixture.root, true, 0.77f),
          "replacement generation C changes material identity");
    fixture.session->reload();
    TerminalEvents third{};
    CHECK(pump_until(*fixture.session, [&] { return third.errors == 1; }, third),
          "injected generation C install failure reaches terminal error");
    const auto after_failure = context.terrain_collision_stats();
    const auto status = fixture.session->terrain_collision_status();
    const auto runtime = fixture.session->ecs().get<matter::ecs::WorldRuntimeState>();
    CHECK(third.errors == 1 && third.finished == 0 &&
              third.error_code == matter::BakeErrorCode::Internal &&
              third.phase == "terrain-collision",
          "install failure emits one Internal terrain-collision error and no Ready");
    CHECK(after_failure.installation_key == retained_b.installation_key &&
              after_failure.shape_count == retained_b.shape_count &&
              after_failure.replacements == retained_b.replacements,
          "failed C preserves all accepted B physics state");
    CHECK(status.state == matter::TerrainCollisionState::Failed &&
              status.failure_code == "install-failed" &&
              runtime.status == matter::ecs::WorldStatus::Failed,
          "failed C leaves collision and world status Failed");
}

void test_cancelled_at_publication_barrier_skips_replacement() {
    std::printf("-- cancelled_at_publication_barrier_skips_replacement\n");
    SessionFixture fixture("cancel_publication_barrier", true, 0.41f);
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition& definition,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate& candidate, std::string&) {
            candidate = candidate_for(definition);
            return true;
        });
    fixture.session->request_bake();
    TerminalEvents baseline_events{};
    CHECK(pump_until(*fixture.session, [&] {
              return baseline_events.finished == 1 ||
                     baseline_events.errors != 0;
          }, baseline_events), "publication-barrier baseline finishes");
    const auto baseline = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    CHECK(baseline.installation_key != 0 && baseline.shape_count != 0,
          "publication-barrier baseline installs collision");

    CHECK(write_world_object(fixture.root, true, 0.68f),
          "publication-barrier generation changes collision identity");
    std::atomic<bool> barrier_reached{false};
    std::atomic<bool> prior_physics_intact{false};
    std::atomic<bool> replacement_world_removed{false};
    fixture.session->set_test_terrain_collision_publication_hook([&] {
        if (barrier_reached.exchange(true, std::memory_order_acq_rel)) return;
        const auto at_barrier = matter::physics::detail::context(
            fixture.session->ecs()).terrain_collision_stats();
        prior_physics_intact.store(
            at_barrier.installation_key == baseline.installation_key &&
                at_barrier.shape_count == baseline.shape_count,
            std::memory_order_release);
        replacement_world_removed.store(
            write_world_object(fixture.root, false),
            std::memory_order_release);
        fixture.session->reload();
    });
    fixture.session->reload();
    TerminalEvents events{};
    CHECK(pump_until(*fixture.session, [&] {
              return events.finished == 1 || events.errors != 0;
          }, events), "superseding barrier generation finishes");
    const auto physics = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    CHECK(barrier_reached.load(std::memory_order_acquire) &&
              prior_physics_intact.load(std::memory_order_acquire) &&
              replacement_world_removed.load(std::memory_order_acquire),
          "cancellation at the publication barrier occurs before replacement");
    CHECK(events.cancelled >= 1 && events.errors == 0 && events.finished == 1 &&
              physics.installation_key == 0 && physics.shape_count == 0 &&
              fixture.session->terrain_collision_status().state ==
                  matter::TerrainCollisionState::Disabled,
          "barrier-cancelled generation never becomes Ready and its successor clears physics");
}

void test_cancelled_after_collision_job_skips_visual_publication() {
    std::printf("-- cancelled_after_collision_job_skips_visual_publication\n");
    SessionFixture fixture("cancel_after_collision_job");
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition& definition,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate& candidate, std::string&) {
            candidate = candidate_for(definition);
            return true;
        });
    std::atomic<bool> after_job_cancelled{false};
    std::atomic<bool> replacement_world_removed{false};
    fixture.session->set_test_terrain_collision_candidate_observer(
        [&](std::weak_ptr<
                const matter::terrain_collision::TerrainCollisionCandidate>
                candidate) {
            if (!candidate.expired() ||
                after_job_cancelled.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            replacement_world_removed.store(
                write_world_object(fixture.root, false),
                std::memory_order_release);
            fixture.session->reload();
        });
    fixture.session->request_bake();
    TerminalEvents events{};
    CHECK(pump_until(*fixture.session, [&] {
              return events.finished == 1 || events.errors != 0;
          }, events), "successor after collision-job cancellation finishes");
    const auto physics = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    CHECK(after_job_cancelled.load(std::memory_order_acquire) &&
              replacement_world_removed.load(std::memory_order_acquire),
          "generation is superseded after its blocking collision job completes");
    CHECK(events.cancelled >= 1 && events.errors == 0 && events.finished == 1 &&
              physics.installation_key == 0 && physics.shape_count == 0 &&
              fixture.session->terrain_collision_status().state ==
                  matter::TerrainCollisionState::Disabled,
          "post-job cancellation skips visual Ready and the successor clears physics");
}

void test_cancelled_queued_collision_job_releases_before_worker_continues() {
    std::printf("-- cancelled_queued_collision_job_releases_before_worker_continues\n");
    SessionFixture fixture("cancel_queued_collision", true, 0.41f);
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition& definition,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate& candidate, std::string&) {
            candidate = candidate_for(definition);
            return true;
        });
    fixture.session->request_bake();
    TerminalEvents baseline_events{};
    CHECK(pump_until(*fixture.session, [&] {
              return baseline_events.finished == 1 ||
                     baseline_events.errors != 0;
          }, baseline_events), "queued-skip baseline reaches Ready");
    const auto baseline = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    CHECK(baseline.installation_key != 0 && baseline.shape_count != 0 &&
              fixture.session->connected_for_test(),
          "queued-skip baseline physics is installed and connected");

    std::atomic<int> observations{0};
    std::atomic<bool> alive_on_worker{false};
    std::atomic<bool> expired_before_worker_continues{false};
    std::atomic<int> publication_hook_calls{0};
    fixture.session->set_test_terrain_collision_candidate_observer(
        [&](std::weak_ptr<
                const matter::terrain_collision::TerrainCollisionCandidate>
                candidate) {
            const int observation = observations.fetch_add(
                1, std::memory_order_acq_rel);
            if (observation == 0) {
                alive_on_worker.store(!candidate.expired(),
                                      std::memory_order_release);
            } else if (observation == 1) {
                expired_before_worker_continues.store(
                    candidate.expired(), std::memory_order_release);
            }
        });
    fixture.session->set_test_terrain_collision_publication_hook([&] {
        publication_hook_calls.fetch_add(1, std::memory_order_acq_rel);
    });
    CHECK(write_world_object(fixture.root, true, 0.68f),
          "queued generation changes collision identity");
    fixture.session->reload();

    // Reload may need earlier install-world GPU work before collision
    // publication. Drain one job at a time only until CandidateReady, then
    // stop pumping and wait for the collision job itself to appear.
    const auto candidate_deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < candidate_deadline &&
           fixture.session->terrain_collision_status().state !=
               matter::TerrainCollisionState::CandidateReady) {
        if (fixture.session->has_pending_gpu_jobs_for_test() &&
            fixture.session->terrain_collision_status().state !=
                matter::TerrainCollisionState::CandidateReady) {
            fixture.session->pump_gpu_jobs(0.0f);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto queued_deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < queued_deadline &&
           !fixture.session->has_pending_gpu_jobs_for_test()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(fixture.session->terrain_collision_status().state ==
              matter::TerrainCollisionState::CandidateReady &&
              fixture.session->has_pending_gpu_jobs_for_test() &&
              observations.load(std::memory_order_acquire) == 1 &&
              alive_on_worker.load(std::memory_order_acquire),
          "candidate is alive while its blocking collision job is queued without pumping");

    fixture.session->set_test_terrain_collision_publication_hook({});
    CHECK(write_world_object(fixture.root, false),
          "successor removes collision before cancelling queued generation");
    fixture.session->reload();
    fixture.session->pump_gpu_jobs(0.0f);
    const auto release_deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < release_deadline &&
           observations.load(std::memory_order_acquire) < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    TerminalEvents events{};
    poll_terminal_events(*fixture.session, events);
    const auto after_skip = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    CHECK(observations.load(std::memory_order_acquire) == 2 &&
              expired_before_worker_continues.load(std::memory_order_acquire) &&
              publication_hook_calls.load(std::memory_order_acquire) == 0,
          "token-skipped queue entry releases its candidate without invoking the job");
    CHECK(after_skip.installation_key == baseline.installation_key &&
              after_skip.shape_count == baseline.shape_count &&
              fixture.session->connected_for_test() && events.finished == 0,
          "cancelled queued generation performs no physics or visual publication before successor action");

    CHECK(pump_until(*fixture.session, [&] {
              return events.finished == 1 || events.errors != 0;
          }, events), "collision-free successor finishes after queued skip");
    const auto final_physics = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    CHECK(events.cancelled >= 1 && events.errors == 0 && events.finished == 1 &&
              final_physics.installation_key == 0 &&
              final_physics.shape_count == 0 &&
              fixture.session->terrain_collision_status().state ==
                  matter::TerrainCollisionState::Disabled,
          "only the successor clears prior physics and reaches Ready");
}

void test_cancelled_build_publishes_no_manifest_or_physics() {
    std::printf("-- cancelled_build_publishes_no_manifest_or_physics\n");
    SessionFixture fixture("cancel");
    if (!fixture.session) return;
    fixture.session->set_test_terrain_collision_build_callback(
        [](const terrain_field::FieldRuntime&, const CanonicalDefinition& definition,
           const std::string&, const std::function<bool()>&,
           TerrainCollisionCandidate& candidate, std::string&) {
            candidate = candidate_for(definition);
            return true;
        });
    fixture.session->request_bake();
    TerminalEvents baseline_events{};
    CHECK(pump_until(*fixture.session, [&] {
              return baseline_events.finished == 1 ||
                     baseline_events.errors != 0;
          }, baseline_events), "accepted baseline collision generation finishes");
    const auto baseline = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    CHECK(baseline.installation_key != 0 && baseline.shape_count != 0,
          "cancellation fixture starts with an active physics generation");
    CHECK(write_world_object(fixture.root, true, 0.68f),
          "candidate generation changes collision installation identity");
    std::mutex mutex;
    std::condition_variable cv;
    bool reached_second_tile = false;
    bool release_second_tile = false;
    std::atomic<std::uint64_t> cancelled_installation_key{0};
    fixture.session->set_test_terrain_collision_build_callback(
        [&](const terrain_field::FieldRuntime& field,
            const CanonicalDefinition& definition,
            const std::string& cache_root,
            const std::function<bool()>& cancelled,
            TerrainCollisionCandidate& candidate,
            std::string& error) {
            cancelled_installation_key.store(
                definition.installation_key, std::memory_order_release);
            matter::terrain_collision::detail::BuildTestHooks hooks{};
            std::atomic<int> tile_number{0};
            hooks.mesh_tile = [&](const terrain_field::FieldRuntime&,
                                  const matter::terrain_collision::SectorCoordinate&,
                                  std::int8_t, float,
                                  terrain_mesher::SectorMesh& mesh,
                                  std::string&) {
                const int number = ++tile_number;
                if (number == 2) {
                    std::unique_lock<std::mutex> lock(mutex);
                    reached_second_tile = true;
                    cv.notify_all();
                    cv.wait(lock, [&] { return release_second_tile; });
                }
                mesh = one_triangle_mesh();
                return true;
            };
            return matter::terrain_collision::detail::
                load_or_build_candidate_with_test_hooks(
                    field, definition, cache_root, cancelled, hooks,
                    candidate, error);
        });
    fixture.session->reload();
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (reached_second_tile) break;
        }
        fixture.session->pump_gpu_jobs(2.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(reached_second_tile, "generation A parks during deterministic tile N");
    }
    const std::uint64_t generation_a =
        cancelled_installation_key.load(std::memory_order_acquire);
    CHECK(generation_a != 0, "cancelled generation exposes its canonical key");
    const auto while_candidate = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    CHECK(while_candidate.installation_key == baseline.installation_key &&
              while_candidate.shape_count == baseline.shape_count,
          "tile-N candidate leaves the accepted physics generation active");
    CHECK(write_world_object(fixture.root, false),
          "generation B removes collision while A is building");
    fixture.session->reload();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_second_tile = true;
    }
    cv.notify_all();
    TerminalEvents events{};
    CHECK(pump_until(*fixture.session, [&] { return events.finished == 1; }, events),
          "superseding collision-free generation B finishes");
    const auto physics = matter::physics::detail::context(
        fixture.session->ecs()).terrain_collision_stats();
    CHECK(!fs::exists(manifest_path(
              fixture.root / ".cache" / "TerrainSession", generation_a)),
          "cancelled tile-N build publishes no generation manifest");
    CHECK(physics.installation_key == 0 && physics.shape_count == 0 &&
              fixture.session->terrain_collision_status().state ==
                  matter::TerrainCollisionState::Disabled,
          "cancelled A changes no physics and B explicitly publishes Disabled");
}

}  // namespace

int main() {
    std::printf("== terrain_collision_session_tests ==\n");
    test_worker_build_app_install_ready_gate_and_identity();
    test_omitted_collision_clears_before_ready();
    test_builder_failure_reports_once_and_never_ready();
    test_prebuild_exception_uses_collision_failure_path();
    test_throwing_builder_copy_failure_is_atomic_and_disconnects();
    test_app_job_exception_before_install_preserves_prior_and_fails_coherently();
    test_empty_publication_exception_uses_nonempty_diagnostic();
    test_post_commit_log_exception_cannot_reject_installed_generation();
    test_transactional_replacement_failure_retains_prior();
    test_cancelled_at_publication_barrier_skips_replacement();
    test_cancelled_after_collision_job_skips_visual_publication();
    test_cancelled_queued_collision_job_releases_before_worker_continues();
    test_cancelled_build_publishes_no_manifest_or_physics();
    return check_summary();
}
