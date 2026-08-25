// Phase B Task 6 — async bake worker command loop tests. Headless (no GL
// context; allow_gl_lt_46=true), tiny tileset-free temp world with a single
// Box.js voxel part placed a handful of times.
//
// The four cases together validate the async event protocol Tasks 8+ and the
// viewer rely on:
//   (a) request_bake_returns_immediately — wall-clock request_bake() < 50 ms
//       on a cache-cold world, and events arrive later during pump.
//   (b) bake_completes_with_finished — full sequence: BakeStarted, one or more
//       BakePartDone with phase=="parts", ending in BakeFinished; the tracer
//       query API reports instance_count() > 0 after.
//   (c) determinism — two fresh caches produce identical
//       {type,module,done,total,phase} sequences.
//   (d) reload_reenters — after (b), reload() drives a second full sequence.
//
// Not run with a display: EngineContext::create(allow_gl_lt_46=true) skips the
// GL 4.6 gate; the tileset-free schema means gpu_run is never invoked; the
// reset job's raster block is skipped in non-gl46 mode; no window is created.

#include "matter/engine_context.h"
#include "matter/scene.h"
#include "matter/world_session.h"
#include "matter/river_runtime.h"
#include "hydrology/hydrology_network_artifact.h"
#include "hydrology/physx_fluid_bake.h"
#include "render/animation_skin_bridge.h"

// Editor-side scene tree cache policy (ImGui-free by design so this suite can
// drive it across a real world switch — see test_warm_session_publishes_graph).
#include "../../MatterEditor/src/scene_tree_model.h"

#include "bake_trace.h"
#include "bake_trace_names.h"

// E3: the session event hub + typed bake events, for the poll_event parity
// / no-frame-pump test (test_e3_poll_event_typed_parity).
#include "matter/event/event_hub.h"
#include "matter/events/bake_events.h"
#include "matter/events/stream_events.h"

#include <chrono>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "check.h"

using clk = std::chrono::steady_clock;
namespace fs = std::filesystem;

// --- Sandbox helpers ------------------------------------------------------

static bool write_file(const fs::path& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << body;
    return f.good();
}

static bool read_file(const fs::path& path, std::string& body) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    body.assign(std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>());
    return file.good() || file.eof();
}

static void remove_tree(const fs::path& path) {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

static bool reset_project(const fs::path& root,
                          const std::string& world_name) {
    remove_tree(root);
    std::error_code ec;
    fs::create_directories(root / "objects", ec);
    if (ec) return false;
    fs::create_directories(root / "worlds", ec);
    if (ec) return false;
    fs::create_directories(root / "shared-lib", ec);
    if (ec) return false;
    fs::create_directories(root / ".cache" / world_name / "parts", ec);
    return !ec;
}

static bool reset_cache(const fs::path& root,
                        const std::string& world_name) {
    remove_tree(root / ".cache");
    std::error_code ec;
    fs::create_directories(root / ".cache" / world_name / "parts", ec);
    return !ec;
}

static bool project_fixture_contract(const fs::path& root,
                                     const std::string& world_name) {
    const bool has_world_source =
        fs::is_regular_file(root / "worlds" / (world_name + ".js"));
    const bool has_no_legacy_world_data = !fs::exists(root / "world_data");
    CHECK(has_world_source,
          "project fixture provides worlds/<Name>.js");
    CHECK(has_no_legacy_world_data,
          "project fixture has no legacy manifest tree");
    return has_world_source && has_no_legacy_world_data;
}

static std::string project_world_root(const std::string& module,
                                      bool expand = false) {
    std::ostringstream root;
    root << "{ module: '" << module << "', "
         << "transform: [1, 0, 0, 0, 0, 1, 0, 0, "
         << "0, 0, 1, 0, 0, 0, 0, 1]";
    if (expand) root << ", expand: true";
    root << " }";
    return root.str();
}

static bool write_project_world(const fs::path& root,
                                const std::string& world_name,
                                const std::vector<std::string>& roots) {
    std::ostringstream source;
    source << "class " << world_name << " extends World {\n"
           << "  static roots = [\n";
    for (const std::string& root_record : roots)
        source << "    " << root_record << ",\n";
    source << "  ];\n"
           << "}\n";
    return write_file(root / "worlds" / (world_name + ".js"), source.str()) &&
           project_fixture_contract(root, world_name);
}

static matter::WorldDesc project_world_desc(const std::string& project_dir,
                                            const char* world_name) {
    return matter::WorldDesc{
        project_dir.c_str(),
        world_name,
        "../shared-lib",
    };
}

// Build a minimal Box project under `root`. Object Box.js is a trivial
// voxel part (a single 0.6 m box brush at origin). Its World class places Box
// three times (no flags -> each root lands at world origin per LocalProvider's
// default placement, but three distinct instance_ids still exercise the
// per-instance publish path). Empty shared-lib.
static bool build_sandbox(const std::string& root) {
    if (!reset_project(root, "Box")) return false;

    // The tiniest mesh Part: a two-triangle floor quad, mirrors FloorDemo.js
    // shape (no voxel session, no shared-lib imports, no children).
    if (!write_file(fs::path(root) / "objects" / "Box.js",
        "class Box extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.5;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0, -S);\n"
        "    this.vertex( S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0,  S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;

    // Three unflagged roots preserve authoring order and produce three publish
    // steps for the single unique part_hash.
    return write_project_world(root, "Box", {
        project_world_root("Box"),
        project_world_root("Box"),
        project_world_root("Box"),
    });
}

static bool build_authored_fluid_sandbox(const fs::path& root) {
    if (!reset_project(root, "FluidAsync")) return false;
    if (!write_file(root / "objects" / "FluidPart.js",
        "class FluidPart extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone); this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(0,0,0); this.vertex(1,0,0); this.vertex(0,1,0);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;
    return write_file(root / "worlds" / "FluidAsync.js",
        "class FluidAsync extends World {\n"
        "  static roots=[{module:'FluidPart',transform:[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}];\n"
        "  hydrology() {\n"
        "    const n=riverNetwork({cellSize:1,seed:7});\n"
        "    const r=n.river('main').inlet([0,8,0],{flow:1})\n"
        "      .curve([[0,8,0],[10,1,0]])\n"
        "      .channelProfile([{at:0,width:4,depth:3,asymmetry:0},"
        "{at:10,width:4,depth:3,asymmetry:0}]);\n"
        "    n.backend('physx');\n"
        "    n.pbd({particleSpacing:.2,restDensity:1000,fixedStep:.01,iterations:4,maxNeighbors:96});\n"
        "    n.limits({batchSteps:8,maxSteps:120,maxParticles:1000});\n"
        "    n.emitter({id:'main-inlet',position:[1,4,1],direction:[1,0,0],initialVelocity:[1,0,0],flow:1,radius:.5,startTime:0,stopTime:1.2});\n"
        "    n.virtualDam({height:4,thickness:.5});\n"
        "    n.fillSensor({upstreamOffset:1,length:1,height:3,resolution:[2,2,2],crestWetFraction:.5,stableWetSteps:1,minimumParticlesPerCell:1});\n"
        "    n.quality({particleRadius:.13,visualVoxel:.5,visualBlendWidth:.05,coarseVoxel:.1,gameplayCell:1,maxVisualParticles:1000,maxGridVertices:100000,maxMeshVertices:100000,maxMeshIndices:300000});\n"
        "    r.section('upper',{from:0,to:8,dryMargin:1}).emitters(['main-inlet']).pool({from:7,to:8,fillLevel:3}).spillway({id:'pool-one',at:8,width:4,effectiveDepth:1,overlap:1,damOffset:.5});\n"
        "    n.bakeSequential(); n.build();\n"
        "  }\n"
        "}\n") && project_fixture_contract(root, "FluidAsync");
}

struct AsyncFluidBackendState {
    std::atomic<int> factory_calls{0};
    std::atomic<int> run_calls{0};
    std::thread::id worker_thread{};
};

class AsyncFluidBackend final : public hydrology::IFluidBakeBackend {
public:
    explicit AsyncFluidBackend(std::shared_ptr<AsyncFluidBackendState> state)
        : state_(std::move(state)) {}

    hydrology::FluidBackendProbe probe() override {
        hydrology::FluidBackendProbe result{};
        result.available = true;
        result.backend_name = "async-fake";
        result.sdk_version = "5.6.1";
        result.device_name = "Async Fake GPU";
        result.code = hydrology::FluidBakeCode::Ready;
        result.sdk_version_hex = 0x05060100u;
        result.cuda_driver_version = 1;
        result.device_luid = {1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
        result.device_luid_valid = true;
        return result;
    }

    bool run(const hydrology::FluidBakeInput& input,
             const hydrology::FluidBakeCallbacks& callbacks,
             hydrology::FluidBakeOutput& output,
             hydrology::FluidBakeError& error) override {
        ++state_->run_calls;
        state_->worker_thread = std::this_thread::get_id();
        if (callbacks.progress) {
            callbacks.progress({1u, input.settings.max_steps, 2u, 0.25f});
            callbacks.progress({2u, input.settings.max_steps, 3u, 0.75f});
        }
        output.particles = {
            {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 2.0f}, 2u},
            {{2.0f, 2.0f, 2.0f}, {0.0f, 0.0f, 1.0f}, 5u},
            {{3.0f, 3.0f, 3.0f}, {0.0f, 0.0f, 3.0f}, 9u},
        };
        output.sensor = {0.75f, 4u, 5u, true, 0.8f, 0.75f, 0.75f, 2u};
        output.stats = {5u, 3u, 3u, 0u, 0u, 0.01};
        output.stats.escape_policy = input.settings.escape_policy;
        output.stats.emitted_particles = 3u;
        output.stats.escape_budget = hydrology::fluid_escape_budget(
            3u, input.settings.escape_policy);
        error = {};
        return true;
    }

private:
    std::shared_ptr<AsyncFluidBackendState> state_;
};

// Snapshot format for determinism comparison.
struct EvRec {
    int type = 0;     // matter::EventType cast to int
    std::string module;
    int done = 0, total = 0;
    std::string phase;
};

struct SkinBindingObservation {
    uint32_t count = 0;
    uint64_t part_hash = 0;
    uint64_t asset_identity = 0;
    uint32_t source_vertex = 0;
    uint32_t influence_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    size_t influence_count = 0;
    bool visible = false;
};

static SkinBindingObservation observe_skin_binding(
    matter::WorldSession& session) {
    SkinBindingObservation observed{};
    session.ecs().each(
        [&observed](flecs::entity,
                    const matter::scene::PartInstance& part,
                    const matter::render::AnimationSkinnedBinding& binding) {
            ++observed.count;
            observed.part_hash = part.part_hash;
            observed.visible = binding.visible;
            if (!binding.asset ||
                binding.lod >= binding.asset->lods.size()) return;
            const auto& lod = binding.asset->lods[binding.lod];
            observed.asset_identity = binding.asset->identity;
            observed.source_vertex = lod.source_vertex;
            observed.influence_vertex = lod.influence_vertex;
            observed.vertex_count = lod.vertex_count;
            observed.first_index = lod.first_index;
            observed.index_count = lod.index_count;
            observed.influence_count = binding.asset->influences
                ? binding.asset->influences->size() : 0;
        });
    return observed;
}

static std::string ev_type_name(matter::EventType t) {
    switch (t) {
        case matter::EventType::BakeStarted:    return "BakeStarted";
        case matter::EventType::BakePartDone:   return "BakePartDone";
        case matter::EventType::BakeFinished:   return "BakeFinished";
        case matter::EventType::BakeError:      return "BakeError";
        // Phase C Task 6: camera-driven refine loop event (append-only).
        case matter::EventType::RefineTileDone: return "RefineTileDone";
    }
    return "?";
}

// Drive one bake to completion by pumping GPU jobs and draining events.
// Returns true on BakeFinished, false on BakeError or timeout.
static bool drive_bake(matter::WorldSession& s, std::vector<EvRec>& log,
                       int timeout_sec = 60) {
    auto deadline = clk::now() + std::chrono::seconds(timeout_sec);
    bool finished = false;
    while (clk::now() < deadline) {
        s.pump_gpu_jobs(4.0f);
        matter::Event ev;
        bool any = false;
        while (s.poll_event(ev)) {
            any = true;
            EvRec r;
            r.type  = (int)ev.type;
            r.module = ev.module;
            r.done  = ev.done;
            r.total = ev.total;
            r.phase = ev.phase;
            log.push_back(r);
            if (ev.type == matter::EventType::BakeFinished) { finished = true; break; }
            if (ev.type == matter::EventType::BakeError) {
                printf("  BakeError: code=%d phase=%s msg=%s\n",
                       (int)ev.code, ev.phase.c_str(), ev.message.c_str());
                return false;
            }
        }
        if (finished) return true;
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    printf("  drive_bake TIMEOUT after %ds\n", timeout_sec);
    return false;
}

// --- (a) request_bake_returns_immediately -------------------------------------
static bool test_returns_immediately(const std::string& sandbox) {
    printf("-- (a) request_bake_returns_immediately\n");
    // Cache-cold: nuke any prior cache so this measures the async return path,
    // not a warm-cache short-circuit that would pass by accident.
    reset_cache(sandbox, "Box");
    std::string err;
    // Keep path strings alive across the create/open_world calls; EngineDesc /
    // WorldDesc hold const char* views, not std::string copies.
    std::string cache_root_s = (fs::path(sandbox) / ".cache").string();
    matter::EngineDesc ed;
    ed.cache_root      = cache_root_s.c_str();
    ed.allow_gl_lt_46  = true;   // headless: skip GL 4.6 gate, skip raster path
    auto engine = matter::EngineContext::create(ed, err);
    CHECK(engine != nullptr, "engine created");
    if (!engine) { printf("  err: %s\n", err.c_str()); return false; }

    matter::WorldDesc wd = project_world_desc(sandbox, "Box");
    auto s = engine->open_world(wd, err);
    CHECK(s != nullptr, "session opened");
    if (!s) { printf("  err: %s\n", err.c_str()); return false; }

    // Cache-cold measurement: request_bake() must NOT block the caller.
    auto t0 = clk::now();
    s->request_bake();
    auto elapsed_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    printf("  request_bake elapsed: %.2f ms\n", elapsed_ms);
    CHECK(elapsed_ms < 50.0, "request_bake returns < 50 ms");

    // Async contract: only BakeStarted (or nothing yet) can have arrived before
    // any pump — the finalizer BakeFinished must NOT have been emitted from
    // the request_bake() call itself. The synchronous implementation drains
    // everything up to BakeFinished before returning; the async worker cannot.
    int pre_pump_events = 0;
    bool pre_pump_finished = false;
    {
        matter::Event ev;
        while (s->poll_event(ev)) {
            ++pre_pump_events;
            if (ev.type == matter::EventType::BakeFinished ||
                ev.type == matter::EventType::BakeError)
                pre_pump_finished = true;
        }
    }
    printf("  events before pump: %d (finished-seen=%d)\n",
           pre_pump_events, (int)pre_pump_finished);
    CHECK(!pre_pump_finished,
          "no BakeFinished/BakeError before pump (bake still in flight)");

    // Events must arrive later while we pump.
    std::vector<EvRec> log;
    bool ok = drive_bake(*s, log);
    CHECK(ok, "bake completed under async pump");
    CHECK(!log.empty(), "at least one event arrived during pump");
    return ok && !log.empty() && !pre_pump_finished;
}

// --- (b) bake_completes_with_finished ---------------------------------------
static bool test_completes_finished(const std::string& sandbox) {
    printf("-- (b) bake_completes_with_finished\n");
    // Fresh cache so we see BakePartDone events for freshly-fetched parts.
    reset_cache(sandbox, "Box");

    std::string err;
    std::string cache_root_s = (fs::path(sandbox) / ".cache").string();
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(ed, err);
    CHECK(engine != nullptr, "engine created");
    if (!engine) return false;

    matter::WorldDesc wd = project_world_desc(sandbox, "Box");
    auto s = engine->open_world(wd, err);
    CHECK(s != nullptr, "session opened");
    if (!s) return false;

    s->request_bake();
    std::vector<EvRec> log;
    bool ok = drive_bake(*s, log);
    CHECK(ok, "bake finished");
    CHECK(!log.empty(), "events recorded");
    if (log.empty()) return false;

    // First event must be BakeStarted; last must be BakeFinished.
    CHECK(log.front().type == (int)matter::EventType::BakeStarted,
          "first event is BakeStarted");
    CHECK(log.back().type == (int)matter::EventType::BakeFinished,
          "last event is BakeFinished");

    // At least one BakePartDone with phase=="parts".
    int parts_count = 0;
    for (const auto& r : log) {
        if (r.type == (int)matter::EventType::BakePartDone && r.phase == "parts")
            ++parts_count;
    }
    printf("  BakePartDone(phase=parts) count: %d\n", parts_count);
    CHECK(parts_count >= 1, "at least one BakePartDone with phase=\"parts\"");

    // Query API should report a positive instance count now.
    uint32_t ic = s->instance_count();
    printf("  instance_count: %u\n", ic);
    CHECK(ic > 0, "instance_count() > 0 after bake");

    // Bake Lab (task 1.2): after BakeFinished, last_bake_trace returns the
    // stage-span tree. reset_cache above nuked .cache (resolve cache included),
    // so this was a full bake: the root's children are exactly the execute_bake
    // stages install, compose, publish, in order, and all spans are closed.
    {
        bake_trace::Span trace;
        s->last_bake_trace(trace);
        CHECK(trace.name && std::strcmp(trace.name, bake_trace::kRootName) == 0,
              "trace root is named \"root\"");
        printf("  bake trace: %zu root children\n", trace.children.size());
        for (const auto& c : trace.children)
            printf("    span %s: %.1f..%.1f ms\n",
                   c.name ? c.name : "(null)", c.begin_ms, c.end_ms);
        CHECK(trace.children.size() == 3,
              "trace root has exactly 3 stage spans (install/compose/publish)");
        if (trace.children.size() == 3) {
            CHECK(std::strcmp(trace.children[0].name, bake_trace::kSpanInstall) == 0,
                  "stage span 0 is \"install\"");
            CHECK(std::strcmp(trace.children[1].name, bake_trace::kSpanCompose) == 0,
                  "stage span 1 is \"compose\"");
            CHECK(std::strcmp(trace.children[2].name, bake_trace::kSpanPublish) == 0,
                  "stage span 2 is \"publish\"");
        }
        for (const auto& c : trace.children) {
            CHECK(c.begin_ms >= 0.0, "stage span begin_ms >= 0");
            CHECK(c.end_ms >= c.begin_ms, "stage span is closed (end >= begin)");
        }
    }

    return true;
}

// --- (c) determinism --------------------------------------------------------
// Run against two fresh caches and diff the {type,module,done,total,phase}
// sequences.
static std::vector<EvRec> run_once_fresh(const std::string& sandbox) {
    reset_cache(sandbox, "Box");
    std::string err;
    std::string cache_root_s = (fs::path(sandbox) / ".cache").string();
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(ed, err);
    std::vector<EvRec> log;
    // A failed create must not crash the whole suite: (c)'s caller reports the
    // empty logs as its own CHECK failures with the run intact.
    if (!engine) return log;
    matter::WorldDesc wd = project_world_desc(sandbox, "Box");
    auto s = engine->open_world(wd, err);
    if (!s) return log;
    s->request_bake();
    drive_bake(*s, log);
    return log;
}

static bool test_determinism(const std::string& sandbox) {
    printf("-- (c) determinism\n");
    auto a = run_once_fresh(sandbox);
    auto b = run_once_fresh(sandbox);
    CHECK(!a.empty() && !b.empty(), "both runs produced events");
    CHECK(a.size() == b.size(), "event counts match");
    printf("  run A: %zu events, run B: %zu events\n", a.size(), b.size());
    if (a.size() != b.size()) return false;
    bool same = true;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].type != b[i].type || a[i].module != b[i].module ||
            a[i].done != b[i].done || a[i].total != b[i].total ||
            a[i].phase != b[i].phase) {
            printf("  DIFF at %zu: A={t=%d m=%s d/t=%d/%d p=%s} B={t=%d m=%s d/t=%d/%d p=%s}\n",
                   i, a[i].type, a[i].module.c_str(), a[i].done, a[i].total, a[i].phase.c_str(),
                   b[i].type, b[i].module.c_str(), b[i].done, b[i].total, b[i].phase.c_str());
            same = false;
        }
    }
    CHECK(same, "event sequences match byte-for-byte");
    return same;
}

// --- (d) reload_reenters ----------------------------------------------------
static bool test_reload_reenters(const std::string& sandbox) {
    printf("-- (d) reload_reenters\n");
    // Warm cache from (b)/(c) is fine; reload should still fire a full sequence.
    std::string err;
    std::string cache_root_s = (fs::path(sandbox) / ".cache").string();
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(ed, err);
    matter::WorldDesc wd = project_world_desc(sandbox, "Box");
    auto s = engine->open_world(wd, err);
    CHECK(s != nullptr, "session opened");
    if (!s) return false;

    // First bake.
    s->request_bake();
    std::vector<EvRec> first;
    CHECK(drive_bake(*s, first), "first bake completed");

    // Reload — must produce a fresh BakeStarted -> BakeFinished.
    s->reload();
    std::vector<EvRec> second;
    CHECK(drive_bake(*s, second), "second (reload) bake completed");
    CHECK(!second.empty() &&
          second.front().type == (int)matter::EventType::BakeStarted,
          "reload begins with BakeStarted");
    CHECK(!second.empty() &&
          second.back().type == (int)matter::EventType::BakeFinished,
          "reload ends with BakeFinished");
    CHECK(s->instance_count() > 0, "world queryable after reload");
    return true;
}

// --- warm_session_publishes_graph (issues/editor-scene-panel-stale) ---------
// A resolve-cache-hit ("warm") launch restored the provider's graph but never
// called publish_graph_snapshot(), so graph_generation() stayed 0 and
// WorldSession::graph_snapshot() returned false for the entire session — the
// editor's scene tree kept whatever it was already showing. The editor half
// of the same defect: the panel's cached generation survives a session
// replacement, and since the counter is per-session (every load stops at 1),
// old and new collide and lock the refresh out even on a cold load. This
// drives the shipped panel policy (scene_tree_model.h) across a real
// cold->warm switch to cover both halves.
static bool test_warm_session_publishes_graph(const std::string& sandbox) {
    printf("-- warm_session_publishes_graph\n");
    reset_cache(sandbox, "Box");

    std::string err;
    std::string cache_root_s = (fs::path(sandbox) / ".cache").string();
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(ed, err);
    CHECK(engine != nullptr, "warm: engine created");
    if (!engine) { printf("  err: %s\n", err.c_str()); return false; }
    matter::WorldDesc wd = project_world_desc(sandbox, "Box");

    viewer::SceneTreeState panel;

    // Cold load: the full install path publishes and the panel sees the root.
    uint64_t cold_gen = 0;
    {
        auto a = engine->open_world(wd, err);
        CHECK(a != nullptr, "warm: cold session opened");
        if (!a) return false;
        a->request_bake();
        std::vector<EvRec> log;
        CHECK(drive_bake(*a, log), "warm: cold bake finished");
        cold_gen = a->graph_generation();
        CHECK(cold_gen > 0, "cold bake published the graph snapshot");
        part_graph_snapshot::Snapshot snap;
        CHECK(a->graph_snapshot(snap) && snap.nodes.count("Box") == 1 &&
                  snap.nodes.at("Box").is_root,
              "cold snapshot names the Box root");
        viewer::sync_scene_tree_graph_cache(panel, a.get());
        CHECK(panel.cached_snapshot.nodes.count("Box") == 1,
              "panel shows the cold world");
        // Session A dies here — the world switch begins.
    }

    auto b = engine->open_world(wd, err);
    CHECK(b != nullptr, "warm: warm session opened");
    if (!b) return false;

    // A frame drawn before the new session's first publish: anything still
    // cached is the dead session's, and its generation may collide with the
    // publish about to happen. The shipped policy must drop it here.
    CHECK(b->graph_generation() == 0, "no publish before the warm bake");
    viewer::sync_scene_tree_graph_cache(panel, b.get());
    CHECK(panel.cached_snapshot.nodes.empty(),
          "pre-publish sync drops the dead session's tree");

    b->request_bake();
    std::vector<EvRec> log;
    CHECK(drive_bake(*b, log), "warm: warm bake finished");

    // Hit guard: a resolve-cache hit skips install+compose, so the trace has
    // a lone publish stage (the full path has all three, see (b) above). If
    // this fires the second load was accidentally cold and proves nothing.
    {
        bake_trace::Span trace;
        b->last_bake_trace(trace);
        bool saw_install = false;
        for (const auto& c : trace.children)
            if (c.name && std::strcmp(c.name, bake_trace::kSpanInstall) == 0)
                saw_install = true;
        CHECK(!saw_install, "second load actually took the resolve-cache hit");
    }

    const uint64_t warm_gen = b->graph_generation();
    CHECK(warm_gen > 0, "warm (cache-hit) bake published the graph snapshot");
    part_graph_snapshot::Snapshot snap;
    CHECK(b->graph_snapshot(snap) && snap.nodes.count("Box") == 1,
          "warm snapshot names the Box root");

    // The collision that made the editor symptom intermittent: one publish
    // per load means the dead session's generation compares equal to the new
    // one. Pinned so a change to the generation scheme re-evaluates the panel
    // policy above.
    CHECK(warm_gen == cold_gen,
          "per-session generations collide across a switch");

    // The panel catches up on the first draw after the publish...
    viewer::sync_scene_tree_graph_cache(panel, b.get());
    CHECK(panel.cached_snapshot.nodes.count("Box") == 1,
          "panel shows the warm world after publish");

    // ...but only because the pre-publish sync above adopted generation 0. A
    // panel that never drew during that window keeps the dead generation and
    // the collision locks it out — which is why clear_app_models resets at
    // the switch seam.
    viewer::SceneTreeState locked;
    locked.cached_graph_gen = cold_gen;
    locked.cached_snapshot.nodes["DeadWorldRoot"] = {};
    viewer::sync_scene_tree_graph_cache(locked, b.get());
    CHECK(locked.cached_snapshot.nodes.count("DeadWorldRoot") == 1 &&
              locked.cached_snapshot.nodes.count("Box") == 0,
          "carried-over generation keeps the dead tree (the seam reset exists for this)");
    viewer::reset_scene_tree_graph_cache(locked);
    viewer::sync_scene_tree_graph_cache(locked, b.get());
    CHECK(locked.cached_snapshot.nodes.count("Box") == 1,
          "seam reset unlocks the refresh despite the collision");

    return true;
}

// --- Task 7 helpers / multi-part sandbox -----------------------------------

// Build a project with multiple distinct objects (Part0..PartN-1) so we have
// enough parts to make a cache-cold bake take measurable time.
// The World class places each object once (no flags).
static bool build_multi_sandbox(const std::string& root, int num_parts) {
    if (!reset_project(root, "Multi")) return false;

    std::vector<std::string> roots;
    for (int i = 0; i < num_parts; ++i) {
        std::string name = "Part" + std::to_string(i);
        // Each object is distinct (uses i as a vertex offset so hashes differ).
        std::ostringstream js;
        js << "class " << name << " extends Part {\n"
           << "  build(p) {\n"
           << "    this.fill(MAT.stone);\n"
           << "    const S = 0.5 + " << i << " * 0.01;\n"
           << "    this.beginShape(SHAPE.triangles);\n"
           << "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
           << "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
           << "    this.endShape();\n"
           << "  }\n"
           << "}\n";
        if (!write_file(fs::path(root) / "objects" / (name + ".js"), js.str()))
            return false;
        roots.push_back(project_world_root(name));
    }
    return write_project_world(root, "Multi", roots);
}

// Like drive_bake but tolerates BakeError events (skip-and-continue tests).
// Returns true if BakeFinished arrived before timeout; fills errors_out with all
// BakeError events observed. also fills log with all events.
struct FullBakeLog {
    std::vector<matter::Event> events;
    bool finished = false;
    int error_count = 0;   // BakeFinished.errors field
};

static bool drive_bake_tolerant(matter::WorldSession& s, FullBakeLog& out,
                                int timeout_sec = 60) {
    auto deadline = clk::now() + std::chrono::seconds(timeout_sec);
    out.finished = false;
    while (clk::now() < deadline) {
        s.pump_gpu_jobs(4.0f);
        matter::Event ev;
        bool any = false;
        while (s.poll_event(ev)) {
            any = true;
            out.events.push_back(ev);
            if (ev.type == matter::EventType::BakeFinished) {
                out.finished = true;
                out.error_count = ev.errors;
                return true;
            }
            if (ev.type == matter::EventType::BakeError) {
                printf("  BakeError: code=%d phase=%s module=%s msg=%s\n",
                       (int)ev.code, ev.phase.c_str(),
                       ev.module.c_str(), ev.message.c_str());
            }
        }
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    printf("  drive_bake_tolerant TIMEOUT after %ds\n", timeout_sec);
    return false;
}

// Helper: open a session on `sandbox` with `world_name` (default "Box").
// Returns nullptr on failure (also prints the error).
static std::unique_ptr<matter::WorldSession> open_session(
    const std::string& sandbox, std::string& err,
    std::unique_ptr<matter::EngineContext>& engine,
    const std::string& world_name = "Box") {
    std::string cache_root_s = (fs::path(sandbox) / ".cache").string();
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    engine = matter::EngineContext::create(ed, err);
    if (!engine) { printf("  engine create failed: %s\n", err.c_str()); return nullptr; }
    matter::WorldDesc wd = project_world_desc(sandbox, world_name.c_str());
    auto s = engine->open_world(wd, err);
    if (!s) { printf("  open_world failed: %s\n", err.c_str()); return nullptr; }
    return s;
}

// --- (e) supersede_cancels_inflight -----------------------------------------
// Start a cache-cold bake with 6+ parts; after the first BakePartDone arrives,
// issue a second request_bake(). Assert either:
//   (a) a Cancelled BakeError appears followed by a new BakeStarted, OR
//   (b) the first bake finished before the supersede landed (inherently racy;
//       accepted as a pass because we can't force the timing).
static bool test_supersede_cancels_inflight(const std::string& sandbox) {
    printf("-- (e) supersede_cancels_inflight\n");

    // Use a multi-part sandbox (6 distinct parts) so the bake is cache-cold
    // and the install phase takes long enough to observe the supersession.
    const std::string multi = sandbox + "_supersede";
    if (!build_multi_sandbox(multi, 6)) {
        printf("  FAIL: build_multi_sandbox\n");
        ++g_failures;
        return false;
    }

    // Nuke any prior cache so the bake is definitely cold.
    reset_cache(multi, "Multi");

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(multi, err, engine, "Multi");
    CHECK(s != nullptr, "supersede: session opened");
    if (!s) { remove_tree(multi); return false; }

    s->request_bake();

    auto deadline = clk::now() + std::chrono::seconds(120);
    bool second_bake_issued = false;
    bool saw_cancelled      = false;
    bool saw_second_started = false;
    bool first_finished     = false;
    int  part_done_count    = 0;
    bool overall_ok         = false;

    while (clk::now() < deadline) {
        s->pump_gpu_jobs(4.0f);
        matter::Event ev;
        while (s->poll_event(ev)) {
            printf("  ev: %s code=%d phase=%s module=%s\n",
                   ev.type == matter::EventType::BakeStarted  ? "BakeStarted"  :
                   ev.type == matter::EventType::BakePartDone ? "BakePartDone" :
                   ev.type == matter::EventType::BakeFinished ? "BakeFinished" :
                   "BakeError",
                   (int)ev.code, ev.phase.c_str(), ev.module.c_str());

            if (ev.type == matter::EventType::BakePartDone && !second_bake_issued) {
                ++part_done_count;
                // Issue supersede after the first BakePartDone.
                s->request_bake();
                second_bake_issued = true;
                printf("  issued second request_bake() after %d BakePartDone\n",
                       part_done_count);
            }
            if (ev.type == matter::EventType::BakeError &&
                ev.code == matter::BakeErrorCode::Cancelled) {
                saw_cancelled = true;
                printf("  saw Cancelled BakeError (supersede worked)\n");
            }
            if (ev.type == matter::EventType::BakeFinished && !second_bake_issued) {
                // First bake finished BEFORE we could supersede (race: acceptable).
                first_finished = true;
                printf("  first bake finished before supersede (race; issuing second now)\n");
                s->request_bake();
                second_bake_issued = true;
            }
            if (ev.type == matter::EventType::BakeStarted && second_bake_issued &&
                !first_finished && !saw_second_started) {
                // This BakeStarted could be part of the first OR second bake.
                // We check for it after the Cancelled event.
                if (saw_cancelled) {
                    saw_second_started = true;
                    printf("  saw second BakeStarted after Cancelled\n");
                }
            }
            if (ev.type == matter::EventType::BakeFinished && second_bake_issued) {
                // Final BakeFinished from the second bake.
                overall_ok = true;
                goto done_supersede;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
done_supersede:
    printf("  second_bake_issued=%d saw_cancelled=%d first_finished_before_supersede=%d overall_ok=%d\n",
           (int)second_bake_issued, (int)saw_cancelled, (int)first_finished, (int)overall_ok);

    CHECK(overall_ok, "supersede: second bake completed");
    // Either the cancellation was observed, or the first bake already finished
    // before we could supersede (both are valid outcomes).
    bool cancellation_or_race = saw_cancelled || first_finished;
    CHECK(cancellation_or_race, "supersede: cancellation observed OR first bake finished before supersede");

    remove_tree(multi);
    return overall_ok && cancellation_or_race;
}

// --- (f) destructor_mid_bake_joins ------------------------------------------
// Start a bake, destroy the session after the first event WITHOUT pumping further.
// The test must complete in < 10 s (deadlock regression guard).
static bool test_destructor_mid_bake_joins(const std::string& sandbox) {
    printf("-- (f) destructor_mid_bake_joins\n");

    reset_cache(sandbox, "Box");

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(sandbox, err, engine);
    CHECK(s != nullptr, "destructor_mid: session opened");
    if (!s) return false;

    s->request_bake();

    // Wait for at least one event so the bake is definitely in flight.
    auto deadline = clk::now() + std::chrono::seconds(30);
    bool got_event = false;
    while (clk::now() < deadline && !got_event) {
        s->pump_gpu_jobs(4.0f);
        matter::Event ev;
        if (s->poll_event(ev)) got_event = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(got_event, "destructor_mid: at least one event before destroy");

    // Destroy the session WITHOUT pumping further. The destructor must join the
    // worker thread within 10 s (deadlock regression).
    auto t0 = clk::now();
    {
        // Destruction happens here. The destructor:
        //   1. commands.shut_down() — cancels token, wakes worker
        //   2. gpu_jobs.shut_down() — unblocks run_blocking waiters
        //   3. worker.join()
        //   4. gpu_jobs.pump(1e9) — drain stragglers on the GL thread
        s.reset();
        engine.reset();
    }
    double elapsed = std::chrono::duration<double>(clk::now() - t0).count();
    printf("  destructor elapsed: %.2fs\n", elapsed);
    CHECK(elapsed < 10.0, "destructor_mid: joins within 10 s");
    return (elapsed < 10.0);
}

// --- (g) oom_injection_skips_part -------------------------------------------
// Inject std::bad_alloc at part_index 1 during the install bake phase.
// Asserts: one BakeError{OutOfMemory}, BakeFinished{errors==1},
//          instance_count() > 0 (the surviving part renders).
static bool test_oom_injection_skips_part(const std::string& sandbox) {
    printf("-- (g) oom_injection_skips_part\n");

    // Build a 2-part sandbox (Part0 + Part1). Fresh cache.
    const std::string multi = sandbox + "_oom";
    if (!build_multi_sandbox(multi, 2)) {
        printf("  FAIL: build_multi_sandbox\n");
        ++g_failures;
        return false;
    }
    reset_cache(multi, "Multi");

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(multi, err, engine, "Multi");
    CHECK(s != nullptr, "oom_inject: session opened");
    if (!s) { remove_tree(multi); return false; }

    // Inject bad_alloc at part_index 1 (second bake in install phase).
    s->set_test_fault_hook([](int idx) {
        if (idx == 1) throw std::bad_alloc();
    });

    s->request_bake();

    FullBakeLog log;
    bool ok = drive_bake_tolerant(*s, log);
    CHECK(ok, "oom_inject: BakeFinished arrived");

    // Count BakeError{OutOfMemory} events.
    int oom_errors = 0;
    for (const auto& ev : log.events) {
        if (ev.type == matter::EventType::BakeError &&
            ev.code == matter::BakeErrorCode::OutOfMemory) {
            ++oom_errors;
            printf("  OOM BakeError: module=%s phase=%s\n",
                   ev.module.c_str(), ev.phase.c_str());
        }
    }
    CHECK(oom_errors >= 1, "oom_inject: at least one OutOfMemory BakeError");
    CHECK(log.error_count == 1, "oom_inject: BakeFinished.errors == 1");
    uint32_t ic = s->instance_count();
    printf("  instance_count after OOM injection: %u\n", ic);
    CHECK(ic > 0, "oom_inject: surviving part has instances (instance_count > 0)");

    remove_tree(multi);
    return ok && oom_errors >= 1 && log.error_count == 1 && ic > 0;
}

// --- (h) broken_script_skips_part -------------------------------------------
// Two roots: one valid (Box.js), one with a JS syntax error (Broken.js).
// Asserts: BakeError{ScriptError, module="Broken"}, BakeFinished{errors==1},
//          instance_count() > 0 (Box still queryable).
static bool test_broken_script_skips_part(const std::string& sandbox) {
    printf("-- (h) broken_script_skips_part\n");

    // Build a sandbox with Box.js (valid) + Broken.js (syntax error).
    const std::string broot = sandbox + "_broken";
    if (!reset_project(broot, "Broken2")) return false;

    // Valid part: same box quad as the main sandbox.
    if (!write_file(fs::path(broot) / "objects" / "ValidPart.js",
        "class ValidPart extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.5;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) { remove_tree(broot); return false; }

    // Broken part: JS syntax error (unmatched brace).
    if (!write_file(fs::path(broot) / "objects" / "BrokenPart.js",
        "class BrokenPart extends Part {\n"
        "  build(p) { this.fill(MAT.stone;\n"  // missing closing paren
        "}")) { remove_tree(broot); return false; }

    if (!write_project_world(broot, "Broken2", {
            project_world_root("ValidPart"),
            project_world_root("BrokenPart"),
        })) { remove_tree(broot); return false; }

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(broot, err, engine, "Broken2");
    CHECK(s != nullptr, "broken_script: session opened");
    if (!s) { remove_tree(broot); return false; }

    s->request_bake();

    FullBakeLog log;
    bool ok = drive_bake_tolerant(*s, log);
    CHECK(ok, "broken_script: BakeFinished arrived");

    // Find BakeError{ScriptError} naming BrokenPart.
    bool saw_script_error = false;
    for (const auto& ev : log.events) {
        if (ev.type == matter::EventType::BakeError &&
            ev.code == matter::BakeErrorCode::ScriptError) {
            printf("  ScriptError BakeError: module=%s phase=%s\n",
                   ev.module.c_str(), ev.phase.c_str());
            if (ev.module.find("BrokenPart") != std::string::npos ||
                ev.module.find("Broken") != std::string::npos)
                saw_script_error = true;
        }
    }
    CHECK(saw_script_error, "broken_script: BakeError{ScriptError} naming BrokenPart");
    CHECK(log.error_count == 1, "broken_script: BakeFinished.errors == 1");
    uint32_t ic = s->instance_count();
    printf("  instance_count after broken script: %u\n", ic);
    CHECK(ic > 0, "broken_script: ValidPart instances queryable (instance_count > 0)");

    remove_tree(broot);
    return ok && saw_script_error && log.error_count == 1 && ic > 0;
}

// --- (i) load_failure_skips_part --------------------------------------------
// Verifies that a load failure in the publish phase (get_or_load returns null,
// or hook throws) skips that part but lets the bake finish with the remaining
// parts still published.
//
// Hook design (deterministic): the hook fires once per part in the INSTALL
// phase (RecordingBaker::bake, idx=0..N-1) and once per part in the PUBLISH
// phase (publish job, idx=0..N-1). We count visits to index 1: the first visit
// (install) is allowed to pass; the SECOND visit (publish) throws. This forces
// exactly one load-phase BakeError (phase="parts") while Part0 (idx=0) still
// publishes successfully.
//
// World: 2-part sandbox (Part0 + Part1). Part0 publishes normally; Part1's
// publish-phase hook fires after install has completed for both parts.
static bool test_load_failure_skips_part(const std::string& sandbox) {
    printf("-- (i) load_failure_skips_part\n");

    const std::string multi = sandbox + "_loadfail";
    if (!build_multi_sandbox(multi, 2)) {
        printf("  FAIL: build_multi_sandbox\n");
        ++g_failures;
        return false;
    }
    reset_cache(multi, "Multi");

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(multi, err, engine, "Multi");
    CHECK(s != nullptr, "load_fail: session opened");
    if (!s) { remove_tree(multi); return false; }

    // Stateful hook: track visits to idx=1. First visit (install phase) passes;
    // second visit (publish phase) throws std::runtime_error → IoError.
    auto visits_at_1 = std::make_shared<int>(0);
    s->set_test_fault_hook([visits_at_1](int idx) {
        if (idx == 1) {
            ++(*visits_at_1);
            if (*visits_at_1 >= 2) {
                // Second visit: publish phase — inject a load failure.
                throw std::runtime_error("injected load failure for part 1");
            }
            // First visit: install phase — allow it to pass.
        }
    });

    s->request_bake();

    FullBakeLog log;
    bool ok = drive_bake_tolerant(*s, log);
    CHECK(ok, "load_fail: BakeFinished arrived");

    // Find BakeError with phase="parts".
    int parts_errors = 0;
    bool saw_io_error = false;
    for (const auto& ev : log.events) {
        if (ev.type == matter::EventType::BakeError && ev.phase == "parts") {
            ++parts_errors;
            if (ev.code == matter::BakeErrorCode::IoError)
                saw_io_error = true;
            printf("  BakeError(parts): code=%d module=%s msg=%s\n",
                   (int)ev.code, ev.module.c_str(), ev.message.c_str());
        }
    }
    CHECK(parts_errors >= 1, "load_fail: at least one BakeError with phase=\"parts\"");
    CHECK(saw_io_error, "load_fail: BakeError code is IoError");
    CHECK(log.error_count == 1, "load_fail: BakeFinished.errors == 1");
    uint32_t ic = s->instance_count();
    printf("  instance_count after load failure: %u\n", ic);
    CHECK(ic > 0, "load_fail: surviving part has instances (instance_count > 0)");

    remove_tree(multi);
    return ok && parts_errors >= 1 && saw_io_error && log.error_count == 1 && ic > 0;
}

// --- Task 3 tests (Phase C): set_bake_focus + distance-ordered publish ------

// Build a sandbox with three distinct leaf objects (BoxA, BoxB, BoxC) and a
// parent "World" that places them at (0,0,0), (100,0,0), (200,0,0) and is
// flagged `expand` so the children become world entries.
// Focus near C (200,0,0) → BakePartDone order: C, B, A.
// Focus near A (0,0,0)   → BakePartDone order: A, B, C.
static bool build_focus_sandbox(const std::string& root) {
    if (!reset_project(root, "FocusWorld")) return false;

    // Three distinct leaf objects. Geometry is the same shape but the class
    // name makes each one a unique module → distinct resolved hash.
    const char* leaf_tmpl =
        "class %s extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.5;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0, -S);\n"
        "    this.vertex( S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0,  S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n";

    char buf[1024];
    for (const char* name : {"BoxA", "BoxB", "BoxC"}) {
        std::snprintf(buf, sizeof(buf), leaf_tmpl, name);
        if (!write_file(fs::path(root) / "objects" / (std::string(name) + ".js"),
                        std::string(buf)))
            return false;
    }

    // Parent that places the three children at distinct X offsets so world
    // entries carry different translations. The root's `expand` flag promotes
    // each child to a first-class world instance with its placement transform.
    if (!write_file(fs::path(root) / "objects" / "World.js",
        "class World extends Part {\n"
        "  static requires = [\n"
        "    { module: 'BoxA' },\n"
        "    { module: 'BoxB' },\n"
        "    { module: 'BoxC' },\n"
        "  ];\n"
        "  build(p) {\n"
        "    this.pushMatrix();\n"
        "    this.translate(0, 0, 0);\n"
        "    this.placeChild('BoxA');\n"
        "    this.popMatrix();\n"
        "    this.pushMatrix();\n"
        "    this.translate(100, 0, 0);\n"
        "    this.placeChild('BoxB');\n"
        "    this.popMatrix();\n"
        "    this.pushMatrix();\n"
        "    this.translate(200, 0, 0);\n"
        "    this.placeChild('BoxC');\n"
        "    this.popMatrix();\n"
        "  }\n"
        "}\n")) return false;

    // `expand` preserves the former world-root semantics and child order.
    return write_project_world(root, "FocusWorld", {
        project_world_root("World", true),
    });
}

// Collect BakePartDone module names (phase=="parts") in arrival order for one
// bake drive. Returns empty vector on error.
static std::vector<std::string> collect_partdone_modules(matter::WorldSession& s,
                                                          int timeout_sec = 60) {
    std::vector<std::string> order;
    auto deadline = clk::now() + std::chrono::seconds(timeout_sec);
    bool finished = false;
    while (clk::now() < deadline && !finished) {
        s.pump_gpu_jobs(4.0f);
        matter::Event ev;
        bool any = false;
        while (s.poll_event(ev)) {
            any = true;
            if (ev.type == matter::EventType::BakePartDone && ev.phase == "parts") {
                order.push_back(ev.module);
            }
            if (ev.type == matter::EventType::BakeFinished) { finished = true; break; }
            if (ev.type == matter::EventType::BakeError) {
                printf("  BakeError: code=%d phase=%s msg=%s\n",
                       (int)ev.code, ev.phase.c_str(), ev.message.c_str());
                return {};
            }
        }
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!finished) { printf("  collect_partdone_modules TIMEOUT\n"); return {}; }
    return order;
}

// (k) focus_orders_publish
// Verifies that set_bake_focus() reorders BakePartDone by ascending distance
// from the focus point. Uses the FocusWorld sandbox where:
//   BoxA is at (0, 0, 0), BoxB at (100, 0, 0), BoxC at (200, 0, 0).
static bool test_focus_orders_publish(const std::string& sandbox) {
    printf("-- (k) focus_orders_publish\n");

    const std::string froot = sandbox + "_focus";
    if (!build_focus_sandbox(froot)) {
        printf("  FAIL: build_focus_sandbox\n");
        ++g_failures;
        return false;
    }

    // Fresh cache for the first run.
    reset_cache(froot, "FocusWorld");

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(froot, err, engine, "FocusWorld");
    CHECK(s != nullptr, "focus: session opened");
    if (!s) { remove_tree(froot); return false; }

    // --- focus near C (200,0,0): expected order C,B,A -----------------------
    {
        float fc[3] = {200.f, 0.f, 0.f};
        s->set_bake_focus(fc);
        s->request_bake();
        auto order = collect_partdone_modules(*s);
        printf("  [focus near C] order:");
        for (const auto& m : order) printf(" %s", m.c_str());
        printf("\n");
        CHECK(order.size() == 3, "focus near C: got 3 BakePartDone(parts)");
        if (order.size() == 3) {
            CHECK(order[0] == "BoxC", "focus near C: first is BoxC");
            CHECK(order[1] == "BoxB", "focus near C: second is BoxB");
            CHECK(order[2] == "BoxA", "focus near C: third is BoxA");
        }
    }

    // --- focus near A (0,0,0): expected order A,B,C; also tests repeatability
    // Reload to get a fresh publish pass (cache will be warm → hits only).
    {
        float fa[3] = {0.f, 0.f, 0.f};
        s->set_bake_focus(fa);
        s->reload();
        auto order = collect_partdone_modules(*s);
        printf("  [focus near A] order:");
        for (const auto& m : order) printf(" %s", m.c_str());
        printf("\n");
        CHECK(order.size() == 3, "focus near A: got 3 BakePartDone(parts)");
        if (order.size() == 3) {
            CHECK(order[0] == "BoxA", "focus near A: first is BoxA");
            CHECK(order[1] == "BoxB", "focus near A: second is BoxB");
            CHECK(order[2] == "BoxC", "focus near A: third is BoxC");
        }
    }

    // --- Repeatability: same focus→same order on a second reload --------------
    {
        float fc[3] = {200.f, 0.f, 0.f};
        s->set_bake_focus(fc);
        s->reload();
        auto order2 = collect_partdone_modules(*s);
        printf("  [focus near C, repeat] order:");
        for (const auto& m : order2) printf(" %s", m.c_str());
        printf("\n");
        CHECK(order2.size() == 3, "focus near C repeat: 3 events");
        if (order2.size() == 3) {
            CHECK(order2[0] == "BoxC", "focus near C repeat: first is BoxC");
            CHECK(order2[1] == "BoxB", "focus near C repeat: second is BoxB");
            CHECK(order2[2] == "BoxA", "focus near C repeat: third is BoxA");
        }
    }

    remove_tree(froot);
    return true;
}

// --- Task 10 tests -----------------------------------------------------------

#ifdef __linux__

// (j) live_edit_inotify_e2e
// enable_live_edit=true on the Box sandbox. Full bake. Record instance_count +
// a raycast part_hash. Rewrite Box.js changing geometry size. Pump tick() +
// pump_gpu_jobs() up to 30 s. Assert:
//   - a BakeFinished (or BakeStarted) arrives without calling reload().
//   - the raycast part_hash CHANGED (new resolved hash).
// Fail-closed sub-case: write a syntax error into Box.js -> BakeError{code:ScriptError}
// arrives; world still queryable with the OLD hash; fix the file -> recovers.
static bool test_live_edit_inotify_e2e(const std::string& sandbox) {
    printf("-- (j) live_edit_inotify_e2e\n");

    // Fresh cache.
    reset_cache(sandbox, "Box");

    std::string err;
    std::string cache_root_s = (fs::path(sandbox) / ".cache").string();

    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(ed, err);
    CHECK(engine != nullptr, "live_edit_e2e: engine created");
    if (!engine) { printf("  err: %s\n", err.c_str()); return false; }

    matter::WorldDesc wd = project_world_desc(sandbox, "Box");
    wd.enable_live_edit = true;
    auto s = engine->open_world(wd, err);
    CHECK(s != nullptr, "live_edit_e2e: session opened");
    if (!s) { printf("  err: %s\n", err.c_str()); return false; }

    // 1) Initial bake.
    s->request_bake();
    std::vector<EvRec> first_log;
    bool initial_ok = drive_bake(*s, first_log, 60);
    CHECK(initial_ok, "live_edit_e2e: initial bake completed");
    if (!initial_ok) return false;

    // Record initial state.
    uint32_t ic_before = s->instance_count();
    printf("  instance_count before edit: %u\n", ic_before);
    CHECK(ic_before > 0, "live_edit_e2e: initial instances > 0");

    // Raycast to get the part_hash before the edit.
    uint64_t hash_before = 0;
    {
        float org[3] = {0.0f, 2.0f, 0.0f};
        float dir[3] = {0.0f,-1.0f, 0.0f};
        matter::RayHit hit;
        if (s->raycast(org, dir, 100.0f, hit))
            hash_before = hit.part_hash;
        else
            // No hit — just grab the first instance's hash as a stand-in.
            for (uint32_t i = 0; i < ic_before && hash_before == 0; ++i) {
                matter::InstanceInfo info;
                if (s->instance_info(i, info)) hash_before = info.part_hash;
            }
    }
    printf("  part_hash before edit: %llu\n", (unsigned long long)hash_before);

    // 2) Rewrite Box.js with a different size (changes the resolved hash).
    write_file(fs::path(sandbox) / "objects" / "Box.js",
        "class Box extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.6;\n"   // changed from 0.5 to 0.6
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0, -S);\n"
        "    this.vertex( S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0,  S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n");
    printf("  Box.js rewritten (S=0.6)\n");

    // 3) Pump tick + gpu_jobs for up to 30 s waiting for BakeFinished.
    bool saw_cone_finished = false;
    {
        auto deadline = clk::now() + std::chrono::seconds(30);
        while (clk::now() < deadline && !saw_cone_finished) {
            s->tick(matter::TickDesc{0.0f});
            s->pump_gpu_jobs(4.0f);
            matter::Event ev;
            while (s->poll_event(ev)) {
                printf("  ev: %s code=%d phase=%s module=%s\n",
                       ev_type_name((matter::EventType)ev.type).c_str(),
                       (int)ev.code, ev.phase.c_str(), ev.module.c_str());
                if (ev.type == matter::EventType::BakeFinished)
                    saw_cone_finished = true;
                if (ev.type == matter::EventType::BakeError)
                    printf("  BakeError: %s\n", ev.message.c_str());
            }
            if (!saw_cone_finished)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(saw_cone_finished, "live_edit_e2e: BakeFinished arrived without reload()");
    if (!saw_cone_finished) return false;

    // Check that the part_hash changed.
    uint64_t hash_after = 0;
    {
        float org[3] = {0.0f, 2.0f, 0.0f};
        float dir[3] = {0.0f,-1.0f, 0.0f};
        matter::RayHit hit;
        if (s->raycast(org, dir, 100.0f, hit))
            hash_after = hit.part_hash;
        else
            for (uint32_t i = 0, ic = s->instance_count(); i < ic && hash_after == 0; ++i) {
                matter::InstanceInfo info;
                if (s->instance_info(i, info)) hash_after = info.part_hash;
            }
    }
    printf("  part_hash after edit: %llu\n", (unsigned long long)hash_after);
    CHECK(hash_after != 0, "live_edit_e2e: hash_after is non-zero");
    CHECK(hash_after != hash_before, "live_edit_e2e: part_hash CHANGED after live edit");

    // 4) Fail-closed sub-case: write a syntax error.
    uint64_t hash_after_break = hash_after;
    write_file(fs::path(sandbox) / "objects" / "Box.js",
        "class Box extends Part {\n"
        "  build(p) { this.fill(MAT.stone;\n"  // syntax error: missing )
        "}\n");
    printf("  Box.js broken (syntax error)\n");

    bool saw_script_error = false;
    {
        auto deadline = clk::now() + std::chrono::seconds(30);
        while (clk::now() < deadline && !saw_script_error) {
            s->tick(matter::TickDesc{0.0f});
            s->pump_gpu_jobs(4.0f);
            matter::Event ev;
            while (s->poll_event(ev)) {
                printf("  ev: %s code=%d phase=%s\n",
                       ev_type_name((matter::EventType)ev.type).c_str(),
                       (int)ev.code, ev.phase.c_str());
                if (ev.type == matter::EventType::BakeError &&
                    ev.code == matter::BakeErrorCode::ScriptError)
                    saw_script_error = true;
            }
            if (!saw_script_error)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(saw_script_error, "live_edit_e2e: BakeError{ScriptError} on syntax error");

    // World still queryable with the old hash (fail-closed).
    {
        float org[3] = {0.0f, 2.0f, 0.0f};
        float dir[3] = {0.0f,-1.0f, 0.0f};
        matter::RayHit hit;
        uint64_t hash_during_broken = 0;
        if (s->raycast(org, dir, 100.0f, hit))
            hash_during_broken = hit.part_hash;
        else
            for (uint32_t i = 0, ic = s->instance_count(); i < ic && hash_during_broken == 0; ++i) {
                matter::InstanceInfo info;
                if (s->instance_info(i, info)) hash_during_broken = info.part_hash;
            }
        printf("  part_hash during broken: %llu (expected %llu)\n",
               (unsigned long long)hash_during_broken,
               (unsigned long long)hash_after_break);
        CHECK(hash_during_broken == hash_after_break,
              "live_edit_e2e: world queryable with last-good hash during syntax error");
    }

    // 5) Fix the file -> recovers.
    write_file(fs::path(sandbox) / "objects" / "Box.js",
        "class Box extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.7;\n"   // different from both v1 and v2
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0, -S);\n"
        "    this.vertex( S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0,  S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n");
    printf("  Box.js fixed (S=0.7)\n");

    bool saw_recovery = false;
    {
        auto deadline = clk::now() + std::chrono::seconds(30);
        while (clk::now() < deadline && !saw_recovery) {
            s->tick(matter::TickDesc{0.0f});
            s->pump_gpu_jobs(4.0f);
            matter::Event ev;
            while (s->poll_event(ev)) {
                printf("  ev: %s code=%d phase=%s\n",
                       ev_type_name((matter::EventType)ev.type).c_str(),
                       (int)ev.code, ev.phase.c_str());
                if (ev.type == matter::EventType::BakeFinished)
                    saw_recovery = true;
            }
            if (!saw_recovery)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(saw_recovery, "live_edit_e2e: BakeFinished after fixing syntax error (recovery)");

    // After recovery, hash changed again.
    {
        uint64_t hash_recovered = 0;
        float org[3] = {0.0f, 2.0f, 0.0f};
        float dir[3] = {0.0f,-1.0f, 0.0f};
        matter::RayHit hit;
        if (s->raycast(org, dir, 100.0f, hit))
            hash_recovered = hit.part_hash;
        else
            for (uint32_t i = 0, ic = s->instance_count(); i < ic && hash_recovered == 0; ++i) {
                matter::InstanceInfo info;
                if (s->instance_info(i, info)) hash_recovered = info.part_hash;
            }
        printf("  part_hash after recovery: %llu\n", (unsigned long long)hash_recovered);
        CHECK(hash_recovered != hash_after_break,
              "live_edit_e2e: part_hash changed again after recovery");
    }

    return saw_cone_finished && saw_script_error && saw_recovery;
}

#endif // __linux__

// --- (l) regenerate_seed_reroll -------------------------------------------
// Task 7 (Phase C): WorldSession::regenerate(seed) — root-params override reload.
//
// Sandbox: Box.js gains `static params = {worldSeed: 1}` and uses worldSeed in
// its geometry so that the part hash depends on the seed.  Three sub-cases:
//   1. Initial bake: parts_baked >= 1.
//   2. regenerate(2): new seed → cache miss → parts_baked >= 1.
//   3. regenerate(2) again: same seed → warm reload → parts_baked == 0.
static bool build_seed_sandbox(const std::string& root) {
    if (!reset_project(root, "SeedBox")) return false;

    // Box.js: static params = {worldSeed: 1} so the hash differs per seed.
    // Geometry size is derived from worldSeed so different seeds produce
    // different hashes (merge_params_canonical folds params into the hash).
    if (!write_file(fs::path(root) / "objects" / "Box.js",
        "class Box extends Part {\n"
        "  static params = { worldSeed: 1 };\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.4 + (p.worldSeed % 10) * 0.01;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0, -S);\n"
        "    this.vertex( S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0,  S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;

    return write_project_world(root, "SeedBox", {
        project_world_root("Box"),
    });
}

static bool test_regenerate_seed_reroll(const std::string& sandbox) {
    printf("-- (l) regenerate_seed_reroll\n");

    const std::string sroot = sandbox + "_seed";
    if (!build_seed_sandbox(sroot)) {
        printf("  FAIL: build_seed_sandbox\n");
        ++g_failures;
        return false;
    }

    // Cold cache: both seed=1 and seed=2 must be genuine misses initially.
    reset_cache(sroot, "SeedBox");

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(sroot, err, engine, "SeedBox");
    CHECK(s != nullptr, "regenerate: session opened");
    if (!s) { remove_tree(sroot); return false; }

    // 1) Initial bake (seed=1 from static params default).
    s->request_bake();
    std::vector<EvRec> log1;
    bool ok1 = drive_bake(*s, log1);
    CHECK(ok1, "regenerate: initial bake completed");
    uint32_t pb1 = s->frame_stats().parts_baked;
    printf("  [seed=1 initial] parts_baked=%u\n", pb1);
    CHECK(pb1 >= 1, "regenerate: initial bake has parts_baked >= 1 (cache miss)");

    // 2) regenerate(2) → different seed → cache miss → re-bakes terrain analog.
    s->regenerate(2);
    std::vector<EvRec> log2;
    bool ok2 = drive_bake(*s, log2);
    CHECK(ok2, "regenerate: seed=2 bake completed");
    uint32_t pb2 = s->frame_stats().parts_baked;
    printf("  [seed=2] parts_baked=%u\n", pb2);
    CHECK(pb2 >= 1, "regenerate: seed=2 re-baked (cache miss, parts_baked >= 1)");

    // 3) regenerate(2) again → same seed → warm reload → cache hit → parts_baked == 0.
    s->regenerate(2);
    std::vector<EvRec> log3;
    bool ok3 = drive_bake(*s, log3);
    CHECK(ok3, "regenerate: seed=2 repeat bake completed");
    uint32_t pb3 = s->frame_stats().parts_baked;
    uint32_t ch3 = s->frame_stats().cache_hits;
    printf("  [seed=2 repeat] parts_baked=%u cache_hits=%u\n", pb3, ch3);
    CHECK(pb3 == 0, "regenerate: same seed is a warm reload (parts_baked == 0)");
    CHECK(ch3 >= 1, "regenerate: same seed has cache_hits >= 1");

    remove_tree(sroot);
    return ok1 && ok2 && ok3 && pb1 >= 1 && pb2 >= 1 && pb3 == 0 && ch3 >= 1;
}

static bool test_production_animated_gallery_binding() {
    printf("-- production AnimatedRigGallery runtime binding\n");
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    const fs::path project_root =
        fs::temp_directory_path() /
        ("me3_animated_gallery_" + std::to_string(stamp));
    if (!reset_project(project_root, "AnimatedRigGallery"))
        return false;
    const fs::path example_root = fs::absolute("../../projects/world_demo");
    std::error_code ec;
    fs::copy_file(
        example_root / "scenes" / "AnimatedRigGallery" / "objects" / "AnimatedRigGallery.js",
        project_root / "objects" / "AnimatedRigGallery.js",
        fs::copy_options::overwrite_existing, ec);
    if (!ec)
        fs::copy_file(
            example_root / "objects" / "Crate.js",
            project_root / "objects" / "Crate.js",
            fs::copy_options::overwrite_existing, ec);
    if (!ec)
        fs::copy_file(
            example_root / "scenes" / "AnimatedRigGallery" / "AnimatedRigGallery.js",
            project_root / "worlds" / "AnimatedRigGallery.js",
            fs::copy_options::overwrite_existing, ec);
    std::string gallery_part_source;
    const bool copied =
        !ec && read_file(
                   project_root / "objects" / "AnimatedRigGallery.js",
                   gallery_part_source);
    CHECK(copied, "shipped gallery integration project copied verbatim");
    if (!copied) {
        remove_tree(project_root);
        return false;
    }

    const auto world_source = [](const char* part, bool include_entity,
                                 bool visible) {
        std::ostringstream source;
        source
            << "class AnimatedRigGallery extends World {\n"
            << "  static roots = [{ module: 'Crate', transform: "
               "[4,0,0,0, 0,0.1,0,-2.5, 0,0,4,0, 0,0,0,1] }];\n"
            << "  static entities = [";
        if (include_entity) {
            source
                << "{ id: 'animated-rig-gallery', "
                << "name: 'Animated Rig Gallery', components: { "
                << "LocalTransform: { translation: [0,0,0] }, "
                << "PartInstance: { part: '" << part
                << "', visible: " << (visible ? "true" : "false")
                << " } } }";
        }
        source << "];\n}\n";
        return source.str();
    };
    const auto tick_and_snapshot =
        [](matter::WorldSession& session,
           std::vector<matter::AnimationDebugInstanceSnapshot>& snapshots) {
            for (int frame = 0; frame != 4; ++frame)
                session.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
            return session.animation_debug_snapshots(snapshots);
        };
    const auto exact_gallery =
        [](const std::vector<matter::AnimationDebugInstanceSnapshot>& values) {
            if (values.size() != 1) return false;
            const auto& asset = values.front().asset;
            std::set<uint64_t> rigid(
                asset.rigid_part_hashes.begin(),
                asset.rigid_part_hashes.end());
            return asset.resolved_hash != 0 && asset.joints.size() == 21 &&
                   asset.targets.size() == 4 &&
                   !asset.lod0_influences.empty() &&
                   asset.rigid_part_hashes.size() == 3 &&
                   rigid.size() == 3 && rigid.count(0) == 0 &&
                   values.front().pose.model_pose.size() == 21;
        };

    std::string err;
    const std::string cache_text = (project_root / ".cache").string();
    matter::EngineDesc engine_desc;
    engine_desc.cache_root = cache_text.c_str();
    engine_desc.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(engine_desc, err);
    CHECK(engine != nullptr, "gallery integration engine created");
    if (!engine) {
        remove_tree(project_root);
        return false;
    }

    const std::string project = project_root.string();
    const std::string shared =
        fs::absolute("../shared-lib").string();
    matter::WorldDesc world_desc{
        project.c_str(), "AnimatedRigGallery", shared.c_str()};
    auto cold = engine->open_world(world_desc, err);
    CHECK(cold != nullptr, "gallery production cold world opened");
    if (!cold) {
        printf("  open_world failed: %s\n", err.c_str());
        remove_tree(project_root);
        return false;
    }

    uint64_t cold_range_part = 0;
    uint32_t cold_range_calls = 0;
    cold->set_test_animation_raster_range_resolver(
        [&cold_range_part, &cold_range_calls](
            uint64_t part_hash, matter::AnimationRasterRange& range) {
            cold_range_part = part_hash;
            ++cold_range_calls;
            range = {1200, 1000000, 3400, 1000000};
            return true;
        });
    cold->request_bake();
    std::vector<EvRec> cold_events;
    const bool cold_baked = drive_bake(*cold, cold_events, 180);
    std::vector<matter::AnimationDebugInstanceSnapshot> cold_snapshots;
    const bool cold_enumerated =
        cold_baked && tick_and_snapshot(*cold, cold_snapshots);
    CHECK(cold_baked && cold_enumerated && exact_gallery(cold_snapshots),
          "shipped 21-joint gallery creates real rigid and skinned runtime bindings");
    const uint64_t cold_asset_hash =
        cold_snapshots.empty() ? 0 : cold_snapshots.front().asset.resolved_hash;
    const SkinBindingObservation cold_skin =
        observe_skin_binding(*cold);
    CHECK(cold_range_calls > 0 && cold_skin.count == 1 &&
              cold_range_part == cold_skin.part_hash &&
              cold_skin.asset_identity == cold_asset_hash &&
              cold_skin.source_vertex == 1200 &&
              cold_skin.influence_vertex == 0 &&
              cold_skin.vertex_count > 0 &&
              cold_skin.influence_count >=
                  cold_skin.influence_vertex + cold_skin.vertex_count &&
              cold_skin.first_index == 3400 &&
              cold_skin.index_count > 0,
          "cold gallery uses production skin reconciliation with injected global raster ranges");
    CHECK(cold->animation_runtime_stats().active_instances == 1 &&
              cold->animation_runtime_stats().active_assets == 1,
          "cold gallery owns exactly one animator and immutable animation asset");
    cold.reset();

    // A second WorldSession against the same cache exercises restore_from_cache,
    // including the entity-only animation root absent from World.roots.
    auto warm = engine->open_world(world_desc, err);
    CHECK(warm != nullptr, "gallery production warm world opened");
    if (!warm) {
        remove_tree(project_root);
        return false;
    }
    uint64_t warm_range_part = 0;
    uint32_t warm_range_calls = 0;
    warm->set_test_animation_raster_range_resolver(
        [&warm_range_part, &warm_range_calls](
            uint64_t part_hash, matter::AnimationRasterRange& range) {
            warm_range_part = part_hash;
            ++warm_range_calls;
            range = {5600, 1000000, 7800, 1000000};
            return true;
        });
    warm->request_bake();
    std::vector<EvRec> warm_events;
    const bool warm_baked = drive_bake(*warm, warm_events, 120);
    std::vector<matter::AnimationDebugInstanceSnapshot> warm_snapshots;
    const bool warm_enumerated =
        warm_baked && tick_and_snapshot(*warm, warm_snapshots);
    CHECK(warm_baked && warm->frame_stats().parts_baked == 0,
          "same-cache second WorldSession restores gallery without rebaking");
    CHECK(warm_enumerated && exact_gallery(warm_snapshots) &&
              warm_snapshots.front().asset.resolved_hash == cold_asset_hash,
          "warm restore republishes the entity-only shipped animation asset");
    const SkinBindingObservation warm_skin =
        observe_skin_binding(*warm);
    CHECK(warm_range_calls > 0 && warm_skin.count == 1 &&
              warm_range_part == warm_skin.part_hash &&
              warm_skin.asset_identity == cold_asset_hash &&
              warm_skin.source_vertex == 5600 &&
              warm_skin.influence_vertex == 0 &&
              warm_skin.vertex_count > 0 &&
              warm_skin.influence_count >=
                  warm_skin.influence_vertex + warm_skin.vertex_count &&
              warm_skin.first_index == 7800 &&
              warm_skin.index_count > 0,
          "warm gallery executes the same production skin reconciliation contract");

    const fs::path world_path =
        project_root / "worlds" / "AnimatedRigGallery.js";
    CHECK(write_file(
              world_path,
              world_source("AnimatedRigGallery", true, false)),
          "gallery visibility variant written");
    warm->reload();
    std::vector<EvRec> hidden_events;
    const bool hidden_baked = drive_bake(*warm, hidden_events, 120);
    std::vector<matter::AnimationDebugInstanceSnapshot> hidden_snapshots;
    CHECK(hidden_baked &&
              tick_and_snapshot(*warm, hidden_snapshots) &&
              exact_gallery(hidden_snapshots) &&
              !hidden_snapshots.front().visible &&
              observe_skin_binding(*warm).count == 1 &&
              !observe_skin_binding(*warm).visible,
          "authored visibility changes refresh the live animation binding");

    CHECK(write_file(
              world_path,
              world_source("AnimatedRigGallery", false, true)),
          "gallery entity-removal variant written");
    warm->reload();
    std::vector<EvRec> removed_events;
    const bool removed_baked = drive_bake(*warm, removed_events, 120);
    std::vector<matter::AnimationDebugInstanceSnapshot> removed_snapshots;
    CHECK(removed_baked &&
              tick_and_snapshot(*warm, removed_snapshots) &&
              removed_snapshots.empty() &&
              observe_skin_binding(*warm).count == 0 &&
              warm->animation_runtime_stats().active_instances == 0 &&
              warm->animation_runtime_stats().active_assets == 0,
          "entity removal releases animator and immutable animation asset ownership");

    std::string alternate_source = gallery_part_source;
    const std::string declaration =
        "class AnimatedRigGallery extends Part";
    const size_t declaration_at = alternate_source.find(declaration);
    if (declaration_at != std::string::npos)
        alternate_source.replace(
            declaration_at, declaration.size(),
            "class AnimatedRigGalleryAlt extends Part");
    const bool alternate_written =
        declaration_at != std::string::npos &&
        write_file(
            project_root / "objects" / "AnimatedRigGalleryAlt.js",
            alternate_source);
    CHECK(alternate_written, "exact gallery replacement module written");
    CHECK(write_file(
              world_path,
              world_source("AnimatedRigGalleryAlt", true, true)),
          "gallery replacement world written");
    warm->reload();
    std::vector<EvRec> replacement_events;
    const bool replacement_baked =
        drive_bake(*warm, replacement_events, 180);
    std::vector<matter::AnimationDebugInstanceSnapshot>
        replacement_snapshots;
    const bool replacement_enumerated =
        replacement_baked &&
        tick_and_snapshot(*warm, replacement_snapshots);
    CHECK(replacement_enumerated &&
              exact_gallery(replacement_snapshots) &&
              replacement_snapshots.front().asset.resolved_hash !=
                  cold_asset_hash &&
              observe_skin_binding(*warm).count == 1 &&
              observe_skin_binding(*warm).asset_identity ==
                  replacement_snapshots.front().asset.resolved_hash &&
              observe_skin_binding(*warm).source_vertex == 5600 &&
              observe_skin_binding(*warm).first_index == 7800 &&
              warm->animation_runtime_stats().active_assets == 1,
          "full gallery replacement publishes a new bounded animation asset");

    bool swaps_bounded = true;
    for (int swap = 0; swap != 2; ++swap) {
        const char* module =
            swap == 0 ? "AnimatedRigGallery" : "AnimatedRigGalleryAlt";
        swaps_bounded &=
            write_file(world_path, world_source(module, true, true));
        warm->reload();
        std::vector<EvRec> swap_events;
        swaps_bounded &=
            drive_bake(*warm, swap_events, 180);
        std::vector<matter::AnimationDebugInstanceSnapshot> swap_snapshots;
        swaps_bounded &=
            tick_and_snapshot(*warm, swap_snapshots) &&
            exact_gallery(swap_snapshots) &&
            observe_skin_binding(*warm).count == 1 &&
            observe_skin_binding(*warm).asset_identity ==
                swap_snapshots.front().asset.resolved_hash &&
            warm->animation_runtime_stats().active_instances == 1 &&
            warm->animation_runtime_stats().active_assets == 1;
    }
    CHECK(swaps_bounded,
          "repeated full gallery swaps keep runtime asset ownership bounded");
    warm.reset();
    engine.reset();
    remove_tree(project_root);
    return cold_baked && cold_enumerated && warm_baked &&
           warm_enumerated && hidden_baked && removed_baked &&
           replacement_enumerated && swaps_bounded;
}

// --- (m) E3 — poll_event typed-parity + no-frame-pump shim -------------------
// Proves the E3 legacy compat shim (event-system.md S I.11 / S II.4 item 6):
//   1. The ~40 emit sites now emit TYPED events onto the session hub; a typed
//      subscriber added via WorldSession::events() observes them.
//   2. poll_event() returns the identical legacy Event sequence — one per call,
//      FIFO (BakeStarted -> BakePartDone... -> BakeFinished) — reconstructed
//      losslessly from the typed events by the private lane::legacy_poll subs.
//   3. It works with NO frame-loop app-lane pump: the drive loop below calls
//      ONLY pump_gpu_jobs (GL work) and poll_event — never hub.pump(lane::app),
//      which E4 adds. poll_event owns and pump_one()s lane::legacy_poll itself.
static bool test_e3_poll_event_typed_parity(const std::string& sandbox) {
    printf("-- (m) e3_poll_event_typed_parity\n");
    reset_cache(sandbox, "Box");
    std::string err;
    std::string cache_root_s = (fs::path(sandbox) / ".cache").string();
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(ed, err);
    CHECK(engine != nullptr, "e3: engine created");
    if (!engine) return false;
    matter::WorldDesc wd = project_world_desc(sandbox, "Box");
    auto s = engine->open_world(wd, err);
    CHECK(s != nullptr, "e3: session opened");
    if (!s) return false;

    // Immediate typed subscribers on the SAME session hub the legacy shim
    // drains. Immediate delivery runs on the emitter thread and observes every
    // emit; the Box fixture is a clean happy-path world whose bake events are
    // all emitted serially on the worker thread, so the immediate order equals
    // the FIFO order poll_event drains. Collect the same 5 fields drive_bake
    // records (type/module/done/total/phase) so the two streams are comparable.
    std::mutex tmu;
    std::vector<EvRec> typed_seq;
    auto push_typed = [&](const EvRec& r) {
        std::lock_guard<std::mutex> lk(tmu);
        typed_seq.push_back(r);
    };
    matter::evt::SubscriptionSet subs;
    subs += s->events().must_subscribe<matter::events::BakeStarted>(
        "test.e3.started", matter::evt::immediate,
        [&](const matter::events::BakeStarted&) {
            EvRec r; r.type = (int)matter::EventType::BakeStarted; push_typed(r);
        });
    subs += s->events().must_subscribe<matter::events::BakePartDone>(
        "test.e3.part_done", matter::evt::immediate,
        [&](const matter::events::BakePartDone& e) {
            EvRec r; r.type = (int)matter::EventType::BakePartDone;
            r.module = e.module; r.done = e.done; r.total = e.total; r.phase = e.phase;
            push_typed(r);
        });
    subs += s->events().must_subscribe<matter::events::BakeFinished>(
        "test.e3.finished", matter::evt::immediate,
        [&](const matter::events::BakeFinished&) {
            EvRec r; r.type = (int)matter::EventType::BakeFinished; push_typed(r);
        });
    subs += s->events().must_subscribe<matter::events::BakeError>(
        "test.e3.error", matter::evt::immediate,
        [&](const matter::events::BakeError& e) {
            EvRec r; r.type = (int)matter::EventType::BakeError;
            r.module = e.module; r.phase = e.phase; push_typed(r);
        });

    // The hub is wired (registry sees the legacy_poll + test subscribers).
    CHECK(!s->events().registry_snapshot().empty(),
          "e3: registry_snapshot non-empty (session hub wired)");

    s->request_bake();

    // Drive to completion with ONLY pump_gpu_jobs + poll_event (NO app-lane
    // pump anywhere — the S II.4 item 6 standalone proof). Count polls to prove
    // one-event-per-call.
    std::vector<EvRec> legacy_seq;
    int  poll_returns = 0;
    bool finished = false;
    auto deadline = clk::now() + std::chrono::seconds(60);
    while (clk::now() < deadline && !finished) {
        s->pump_gpu_jobs(4.0f);
        matter::Event ev;
        bool any = false;
        while (s->poll_event(ev)) {
            any = true;
            ++poll_returns;
            EvRec r;
            r.type = (int)ev.type; r.module = ev.module;
            r.done = ev.done; r.total = ev.total; r.phase = ev.phase;
            legacy_seq.push_back(r);
            if (ev.type == matter::EventType::BakeFinished) { finished = true; break; }
            if (ev.type == matter::EventType::BakeError)
                printf("  e3 BakeError phase=%s msg=%s\n", ev.phase.c_str(), ev.message.c_str());
        }
        if (!any && !finished) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    CHECK(finished, "e3: BakeFinished arrived via poll_event with NO app-lane pump");
    CHECK(poll_returns == (int)legacy_seq.size(),
          "e3: poll_event is one-event-per-call (poll count == events drained)");
    CHECK(!legacy_seq.empty() &&
          legacy_seq.front().type == (int)matter::EventType::BakeStarted,
          "e3: legacy sequence starts with BakeStarted");
    CHECK(!legacy_seq.empty() &&
          legacy_seq.back().type == (int)matter::EventType::BakeFinished,
          "e3: legacy sequence ends with BakeFinished");
    int legacy_parts = 0;
    for (const auto& r : legacy_seq)
        if (r.type == (int)matter::EventType::BakePartDone && r.phase == "parts")
            ++legacy_parts;
    CHECK(legacy_parts >= 1, "e3: legacy sequence has >=1 BakePartDone(parts)");

    // Cross-thread owner barrier: the immediate lambdas capture test locals, so
    // quiesce them before comparing / tearing down (event-system.md S I.5/I.6).
    subs.unsubscribe_all_and_wait();

    // Compare the typed-subscriber stream to the legacy poll stream, both
    // truncated at the first BakeFinished (post-finished deferred tileset events
    // are excluded from both). Field-identical == proof the typed events carry
    // exactly what the legacy Event needed AND that a hub subscriber sees the
    // same sequence poll_event returns.
    std::vector<EvRec> typed_trunc;
    {
        std::lock_guard<std::mutex> lk(tmu);
        for (const auto& r : typed_seq) {
            typed_trunc.push_back(r);
            if (r.type == (int)matter::EventType::BakeFinished) break;
        }
    }
    CHECK(typed_trunc.size() == legacy_seq.size(),
          "e3: typed subscriber saw the same number of events as poll_event");
    bool elementwise = typed_trunc.size() == legacy_seq.size();
    for (size_t i = 0; elementwise && i < legacy_seq.size(); ++i) {
        const auto& a = typed_trunc[i];
        const auto& b = legacy_seq[i];
        if (a.type != b.type || a.module != b.module || a.done != b.done ||
            a.total != b.total || a.phase != b.phase)
            elementwise = false;
    }
    CHECK(elementwise,
          "e3: typed-subscriber sequence == legacy poll_event sequence (field-identical)");

    return finished;
}

// Task 7 fluid lifecycle proof in the general async suite. The focused PhysX
// contract suite exercises cache, failure, LUID mismatch, and supersession;
// this case keeps the core worker/event promise visible beside the editor's
// other request_bake() guarantees.
static bool test_authored_fluid_uses_worker_and_gpu_job_seam(
    const std::string& sandbox) {
    printf("-- authored_fluid_uses_worker_and_gpu_job_seam\n");
    const fs::path root = fs::path(sandbox).string() + "_fluid";
    if (!build_authored_fluid_sandbox(root)) {
        CHECK(false, "async fluid fixture created");
        return false;
    }
    const std::string cache_root = (root / ".cache").string();
    matter::EngineDesc engine_desc{};
    engine_desc.cache_root = cache_root.c_str();
    engine_desc.allow_gl_lt_46 = true;
    std::string error;
    auto engine = matter::EngineContext::create(engine_desc, error);
    CHECK(engine != nullptr, "async fluid engine created");
    if (!engine) { remove_tree(root); return false; }
    const std::string project_dir = root.string();
    matter::WorldDesc world_desc = project_world_desc(project_dir, "FluidAsync");
    auto session = engine->open_world(world_desc, error);
    CHECK(session != nullptr, "async authored-fluid session opened");
    if (!session) { remove_tree(root); return false; }

    auto backend_state = std::make_shared<AsyncFluidBackendState>();
    const std::thread::id caller_thread = std::this_thread::get_id();
    std::thread::id visual_thread{};
    int visual_calls = 0;
    session->set_test_fluid_bake_dependencies(
        [backend_state] {
            ++backend_state->factory_calls;
            return std::make_shared<AsyncFluidBackend>(backend_state);
        },
        [&](const gpu_meshing::ParticleJob& job,
            gpu_meshing::MeshResult& mesh, gpu_meshing::Stats&,
            gpu_meshing::Error&, const gpu_meshing::BuildControl&) {
            ++visual_calls;
            visual_thread = std::this_thread::get_id();
            mesh.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                              0.0f, 1.0f, 0.0f};
            mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                            0.0f, 0.0f, 1.0f};
            mesh.indices = {0u, 1u, 2u};
            mesh.material = job.material;
            mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
            return true;
        });
    session->set_test_fluid_renderer_luid(
        {1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u});
    CHECK(backend_state->factory_calls.load() == 0,
          "async authored fluid backend remains lazy before request_bake");
    session->request_bake();
    std::vector<EvRec> events;
    const bool finished = drive_bake(*session, events, 60);
    int hydrology_progress = 0;
    for (const EvRec& event : events)
        if (event.type == static_cast<int>(matter::EventType::BakePartDone) &&
            event.phase == "hydrology")
            ++hydrology_progress;
    const matter::HydrologyStatus status = session->hydrology_status();
    CHECK(finished && session->instance_count() > 0,
          "async authored fluid bake preserves and publishes dry world content");
    CHECK(backend_state->factory_calls.load() == 1 &&
              backend_state->run_calls.load() == 1 &&
              backend_state->worker_thread != caller_thread,
          "requested PhysX work runs once on the existing bake worker");
    CHECK(visual_calls == 1 && visual_thread == caller_thread,
          "particle visual work runs once through the app-thread GPU job seam");
    CHECK(hydrology_progress >= 3 &&
              status.state == matter::HydrologyState::Ready &&
              status.progress == 1.0f &&
              session->has_accepted_fluid_artifact_for_test(),
          "thread-safe hydrology progress reaches Ready only with an accepted artifact");
    remove_tree(root);
    return finished;
}

static bool test_cancelled_fluid_generation_publishes_neither_half(
    const std::string& sandbox) {
    printf("-- cancelled_fluid_generation_publishes_neither_half\n");
    const fs::path root = fs::path(sandbox).string() + "_fluid_publication";
    if (!build_authored_fluid_sandbox(root)) {
        CHECK(false, "fluid publication fixture created");
        return false;
    }
    const std::string cache_root = (root / ".cache").string();
    matter::EngineDesc engine_desc{};
    engine_desc.cache_root = cache_root.c_str();
    engine_desc.allow_gl_lt_46 = true;
    std::string error;
    auto engine = matter::EngineContext::create(engine_desc, error);
    CHECK(engine != nullptr, "fluid publication engine created");
    if (!engine) { remove_tree(root); return false; }
    const std::string project_dir = root.string();
    auto session = engine->open_world(
        project_world_desc(project_dir, "FluidAsync"), error);
    CHECK(session != nullptr, "fluid publication session opened");
    if (!session) { remove_tree(root); return false; }

    auto backend_state = std::make_shared<AsyncFluidBackendState>();
    session->set_test_fluid_bake_dependencies(
        [backend_state] {
            ++backend_state->factory_calls;
            return std::make_shared<AsyncFluidBackend>(backend_state);
        },
        [](const gpu_meshing::ParticleJob& job,
           gpu_meshing::MeshResult& mesh, gpu_meshing::Stats&,
           gpu_meshing::Error&, const gpu_meshing::BuildControl&) {
            mesh.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                              0.0f, 1.0f, 0.0f};
            mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                            0.0f, 0.0f, 1.0f};
            mesh.indices = {0u, 1u, 2u};
            mesh.material = job.material;
            mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
            return true;
        });
    session->set_test_fluid_renderer_luid(
        {1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u});

    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    int before_calls = 0;
    int after_calls = 0;
    bool release_before_a = false;
    bool release_after_a = false;
    session->set_test_fluid_before_publication_hook([&] {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        ++before_calls;
        barrier_cv.notify_all();
        if (before_calls == 1)
            barrier_cv.wait(lock, [&] { return release_before_a; });
    });
    session->set_test_fluid_after_publication_hook([&] {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        ++after_calls;
        barrier_cv.notify_all();
        if (after_calls == 1)
            barrier_cv.wait(lock, [&] { return release_after_a; });
    });

    session->request_bake();
    const auto wait_for_barrier = [&](const auto& predicate) {
        const auto deadline = clk::now() + std::chrono::seconds(60);
        while (clk::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(barrier_mutex);
                if (predicate()) return true;
            }
            session->pump_gpu_jobs(4.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    CHECK(wait_for_barrier([&] { return before_calls == 1; }),
          "generation A parks before the publication boundary");
    std::string world_source;
    const fs::path world_path = root / "worlds" / "FluidAsync.js";
    CHECK(read_file(world_path, world_source),
          "generation B source was readable");
    const std::size_t seed = world_source.find("seed:7");
    CHECK(seed != std::string::npos, "generation A seed was found");
    if (seed != std::string::npos) world_source.replace(seed, 6u, "seed:8");
    CHECK(write_file(world_path, world_source),
          "generation B changes accepted network identity");
    session->reload();
    CHECK(!session->river_runtime_binding() &&
              !session->has_accepted_fluid_artifact_for_test(),
          "requesting B exposes neither A CPU binding nor A render gate");
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        release_before_a = true;
    }
    barrier_cv.notify_all();
    CHECK(wait_for_barrier([&] { return after_calls == 1; }),
          "cancelled A reaches the commit-or-skip observation point");
    CHECK(!session->river_runtime_binding() &&
              !session->has_accepted_fluid_artifact_for_test(),
          "cancelled A publishes neither CPU nor render state");
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        release_after_a = true;
    }
    barrier_cv.notify_all();

    FullBakeLog events;
    const bool finished = drive_bake_tolerant(*session, events, 60);
    const auto binding = session->river_runtime_binding();
    CHECK(finished && binding &&
              session->has_accepted_fluid_artifact_for_test(),
          "generation B atomically publishes CPU and render visibility");

    hydrology::HydrologyNetworkArtifact accepted_manifest{};
    bool found_manifest = false;
    std::error_code iterator_error;
    for (fs::recursive_directory_iterator it(root / ".cache", iterator_error),
         end; !iterator_error && it != end; it.increment(iterator_error)) {
        if (!it->is_regular_file() || it->path().extension() != ".mhyn")
            continue;
        std::ifstream stream(it->path(), std::ios::binary);
        std::vector<std::uint8_t> bytes(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        hydrology::HydrologyNetworkArtifact candidate{};
        gpu_meshing::Error manifest_error{};
        if (hydrology::deserialize_network_artifact(
                bytes, candidate, manifest_error) && binding &&
            candidate.payload_digest == binding->generation()) {
            accepted_manifest = std::move(candidate);
            found_manifest = true;
            break;
        }
    }
    CHECK(found_manifest && binding &&
              binding->generation() == accepted_manifest.payload_digest &&
              binding->runtime_digest() ==
                  accepted_manifest.runtime_field_digest &&
              binding->presentation_digest() ==
                  accepted_manifest.presentation_field_digest,
          "B binding generation and field digests match its accepted manifest");

    bool sampled = false;
    matter::Float3 wet_position{};
    if (binding) {
        for (float z = -10.0f; z <= 20.0f && !sampled; z += 0.5f) {
            for (float x = -10.0f; x <= 20.0f && !sampled; x += 0.5f) {
                matter::RiverFieldSample field{};
                if (binding->sample({x, 0.0f, z}, field)) {
                    wet_position = {x, 0.0f, z};
                    sampled = field.wet_valid &&
                              std::isfinite(field.surface_position_m.y) &&
                              std::isfinite(field.surface_normal.y) &&
                              field.surface_normal.y > 0.0f &&
                              std::isfinite(field.velocity_mps.x) &&
                              std::isfinite(field.velocity_mps.y) &&
                              std::isfinite(field.velocity_mps.z) &&
                              field.turbulence >= 0.0f &&
                              field.turbulence <= 1.0f &&
                              field.aeration >= 0.0f &&
                              field.aeration <= 1.0f &&
                              field.foam_potential >= 0.0f &&
                              field.foam_potential <= 1.0f;
                }
            }
        }
    }
    CHECK(sampled,
          "published runtime binding combines finite gameplay and presentation channels");

    int replacement_before_calls = 0;
    int replacement_after_calls = 0;
    bool release_replacement_before = false;
    bool release_replacement_after = false;
    session->set_test_fluid_before_publication_hook([&] {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        ++replacement_before_calls;
        barrier_cv.notify_all();
        if (replacement_before_calls == 1)
            barrier_cv.wait(lock, [&] { return release_replacement_before; });
    });
    session->set_test_fluid_after_publication_hook([&] {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        ++replacement_after_calls;
        barrier_cv.notify_all();
        if (replacement_after_calls == 1)
            barrier_cv.wait(lock, [&] { return release_replacement_after; });
    });
    const std::size_t seed_b = world_source.find("seed:8");
    CHECK(seed_b != std::string::npos, "accepted B seed was found");
    if (seed_b != std::string::npos)
        world_source.replace(seed_b, 6u, "seed:9");
    CHECK(write_file(world_path, world_source),
          "generation C changes accepted network identity");
    session->reload();
    CHECK(wait_for_barrier([&] { return replacement_before_calls == 1; }),
          "generation C parks before replacing accepted B");
    matter::RiverFieldSample retained_sample{};
    CHECK(binding && binding->sample(wet_position, retained_sample),
          "accepted B remains sampleable while C is only a candidate");

    const std::size_t seed_c = world_source.find("seed:9");
    CHECK(seed_c != std::string::npos, "candidate C seed was found");
    if (seed_c != std::string::npos)
        world_source.replace(seed_c, 6u, "seed:10");
    CHECK(write_file(world_path, world_source),
          "generation D supersedes candidate C");
    session->reload();
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        release_replacement_before = true;
    }
    barrier_cv.notify_all();
    CHECK(wait_for_barrier([&] { return replacement_after_calls == 1; }),
          "cancelled C reaches its commit-or-skip observation point");
    retained_sample = {};
    CHECK(session->river_runtime_binding() == binding && binding &&
              binding->sample(wet_position, retained_sample),
          "cancelled C leaves retained accepted B's lease valid");
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        release_replacement_after = true;
    }
    barrier_cv.notify_all();
    FullBakeLog replacement_events;
    const bool replacement_finished =
        drive_bake_tolerant(*session, replacement_events, 60);
    const auto replacement_binding = session->river_runtime_binding();
    retained_sample.wet_valid = true;
    CHECK(replacement_finished && replacement_binding &&
              replacement_binding != binding &&
              replacement_binding->generation() != binding->generation() &&
              !binding->sample(wet_position, retained_sample) &&
              !retained_sample.wet_valid,
          "successful D atomically invalidates a caller-retained B binding");

    session->set_test_fluid_before_publication_hook({});
    session->set_test_fluid_after_publication_hook({});
    session->set_test_fluid_bake_dependencies(
        [backend_state] {
            ++backend_state->factory_calls;
            return std::make_shared<AsyncFluidBackend>(backend_state);
        },
        [](const gpu_meshing::ParticleJob&, gpu_meshing::MeshResult&,
           gpu_meshing::Stats&, gpu_meshing::Error& visual_error,
           const gpu_meshing::BuildControl&) {
            visual_error = {gpu_meshing::ErrorCode::VulkanFailure,
                            "injected replacement visual failure"};
            return false;
        });
    const std::size_t seed_d = world_source.find("seed:10");
    CHECK(seed_d != std::string::npos, "accepted D seed was found");
    if (seed_d != std::string::npos)
        world_source.replace(seed_d, 7u, "seed:11");
    const std::size_t spacing = world_source.find("particleSpacing:.2");
    CHECK(spacing != std::string::npos,
          "accepted D simulation contract was found");
    if (spacing != std::string::npos)
        world_source.replace(spacing, 18u, "particleSpacing:.21");
    CHECK(write_file(world_path, world_source),
          "generation E changes the section simulation contract");
    session->reload();
    FullBakeLog failed_events;
    drive_bake_tolerant(*session, failed_events, 60);
    matter::RiverFieldSample replacement_sample{};
    CHECK(replacement_binding &&
              session->river_runtime_binding() == replacement_binding &&
              replacement_binding->sample(wet_position, replacement_sample),
          "failed E leaves retained accepted D's lease valid");

    session.reset();
    engine.reset();
    remove_tree(root);
    return finished && found_manifest && sampled && replacement_finished;
}

int main() {
    // Unique writable sandbox so parallel test runs do not collide.
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    std::string sandbox =
        (fs::temp_directory_path() /
         ("me3_asyncbake_" + std::to_string(stamp))).string();
    if (!build_sandbox(sandbox)) {
        printf("FAIL: build_sandbox\n");
        return 1;
    }
    if (!project_fixture_contract(sandbox, "Box")) {
        return 1;
    }

    test_returns_immediately(sandbox);
    test_completes_finished(sandbox);
    test_determinism(sandbox);
    test_reload_reenters(sandbox);

    // issues/editor-scene-panel-stale: warm loads must publish the graph
    // snapshot, and the editor's cache policy must survive the cross-session
    // generation collision.
    test_warm_session_publishes_graph(sandbox);

    // Task 7 tests.
    test_supersede_cancels_inflight(sandbox);
    test_destructor_mid_bake_joins(sandbox);
    test_oom_injection_skips_part(sandbox);
    test_broken_script_skips_part(sandbox);

    // Task 7 fix (review): load-phase skip-and-continue in publish jobs.
    test_load_failure_skips_part(sandbox);

    // Task 3 (Phase C): set_bake_focus + distance-ordered publish.
    test_focus_orders_publish(sandbox);

#ifdef __linux__
    // Task 10: inotify live-edit end-to-end.
    test_live_edit_inotify_e2e(sandbox);
#endif

    // Task 7 (Phase C): regenerate(seed) — root param override reload.
    test_regenerate_seed_reroll(sandbox);
    test_production_animated_gallery_binding();

    // Task 7 PhysX fluid bake integration on the same worker/GPU-job lifecycle.
    test_authored_fluid_uses_worker_and_gpu_job_seam(sandbox);
    test_cancelled_fluid_generation_publishes_neither_half(sandbox);

    // E3 milestone (event-system.md): typed bake events + legacy poll_event
    // shim over lane::legacy_poll. Runs LAST so it cannot perturb any prior
    // test's cache/state assumptions.
    test_e3_poll_event_typed_parity(sandbox);

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    // Best-effort cleanup of the writable temporary project.
    remove_tree(sandbox);
    return g_failures ? 1 : 0;
}
