/* asset_store_tests.cpp -- the correctness suite for libs/AssetStoreLib.
 *
 * These tests ARE the deliverable of M5's first half. The library's whole claim
 * is that a cache built on it cannot be corrupted by a crash and cannot hand a
 * caller garbage, so each test below is written to fail loudly if that claim is
 * false -- not to exercise the happy path.
 *
 *   1. crash-mid-write   a REAL child process dies with _exit(3) partway
 *                        through an append. The parent proves the torn bytes
 *                        actually reached the disk, then proves that reopening
 *                        makes them vanish and the torn blob reads as absent.
 *   2. corruption        a byte is flipped in a payload inside a pack file;
 *                        the read must come back Corrupt, not garbage, not a
 *                        crash.
 *   3. eviction          fill past a disk budget, show LRU order picked the
 *                        victims, show the survivors still read byte-exact
 *                        after compaction reclaims the space.
 *   4. concurrent soak   six reader threads plus a separate reader PROCESS
 *                        against one writer that is appending and committing
 *                        the whole time. Every delivered byte must be right.
 *   5. determinism       the same sequence of puts produces byte-identical
 *                        pack and index files.
 *
 * Plus the supporting behaviour those rest on: dedup, batching/coalescing, the
 * cross-process writer lock, and compaction.
 *
 * The suite spawns itself as a child in three modes -- see main(). */

#include "asset_store.h"
#include "asset_pages.h"
#include "../src/store_format.h"
/* Internal, but the checksum is the mechanism behind "corruption is a miss", so
 * the suite verifies it directly rather than only through its effects. */
#include "../src/store_hash.h"

#include <atomic>
#include <filesystem>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <process.h>
#else
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

using namespace asset_store;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        ++g_checks;                                             \
        if (!(cond)) {                                          \
            printf("  FAIL: ");                                 \
            printf(__VA_ARGS__);                                \
            printf("   [%s:%d]\n", __FILE__, __LINE__);         \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------- utilities -- */

/* Deterministic pseudo-random payload: content is a pure function of (seed,
 * len), so any process can regenerate the exact bytes it expects to read. */
static void fill_payload(std::vector<uint8_t>& buf, uint64_t seed, size_t len) {
    buf.resize(len);
    uint64_t x = seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
    for (size_t i = 0; i < len; ++i) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        buf[i] = (uint8_t)((x * 0x2545F4914F6CDD1Dull) >> 56);
    }
}

static std::string temp_root() {
    const char* base = getenv("TEMP");
    if (!base || !*base) base = getenv("TMP");
    if (!base || !*base) base = getenv("TMPDIR");
    if (!base || !*base) base = ".";
    std::string s = base;
    for (char& c : s) if (c == '\\') c = '/';
    while (!s.empty() && s[s.size() - 1] == '/') s.erase(s.size() - 1);
    return s;
}

static std::string self_path() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf, n);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; return std::string(buf); }
    return "./asset_store_tests";
#endif
}

/* Spawns this same executable with the given arguments and waits. Returns the
 * child's exit code, or -1 if it could not be started. Deliberately not
 * system(): no shell, no quoting surprises, and the exit code survives. */
static int run_child(const std::vector<std::string>& args) {
    std::string exe = self_path();
#ifdef _WIN32
    std::string cmd = "\"" + exe + "\"";
    for (const std::string& a : args) cmd += " \"" + a + "\"";
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return -2;
#endif
}

static void rm_tree(const std::string& dir) {
#ifdef _WIN32
    /* cmd.exe wants backslashes in the path, but not in the redirect. */
    std::string win = dir;
    for (size_t i = 0; i < win.size(); ++i) if (win[i] == '/') win[i] = '\\';
    std::string cmd = "rd /s /q \"" + win + "\" >nul 2>&1";
    system(cmd.c_str());
#else
    system(("rm -rf '" + dir + "'").c_str());
#endif
}

static uint64_t raw_file_size(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n < 0 ? 0 : (uint64_t)n;
}

static bool read_whole_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = out.empty() || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

/* ------------------------------------- child modes (see main() for the map) */

/* The torn blob: big enough that the abort lands well inside the payload. */
static const uint64_t kTornSeed = 99001;
static const size_t kTornLen = 512 * 1024;
static const uint64_t kAbortAfter = 40 * 1024;

static int child_crashwriter(const std::string& dir) {
    StoreConfig cfg;
    cfg.dir = dir;
    cfg.max_pack_bytes = 8ull * 1024 * 1024;
    cfg.block_for_lock = true;
    std::string err;
    auto s = BlobStore::open(cfg, &err);
    if (!s) { fprintf(stderr, "child: open failed: %s\n", err.c_str()); return 10; }

    /* Three good blobs, properly committed. These must survive the crash. */
    std::vector<uint8_t> buf;
    for (int i = 0; i < 3; ++i) {
        fill_payload(buf, 500 + i, 4096 + i * 777);
        if (s->put(buf.data(), buf.size(), nullptr) != Status::Ok) return 11;
    }
    if (!s->flush_index()) return 12;

    /* Now the tear. Re-open with the abort hook armed so the fourth append dies
     * in the middle of writing its payload -- after the record header and
     * 40 KB of body have really hit the file. */
    s.reset();
    cfg.debug_abort_mid_payload = kAbortAfter;
    s = BlobStore::open(cfg, &err);
    if (!s) return 13;
    fill_payload(buf, kTornSeed, kTornLen);
    s->put(buf.data(), buf.size(), nullptr);
    /* put() must not return -- the hook calls _exit(3). */
    fprintf(stderr, "child: abort hook did not fire\n");
    return 14;
}

static int child_reader(const std::string& dir) {
    StoreConfig cfg;
    cfg.dir = dir;
    cfg.read_only = true;
    std::string err;
    auto s = BlobStore::open(cfg, &err);
    if (!s) { fprintf(stderr, "child reader: open failed: %s\n", err.c_str()); return 20; }

    MemArena* arena = mem_arena_create(1 << 20);
    std::vector<BlobHash> hashes = s->all_hashes();
    if (hashes.empty()) { mem_arena_destroy(arena); return 21; }

    /* Read everything through the batch path and insist every byte checksums.
     * A torn read from under the writer would surface here as Corrupt. */
    for (int pass = 0; pass < 4; ++pass) {
        s->reload_index();
        hashes = s->all_hashes();
        ReadBatch b(*s);
        b.reserve(hashes.size());
        for (const BlobHash& h : hashes) b.add(h);
        mem_arena_reset(arena);
        if (!b.submit(arena)) { mem_arena_destroy(arena); return 22; }
        for (size_t i = 0; i < b.size(); ++i) {
            const ReadResult& r = b.result(i);
            if (r.status != Status::Ok) { mem_arena_destroy(arena); return 23; }
            if (hash_bytes(r.data, r.size) != r.hash) { mem_arena_destroy(arena); return 24; }
        }
    }
    mem_arena_destroy(arena);
    return 0;
}

/* Tries to take the writer lock without blocking. 7 means it succeeded. */
static int child_lockprobe(const std::string& dir) {
    StoreConfig cfg;
    cfg.dir = dir;
    cfg.read_only = false;
    cfg.block_for_lock = false;
    std::string err;
    auto s = BlobStore::open(cfg, &err);
    return s ? 7 : 0;
}

/* ============================================================== the tests == */

/* Foundations the rest of the suite leans on: bytes come back, identical bytes
 * dedup to one copy, and a hash nobody stored is Missing. */
static void test_roundtrip_and_dedup(const std::string& root) {
    printf("- roundtrip, dedup, miss\n");
    std::string dir = root + "/basic";
    rm_tree(dir);

    StoreConfig cfg;
    cfg.dir = dir;
    std::string err;
    auto s = BlobStore::open(cfg, &err);
    CHECK(s != nullptr, "open a fresh store: %s", err.c_str());
    if (!s) return;

    MemArena* arena = mem_arena_create(1 << 20);

    std::vector<uint8_t> a, b;
    fill_payload(a, 1, 1000);
    fill_payload(b, 2, 65536);

    BlobHash ha, hb, ha2;
    CHECK(s->put(a.data(), a.size(), &ha) == Status::Ok, "put a");
    CHECK(s->put(b.data(), b.size(), &hb) == Status::Ok, "put b");
    CHECK(s->put(a.data(), a.size(), &ha2) == Status::Ok, "put a again");
    CHECK(ha == ha2, "identical bytes hash identically");
    CHECK(s->blob_count() == 2, "the duplicate did not become a second blob (count=%zu)",
          s->blob_count());

    const uint8_t* p = nullptr;
    size_t n = 0;
    CHECK(s->read(ha, arena, &p, &n) == Status::Ok, "read a");
    CHECK(n == a.size() && p && memcmp(p, a.data(), n) == 0, "a came back byte-exact");
    CHECK(s->read(hb, arena, &p, &n) == Status::Ok, "read b");
    CHECK(n == b.size() && p && memcmp(p, b.data(), n) == 0, "b came back byte-exact");

    BlobHash nowhere = hash_bytes("nobody stored this", 18);
    CHECK(s->read(nowhere, arena, &p, &n) == Status::Missing, "an unknown hash is Missing");
    CHECK(p == nullptr, "a miss leaves the out-pointer null");

    /* Uncommitted puts are visible to the writer that made them, and gone for
     * anyone who reopens. */
    CHECK(s->flush_index(), "flush index");
    s.reset();

    s = BlobStore::open(cfg, &err);
    CHECK(s != nullptr, "reopen: %s", err.c_str());
    if (s) {
        CHECK(s->blob_count() == 2, "both blobs survived the reopen");
        CHECK(s->read(ha, arena, &p, &n) == Status::Ok && n == a.size() &&
              memcmp(p, a.data(), n) == 0, "a survived the reopen byte-exact");
    }

    mem_arena_destroy(arena);
    s.reset();
    rm_tree(dir);
}

/* TEST 1 -- the property the whole design rests on.
 *
 * A child process is killed for real, mid-append. We prove three things in
 * order: the tear reached the disk; reopening erases it; and the store is not
 * merely readable afterwards but still WRITABLE and consistent. */
static void test_crash_mid_write(const std::string& root) {
    printf("- crash mid-write: torn append is invisible after reopen\n");
    std::string dir = root + "/crash";
    rm_tree(dir);

    StoreConfig cfg;
    cfg.dir = dir;
    cfg.max_pack_bytes = 8ull * 1024 * 1024;

    /* Round one: five blobs, committed, store closed cleanly. */
    std::vector<BlobHash> good;
    std::vector<uint8_t> buf;
    {
        std::string err;
        auto s = BlobStore::open(cfg, &err);
        CHECK(s != nullptr, "open: %s", err.c_str());
        if (!s) return;
        for (int i = 0; i < 5; ++i) {
            fill_payload(buf, 100 + i, 8192 + i * 1234);
            BlobHash h;
            CHECK(s->put(buf.data(), buf.size(), &h) == Status::Ok, "put %d", i);
            good.push_back(h);
        }
        CHECK(s->flush_index(), "commit round one");
    }

    uint64_t committed_size = raw_file_size(dir + "/p0_0.pack");
    CHECK(committed_size > 0, "pack 0 exists after round one");

    /* Round two, in a child that dies mid-payload. */
    int code = run_child({"crashwriter", dir});
    CHECK(code == 3, "the child died in the abort hook (exit=%d, expected 3)", code);

    /* The tear must be REAL. If the child had died before touching the file
     * this test would prove nothing, so assert the pack physically grew past
     * what the committed index vouches for. */
    uint64_t torn_size = raw_file_size(dir + "/p0_0.pack");
    CHECK(torn_size > committed_size,
          "the pack physically grew past the committed extent "
          "(committed=%llu, on disk after the crash=%llu)",
          (unsigned long long)committed_size, (unsigned long long)torn_size);

    fill_payload(buf, kTornSeed, kTornLen);
    BlobHash torn_hash = hash_bytes(buf.data(), buf.size());

    /* Reopen. Recovery is: trust the index, cut the rest away. */
    {
        std::string err;
        auto s = BlobStore::open(cfg, &err);
        CHECK(s != nullptr, "reopen after the crash: %s", err.c_str());
        if (!s) return;

        CHECK(s->blob_count() == 8,
              "the 5 + 3 committed blobs are all there, and only those (count=%zu)",
              s->blob_count());
        CHECK(!s->contains(torn_hash), "the torn blob is ABSENT, not present-and-broken");

        MemArena* arena = mem_arena_create(1 << 20);
        const uint8_t* p = nullptr;
        size_t n = 0;
        CHECK(s->read(torn_hash, arena, &p, &n) == Status::Missing,
              "reading the torn blob is a plain miss");
        CHECK(p == nullptr && n == 0, "the torn read yielded no bytes at all");

        for (size_t i = 0; i < good.size(); ++i) {
            fill_payload(buf, 100 + (int)i, 8192 + (int)i * 1234);
            CHECK(s->read(good[i], arena, &p, &n) == Status::Ok, "survivor %zu reads", i);
            CHECK(n == buf.size() && p && memcmp(p, buf.data(), n) == 0,
                  "survivor %zu is byte-exact", i);
        }

        uint64_t healed = raw_file_size(dir + "/p0_0.pack");
        CHECK(healed < torn_size, "recovery truncated the torn tail (%llu -> %llu)",
              (unsigned long long)torn_size, (unsigned long long)healed);

        /* And the store still works: a fresh append lands where the tear was. */
        fill_payload(buf, 777, 20000);
        BlobHash after;
        CHECK(s->put(buf.data(), buf.size(), &after) == Status::Ok, "put after recovery");
        CHECK(s->flush_index(), "commit after recovery");
        mem_arena_destroy(arena);
        s.reset();

        auto s2 = BlobStore::open(cfg, &err);
        CHECK(s2 != nullptr, "reopen once more: %s", err.c_str());
        if (s2) {
            CHECK(s2->blob_count() == 9, "the post-recovery write committed (count=%zu)",
                  s2->blob_count());
            MemArena* ar = mem_arena_create(1 << 20);
            CHECK(s2->read(after, ar, &p, &n) == Status::Ok && n == buf.size() &&
                  memcmp(p, buf.data(), n) == 0, "the post-recovery blob is byte-exact");
            mem_arena_destroy(ar);
        }
    }
    rm_tree(dir);
}

/* TEST 2 -- a flipped byte in a pack payload must degrade to a miss. */
static void test_corruption_is_a_miss(const std::string& root) {
    printf("- corruption: a flipped payload byte reads Corrupt, never garbage\n");
    std::string dir = root + "/corrupt";
    rm_tree(dir);

    StoreConfig cfg;
    cfg.dir = dir;
    std::string err;

    std::vector<BlobHash> hs;
    std::vector<uint8_t> buf;
    BlobLocation victim_loc;
    BlobHash victim;
    {
        auto s = BlobStore::open(cfg, &err);
        CHECK(s != nullptr, "open: %s", err.c_str());
        if (!s) return;
        for (int i = 0; i < 6; ++i) {
            fill_payload(buf, 300 + i, 16384);
            BlobHash h;
            s->put(buf.data(), buf.size(), &h);
            hs.push_back(h);
        }
        CHECK(s->flush_index(), "commit");
        victim = hs[3];
        CHECK(s->locate(victim, &victim_loc), "locate the victim blob");
    }

    /* Flip one bit, deep inside the payload -- past the header, so nothing but
     * the payload CRC can catch it. */
    {
        std::string pack = dir + "/p0_0.pack";
        FILE* f = fopen(pack.c_str(), "r+b");
        CHECK(f != nullptr, "open the pack for surgery");
        if (!f) return;
        uint64_t off = victim_loc.offset + victim_loc.length / 2;
        fseek(f, (long)off, SEEK_SET);
        int c = fgetc(f);
        fseek(f, (long)off, SEEK_SET);
        fputc(c ^ 0x01, f);
        fclose(f);
    }

    {
        auto s = BlobStore::open(cfg, &err);
        CHECK(s != nullptr, "reopen: %s", err.c_str());
        if (!s) return;
        MemArena* arena = mem_arena_create(1 << 20);
        const uint8_t* p = (const uint8_t*)1;
        size_t n = 1;
        Status st = s->read(victim, arena, &p, &n);
        CHECK(st == Status::Corrupt, "the damaged blob reads Corrupt (got %s)",
              status_name(st));
        CHECK(p == nullptr && n == 0, "no bytes were handed back for a corrupt blob");

        /* Its neighbours in the very same pack are untouched. Damage is
         * per-blob, not per-file. */
        for (size_t i = 0; i < hs.size(); ++i) {
            if (hs[i] == victim) continue;
            fill_payload(buf, 300 + (int)i, 16384);
            const uint8_t* q = nullptr;
            size_t m = 0;
            CHECK(s->read(hs[i], arena, &q, &m) == Status::Ok, "neighbour %zu still reads", i);
            CHECK(m == buf.size() && q && memcmp(q, buf.data(), m) == 0,
                  "neighbour %zu is byte-exact", i);
        }

        /* And a batch containing the corrupt blob still delivers the rest --
         * one bad blob does not poison its chunk. */
        ReadBatch b(*s);
        for (const BlobHash& h : hs) b.add(h);
        mem_arena_reset(arena);
        CHECK(b.submit(arena), "batch submit over a damaged pack");
        CHECK(b.stats().corrupt == 1, "exactly one blob in the batch was corrupt (%u)",
              b.stats().corrupt);
        int ok_count = 0;
        for (size_t i = 0; i < b.size(); ++i)
            if (b.result(i).status == Status::Ok) ++ok_count;
        CHECK(ok_count == 5, "the other five were delivered anyway (%d)", ok_count);

        mem_arena_destroy(arena);
    }
    rm_tree(dir);
}

/* TEST 3 -- LRU eviction against a disk budget, then real space reclaimed. */
static void test_eviction_to_budget(const std::string& root) {
    printf("- eviction: LRU against a disk budget, survivors intact\n");
    std::string dir = root + "/evict";
    rm_tree(dir);

    const int kBlobs = 24;
    const size_t kSize = 32 * 1024;
    const uint64_t kBudget = 8 * kSize;   /* room for 8 of the 24 */

    StoreConfig cfg;
    cfg.dir = dir;
    cfg.max_pack_bytes = 4ull * 1024 * 1024;
    std::string err;
    auto s = BlobStore::open(cfg, &err);
    CHECK(s != nullptr, "open: %s", err.c_str());
    if (!s) return;

    RefTableConfig rcfg;
    rcfg.budget_bytes = kBudget;
    auto t = RefTable::open(*s, rcfg, &err);
    CHECK(t != nullptr, "open ref table: %s", err.c_str());
    if (!t) return;

    std::vector<uint8_t> buf;
    std::vector<std::string> keys;
    for (int i = 0; i < kBlobs; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "artifact/%03d", i);
        keys.push_back(key);
        fill_payload(buf, 4000 + i, kSize);
        BlobHash h;
        CHECK(s->put(buf.data(), buf.size(), &h) == Status::Ok, "put %d", i);
        t->put(key, h, /*kind*/ 1, kSize);
    }
    CHECK(s->flush_index(), "commit blobs");
    CHECK(t->live_bytes() == (uint64_t)kBlobs * kSize,
          "all %d blobs are accounted against the budget (%llu)", kBlobs,
          (unsigned long long)t->live_bytes());
    CHECK(t->live_bytes() > kBudget, "we are genuinely over budget before eviction");

    /* Touch the four oldest keys so they become the four newest. If eviction is
     * LRU rather than insertion-order, exactly these must survive. */
    for (int i = 0; i < 4; ++i) {
        RefInfo ri;
        CHECK(t->lookup(keys[i], &ri), "touch %s", keys[i].c_str());
    }

    EvictStats es = t->evict_to_budget();
    printf("    evicted %llu refs, freed %llu bytes, live now %llu (budget %llu)\n",
           (unsigned long long)es.refs_evicted, (unsigned long long)es.bytes_freed,
           (unsigned long long)es.bytes_live_after, (unsigned long long)kBudget);

    CHECK(t->live_bytes() <= kBudget, "live bytes are back inside the budget (%llu <= %llu)",
          (unsigned long long)t->live_bytes(), (unsigned long long)kBudget);
    CHECK(t->count() == 8, "eight refs survived (%zu)", t->count());

    for (int i = 0; i < 4; ++i)
        CHECK(t->peek(keys[i], nullptr), "the touched key %s survived LRU", keys[i].c_str());
    for (int i = 4; i < 4 + 12; ++i)
        CHECK(!t->peek(keys[i], nullptr), "the stale key %s was evicted", keys[i].c_str());
    for (int i = kBlobs - 4; i < kBlobs; ++i)
        CHECK(t->peek(keys[i], nullptr), "the newest key %s survived", keys[i].c_str());

    /* Evicting refs does not free disk on its own. Compaction is what does. */
    uint64_t packs_before = s->pack_bytes();
    CompactStats cs;
    CHECK(t->compact(&cs), "compact");
    printf("    compaction kept %llu blobs / %llu bytes, dropped %llu, reclaimed %llu bytes\n",
           (unsigned long long)cs.blobs_kept, (unsigned long long)cs.bytes_kept,
           (unsigned long long)cs.blobs_dropped, (unsigned long long)cs.bytes_reclaimed);
    CHECK(cs.blobs_kept == 8, "compaction kept exactly the referenced blobs (%llu)",
          (unsigned long long)cs.blobs_kept);
    CHECK(cs.blobs_dropped == (uint64_t)kBlobs - 8, "the orphans were dropped (%llu)",
          (unsigned long long)cs.blobs_dropped);
    CHECK(s->pack_bytes() < packs_before, "pack bytes actually shrank (%llu -> %llu)",
          (unsigned long long)packs_before, (unsigned long long)s->pack_bytes());
    CHECK(s->blob_count() == 8, "the index now names only the survivors (%zu)",
          s->blob_count());

    /* The whole point: survivors are still correct after their bytes moved. */
    MemArena* arena = mem_arena_create(1 << 20);
    for (const std::string& k : t->keys()) {
        RefInfo ri;
        CHECK(t->peek(k, &ri), "peek %s", k.c_str());
        int idx = atoi(k.c_str() + strlen("artifact/"));
        fill_payload(buf, 4000 + idx, kSize);
        const uint8_t* p = nullptr;
        size_t n = 0;
        Status st = s->read(ri.hash, arena, &p, &n);
        CHECK(st == Status::Ok, "survivor %s reads after compaction (%s)", k.c_str(),
              status_name(st));
        CHECK(n == kSize && p && memcmp(p, buf.data(), n) == 0,
              "survivor %s is byte-exact after its bytes were rewritten", k.c_str());
    }
    mem_arena_destroy(arena);

    /* And the surviving refs come back after a close/reopen cycle. */
    s.reset(); t.reset();
    {
        auto s2 = BlobStore::open(cfg, &err);
        CHECK(s2 != nullptr, "reopen after compaction: %s", err.c_str());
        if (s2) {
            auto t2 = RefTable::open(*s2, rcfg, &err);
            CHECK(t2 != nullptr, "reopen ref table: %s", err.c_str());
            if (t2) {
                CHECK(t2->count() == 8, "refs persisted (%zu)", t2->count());
                CHECK(t2->live_bytes() <= kBudget, "still inside the budget after reload");
            }
            CHECK(s2->blob_count() == 8, "blob index persisted (%zu)", s2->blob_count());
        }
    }
    rm_tree(dir);
}

/* TEST 4 -- many readers, one writer, no torn reads. */
static void test_concurrent_soak(const std::string& root) {
    printf("- concurrent soak: 6 reader threads + 1 reader process vs a live writer\n");
    std::string dir = root + "/soak";
    rm_tree(dir);

    StoreConfig wcfg;
    wcfg.dir = dir;
    wcfg.max_pack_bytes = 16ull * 1024 * 1024;
    std::string err;
    auto w = BlobStore::open(wcfg, &err);
    CHECK(w != nullptr, "open writer: %s", err.c_str());
    if (!w) return;

    /* A seeded population so readers have something to chew on from the start. */
    std::vector<uint8_t> buf;
    for (int i = 0; i < 120; ++i) {
        fill_payload(buf, 9000 + i, 4096 + (i * 331) % 60000);
        w->put(buf.data(), buf.size(), nullptr);
    }
    CHECK(w->flush_index(), "commit the seed population");

    /* While the writer keeps appending and committing, a second PROCESS must be
     * refused the writer lock -- single writer, enforced across processes. */
    int probe = run_child({"lockprobe", dir});
    CHECK(probe == 0, "a second process could NOT take the writer lock (probe=%d)", probe);

    std::atomic<bool> stop(false);
    std::atomic<int> bad_status(0);
    std::atomic<int> bad_bytes(0);
    std::atomic<long long> reads(0);

    auto reader_fn = [&](int id) {
        StoreConfig rcfg;
        rcfg.dir = dir;
        rcfg.read_only = true;
        std::string e;
        auto r = BlobStore::open(rcfg, &e);
        if (!r) { bad_status.fetch_add(1000); return; }
        MemArena* arena = mem_arena_create(1 << 20);
        uint32_t x = 12345u + id * 7919u;
        while (!stop.load()) {
            r->reload_index();
            std::vector<BlobHash> all = r->all_hashes();
            if (all.empty()) continue;
            ReadBatch b(*r);
            size_t take = all.size() < 32 ? all.size() : 32;
            for (size_t i = 0; i < take; ++i) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                b.add(all[x % all.size()]);
            }
            mem_arena_reset(arena);
            if (!b.submit(arena)) { bad_status.fetch_add(1); continue; }
            for (size_t i = 0; i < b.size(); ++i) {
                const ReadResult& res = b.result(i);
                /* Missing is legal: this reader's index snapshot may predate a
                 * commit. Corrupt or IoError is NOT -- that would be a torn
                 * read, which is the thing this test exists to rule out. */
                if (res.status == Status::Missing) continue;
                if (res.status != Status::Ok) { bad_status.fetch_add(1); continue; }
                /* Content-addressed: rehashing the delivered bytes is a total
                 * check that they are exactly the bytes that were stored. */
                if (hash_bytes(res.data, res.size) != res.hash) bad_bytes.fetch_add(1);
                reads.fetch_add(1);
            }
        }
        mem_arena_destroy(arena);
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < 6; ++i) readers.emplace_back(reader_fn, i);

    /* The writer churns: append a handful, commit, repeat. Every commit is a
     * rename racing against six readers mid-batch. */
    for (int round = 0; round < 60; ++round) {
        for (int i = 0; i < 5; ++i) {
            fill_payload(buf, 20000 + round * 5 + i, 8192 + ((round * 5 + i) * 977) % 90000);
            w->put(buf.data(), buf.size(), nullptr);
        }
        if (!w->flush_index()) { CHECK(false, "commit in round %d", round); break; }
    }

    /* A whole separate process reading the committed store while the writer
     * still holds the lock. */
    int rc = run_child({"reader", dir});
    CHECK(rc == 0, "the reader PROCESS verified every blob (exit=%d)", rc);

    stop.store(true);
    for (std::thread& t : readers) t.join();

    printf("    %lld verified blob reads across 6 threads\n", (long long)reads.load());
    CHECK(reads.load() > 1000, "the soak actually did work (%lld reads)",
          (long long)reads.load());
    CHECK(bad_status.load() == 0, "no Corrupt/IoError result in any reader (%d)",
          bad_status.load());
    CHECK(bad_bytes.load() == 0, "every delivered payload rehashed to its own key (%d bad)",
          bad_bytes.load());
    CHECK(w->blob_count() == 420, "writer committed all 420 blobs (%zu)", w->blob_count());

    w.reset();
    rm_tree(dir);
}

/* TEST 5 -- the same writes produce the same bytes. */
static void test_determinism(const std::string& root) {
    printf("- determinism: identical writes produce identical pack and index bytes\n");
    std::string dirA = root + "/detA";
    std::string dirB = root + "/detB";
    rm_tree(dirA);
    rm_tree(dirB);

    auto build = [&](const std::string& dir) {
        StoreConfig cfg;
        cfg.dir = dir;
        cfg.max_pack_bytes = 512 * 1024;   /* force a pack roll too */
        std::string err;
        auto s = BlobStore::open(cfg, &err);
        if (!s) return false;
        std::vector<uint8_t> buf;
        for (int i = 0; i < 40; ++i) {
            fill_payload(buf, 6000 + i, 3000 + (i * 1777) % 50000);
            if (s->put(buf.data(), buf.size(), nullptr) != Status::Ok) return false;
            if (i % 7 == 6) s->flush_index();   /* commits at irregular points */
        }
        return s->flush_index();
    };

    CHECK(build(dirA), "build store A");
    CHECK(build(dirB), "build store B");

    std::vector<uint8_t> a, b;
    int packs_compared = 0;
    for (uint32_t i = 0;; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "/p0_%u.pack", (unsigned)i);
        std::string pa = dirA + name, pb = dirB + name;
        if (!read_whole_file(pa, a)) break;
        CHECK(read_whole_file(pb, b), "store B has pack %u too", i);
        CHECK(a.size() == b.size() && a == b, "pack %u is byte-identical (%zu vs %zu bytes)",
              i, a.size(), b.size());
        ++packs_compared;
    }
    CHECK(packs_compared >= 2, "the run rolled over into a second pack (%d compared)",
          packs_compared);

    CHECK(read_whole_file(dirA + "/index.bin", a), "read index A");
    CHECK(read_whole_file(dirB + "/index.bin", b), "read index B");
    CHECK(a.size() == b.size() && a == b, "index.bin is byte-identical");

    rm_tree(dirA);
    rm_tree(dirB);
}

/* Supporting behaviour: the batch really does coalesce, which is the entire
 * mechanism the benchmark measures. */
static void test_batch_coalescing(const std::string& root) {
    printf("- batching: N sequential blobs become a handful of reads\n");
    std::string dir = root + "/batch";
    rm_tree(dir);

    StoreConfig cfg;
    cfg.dir = dir;
    cfg.max_pack_bytes = 64ull * 1024 * 1024;
    std::string err;
    auto s = BlobStore::open(cfg, &err);
    CHECK(s != nullptr, "open: %s", err.c_str());
    if (!s) return;

    std::vector<uint8_t> buf;
    std::vector<BlobHash> hs;
    for (int i = 0; i < 200; ++i) {
        fill_payload(buf, 7000 + i, 20000);
        BlobHash h;
        s->put(buf.data(), buf.size(), &h);
        hs.push_back(h);
    }
    CHECK(s->flush_index(), "commit");

    MemArena* arena = mem_arena_create(8 << 20);
    ReadBatch b(*s);
    b.reserve(hs.size());
    /* Add them in a deliberately scrambled order -- submit() is what puts them
     * back into physical order. */
    for (size_t i = 0; i < hs.size(); ++i) b.add(hs[(i * 97) % hs.size()]);
    CHECK(b.submit(arena), "submit");

    printf("    %u requests -> %u chunk reads, %llu bytes read for %llu delivered\n",
           b.stats().requests, b.stats().chunk_reads,
           (unsigned long long)b.stats().bytes_read,
           (unsigned long long)b.stats().bytes_delivered);
    CHECK(b.stats().chunk_reads <= 4,
          "200 contiguous blobs collapsed into <=4 reads (got %u)", b.stats().chunk_reads);
    CHECK(b.stats().bytes_read < b.stats().bytes_delivered * 11 / 10,
          "coalescing did not read much more than it delivered");

    int ok = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        const ReadResult& r = b.result(i);
        if (r.status != Status::Ok) continue;
        if (hash_bytes(r.data, r.size) == r.hash) ++ok;
    }
    CHECK(ok == 200, "every blob in the batch came back correct (%d)", ok);

    /* Results are in add() order, not read order -- callers index by request. */
    CHECK(b.result(0).hash == hs[0], "result 0 corresponds to request 0");
    CHECK(b.result(1).hash == hs[97 % hs.size()], "result 1 corresponds to request 1");

    mem_arena_destroy(arena);
    s.reset();
    rm_tree(dir);
}

/* Supporting behaviour: reads land in the caller's arena, not the heap. */
static void test_arena_landing(const std::string& root) {
    printf("- arena landing: payloads come out of the caller's MemoryLib arena\n");
    std::string dir = root + "/arena";
    rm_tree(dir);

    StoreConfig cfg;
    cfg.dir = dir;
    std::string err;
    auto s = BlobStore::open(cfg, &err);
    CHECK(s != nullptr, "open: %s", err.c_str());
    if (!s) return;

    std::vector<uint8_t> buf;
    std::vector<BlobHash> hs;
    for (int i = 0; i < 32; ++i) {
        fill_payload(buf, 8000 + i, 10000);
        BlobHash h;
        s->put(buf.data(), buf.size(), &h);
        hs.push_back(h);
    }
    s->flush_index();

    MemArena* arena = mem_arena_create(1 << 20);
    MemStats before;
    mem_arena_get_stats(arena, &before);

    ReadBatch b(*s);
    for (const BlobHash& h : hs) b.add(h);
    CHECK(b.submit(arena), "submit");

    MemStats after;
    mem_arena_get_stats(arena, &after);
    CHECK(after.liveBytes >= before.liveBytes + 32 * 10000,
          "the arena absorbed every payload (%zu -> %zu live bytes)",
          before.liveBytes, after.liveBytes);
    /* One allocation per coalesced CHUNK, not per blob: the chunk is read
     * straight into the arena and the results point into it. Thirty-two
     * contiguous blobs are one chunk, so one allocation. */
    CHECK(after.totalAllocs - before.totalAllocs == b.stats().chunk_reads,
          "one arena allocation per chunk read (%zu allocs, %u chunks)",
          after.totalAllocs - before.totalAllocs, b.stats().chunk_reads);
    CHECK(b.stats().chunk_reads == 1, "the 32 blobs coalesced into a single read (%u)",
          b.stats().chunk_reads);

    /* The payloads really are inside that one allocation, in physical order. */
    const uint8_t* lo = b.result(0).data;
    const uint8_t* hi = b.result(31).data;
    CHECK(lo != nullptr && hi != nullptr, "both ends were delivered");
    CHECK(hi > lo && (size_t)(hi - lo) < 32 * 10008 + 4096,
          "every payload lives inside one contiguous arena chunk");
    for (size_t i = 0; i < b.size(); ++i)
        CHECK(hash_bytes(b.result(i).data, b.result(i).size) == b.result(i).hash,
              "blob %zu is byte-exact where it lies", i);

    /* A reset frees all 32 payloads at once -- the reason arenas are the
     * landing zone in the first place. */
    mem_arena_reset(arena);
    MemStats reset_stats;
    mem_arena_get_stats(arena, &reset_stats);
    CHECK(reset_stats.liveBytes == 0, "reset released every payload in one call (%zu)",
          reset_stats.liveBytes);

    mem_arena_destroy(arena);
    s.reset();
    rm_tree(dir);
}

/* The checksum itself. Every "corruption is a miss" guarantee reduces to this
 * function being right, and it was rewritten to slice-by-eight for speed after
 * the benchmark found it dominating the read path -- so it is pinned against
 * the published CRC-32 check value and against a from-scratch reference. */
static void test_crc32_is_correct() {
    printf("- crc32: slice-by-eight agrees with the reference, bit for bit\n");

    CHECK(crc32("123456789", 9) == 0xCBF43926u,
          "the standard CRC-32 check value (got 0x%08X)",
          (unsigned)crc32("123456789", 9));
    CHECK(crc32("", 0) == 0u, "the empty string checksums to zero");

    /* A byte-at-a-time reference, written here so the fast path has something
     * independent to be wrong against. */
    auto reference = [](const uint8_t* p, size_t n) {
        uint32_t table[256];
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    };

    /* Every length from 0 to 200 covers each residue of the eight-byte stride,
     * including all seven tail cases. */
    std::vector<uint8_t> buf;
    int mismatches = 0;
    for (size_t len = 0; len <= 200; ++len) {
        fill_payload(buf, 4242 + len, len ? len : 1);
        if (len == 0) buf.clear();
        if (crc32(buf.data(), len) != reference(buf.data(), len)) ++mismatches;
    }
    CHECK(mismatches == 0, "all 201 lengths agree with the reference (%d differ)",
          mismatches);

    /* And one large buffer, to exercise the fast loop properly. */
    fill_payload(buf, 31337, 1 << 20);
    CHECK(crc32(buf.data(), buf.size()) == reference(buf.data(), buf.size()),
          "a 1 MiB buffer agrees with the reference");

    /* A single flipped bit anywhere must change the checksum -- the property
     * the corruption test depends on. */
    fill_payload(buf, 555, 4096);
    uint32_t base = crc32(buf.data(), buf.size());
    int missed = 0;
    for (size_t i = 0; i < buf.size(); i += 97) {
        buf[i] ^= 0x01;
        if (crc32(buf.data(), buf.size()) == base) ++missed;
        buf[i] ^= 0x01;
    }
    CHECK(missed == 0, "every single-bit flip changed the checksum (%d missed)", missed);
}

/* Maintenance cannot retire packs still reachable from a reader snapshot,
 * including a reader that has never opened its first pack. */
static void test_quiescent_compaction(const std::string& root) {
    printf("- maintenance: lazy readers prevent compaction until they retire\n");
    std::string dir = root + "/reload";
    rm_tree(dir);

    std::string err;
    std::vector<uint8_t> buf;
    BlobHash h;

    StoreConfig wcfg;
    wcfg.dir = dir;
    auto w = BlobStore::open(wcfg, &err);
    CHECK(w != nullptr, "open writer: %s", err.c_str());
    if (!w) return;
    fill_payload(buf, 909090, 6000);
    CHECK(w->put(buf.data(), buf.size(), &h) == Status::Ok, "put the blob");
    CHECK(w->flush_index(), "commit the first index");

    StoreConfig rcfg;
    rcfg.dir = dir;
    rcfg.read_only = true;
    auto r = BlobStore::open(rcfg, &err);
    CHECK(r != nullptr, "open reader: %s", err.c_str());
    if (!r) return;
    CHECK(r->generation() == w->generation(), "reader starts on the writer's generation");

    uint64_t size_before = raw_file_size(dir + "/index.bin");

    /* Keep everything: one pack, one entry, before and after. */
    CompactStats cs;
    CHECK(!w->compact(&h, 1, &cs), "lazy reader blocks pack retirement");
    CHECK(w->generation() == 0, "failed maintenance preserves prior generation");
    r.reset();
    CHECK(w->compact(&h, 1, &cs), "compact keeping every blob");
    r = BlobStore::open(rcfg, &err);
    CHECK(r != nullptr, "reader reopens after maintenance"); if (!r) return;
    CHECK(cs.blobs_kept == 1, "the blob survived compaction");
    CHECK(w->generation() == 1, "compaction started a new generation");

    uint64_t size_after = raw_file_size(dir + "/index.bin");
    CHECK(size_before == size_after,
          "the two indexes really are the same length (%llu vs %llu) -- if this "
          "fails the test is no longer exercising the stamp collision",
          (unsigned long long)size_before, (unsigned long long)size_after);

    CHECK(r->reload_index(), "reader reloads: %s", r->last_error().c_str());
    CHECK(r->generation() == 1, "the reader picked up the new generation");

    MemArena* arena = mem_arena_create(1 << 20);
    const uint8_t* data = nullptr;
    size_t len = 0;
    Status st = r->read(h, arena, &data, &len);
    CHECK(st == Status::Ok, "the reloaded reader can still read the blob (%s)",
          status_name(st));
    CHECK(len == buf.size() && data != nullptr && memcmp(data, buf.data(), buf.size()) == 0,
          "and the bytes are the ones that were written");
    mem_arena_destroy(arena);
}

/* A read-only handle must be read-only all the way down -- including the ref
 * table sitting on top of it, whose flush would otherwise stamp a reader's
 * private view over the writer's refs.bin. */
static void test_read_only_cannot_write(const std::string& root) {
    printf("- read-only: neither the store nor its ref table can write\n");
    std::string dir = root + "/ro";
    rm_tree(dir);

    std::vector<uint8_t> buf;
    BlobHash h;
    std::string err;
    {
        StoreConfig cfg;
        cfg.dir = dir;
        auto s = BlobStore::open(cfg, &err);
        CHECK(s != nullptr, "open writer: %s", err.c_str());
        if (!s) return;
        fill_payload(buf, 12321, 5000);
        s->put(buf.data(), buf.size(), &h);
        s->flush_index();
        RefTableConfig rcfg;
        auto t = RefTable::open(*s, rcfg, &err);
        CHECK(t != nullptr, "open ref table");
        if (t) { t->put("keep/me", h, 1, 5000); CHECK(t->flush(), "flush refs"); }
    }

    uint64_t refs_size_before = raw_file_size(dir + "/refs.bin");
    uint64_t pack_size_before = raw_file_size(dir + "/p0_0.pack");

    StoreConfig rcfg;
    rcfg.dir = dir;
    rcfg.read_only = true;
    auto r = BlobStore::open(rcfg, &err);
    CHECK(r != nullptr, "open read-only: %s", err.c_str());
    if (!r) return;

    CHECK(r->read_only(), "the handle reports itself read-only");
    fill_payload(buf, 45678, 3000);
    CHECK(r->put(buf.data(), buf.size(), nullptr) == Status::ReadOnly,
          "put through a read-only handle is refused");
    CHECK(!r->flush_index(), "flush_index through a read-only handle is refused");
    CHECK(!r->compact(nullptr, 0, nullptr), "compact through a read-only handle is refused");

    RefTableConfig tcfg;
    auto t = RefTable::open(*r, tcfg, &err);
    CHECK(t != nullptr, "open ref table over a read-only store: %s", err.c_str());
    if (t) {
        CHECK(t->count() == 1, "the reader sees the writer's ref (%zu)", t->count());
        BlobHash other = hash_bytes("something else", 14);
        CHECK(!t->put("sneak/in", other, 2, 10), "ref put is refused");
        CHECK(!t->erase("keep/me"), "ref erase is refused");
        CHECK(!t->flush(), "ref flush is refused");
        /* lookup() may bump last-access in memory -- that is harmless precisely
         * because flush() will never carry it to disk. */
        RefInfo ri;
        CHECK(t->lookup("keep/me", &ri), "lookup still works for a reader");
        CHECK(!t->flush(), "and still cannot be persisted afterwards");
    }

    CHECK(raw_file_size(dir + "/refs.bin") == refs_size_before,
          "refs.bin on disk is untouched");
    CHECK(raw_file_size(dir + "/p0_0.pack") == pack_size_before,
          "the pack on disk is untouched");

    r.reset();
    t.reset();
    rm_tree(dir);
}

static void test_manifest_directory_reuse(const std::string& root) {
    printf("- manifest directory: reuse unchanged data and observe replacement/removal\n");
    StoreConfig cfg;cfg.dir=root+"/manifest_reuse";std::string error;
    auto writer=BlobStore::open(cfg,&error);CHECK(writer!=nullptr,"manifest writer");if(!writer)return;
    auto refs=RefTable::open(*writer,{},&error);CHECK(refs!=nullptr,"manifest refs");if(!refs)return;
    PageLimits limits;PageSection section{1,1,1,{1,2,3,4}};
    std::vector<uint8_t> bytes;BlobHash first,second;
    CHECK(encode_page(1,{section},{},limits,bytes,error),"encode first manifest");
    CHECK(publish_page_manifest(*writer,*refs,"asset",bytes,limits,first,error),"publish first manifest");
    PageCacheConfig cc;cc.store=cfg;auto reader=PageCache::open(cc,error);
    CHECK(reader!=nullptr,"manifest reader");if(!reader)return;
    for(int i=0;i<16;++i){auto result=reader->read_manifest("asset");CHECK(result.page&&result.page->hash==first,"unchanged manifest remains readable");}
    CHECK(reader->stats().reference_reloads==1,"warm lookup parses the directory once");
    section.bytes[0]=9;CHECK(encode_page(1,{section},{},limits,bytes,error),"encode same-size replacement");
    CHECK(publish_page_manifest(*writer,*refs,"asset",bytes,limits,second,error),"publish replacement manifest");
    auto result=reader->read_manifest("asset");CHECK(result.page&&result.page->hash==second,"existing reader observes same-size atomic reference replacement");
    CHECK(reader->stats().reference_reloads==2,"changed directory reloads once");
    CHECK(refs->erase("asset")&&refs->flush(),"publish erased reference");
    CHECK(reader->read_manifest("asset").status==PageStatus::Missing,"erased manifest cannot survive in the directory cache");
    CHECK(reader->stats().reference_reloads==3,"erased directory reloads once");
    CHECK(publish_page_manifest(*writer,*refs,"asset",bytes,limits,second,error),"republish reference");
    result=reader->read_manifest("asset");CHECK(result.page&&result.page->hash==second,"missing reference recovers after publication");
}

static void test_bounded_reads(const std::string& root) {
    printf("- bounded reads: span, total holes, deduplicated checksums and exact reservation\n");
    for (int mode = 0; mode < 3; ++mode) {
        StoreConfig cfg; cfg.dir = root + "/bounded" + std::to_string(mode);
        cfg.batch_max_bytes = mode == 0 ? 240 : 4096;
        cfg.batch_max_overread_bytes = mode == 1 ? 32 : 4096;
        cfg.batch_gap_bytes = mode == 2 ? 0 : 4096;
        std::string error; auto store = BlobStore::open(cfg, &error);
        CHECK(store != nullptr, "open bounded store"); if (!store) continue;
        std::vector<BlobHash> hashes;
        for (int i = 0; i < 5; ++i) {
            std::vector<uint8_t> data(100, static_cast<uint8_t>(i)); BlobHash h;
            CHECK(store->put(data.data(), data.size(), &h) == Status::Ok, "put bounded blob"); hashes.push_back(h);
        }
        ReadBatch batch(*store);
        for (auto h : hashes) { batch.add(h); batch.add(h); }
        const size_t needed = batch.allocation_bytes();
        auto* arena = mem_arena_create(needed);
        CHECK(batch.submit(arena), "bounded submit");
        CHECK(batch.stats().chunk_reads == (mode == 0 ? 3u : 5u), "span/holes bound read count");
        CHECK(batch.stats().checksum_count == 5 && batch.stats().duplicate_requests == 5, "validate unique blobs only");
        CHECK(batch.stats().unique_bytes_delivered == 500 && batch.stats().bytes_delivered == 1000, "dedup accounting");
        MemStats st{}; mem_arena_get_stats(arena, &st);
        CHECK(st.pageCount == 1 && st.liveBytes == needed, "reservation exact, no arena growth");
        for (size_t i = 0; i < batch.size(); i += 2)
            CHECK(batch.result(i).data == batch.result(i+1).data && batch.result(i).status == Status::Ok, "duplicate shared view");
        mem_arena_destroy(arena);
    }
}

static void test_failed_page_publication(const std::string& root) {
    printf("- page commits: failed flush/ref publication retains the accepted revision\n");
    StoreConfig cfg; cfg.dir = root + "/failed_page_commit";
    std::string error; auto writer = BlobStore::open(cfg, &error);
    CHECK(writer != nullptr, "open failed-commit writer"); if (!writer) return;
    auto refs = RefTable::open(*writer, {}, &error); CHECK(refs != nullptr, "refs"); if (!refs) return;
    PageLimits limits; PageSection section{1,1,1,{1,2,3,4}};
    std::vector<uint8_t> first, second;
    CHECK(encode_page(1, {section}, {}, limits, first, error), "first manifest");
    BlobHash accepted;
    CHECK(publish_page_manifest(*writer, *refs, "asset", first, limits, accepted, error), "accept first revision");
    section.bytes[0] = 9;
    CHECK(encode_page(1, {section}, {}, limits, second, error), "second manifest");
    std::filesystem::create_directory(cfg.dir + "/refs.tmp");
    BlobHash attempted;
    CHECK(!publish_page_manifest(*writer, *refs, "asset", second, limits, attempted, error), "ref write failure refused");
    RefInfo info;CHECK(refs->peek("asset", &info) && info.hash == accepted, "in-memory ref restored");
    PageCacheConfig pc;pc.store = cfg;auto reader = PageCache::open(pc,error);
    CHECK(reader && reader->read_manifest("asset").page && reader->read_manifest("asset").page->hash == accepted, "on-disk ref retains accepted revision");
    std::filesystem::remove(cfg.dir + "/refs.tmp");
    reader.reset();refs.reset();writer.reset();
    cfg.debug_fail_pack_flush = true;
    writer = BlobStore::open(cfg, &error);refs = RefTable::open(*writer, {}, &error);
    section.bytes[0] = 10;CHECK(encode_page(1, {section}, {}, limits, second, error), "third manifest");
    CHECK(!publish_page_manifest(*writer, *refs, "asset", second, limits, attempted, error), "payload flush failure prevents index and ref publication");
    pc.store = cfg;reader = PageCache::open(pc,error);
    CHECK(reader && reader->read_manifest("asset").page && reader->read_manifest("asset").page->hash == accepted, "failed durability leaves old accepted revision readable");
    CHECK(reader && reader->read({hash_bytes(second.data(),second.size())})[0].status == PageStatus::Missing, "failed flush payload is invisible");
}

static void test_bulk_append(const std::string& root) {
    printf("- bulk append: bounded staging, dedup, rollover and visibility\n");
    StoreConfig cfg; cfg.dir = root + "/bulk"; cfg.max_pack_bytes = 300;
    std::string error; auto writer = BlobStore::open(cfg, &error);
    CHECK(writer != nullptr, "open bulk writer"); if (!writer) return;
    auto rcfg = cfg; rcfg.read_only = true;
    auto reader = BlobStore::open(rcfg, &error); CHECK(reader != nullptr, "open old reader"); if (!reader) return;
    std::vector<uint8_t> a(100, 1), b(100, 2), c(100, 3);
    std::vector<BlobInput> input{{a.data(), a.size()}, {b.data(), b.size()}, {a.data(), a.size()}, {c.data(), c.size()}};
    std::vector<BlobHash> hashes; WriteBatchStats stats;
    CHECK(writer->put_batch(input, 200, hashes, &stats) == Status::IoError && writer->blob_count() == 0, "reject oversized before any append");
    CHECK(writer->put_batch(input, 1024, hashes, &stats) == Status::Ok, "bulk append");
    CHECK(hashes.size() == 4 && hashes[0] == hashes[2], "caller-order hashes and dedup");
    CHECK(stats.write_calls == 2 && stats.blobs_written == 3 && stats.bytes_written == 408, "one write per pack");
    CHECK(!reader->contains(hashes[0]), "uncommitted invisible");
    CHECK(writer->flush_index() && reader->reload_index(), "commit and refresh");
    CHECK(reader->contains(hashes[3]), "reader sees whole batch");
    CHECK(writer->put_batch(input, 1, hashes, &stats) == Status::Ok && stats.write_calls == 0, "all duplicate batch writes nothing");
    auto* arena = mem_arena_create(1024); ReadBatch read(*reader);
    for (auto h : hashes) read.add(h);
    CHECK(read.submit(arena), "read bulk pages");
    for (size_t i = 0; i < read.size(); ++i)
        CHECK(read.result(i).status == Status::Ok && hash_bytes(read.result(i).data, read.result(i).size) == hashes[i], "bulk byte identity");
    mem_arena_destroy(arena);
}

static void test_asset_pages(const std::string& root) {
    printf("- binary pages: validation, warm reuse, pins, cancellation and manifest ordering\n");
    PageLimits limits; std::string error; std::vector<uint8_t> bytes;
    PageSection section; section.type = 7; section.stride = 4; section.bytes.assign(64, 42);
    CHECK(encode_page(9, {section}, {}, limits, bytes, error), "encode page");
    PageView view;
    CHECK(decode_page(bytes.data(), bytes.size(), limits, view, error), "decode page");
    CHECK(view.find(7) && view.find(7)->count == 16 && view.find(7)->data[0] == 42, "offset view");
    for (size_t n = 0; n < bytes.size(); ++n)
        CHECK(!decode_page(bytes.data(), n, limits, view, error), "reject truncated page at %zu", n);
    const size_t fields[] = {0, 4, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56};
    for (size_t offset : fields) {
        auto bad = bytes; put_u32(bad.data() + offset, 0xFFFFFFFFu);
        // Nonzero section type/schema are application-defined, not a generic parser restriction.
        if (offset == 32 || offset == 36) continue;
        CHECK(!decode_page(bad.data(), bad.size(), limits, view, error), "reject corrupt field %zu", offset);
    }
    CHECK(!encode_page(9, {section,section}, {}, limits, bytes, error), "reject duplicate sections");
    StoreConfig cfg; cfg.dir = root + "/pages";
    auto writer = BlobStore::open(cfg, &error); CHECK(writer != nullptr, "open page writer"); if (!writer) return;
    BlobHash first, second, third;
    CHECK(writer->put(bytes.data(), bytes.size(), &first) == Status::Ok, "write first page");
    section.bytes[0] = 13; CHECK(encode_page(9, {section}, {}, limits, bytes, error), "second page");
    CHECK(writer->put(bytes.data(), bytes.size(), &second) == Status::Ok, "write second page");
    section.bytes[0] = 14; CHECK(encode_page(9, {section}, {}, limits, bytes, error), "third page");
    CHECK(writer->put(bytes.data(), bytes.size(), &third) == Status::Ok, "write third page");
    CHECK(writer->flush_index(), "commit pages");
    PageCacheConfig cc; cc.store = cfg; cc.resident_bytes = 288; // two 128-byte pages and a record header
    auto cache = PageCache::open(cc, error); CHECK(cache != nullptr, "open cache"); if (!cache) return;
    auto result = cache->read({second, first, first});
    CHECK(result[0].status == PageStatus::Ok && result[1].status == PageStatus::Ok, "cold pages");
    CHECK(result[1].page == result[2].page, "deduplicated handles");
    CHECK(cache->stats().disk_reads == 1 && cache->stats().checksums == 2, "one read, two checksums");
    auto warm = cache->read({first});
    CHECK(warm[0].page == result[1].page && cache->stats().disk_reads == 1 && cache->stats().checksums == 2, "warm read skips disk and CRC");
    auto blocked = cache->read({third});
    CHECK(blocked[0].status == PageStatus::BudgetExceeded, "pinned cache allocation blocks admission");
    auto still_warm = cache->read({first});
    CHECK(still_warm[0].page == warm[0].page && cache->stats().disk_reads == 1, "pressure preserves lookup of pinned ranges");
    still_warm.clear();
    cache->clear();
    CHECK(cache->stats().resident_payload_bytes == 288, "eviction still charges externally pinned coalesced range");
    auto pressure = cache->read({third}); CHECK(pressure[0].status == PageStatus::BudgetExceeded, "cannot exceed budget through pins");
    result.clear(); warm.clear();
    CHECK(cache->stats().resident_payload_bytes == 0, "last pin releases allocation");
    auto next = cache->read({third}); CHECK(next[0].status == PageStatus::Ok, "retry after pins retire");
    std::atomic<bool> cancel{true};
    auto cancelled = cache->read({third}, &cancel);
    CHECK(cancelled[0].status == PageStatus::Cancelled && !cancelled[0].page, "cancelled request never exposes page");
    cache.reset();
    CHECK(next[0].page && next[0].page->view.find(7)->data[0] == 14, "handle survives cache teardown");
    // Reopening caches shares the original fixed bank, including old external pins.
    PageCacheConfig bank_cfg; bank_cfg.store = cfg; bank_cfg.resident_bytes = 256;
    bank_cfg.bank = PageBank::create(256, 64, 4);
    auto bank_cache = PageCache::open(bank_cfg, error);
    auto pinned = bank_cache->read({first, second});
    CHECK(pinned[0].status == PageStatus::BudgetExceeded, "rounded coalesced batch exceeds bank");
    pinned = bank_cache->read({first});
    CHECK(pinned[0].status == PageStatus::Ok, "bank page read");
    const auto* address = pinned[0].page->bytes;
    bank_cache.reset();
    bank_cache = PageCache::open(bank_cfg, error);
    auto second_pin = bank_cache->read({second});
    CHECK(second_pin[0].status == PageStatus::Ok, "second lease uses remaining bank capacity");
    CHECK(bank_cache->read({third})[0].status == PageStatus::BudgetExceeded, "shared bank pins enforce capacity across reopen");
    CHECK(pinned[0].page->view.find(7)->data[0] == 42, "old cache handle remains valid");
    pinned.clear();
    auto reused = bank_cache->read({third});
    CHECK(reused[0].status == PageStatus::Ok && reused[0].page->bytes == address, "released bank range reused");
    CHECK(bank_cache->stats().bank.backing_allocations == 1, "no payload backing allocation on reopen or reread");
    bank_cache->clear(); second_pin.clear(); reused.clear();
    CHECK(bank_cfg.bank->stats().occupied == 0, "all leases retired");
    auto cross_thread = bank_cache->read({first});
    auto cross_thread_second = bank_cache->read({second});
    bank_cache->clear();
    std::thread retire([held = std::move(cross_thread)]() mutable { held.clear(); });
    auto racing = bank_cache->read({third});
    CHECK(racing[0].status == PageStatus::Ok || racing[0].status == PageStatus::BudgetExceeded,
          "cross-thread retirement has bounded admission");
    retire.join();
    CHECK(bank_cache->read({third})[0].status == PageStatus::Ok, "admission succeeds after cross-thread retirement");
    bank_cache->clear(); racing.clear(); cross_thread_second.clear();
    CHECK(bank_cfg.bank->stats().occupied == 0, "cross-thread leases fully retire");
    PageCacheConfig ahead; ahead.store=cfg; ahead.resident_bytes=4096; ahead.read_ahead_bytes=1024;
    auto prefetch=PageCache::open(ahead,error);
    auto demanded_page=prefetch->read({first});
    CHECK(demanded_page[0].status==PageStatus::Ok && prefetch->stats().prefetched_pages==2,
          "physical neighbors prefetched with first demand");
    CHECK(prefetch->stats().disk_reads==1,"neighbor pages share one contiguous physical read");
    auto neighbors=prefetch->read({second,third});
    CHECK(neighbors[0].status==PageStatus::Ok && neighbors[1].status==PageStatus::Ok && prefetch->stats().disk_reads==1,
          "later neighboring requests avoid disk reads");
    ahead.resident_bytes=128;auto tiny_prefetch=PageCache::open(ahead,error);
    CHECK(tiny_prefetch->read({first})[0].status==PageStatus::Ok && tiny_prefetch->stats().prefetched_pages==0,
          "speculative read drops out before displacing mandatory demand under pressure");
    section.bytes[0]=15;BlobHash fourth;
    CHECK(encode_page(9,{section},{},limits,bytes,error) && writer->put(bytes.data(),bytes.size(),&fourth)==Status::Ok && writer->flush_index(),
          "append neighboring page without a compaction generation change");
    demanded_page.clear();neighbors.clear();prefetch->clear();
    CHECK(prefetch->refresh(),"reload appended physical directory");
    const auto prefetched_before=prefetch->stats().prefetched_pages;
    CHECK(prefetch->read({first})[0].status==PageStatus::Ok && prefetch->stats().prefetched_pages==prefetched_before+3,
          "physical locality refresh discovers appended neighbor");
    PageCacheConfig partitioned_config;partitioned_config.store=cfg;partitioned_config.resident_bytes=4096;
    partitioned_config.max_read_bytes=128;
    auto partitioned=PageCache::open(partitioned_config,error);
    auto ordered=partitioned->read_partitioned({third,first,second});
    CHECK(ordered[0].status==PageStatus::Ok && ordered[1].status==PageStatus::Ok && ordered[2].status==PageStatus::Ok,
          "oversized demand batch splits into admitted reads instead of starving valid pages");
    CHECK(ordered[0].page->hash==third && ordered[1].page->hash==first && ordered[2].page->hash==second,
          "partitioned reads preserve caller order");
    // Fixed-buffer reads reject undersized storage without touching sentinels.
    ReadBatch bounded(*writer); bounded.add(first);
    std::vector<uint64_t> storage((bounded.allocation_bytes()+7)/8+2, UINT64_MAX);
    CHECK(!bounded.submit(storage.data()+1, bounded.allocation_bytes()-1), "bounded read rejects undersized buffer");
    CHECK(storage[1] == UINT64_MAX && bounded.stats().chunk_reads == 0, "undersized read performs no IO or writes");
    CHECK(bounded.submit(storage.data()+1, bounded.allocation_bytes()), "bounded read succeeds");
    CHECK(storage.front() == UINT64_MAX && storage.back() == UINT64_MAX, "bounded read leaves guards intact");
    auto refs = RefTable::open(*writer, {}, &error); CHECK(refs != nullptr, "open refs"); if (!refs) return;
    std::vector<uint8_t> manifest;
    CHECK(encode_page(10, {}, {first, second}, limits, manifest, error), "encode manifest");
    BlobHash mh;
    CHECK(publish_page_manifest(*writer, *refs, "asset/one", manifest, limits, mh, error), "publish manifest after pages");
    RefInfo ri; CHECK(refs->peek("asset/one", &ri) && ri.hash == mh, "published identity");
    PageCacheConfig batch_config; batch_config.store = cfg;
    auto batch_reader = PageCache::open(batch_config, error);
    CHECK(batch_reader && !batch_reader->read_manifest("asset/batched").page, "reader starts before batched publication");
    BlobHash batched_hash;
    CHECK(publish_page_manifest(*writer, *refs, "asset/batched", manifest, limits, batched_hash, error, false),
          "stage manifest without committing");
    CHECK(batch_reader && !batch_reader->read_manifest("asset/batched").page, "uncommitted reference is invisible to readers");
    CHECK(writer->flush_index() && refs->flush(), "commit a batch in index then reference order");
    CHECK(batch_reader && batch_reader->read_manifest("asset/batched").page, "existing reader observes committed batch");
    PageCacheConfig memory_config; memory_config.store = cfg; memory_config.resident_bytes = manifest.size();
    auto memory_cache = PageCache::open(memory_config, error);
    CHECK(memory_cache != nullptr, "open in-memory page admission cache");
    if (memory_cache) {
        auto admitted = memory_cache->insert(manifest, error);
        CHECK(admitted.page && admitted.page->hash == batched_hash && memory_cache->stats().disk_reads == 0,
              "encoded producer page is admitted without a disk read");
        CHECK(memory_cache->insert(manifest, error).page == admitted.page, "producer pages reuse existing immutable allocations");
        CHECK(memory_cache->insert(bytes, error).status == PageStatus::BudgetExceeded, "producer admission cannot exceed pinned budget");
        auto corrupt = manifest; corrupt[0] ^= 1;
        CHECK(memory_cache->insert(corrupt, error).status == PageStatus::Corrupt, "invalid producer page is rejected");
        memory_cache.reset();
        CHECK(admitted.page && admitted.page->view.kind == 10, "producer page remains valid after cache teardown");
    }
    auto invalid = hash_bytes("absent", 6);
    CHECK(encode_page(10, {}, {invalid}, limits, manifest, error), "encode missing dependency");
    CHECK(!publish_page_manifest(*writer, *refs, "asset/one", manifest, limits, mh, error), "reject missing dependency");
    CHECK(refs->peek("asset/one", &ri) && ri.hash == mh, "previous reference retained");
    PageCacheConfig oversized; oversized.store = cfg; oversized.limits.max_bytes = 64;
    auto limited = PageCache::open(oversized, error);
    CHECK(limited && limited->read({first})[0].status == PageStatus::Corrupt, "oversized page rejected before allocating");
    CHECK(limited && limited->stats().resident_payload_bytes == 0, "no oversized allocation");
}

static void test_page_reachability(const std::string& root) {
    printf("- page maintenance: transitive children survive, orphans are reclaimed\n");
    StoreConfig cfg;cfg.dir=root+"/page_gc";std::string error;
    auto writer=BlobStore::open(cfg,&error);CHECK(writer!=nullptr,"GC writer");if(!writer)return;
    auto refs=RefTable::open(*writer,{},&error);CHECK(refs!=nullptr,"GC refs");if(!refs)return;
    PageLimits limits;std::vector<uint8_t> bytes;BlobHash child,parent,manifest,orphan;
    PageSection section{1,1,1,{7,8,9}};
    CHECK(encode_page(1,{section},{},limits,bytes,error),"child page");writer->put(bytes.data(),bytes.size(),&child);
    CHECK(encode_page(2,{}, {child},limits,bytes,error),"parent page");writer->put(bytes.data(),bytes.size(),&parent);
    CHECK(encode_page(3,{}, {parent},limits,bytes,error),"root manifest");
    CHECK(publish_page_manifest(*writer,*refs,"asset",bytes,limits,manifest,error),"publish dependency chain");
    section.bytes[0]=99;encode_page(1,{section},{},limits,bytes,error);writer->put(bytes.data(),bytes.size(),&orphan);writer->flush_index();
    PageCacheConfig cc;cc.store=cfg;auto reader=PageCache::open(cc,error);CHECK(reader!=nullptr,"GC reader");
    CompactStats stats;
    CHECK(!compact_page_store(*writer,*refs,{},limits,100,stats,error),"active reader defers maintenance");
    reader.reset();
    CHECK(!compact_page_store(*writer,*refs,{},limits,1,stats,error)&&writer->contains(orphan),"traversal limit cannot publish partial reachability");
    CHECK(compact_page_store(*writer,*refs,{},limits,100,stats,error),"%s",error.c_str());
    CHECK(writer->contains(child)&&writer->contains(parent)&&writer->contains(manifest)&&!writer->contains(orphan),"transitive reachability retained");
    CHECK(stats.blobs_kept==3&&stats.blobs_dropped==1,"maintenance accounting");
    reader=PageCache::open(cc,error);
    CHECK(reader&&reader->read({child})[0].status==PageStatus::Ok,"reopened reader uses compacted child");
}

static void test_index_extent_validation(const std::string& root) {
    printf("- index validation: bounded allocation, overflowing/overlapping extents and duplicate IDs\n");
    StoreConfig cfg;cfg.dir=root+"/index_bounds";std::string error;
    auto writer=BlobStore::open(cfg,&error);CHECK(writer!=nullptr,"index writer");if(!writer)return;
    const std::vector<uint8_t> a(64,1),b(64,2);BlobHash hash;
    writer->put(a.data(),a.size(),&hash);writer->put(b.data(),b.size(),&hash);CHECK(writer->flush_index(),"base index");
    std::vector<uint8_t> original;CHECK(read_whole_file(cfg.dir+"/index.bin",original),"read index fixture");
    const size_t entry=kIndexHeaderBytes+8;
    const auto write=[&](const std::vector<uint8_t>& bytes){FILE* f=fopen((cfg.dir+"/index.bin").c_str(),"wb");if(!f)return false;const bool ok=fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();fclose(f);return ok;};
    auto rcfg=cfg;rcfg.read_only=true;
    for(int mutation=0;mutation<8;++mutation) {
        auto bad=original;
        switch(mutation) {
        case 0:put_u64(bad.data()+entry+16,UINT64_MAX-7);break;
        case 1:put_u32(bad.data()+entry+28,0);break;
        case 2:put_u64(bad.data()+entry+16,33);break;
        case 3:put_u32(bad.data()+entry+28,UINT32_MAX);break;
        case 4:put_u64(bad.data()+entry,0);put_u64(bad.data()+entry+8,0);break;
        case 5:put_u64(bad.data()+entry+16,16);break;
        case 6:put_u64(bad.data()+entry+kIndexEntryBytes+16,get_u64(bad.data()+entry+16));break;
        case 7:put_u64(bad.data()+entry+kIndexEntryBytes,get_u64(bad.data()+entry));put_u64(bad.data()+entry+kIndexEntryBytes+8,get_u64(bad.data()+entry+8));break;
        }
        put_u32(bad.data()+bad.size()-4,crc32(bad.data(),bad.size()-4));
        CHECK(write(bad),"write malformed index");
        CHECK(!BlobStore::open(rcfg,&error),"reject malformed checksummed index %d",mutation);
    }
    CHECK(write(original),"restore valid index");
    rcfg.max_index_bytes=original.size()-1;
    CHECK(!BlobStore::open(rcfg,&error),"index byte limit rejects before allocation");
    rcfg.max_index_bytes=original.size();CHECK(BlobStore::open(rcfg,&error)!=nullptr,"exact index budget accepted");
}

static void test_repair_same_identity(const std::string& root) {
    printf("- corruption repair: append a regenerated page, atomically refresh same-size index\n");
    StoreConfig cfg;cfg.dir=root+"/repair";std::string error;
    auto writer=BlobStore::open(cfg,&error);CHECK(writer!=nullptr,"repair writer");if(!writer)return;
    std::vector<uint8_t> payload(512,37);BlobHash hash;
    CHECK(writer->put(payload.data(),payload.size(),&hash)==Status::Ok&&writer->flush_index(),"original commit");
    BlobLocation location;CHECK(writer->locate(hash,&location),"original location");
    auto reader_cfg=cfg;reader_cfg.read_only=true;auto reader=BlobStore::open(reader_cfg,&error);
    CHECK(reader!=nullptr,"old snapshot reader");if(!reader)return;
    FILE* f=fopen(writer->pack_path(location.pack).c_str(),"r+b");CHECK(f!=nullptr,"open corruptible pack");if(!f)return;
    fseek(f,static_cast<long>(location.offset),SEEK_SET);fputc(99,f);fclose(f);
    auto* arena=mem_arena_create(2048);const uint8_t* bytes=nullptr;size_t length=0;
    CHECK(reader->read(hash,arena,&bytes,&length)==Status::Corrupt,"old payload is corrupt");
    const auto index_size=raw_file_size(cfg.dir+"/index.bin");
    CHECK(writer->repair_blob(hash,"wrong",5)==Status::IoError,"repair rejects wrong identity");
    CHECK(writer->repair_blob(hash,payload.data(),payload.size())==Status::Ok,"regenerated payload appended");
    CHECK(reader->read(hash,arena,&bytes,&length)==Status::Corrupt,"repair is not visible before index commit");
    CHECK(writer->flush_index(),"commit repair");
    CHECK(index_size==raw_file_size(cfg.dir+"/index.bin"),"repair index has same byte length");
    CHECK(reader->reload_index(),"same-size index refresh");
    CHECK(reader->read(hash,arena,&bytes,&length)==Status::Ok&&length==payload.size()&&!memcmp(bytes,payload.data(),length),"new snapshot resolves the repaired bytes");
    mem_arena_destroy(arena);
}

static void test_refresh_during_publication(const std::string& root) {
    printf("- refresh race: newer commit during index read remains discoverable\n");
    StoreConfig cfg;cfg.dir=root+"/refresh_race";std::string error;
    auto writer=BlobStore::open(cfg,&error);CHECK(writer!=nullptr,"race writer");if(!writer)return;
    CHECK(writer->put("old",3,nullptr)==Status::Ok&&writer->flush_index(),"old revision");
    struct Context { BlobStore* writer; BlobHash hash; bool done=false; } context{writer.get(),{}};
    cfg.read_only=true;cfg.debug_after_index_read_context=&context;
    cfg.debug_after_index_read=[](void* opaque) {
        auto& c=*static_cast<Context*>(opaque);if(c.done)return;c.done=true;
        CHECK(c.writer->put("new",3,&c.hash)==Status::Ok&&c.writer->flush_index(),"interleaved commit");
    };
    auto reader=BlobStore::open(cfg,&error);CHECK(reader!=nullptr,"race reader");if(!reader)return;
    CHECK(context.done&&!reader->contains(context.hash),"reader holds original captured bytes");
    CHECK(reader->reload_index()&&reader->contains(context.hash),"next refresh sees interleaved revision");
}

static int page_benchmark() {
    using Clock=std::chrono::steady_clock;
    const auto directory=std::filesystem::temp_directory_path()/ ("matter_page_benchmark_"+std::to_string(Clock::now().time_since_epoch().count()));
    printf("page_bytes,pages,write_ms,first_read_ms,retained_read_ms,disk_reads,disk_bytes,resident_payload_bytes,warm_extra_reads,warm_extra_checksums\n");
    for(uint32_t size:{65536u,262144u,1048576u}) {
        StoreConfig cfg;cfg.dir=(directory/std::to_string(size)).string();std::string error;
        auto store=BlobStore::open(cfg,&error);if(!store)return 1;
        const uint32_t count=32u*1024*1024/size;
        const uint32_t batch_count=4u*1024*1024/size;
        std::vector<std::vector<uint8_t>> pages;pages.reserve(count);PageLimits limits;
        for(uint32_t i=0;i<count;++i) {
            PageSection section;section.type=1;fill_payload(section.bytes,20000+i,size-64);
            std::vector<uint8_t> bytes;if(!encode_page(1,{section},{},limits,bytes,error))return 2;pages.push_back(std::move(bytes));
        }
        std::vector<BlobHash> hashes;
        const auto write_start=Clock::now();
        for(uint32_t first=0;first<count;first+=batch_count) {
            std::vector<BlobInput> inputs;for(uint32_t i=first;i<first+batch_count;++i)inputs.push_back({pages[i].data(),pages[i].size()});
            std::vector<BlobHash> batch;
            if(store->put_batch(inputs,8u*1024*1024,batch)!=Status::Ok)return 3;
            hashes.insert(hashes.end(),batch.begin(),batch.end());
        }
        if(!store->flush_index())return 4;
        const double write_ms=std::chrono::duration<double,std::milli>(Clock::now()-write_start).count();
        PageCacheConfig pc;pc.store=cfg;auto cache=PageCache::open(pc,error);if(!cache)return 5;
        const auto read_all=[&] {
            for(uint32_t first=0;first<count;first+=batch_count) {
                auto result=cache->read({hashes.begin()+first,hashes.begin()+first+batch_count});
                for(const auto& page:result)if(page.status!=PageStatus::Ok)return false;
            }
            return true;
        };
        const auto read_start=Clock::now();if(!read_all())return 6;
        const double read_ms=std::chrono::duration<double,std::milli>(Clock::now()-read_start).count();
        const auto before=cache->stats();const auto warm_start=Clock::now();
        constexpr uint32_t repeats=100;
        for(uint32_t i=0;i<repeats;++i)if(!read_all())return 7;
        const double warm_ms=std::chrono::duration<double,std::milli>(Clock::now()-warm_start).count()/repeats;
        const auto after=cache->stats();
        printf("%u,%u,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu\n",size,count,write_ms,read_ms,warm_ms,
               (unsigned long long)after.disk_reads,(unsigned long long)after.disk_bytes,(unsigned long long)after.resident_payload_bytes,
               (unsigned long long)(after.disk_reads-before.disk_reads),(unsigned long long)(after.checksums-before.checksums));
    }
    std::filesystem::remove_all(directory);return 0;
}

/* ==================================================================== main */

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--page-bench") return page_benchmark();
    /* Child modes. The suite re-executes itself so the crash test can kill a
     * real process and the lock test can contend across process boundaries. */
    if (argc >= 3) {
        std::string mode = argv[1];
        std::string dir = argv[2];
        if (mode == "crashwriter") return child_crashwriter(dir);
        if (mode == "reader")      return child_reader(dir);
        if (mode == "lockprobe")   return child_lockprobe(dir);
    }

    char suffix[64];
    snprintf(suffix, sizeof(suffix), "/asset_store_tests_%d",
#ifdef _WIN32
             (int)_getpid()
#else
             (int)getpid()
#endif
             );
    std::string root = temp_root() + suffix;
    rm_tree(root);

    printf("AssetStoreLib tests (scratch: %s)\n\n", root.c_str());

    test_crc32_is_correct();
    test_roundtrip_and_dedup(root);
    test_crash_mid_write(root);
    test_corruption_is_a_miss(root);
    test_eviction_to_budget(root);
    test_concurrent_soak(root);
    test_determinism(root);
    test_batch_coalescing(root);
    test_arena_landing(root);
    test_bounded_reads(root);
    test_manifest_directory_reuse(root);
    test_asset_pages(root);
    test_bulk_append(root);
    test_failed_page_publication(root);
    test_page_reachability(root);
    test_index_extent_validation(root);
    test_repair_same_identity(root);
    test_refresh_during_publication(root);
    test_quiescent_compaction(root);
    test_read_only_cannot_write(root);

    rm_tree(root);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) printf("All AssetStoreLib tests passed\n");
    return g_failures ? 1 : 0;
}
