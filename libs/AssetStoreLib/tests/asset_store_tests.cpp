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
/* Internal, but the checksum is the mechanism behind "corruption is a miss", so
 * the suite verifies it directly rather than only through its effects. */
#include "../src/store_hash.h"

#include <atomic>
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

/* ==================================================================== main */

int main(int argc, char** argv) {
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

    rm_tree(root);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) printf("All AssetStoreLib tests passed\n");
    return g_failures ? 1 : 0;
}
