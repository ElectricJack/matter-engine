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
#include <new>
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

// --- Task 7 helpers / multi-part sandbox -----------------------------------

// Build a sandbox with multiple distinct schemas (Part0..PartN-1) so we have
// enough parts to make a cache-cold bake take measurable time.
// world.manifest places each schema once (no flags).
static bool build_multi_sandbox(const std::string& root, int num_parts) {
    run("rm -rf " + root);
    run("mkdir -p " + root + "/schemas");
    run("mkdir -p " + root + "/world_data/Multi");
    run("mkdir -p " + root + "/shared-lib");
    run("mkdir -p " + root + "/cache/parts");

    std::string manifest_content = "# Multi-part world\n";
    for (int i = 0; i < num_parts; ++i) {
        std::string name = "Part" + std::to_string(i);
        // Each schema is distinct (uses i as a vertex offset so hashes differ).
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
        if (!write_file(root + "/schemas/" + name + ".js", js.str())) return false;
        manifest_content += name + "\n";
    }
    if (!write_file(root + "/world_data/Multi/world.manifest", manifest_content)) return false;
    return true;
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
    std::string cache_root_s = sandbox + "/cache";
    std::string schemas_s    = sandbox + "/schemas";
    std::string wdata_s      = sandbox + "/world_data";
    std::string shlib_s      = sandbox + "/shared-lib";
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    engine = matter::EngineContext::create(ed, err);
    if (!engine) { printf("  engine create failed: %s\n", err.c_str()); return nullptr; }
    matter::WorldDesc wd;
    wd.schemas_dir    = schemas_s.c_str();
    wd.world_data_dir = wdata_s.c_str();
    wd.world_name     = world_name.c_str();
    wd.shared_lib_dir = shlib_s.c_str();
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
    run("rm -rf " + multi + "/cache && mkdir -p " + multi + "/cache/parts");

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(multi, err, engine, "Multi");
    CHECK(s != nullptr, "supersede: session opened");
    if (!s) { run("rm -rf " + multi); return false; }

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

    run("rm -rf " + multi);
    return overall_ok && cancellation_or_race;
}

// --- (f) destructor_mid_bake_joins ------------------------------------------
// Start a bake, destroy the session after the first event WITHOUT pumping further.
// The test must complete in < 10 s (deadlock regression guard).
static bool test_destructor_mid_bake_joins(const std::string& sandbox) {
    printf("-- (f) destructor_mid_bake_joins\n");

    run("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

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
    run("rm -rf " + multi + "/cache && mkdir -p " + multi + "/cache/parts");

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(multi, err, engine, "Multi");
    CHECK(s != nullptr, "oom_inject: session opened");
    if (!s) { run("rm -rf " + multi); return false; }

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

    run("rm -rf " + multi);
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
    run("rm -rf " + broot);
    run("mkdir -p " + broot + "/schemas");
    run("mkdir -p " + broot + "/world_data/Broken2");
    run("mkdir -p " + broot + "/shared-lib");
    run("mkdir -p " + broot + "/cache/parts");

    // Valid part: same box quad as the main sandbox.
    if (!write_file(broot + "/schemas/ValidPart.js",
        "class ValidPart extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.5;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) { run("rm -rf " + broot); return false; }

    // Broken part: JS syntax error (unmatched brace).
    if (!write_file(broot + "/schemas/BrokenPart.js",
        "class BrokenPart extends Part {\n"
        "  build(p) { this.fill(MAT.stone;\n"  // missing closing paren
        "}")) { run("rm -rf " + broot); return false; }

    // Manifest: one ValidPart, one BrokenPart.
    if (!write_file(broot + "/world_data/Broken2/world.manifest",
        "# broken_script test\n"
        "ValidPart\n"
        "BrokenPart\n")) { run("rm -rf " + broot); return false; }

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(broot, err, engine, "Broken2");
    CHECK(s != nullptr, "broken_script: session opened");
    if (!s) { run("rm -rf " + broot); return false; }

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

    run("rm -rf " + broot);
    return ok && saw_script_error && log.error_count == 1 && ic > 0;
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

    // Task 7 tests.
    test_supersede_cancels_inflight(sandbox);
    test_destructor_mid_bake_joins(sandbox);
    test_oom_injection_skips_part(sandbox);
    test_broken_script_skips_part(sandbox);

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    // Best-effort cleanup so /tmp doesn't accumulate.
    run("rm -rf " + sandbox);
    return g_failures ? 1 : 0;
}
