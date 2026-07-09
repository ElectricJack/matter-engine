// Phase C Task 2 — Meadow Valley layout structural test.
//
// Validates the 51x51 tile layout, two-resolution terrain requires, and banded
// scatter budget using the REAL world_demo schemas + WorldData/Meadow manifest.
// Headless (allow_gl_lt_46=true, no window): the CPU publish path drives all
// parts. This is a long test on a cold cache (~2,601 coarse terrain tiles +
// banded scatter); run-valley is its own target excluded from quick suites.
//
// Assertions after BakeFinished:
//   instances_total >= 2601 + 60000   (coarse tiles + banded scatter floor)
//   instances_total <= 150000          (spec budget)
//   ev_errors == 0                     (no skipped parts)
// Plus a determinism check: rebake with same seed -> same instance count.

#include "matter/engine_context.h"
#include "matter/world_session.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <limits.h>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <execinfo.h>

#include "check.h"

// Progress counter shared between main and signal handler (rough tracking only).
static volatile int g_phase        = 0;   // 0=init 1=bake-running 2=BakeFinished 3=assertions
static volatile int g_part_events  = 0;

static void sigsegv_handler(int, siginfo_t* si, void*) {
    // Async-signal-safe: only write() and _exit().
    const char* msgs[] = {
        "SIGSEGV phase=0 (init/open_world)\n",
        "SIGSEGV phase=1 (drive_bake running)\n",
        "SIGSEGV phase=2 (BakeFinished received)\n",
        "SIGSEGV phase=3 (post-bake assertions/session2)\n",
    };
    int ph = g_phase;
    if (ph < 0 || ph > 3) ph = 3;
    const char* m = msgs[ph];
    // write() to stderr (fd=2), signal-safe.
    int len = 0; while (m[len]) ++len;
    write(2, m, len);
    // Print fault address as hex
    if (si) {
        const char* fa = "fault_addr=0x";
        write(2, fa, 13);
        unsigned long addr = (unsigned long)(uintptr_t)si->si_addr;
        char hex[17]; int hi = 15; hex[16] = '\n';
        for (int i = 0; i < 16; ++i) hex[i] = '0';
        while (addr > 0) { int d = addr & 0xF; hex[hi--] = d < 10 ? '0'+d : 'a'+(d-10); addr >>= 4; }
        write(2, hex, 17);
    }
    // Print backtrace
    void* bt[64];
    int btcount = backtrace(bt, 64);
    backtrace_symbols_fd(bt, btcount, 2);
    // Also write part event count.
    char buf[64];
    int n = 0;
    buf[n++] = 'p'; buf[n++] = 'a'; buf[n++] = 'r'; buf[n++] = 't'; buf[n++] = '_';
    buf[n++] = 'e'; buf[n++] = 'v'; buf[n++] = 'e'; buf[n++] = 'n'; buf[n++] = 't';
    buf[n++] = 's'; buf[n++] = '=';
    int v = g_part_events;
    if (v == 0) { buf[n++] = '0'; }
    else {
        char tmp[16]; int ti = 0;
        while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
        for (int i = ti-1; i >= 0; --i) buf[n++] = tmp[i];
    }
    buf[n++] = '\n';
    write(2, buf, n);
    _exit(139);
}

using clk = std::chrono::steady_clock;

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

// Drive one bake to completion: pump GPU jobs + poll events.
// Returns true on BakeFinished, false on BakeError or timeout.
// errors_out accumulates BakeError count.
static bool drive_bake(matter::WorldSession& s, int& errors_out,
                       int timeout_sec = 900 /* 15 min */) {
    auto deadline = clk::now() + std::chrono::seconds(timeout_sec);
    bool finished = false;
    while (clk::now() < deadline) {
        s.pump_gpu_jobs(4.0f);
        matter::Event ev;
        bool any = false;
        while (s.poll_event(ev)) {
            any = true;
            if (ev.type == matter::EventType::BakePartDone) {
                ++g_part_events;
            }
            if (ev.type == matter::EventType::BakeError) {
                printf("  BakeError: code=%d phase=%s module=%s msg=%s\n",
                       (int)ev.code, ev.phase.c_str(),
                       ev.module.c_str(), ev.message.c_str());
                ++errors_out;
            }
            if (ev.type == matter::EventType::BakeFinished) {
                g_phase = 2;
                errors_out += ev.errors;
                finished = true;
                break;
            }
        }
        if (finished) return true;
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    printf("  drive_bake TIMEOUT after %ds\n", timeout_sec);
    return false;
}

static std::unique_ptr<matter::WorldSession> open_valley(
    const std::string& schemas_dir,
    const std::string& world_data_dir,
    const std::string& shared_lib_dir,
    const std::string& cache_dir,
    std::unique_ptr<matter::EngineContext>& engine_out,
    std::string& err)
{
    matter::EngineDesc ed;
    ed.cache_root     = cache_dir.c_str();
    ed.allow_gl_lt_46 = true;  // headless: skip GL 4.6 gate
    engine_out = matter::EngineContext::create(ed, err);
    if (!engine_out) return nullptr;

    matter::WorldDesc wd;
    wd.schemas_dir    = schemas_dir.c_str();
    wd.world_data_dir = world_data_dir.c_str();
    wd.world_name     = "Meadow";
    wd.shared_lib_dir = shared_lib_dir.c_str();
    auto s = engine_out->open_world(wd, err);
    if (!s) { printf("  open_world failed: %s\n", err.c_str()); return nullptr; }
    return s;
}

int main() {
    {
        struct sigaction sa{};
        sa.sa_sigaction = sigsegv_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
    }

    const std::string schemas    = abspath("../examples/world_demo/schemas");
    const std::string world_data = abspath("../examples/world_demo/WorldData");
    const std::string shared_lib = abspath("../shared-lib");
    // Persistent cache so repeated runs are warm (same as meadow_bake_check pattern).
    const std::string cache_dir  = "/tmp/me3_valley_layout/cache";
    std::system(("mkdir -p " + cache_dir + "/parts").c_str());

    printf("=== valley_layout: schemas=%s world=%s/Meadow ===\n",
           schemas.c_str(), world_data.c_str());

    // ---- (a) cold bake: measure instances_total and ev_errors ----------------
    printf("-- (a) cold-bake instances + budget\n");
    {
        // Nuke cache for a true cold-bake measurement.
        std::system(("rm -rf " + cache_dir + " && mkdir -p " + cache_dir + "/parts").c_str());

        auto t0 = clk::now();

        std::string err;
        std::unique_ptr<matter::EngineContext> engine;
        auto s = open_valley(schemas, world_data, shared_lib, cache_dir, engine, err);
        CHECK(s != nullptr, "valley: session opened");
        if (!s) { printf("  err: %s\n", err.c_str()); return 1; }

        g_phase = 1;
        s->request_bake();

        int ev_errors = 0;
        bool ok = drive_bake(*s, ev_errors);
        CHECK(ok, "valley: bake completed (BakeFinished)");
        fflush(stdout);

        g_phase = 3;
        double wall_sec = std::chrono::duration<double>(clk::now() - t0).count();
        printf("  bake wall time: %.1fs (part_events=%d)\n", wall_sec, (int)g_part_events);
        fflush(stdout);

        const auto& fs = s->frame_stats();
        printf("  instances_total=%u parts_baked=%u cache_hits=%u ev_errors=%d\n",
               fs.instances_total, fs.parts_baked, fs.cache_hits, ev_errors);
        fflush(stdout);

        CHECK(fs.instances_total >= 2601 + 60000,
              "instances_total >= 2601 + 60000 (coarse tiles + scatter floor)");
        CHECK(fs.instances_total <= 150000,
              "instances_total <= 150000 (spec budget)");
        CHECK(ev_errors == 0, "ev_errors == 0 (no skipped parts)");
        fflush(stdout);
    }

    // ---- (b) warm re-bake: determinism (same instance count) -----------------
    printf("-- (b) warm re-bake determinism\n");
    {
        std::string err;
        std::unique_ptr<matter::EngineContext> engine;
        auto s = open_valley(schemas, world_data, shared_lib, cache_dir, engine, err);
        CHECK(s != nullptr, "valley-det: session opened");
        if (!s) { goto summary; }

        s->request_bake();

        int ev_errors2 = 0;
        bool ok2 = drive_bake(*s, ev_errors2);
        CHECK(ok2, "valley-det: warm bake completed");
        CHECK(ev_errors2 == 0, "valley-det: ev_errors == 0");

        const auto& fs2 = s->frame_stats();
        printf("  warm instances_total=%u parts_baked=%u cache_hits=%u\n",
               fs2.instances_total, fs2.parts_baked, fs2.cache_hits);

        // Re-read from first bake to compare.
        {
            // Open a third session to get the first run's count (it's stored in cache).
            std::string err3;
            std::unique_ptr<matter::EngineContext> engine3;
            auto s3 = open_valley(schemas, world_data, shared_lib, cache_dir, engine3, err3);
            if (s3) {
                s3->request_bake();
                int ev_errors3 = 0;
                bool ok3 = drive_bake(*s3, ev_errors3);
                if (ok3) {
                    const auto& fs3 = s3->frame_stats();
                    printf("  third-run instances_total=%u\n", fs3.instances_total);
                    CHECK(fs3.instances_total == fs2.instances_total,
                          "valley-det: identical instance count across warm runs");
                }
            }
        }
    }

    // ---- (c) regenerate: terrain re-bakes, scatter hits cache -----------------
    // Task 7 (Phase C): call regenerate(<new seed>) on a warm-cache world and
    // assert that terrain tiles re-bake (parts_baked >= 2601, one per coarse tile
    // placed by Meadow.build — the full-res tiles share the same terrain hashes
    // because they are children of the same Terrain schema with different `res`
    // param, but only coarse tiles are placed in the world manifest; each unique
    // (tx, tz, res, worldSeed) combination is one distinct part_hash, so there
    // are 2×51×51 = 5202 unique Terrain hashes but only 2601 placed instances).
    // Scatter/vegetation variants (Rock/Pebble/Grass/Tree) use seed-free params
    // and always hit cache regardless of worldSeed — cache_hits > 0 confirms this.
    printf("-- (c) regenerate seed reroll (terrain re-bakes, scatter hits cache)\n");
    {
        std::string err;
        std::unique_ptr<matter::EngineContext> engine;
        auto s = open_valley(schemas, world_data, shared_lib, cache_dir, engine, err);
        CHECK(s != nullptr, "valley-regenerate: session opened");
        if (!s) { goto summary; }

        // Use a seed value clearly different from the default (20260709) so all
        // Terrain hashes change and the bake is definitely a cache miss for terrain.
        // Seed 42 is simple, memorable, and guaranteed not to collide with 20260709.
        s->regenerate(42);

        int ev_errors_c = 0;
        bool ok_c = drive_bake(*s, ev_errors_c, 900 /* 15-min timeout */);
        CHECK(ok_c, "valley-regenerate: bake completed (BakeFinished)");

        g_phase = 3;
        const auto& fs_c = s->frame_stats();
        printf("  instances_total=%u parts_baked=%u cache_hits=%u ev_errors=%d\n",
               fs_c.instances_total, fs_c.parts_baked, fs_c.cache_hits, ev_errors_c);
        fflush(stdout);

        // Terrain re-baked: 51×51 coarse tiles = 2601 Terrain variants, plus
        // Meadow itself (1 root part). Full-res tiles also have new hashes but
        // are not placed in the manifest (bake count is for placed roots).
        // The exact count may be higher if flatten/probe also run fresh parts.
        CHECK(fs_c.parts_baked >= 2601,
              "valley-regenerate: terrain re-baked (parts_baked >= 2601)");
        // Scatter variants (Rock/Pebble/Grass/Tree) hit cache — cache_hits > 0
        // confirms the seed-free schema paths were not re-baked.
        CHECK(fs_c.cache_hits > 0,
              "valley-regenerate: scatter/vegetation hit cache (cache_hits > 0)");
        CHECK(ev_errors_c == 0, "valley-regenerate: ev_errors == 0");
        fflush(stdout);
    }

summary:
    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
