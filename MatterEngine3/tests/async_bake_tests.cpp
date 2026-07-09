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
#include "matter/world_session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "check.h"

using clk = std::chrono::steady_clock;

// --- Sandbox helpers ------------------------------------------------------

static void run(const std::string& cmd) { std::system(cmd.c_str()); }

static bool write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    if (!f) return false;
    f << body;
    return true;
}

// Build a minimal Box world sandbox under `root`. Schema Box.js is a trivial
// voxel part (a single 0.6 m box brush at origin). world.manifest places Box
// three times (no flags -> each root lands at world origin per LocalProvider's
// default placement, but three distinct instance_ids still exercise the
// per-instance publish path). Empty shared-lib.
static bool build_sandbox(const std::string& root) {
    run("rm -rf " + root);
    run("mkdir -p " + root + "/schemas");
    run("mkdir -p " + root + "/world_data/Box");
    run("mkdir -p " + root + "/shared-lib");
    run("mkdir -p " + root + "/cache/parts");

    // The tiniest mesh Part: a two-triangle floor quad, mirrors FloorDemo.js
    // shape (no voxel session, no shared-lib imports, no children).
    if (!write_file(root + "/schemas/Box.js",
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

    // Manifest: three placements of Box. LocalProvider places unflagged roots
    // at the origin; three copies means three manifest instances → three
    // publish steps for the single unique part_hash.
    if (!write_file(root + "/world_data/Box/world.manifest",
        "# Box world (async-bake tests fixture)\n"
        "Box\n"
        "Box\n"
        "Box\n")) return false;

    return true;
}

// Snapshot format for determinism comparison.
struct EvRec {
    int type = 0;     // matter::EventType cast to int
    std::string module;
    int done = 0, total = 0;
    std::string phase;
};

static std::string ev_type_name(matter::EventType t) {
    switch (t) {
        case matter::EventType::BakeStarted:  return "BakeStarted";
        case matter::EventType::BakePartDone: return "BakePartDone";
        case matter::EventType::BakeFinished: return "BakeFinished";
        case matter::EventType::BakeError:    return "BakeError";
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
    run("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");
    std::string err;
    // Keep path strings alive across the create/open_world calls; EngineDesc /
    // WorldDesc hold const char* views, not std::string copies.
    std::string cache_root_s = sandbox + "/cache";
    std::string schemas_s    = sandbox + "/schemas";
    std::string wdata_s      = sandbox + "/world_data";
    std::string shlib_s      = sandbox + "/shared-lib";
    matter::EngineDesc ed;
    ed.cache_root      = cache_root_s.c_str();
    ed.allow_gl_lt_46  = true;   // headless: skip GL 4.6 gate, skip raster path
    auto engine = matter::EngineContext::create(ed, err);
    CHECK(engine != nullptr, "engine created");
    if (!engine) { printf("  err: %s\n", err.c_str()); return false; }

    matter::WorldDesc wd;
    wd.schemas_dir    = schemas_s.c_str();
    wd.world_data_dir = wdata_s.c_str();
    wd.world_name     = "Box";
    wd.shared_lib_dir = shlib_s.c_str();
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
    run("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

    std::string err;
    std::string cache_root_s = sandbox + "/cache";
    std::string schemas_s    = sandbox + "/schemas";
    std::string wdata_s      = sandbox + "/world_data";
    std::string shlib_s      = sandbox + "/shared-lib";
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(ed, err);
    CHECK(engine != nullptr, "engine created");
    if (!engine) return false;

    matter::WorldDesc wd;
    wd.schemas_dir    = schemas_s.c_str();
    wd.world_data_dir = wdata_s.c_str();
    wd.world_name     = "Box";
    wd.shared_lib_dir = shlib_s.c_str();
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

    return true;
}

// --- (c) determinism --------------------------------------------------------
// Run against two fresh caches and diff the {type,module,done,total,phase}
// sequences.
static std::vector<EvRec> run_once_fresh(const std::string& sandbox) {
    run("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");
    std::string err;
    std::string cache_root_s = sandbox + "/cache";
    std::string schemas_s    = sandbox + "/schemas";
    std::string wdata_s      = sandbox + "/world_data";
    std::string shlib_s      = sandbox + "/shared-lib";
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(ed, err);
    matter::WorldDesc wd;
    wd.schemas_dir    = schemas_s.c_str();
    wd.world_data_dir = wdata_s.c_str();
    wd.world_name     = "Box";
    wd.shared_lib_dir = shlib_s.c_str();
    auto s = engine->open_world(wd, err);
    std::vector<EvRec> log;
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
    std::string cache_root_s = sandbox + "/cache";
    std::string schemas_s    = sandbox + "/schemas";
    std::string wdata_s      = sandbox + "/world_data";
    std::string shlib_s      = sandbox + "/shared-lib";
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(ed, err);
    matter::WorldDesc wd;
    wd.schemas_dir    = schemas_s.c_str();
    wd.world_data_dir = wdata_s.c_str();
    wd.world_name     = "Box";
    wd.shared_lib_dir = shlib_s.c_str();
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

int main() {
    // Unique sandbox per pid so parallel test runs don't collide.
    std::string sandbox = "/tmp/me3_asyncbake_" + std::to_string((int)getpid());
    if (!build_sandbox(sandbox)) {
        printf("FAIL: build_sandbox\n");
        return 1;
    }

    test_returns_immediately(sandbox);
    test_completes_finished(sandbox);
    test_determinism(sandbox);
    test_reload_reenters(sandbox);

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    // Best-effort cleanup so /tmp doesn't accumulate.
    run("rm -rf " + sandbox);
    return g_failures ? 1 : 0;
}
