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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
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

// Snapshot format for determinism comparison.
struct EvRec {
    int type = 0;     // matter::EventType cast to int
    std::string module;
    int done = 0, total = 0;
    std::string phase;
};

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
    matter::WorldDesc wd = project_world_desc(sandbox, "Box");
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

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    // Best-effort cleanup of the writable temporary project.
    remove_tree(sandbox);
    return g_failures ? 1 : 0;
}
