// refine_loop_tests.cpp — Phase C Task 6 — camera-driven refine loop.
//
// GPU flavor (needs GL context for PartStore/GpuCuller GPU objects).
// Uses a 3×3 two-res MiniValley sandbox: a root schema that places 9 coarse
// Terrain tiles (tx 0..2, tz 0..2) each with a full-res counterpart, so the
// RefineController starts with 9 Coarse tiles and can refine all of them.
//
// Schema design for MiniValley:
//   CoarseTile{tx,tz} — trivial mesh part, module="Terrain", params include
//                        res="coarse", tx, tz.
//   FullTile{tx,tz}   — trivial mesh part, module="Terrain", params include
//                        res="full", tx, tz.
// The root MiniValley.js uses requires() to pull both variants, then places
// the 9 coarse tiles with placeChild. The engine's RefineController pairs them
// by (tx,tz) and records coarse_hash + full_hash.
//
// Tests:
//   (a) test_refines_toward_focus — after BakeFinished, focus at tile (2,2);
//       pump ≤ 30s; assert RefineTileDone events arrive and done reaches 9.
//   (b) test_eviction — MATTER_REFINE_RADIUS small; focus (0,0) until Full;
//       move focus far; assert full_count drops (RefineTileDone with lower done).
//   (c) test_supersede_cancels_refine — mid-refine reload(); no stale
//       RefineTileDone after new BakeStarted; sequence stays coherent.
//
// Run with: GALLIUM_DRIVER=d3d12 (gpu flavor needs it).

#include "matter/engine_context.h"
#include "matter/world_session.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "check.h"

using clk = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Sandbox helpers
// ---------------------------------------------------------------------------

static void run_cmd(const std::string& cmd) { std::system(cmd.c_str()); }

static bool write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    if (!f) return false;
    f << body;
    return true;
}

// Build a 3x3 MiniValley sandbox.
// The root MiniValley.js places 9 CoarseTile_tx_tz schemas.
// Each CoarseTile and FullTile schema is a Terrain module that includes
// res, tx, tz in its params and exposes them via static get params().
// The key requirement: the engine pairs coarse+full by (tx,tz) in the snapshot.
//
// Schema design: CoarseTile_0_0.js etc. are "Terrain" instances with params.
// We instead create a single Terrain.js that is parametric (takes res, tx, tz)
// and a MiniValley root that places it twice per tile (coarse+full).
static bool build_miniValley_sandbox(const std::string& root) {
    run_cmd("rm -rf " + root);
    run_cmd("mkdir -p " + root + "/schemas");
    run_cmd("mkdir -p " + root + "/world_data/MiniValley");
    run_cmd("mkdir -p " + root + "/shared-lib");
    run_cmd("mkdir -p " + root + "/cache/parts");

    // Terrain.js — parametric part; res, tx, tz are params.
    // Returns a tiny quad mesh regardless of params.
    if (!write_file(root + "/schemas/Terrain.js",
        "class Terrain extends Part {\n"
        "  static get params() { return { res:'coarse', tx:0, tz:0 }; }\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const tx = p.tx || 0;\n"
        "    const tz = p.tz || 0;\n"
        "    const S = 5.0;\n"  // 10m tile
        "    const ox = tx * 10.0;\n"
        "    const oz = tz * 10.0;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(ox,     0, oz    );\n"
        "    this.vertex(ox,     0, oz + S);\n"
        "    this.vertex(ox + S, 0, oz    );\n"
        "    this.vertex(ox + S, 0, oz    );\n"
        "    this.vertex(ox,     0, oz + S);\n"
        "    this.vertex(ox + S, 0, oz + S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;

    // MiniValley.js — root that places 9 coarse tiles and requires 9 full tiles.
    // Each coarse tile is placed with placeChild using the identity transform
    // offset by (tx*10, 0, tz*10) in the last column of a row-major 4x4.
    // The requires() array ensures all 18 nodes (9 coarse + 9 full) appear in
    // the graph snapshot so RefineController::build() can pair them.
    std::ostringstream root_js;
    root_js << "class MiniValley extends Part {\n";
    root_js << "  static get requires() {\n    return [\n";
    for (int tx = 0; tx < 3; ++tx) {
        for (int tz = 0; tz < 3; ++tz) {
            root_js << "      { module:'Terrain', params:{ res:'full',   tx:" << tx << ", tz:" << tz << " } },\n";
            root_js << "      { module:'Terrain', params:{ res:'coarse', tx:" << tx << ", tz:" << tz << " } },\n";
        }
    }
    root_js << "    ];\n  }\n";
    root_js << "  build(p) {\n";
    root_js << "    this.fill(MAT.stone);\n";
    // Place a tiny root surface so this part has geometry.
    root_js << "    this.beginShape(SHAPE.triangles);\n";
    root_js << "    this.vertex(0,0,0); this.vertex(1,0,0); this.vertex(0,1,0);\n";
    root_js << "    this.endShape();\n";
    // Place coarse tiles.
    for (int tx = 0; tx < 3; ++tx) {
        for (int tz = 0; tz < 3; ++tz) {
            // Row-major 4x4 identity with translation at [3]=ox, [7]=0, [11]=oz.
            float ox = (float)(tx * 10);
            float oz = (float)(tz * 10);
            root_js << "    this.placeChild('Terrain', { res:'coarse', tx:" << tx << ", tz:" << tz
                    << " }, [1,0,0," << ox << ", 0,1,0,0, 0,0,1," << oz << ", 0,0,0,1]);\n";
        }
    }
    root_js << "  }\n}\n";
    if (!write_file(root + "/schemas/MiniValley.js", root_js.str())) return false;

    // World manifest: one MiniValley placement.
    if (!write_file(root + "/world_data/MiniValley/world.manifest",
        "# MiniValley 3x3 refine-loop test fixture\n"
        "MiniValley\n")) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Drive helpers
// ---------------------------------------------------------------------------

// Open a headless session (allow_gl_lt_46=true) for the sandbox.
static std::unique_ptr<matter::WorldSession> open_session(
    const std::string& sandbox,
    std::unique_ptr<matter::EngineContext>& engine_out,
    std::string& err)
{
    matter::EngineDesc ed;
    std::string cr = sandbox + "/cache";
    ed.cache_root     = cr.c_str();
    ed.allow_gl_lt_46 = true;
    engine_out = matter::EngineContext::create(ed, err);
    if (!engine_out) return nullptr;

    matter::WorldDesc wd;
    std::string schemas = sandbox + "/schemas";
    std::string wdata   = sandbox + "/world_data";
    std::string shlib   = sandbox + "/shared-lib";
    wd.schemas_dir    = schemas.c_str();
    wd.world_data_dir = wdata.c_str();
    wd.world_name     = "MiniValley";
    wd.shared_lib_dir = shlib.c_str();
    return engine_out->open_world(wd, err);
}

struct BakeResult {
    bool finished = false;
    int  errors   = 0;
};

// Drive bake to BakeFinished; returns true on success. Stops on BakeError.
static BakeResult drive_bake(matter::WorldSession& s, int timeout_sec = 60) {
    BakeResult r;
    auto deadline = clk::now() + std::chrono::seconds(timeout_sec);
    while (clk::now() < deadline) {
        s.pump_gpu_jobs(4.0f);
        matter::Event ev;
        bool any = false;
        while (s.poll_event(ev)) {
            any = true;
            if (ev.type == matter::EventType::BakeFinished) {
                r.errors  += ev.errors;
                r.finished = true;
                break;
            }
            if (ev.type == matter::EventType::BakeError) {
                printf("  BakeError: code=%d phase=%s msg=%s\n",
                       (int)ev.code, ev.phase.c_str(), ev.message.c_str());
                ++r.errors;
            }
        }
        if (r.finished) return r;
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    printf("  drive_bake TIMEOUT after %ds\n", timeout_sec);
    return r;
}

// ---------------------------------------------------------------------------
// (a) test_refines_toward_focus
// After BakeFinished, set focus at tile (2,2) center (x=25, z=25).
// Pump events+jobs ≤ 30s; assert RefineTileDone events arrive,
// and done reaches 9 (radius large by default MATTER_REFINE_RADIUS=160).
// ---------------------------------------------------------------------------
static bool test_refines_toward_focus(const std::string& sandbox) {
    printf("\n[test_refines_toward_focus]\n");

    // Cold cache.
    run_cmd("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(sandbox, engine, err);
    CHECK(s != nullptr, "refine-focus: session opened");
    if (!s) { printf("  err: %s\n", err.c_str()); return false; }

    // Set focus at tile (2,2) center before baking so publish is focused too.
    float focus_far[3] = { 25.0f, 0.0f, 25.0f };
    s->set_bake_focus(focus_far);

    s->request_bake();
    auto br = drive_bake(*s, 60);
    CHECK(br.finished, "refine-focus: BakeFinished received");
    CHECK(br.errors == 0, "refine-focus: no bake errors");
    if (!br.finished) return false;

    // Now pump the refine loop for up to 30s, collecting RefineTileDone events.
    int refine_done_max = 0;
    int refine_events   = 0;
    bool first_module_ok = false;

    auto deadline = clk::now() + std::chrono::seconds(30);
    while (clk::now() < deadline) {
        s->pump_gpu_jobs(4.0f);
        matter::Event ev;
        while (s->poll_event(ev)) {
            if (ev.type == matter::EventType::RefineTileDone) {
                ++refine_events;
                if (ev.done > refine_done_max) refine_done_max = ev.done;
                if (refine_events == 1) {
                    // First refined tile should have phase="refine".
                    first_module_ok = (ev.phase == "refine");
                    printf("  first RefineTileDone: module=%s done=%d total=%d phase=%s\n",
                           ev.module.c_str(), ev.done, ev.total, ev.phase.c_str());
                }
                if (ev.done >= 9) break;
            } else if (ev.type == matter::EventType::BakeError) {
                printf("  unexpected BakeError: %s\n", ev.message.c_str());
            }
        }
        if (refine_done_max >= 9) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    printf("  refine_events=%d refine_done_max=%d\n", refine_events, refine_done_max);
    CHECK(refine_events > 0,  "refine-focus: at least one RefineTileDone arrived");
    CHECK(first_module_ok,    "refine-focus: first RefineTileDone has phase=refine");
    CHECK(refine_done_max >= 9, "refine-focus: done reaches 9 (all tiles upgraded)");
    return true;
}

// ---------------------------------------------------------------------------
// (b) test_eviction
// Set MATTER_REFINE_RADIUS small (via env) so only 1–2 tiles refine at (0,0).
// Then move focus far (x=1000) and wait; assert full_count drops
// (a RefineTileDone with decreasing `done` is emitted on eviction).
// ---------------------------------------------------------------------------
static bool test_eviction(const std::string& sandbox) {
    printf("\n[test_eviction]\n");

    // Warm cache (reuse prior sandbox bake).
    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(sandbox, engine, err);
    CHECK(s != nullptr, "refine-evict: session opened");
    if (!s) { printf("  err: %s\n", err.c_str()); return false; }

    // Focus at tile (0,0) center. MATTER_REFINE_RADIUS is set by the test runner
    // (default 160 if not set). For this test we want a small radius so only a few
    // tiles fit; we expose it via the env override (read in Impl init).
    // The test relies on MATTER_REFINE_RADIUS=12 so only tile (0,0) is within range
    // (tile center at (5,0,5); radius 12 just covers it at dist≈7.07).
    // We set the env before open_session above would have been called, but since
    // open_session has already run, we need to rely on re-reading or a separate
    // session. The env var is read once at Impl init (open_world), so we must set
    // it before calling open_world. However, we already opened the session above.
    // Solution: close and reopen with the env set.
    s.reset();
    engine.reset();

    // Set radius to 12 so only tile (0,0) (center at 5,0,5, dist≈7.07) fits.
    setenv("MATTER_REFINE_RADIUS", "12", 1);

    s = open_session(sandbox, engine, err);
    CHECK(s != nullptr, "refine-evict: session (small radius) opened");
    if (!s) { unsetenv("MATTER_REFINE_RADIUS"); return false; }

    float focus_near[3] = { 5.0f, 0.0f, 5.0f };
    s->set_bake_focus(focus_near);

    s->request_bake();
    auto br = drive_bake(*s, 60);
    CHECK(br.finished, "refine-evict: BakeFinished");
    if (!br.finished) { unsetenv("MATTER_REFINE_RADIUS"); return false; }

    // Pump refine loop until tile (0,0) is Full (done >= 1).
    int done_near  = 0;
    bool got_near_full = false;
    {
        auto deadline = clk::now() + std::chrono::seconds(15);
        while (clk::now() < deadline && !got_near_full) {
            s->pump_gpu_jobs(4.0f);
            matter::Event ev;
            while (s->poll_event(ev)) {
                if (ev.type == matter::EventType::RefineTileDone) {
                    done_near = ev.done;
                    printf("  near RefineTileDone done=%d total=%d\n", ev.done, ev.total);
                    if (ev.done >= 1) { got_near_full = true; break; }
                }
            }
            if (!got_near_full) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    CHECK(got_near_full, "refine-evict: at least one tile refined near focus");

    // Now move focus far — tile (0,0) should get evicted.
    float focus_far[3] = { 1000.0f, 0.0f, 1000.0f };
    s->set_bake_focus(focus_far);

    // Pump until done drops below done_near or we get a RefineTileDone with done < done_near.
    int min_done_seen = done_near;
    bool got_eviction = false;
    {
        auto deadline = clk::now() + std::chrono::seconds(15);
        while (clk::now() < deadline && !got_eviction) {
            s->pump_gpu_jobs(4.0f);
            matter::Event ev;
            while (s->poll_event(ev)) {
                if (ev.type == matter::EventType::RefineTileDone) {
                    printf("  eviction RefineTileDone done=%d total=%d\n", ev.done, ev.total);
                    if (ev.done < min_done_seen) {
                        min_done_seen  = ev.done;
                        got_eviction   = true;
                        break;
                    }
                }
            }
            if (!got_eviction) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    unsetenv("MATTER_REFINE_RADIUS");

    CHECK(got_eviction, "refine-evict: full_count dropped after moving focus far (eviction event)");
    return true;
}

// ---------------------------------------------------------------------------
// (c) test_supersede_cancels_refine
// Start bake, let it finish, let refine begin, then call reload().
// Assert that a new BakeStarted event arrives and no RefineTileDone comes
// AFTER the new BakeStarted (stale refine must be flushed).
// ---------------------------------------------------------------------------
static bool test_supersede_cancels_refine(const std::string& sandbox) {
    printf("\n[test_supersede_cancels_refine]\n");

    std::string err;
    std::unique_ptr<matter::EngineContext> engine;
    auto s = open_session(sandbox, engine, err);
    CHECK(s != nullptr, "refine-supersede: session opened");
    if (!s) return false;

    float focus[3] = { 5.0f, 0.0f, 5.0f };
    s->set_bake_focus(focus);

    s->request_bake();
    auto br = drive_bake(*s, 60);
    CHECK(br.finished, "refine-supersede: first BakeFinished");
    if (!br.finished) return false;

    // Let at least one refine step run.
    bool got_first_refine = false;
    {
        auto deadline = clk::now() + std::chrono::seconds(10);
        while (clk::now() < deadline && !got_first_refine) {
            s->pump_gpu_jobs(4.0f);
            matter::Event ev;
            while (s->poll_event(ev)) {
                if (ev.type == matter::EventType::RefineTileDone) {
                    got_first_refine = true;
                    break;
                }
            }
            if (!got_first_refine) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Whether or not we got a refine event, now trigger supersession.
    printf("  got_first_refine=%d; triggering reload()\n", (int)got_first_refine);
    s->reload();

    // Drive the second bake to completion.
    bool second_started = false;
    bool second_finished = false;
    bool stale_refine_after_started = false;
    bool saw_bake_started_2 = false;

    {
        auto deadline = clk::now() + std::chrono::seconds(60);
        while (clk::now() < deadline) {
            s->pump_gpu_jobs(4.0f);
            matter::Event ev;
            bool any = false;
            while (s->poll_event(ev)) {
                any = true;
                if (ev.type == matter::EventType::BakeStarted) {
                    if (!second_started) {
                        second_started    = true;
                        saw_bake_started_2 = true;
                        printf("  new BakeStarted received\n");
                    }
                }
                if (ev.type == matter::EventType::RefineTileDone && saw_bake_started_2) {
                    // A RefineTileDone after the NEW BakeStarted but before BakeFinished
                    // would indicate stale refine steps executing during a live bake.
                    // However, note that RefineTileDone can legitimately arrive after
                    // BakeFinished of the NEW bake — we only flag pre-BakeFinished ones.
                    if (!second_finished) {
                        stale_refine_after_started = true;
                        printf("  STALE RefineTileDone during new bake (phase=%s)\n",
                               ev.phase.c_str());
                    }
                }
                if (ev.type == matter::EventType::BakeFinished && second_started) {
                    second_finished = true;
                    break;
                }
            }
            if (second_finished) break;
            if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    CHECK(second_started,  "refine-supersede: new BakeStarted received after reload()");
    CHECK(second_finished, "refine-supersede: second BakeFinished received");
    CHECK(!stale_refine_after_started,
          "refine-supersede: no RefineTileDone during new bake (stale refine flushed)");
    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    printf("=== refine_loop_tests ===\n");

    // One persistent sandbox for all tests (warm cache shared across cases).
    const std::string sandbox = "/tmp/me3_refine_loop_tests";
    if (!build_miniValley_sandbox(sandbox)) {
        printf("FATAL: build_miniValley_sandbox failed\n");
        return 1;
    }

    bool ok_a = test_refines_toward_focus(sandbox);
    bool ok_b = test_eviction(sandbox);
    bool ok_c = test_supersede_cancels_refine(sandbox);

    printf("\n=== SUMMARY ===\n");
    printf("  (a) refines_toward_focus:    %s\n", ok_a ? "PASS" : "FAIL");
    printf("  (b) eviction:                %s\n", ok_b ? "PASS" : "FAIL");
    printf("  (c) supersede_cancels_refine:%s\n", ok_c ? "PASS" : "FAIL");

    return check_summary();
}
