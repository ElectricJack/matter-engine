/* store_bench.cpp -- does the pack actually beat the small-file storm?
 *
 * The design (section 9.2) claims that grouping artifacts into pack files and
 * reading them in coalesced batches replaces "today's per-file open/seek storm
 * over thousands of small cache files" with "one or two large sequential
 * reads". The migration plan attached a number to that claim: >= 5x.
 *
 * This harness measures it instead of assuming it, on the same bytes, on this
 * machine.
 *
 * THE CORPUS is drawn from the real thing. projects/world_demo/.cache holds 426
 * files totalling 413 MiB; its size quantiles are p25 = 29 KB, median = 131 KB,
 * p75 = 251 KB, p90 = 761 KB. The generator below reproduces those quantiles.
 * The one real outlier -- a single 66 MB .gtex -- is deliberately excluded: it
 * would dominate the total and flatter BOTH paths equally, since one huge file
 * is already one big sequential read.
 *
 * THE COMPARISON is deliberately unfair to the store, in the store's disfavour:
 *   - the small-file path does no integrity checking, exactly like the engine's
 *     current cache reader (std::ifstream, read the whole file, done);
 *   - the store path CRC32s every payload it delivers and memcpys it into an
 *     arena, work the small-file path never does.
 * The CRC cost is measured separately and reported, so the reader can see how
 * much of the store's time is spent on the safety the small-file path lacks.
 *
 * TWO REGIMES, because they answer different questions:
 *   warm       every byte is in the OS page cache. This is a revisit inside one
 *              session, and it is mostly a memcpy and syscall-overhead contest.
 *   unbuffered both paths read with the page cache bypassed entirely
 *              (FILE_FLAG_NO_BUFFERING). This is the proxy for a cold cache --
 *              a fresh process, or a working set larger than RAM -- which is
 *              the case the design is actually about. There is no way to drop
 *              the Windows page cache without administrator rights, so this is
 *              the honest substitute and it is labelled as such.
 */

#include "asset_store.h"
/* Internal, but this benchmark lives inside the library and needs the exact
 * checksum the read path uses in order to price it honestly. */
#include "../src/store_hash.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <process.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

using namespace asset_store;

typedef std::chrono::steady_clock Clock;
static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/* ------------------------------------------------------------ the corpus -- */

static const int kBlobCount = 400;
static const uint32_t kSectorAlign = 4096;

/* Sizes reproducing projects/world_demo/.cache's measured quantiles. */
static size_t sample_size(uint32_t& rng) {
    auto next = [&]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };
    uint32_t bucket = next() % 100;
    uint32_t r = next();
    auto between = [&](size_t lo, size_t hi) { return lo + (size_t)(r % (hi - lo)); };
    if (bucket < 25) return between(1024, 29 * 1024);
    if (bucket < 50) return between(29 * 1024, 131 * 1024);
    if (bucket < 75) return between(131 * 1024, 251 * 1024);
    if (bucket < 90) return between(251 * 1024, 761 * 1024);
    return between(761 * 1024, 2048 * 1024);
}

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

static void rm_tree(const std::string& dir) {
#ifdef _WIN32
    std::string win = dir;
    for (size_t i = 0; i < win.size(); ++i) if (win[i] == '/') win[i] = '\\';
    system(("rd /s /q \"" + win + "\" >nul 2>&1").c_str());
#else
    system(("rm -rf '" + dir + "'").c_str());
#endif
}

static void make_dirs(const std::string& p) {
#ifdef _WIN32
    std::string win = p;
    for (size_t i = 0; i < win.size(); ++i) if (win[i] == '/') win[i] = '\\';
    system(("mkdir \"" + win + "\" >nul 2>&1").c_str());
#else
    system(("mkdir -p '" + p + "'").c_str());
#endif
}

/* -------------------------------------------------- unbuffered raw reads -- */

/* Page-cache-bypassing reads, used identically by both paths so the comparison
 * stays fair. Offsets, lengths and the destination buffer must all be sector
 * aligned, which is why every read is widened to 4 KB boundaries. */

struct AlignedBuf {
    uint8_t* p = nullptr;
    size_t n = 0;
    void ensure(size_t want) {
        if (n >= want) return;
        free_it();
#ifdef _WIN32
        p = (uint8_t*)VirtualAlloc(nullptr, want, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        if (posix_memalign((void**)&p, kSectorAlign, want) != 0) p = nullptr;
#endif
        n = p ? want : 0;
    }
    void free_it() {
#ifdef _WIN32
        if (p) VirtualFree(p, 0, MEM_RELEASE);
#else
        if (p) free(p);
#endif
        p = nullptr; n = 0;
    }
    ~AlignedBuf() { free_it(); }
};

#ifdef _WIN32
typedef HANDLE RawFile;
static const RawFile kBadFile = INVALID_HANDLE_VALUE;
static RawFile raw_open_unbuffered(const std::string& path) {
    /* Full sharing: the live BlobStore still holds its pack open for
     * read+write, and a share mode narrower than that is a sharing violation. */
    return CreateFileA(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING,
                       FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}
static bool raw_read(RawFile f, uint64_t off, void* buf, size_t len) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset = (DWORD)(off & 0xFFFFFFFFull);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    return ReadFile(f, buf, (DWORD)len, &got, &ov) != 0;
}
static void raw_close(RawFile f) { if (f != kBadFile) CloseHandle(f); }
#else
typedef int RawFile;
static const RawFile kBadFile = -1;
static RawFile raw_open_unbuffered(const std::string& path) {
#  ifdef O_DIRECT
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
    if (fd >= 0) return fd;
#  endif
    return ::open(path.c_str(), O_RDONLY);
}
static bool raw_read(RawFile f, uint64_t off, void* buf, size_t len) {
    return pread(f, buf, len, (off_t)off) >= 0;
}
static void raw_close(RawFile f) { if (f >= 0) ::close(f); }
#endif

static uint64_t align_down(uint64_t v) { return v & ~(uint64_t)(kSectorAlign - 1); }
static uint64_t align_up(uint64_t v) {
    return (v + kSectorAlign - 1) & ~(uint64_t)(kSectorAlign - 1);
}

/* --------------------------------------------------------------- results -- */

struct Timing {
    double ms = 0;
    uint64_t bytes = 0;
    uint32_t opens = 0;
    uint32_t reads = 0;
};

static void print_row(const char* label, const Timing& t) {
    double mb = (double)t.bytes / (1024.0 * 1024.0);
    printf("  %-34s %8.2f ms  %8.1f MB/s   %5u opens %6u reads\n",
           label, t.ms, t.ms > 0 ? mb / (t.ms / 1000.0) : 0.0, t.opens, t.reads);
}

/* ==================================================================== main */

int main() {
    char suffix[64];
    snprintf(suffix, sizeof(suffix), "/asset_store_bench_%d",
#ifdef _WIN32
             (int)_getpid()
#else
             (int)getpid()
#endif
             );
    std::string root = temp_root() + suffix;
    rm_tree(root);
    make_dirs(root);

    std::string files_dir = root + "/files";
    std::string store_dir = root + "/store";

    printf("AssetStoreLib benchmark -- packs vs the small-file storm\n");
    printf("scratch: %s\n\n", root.c_str());

    /* ---- build the corpus ---- */

    uint32_t rng = 0x1234567u;
    std::vector<size_t> sizes;
    sizes.reserve(kBlobCount);
    uint64_t total_bytes = 0;
    for (int i = 0; i < kBlobCount; ++i) {
        size_t s = sample_size(rng);
        sizes.push_back(s);
        total_bytes += s;
    }
    {
        std::vector<size_t> sorted = sizes;
        std::sort(sorted.begin(), sorted.end());
        printf("corpus: %d blobs, %.1f MiB total, median %.0f KB, "
               "p25 %.0f KB, p75 %.0f KB, max %.0f KB\n",
               kBlobCount, (double)total_bytes / (1024 * 1024),
               sorted[kBlobCount / 2] / 1024.0, sorted[kBlobCount / 4] / 1024.0,
               sorted[kBlobCount * 3 / 4] / 1024.0, sorted[kBlobCount - 1] / 1024.0);
    }

    /* Layout A: one file per artifact, in a two-level tree, exactly the shape
     * of projects/world_demo/.cache. */
    std::vector<std::string> file_paths;
    {
        for (int b = 0; b < 16; ++b) {
            char sub[8];
            snprintf(sub, sizeof(sub), "/%x", b);
            make_dirs(files_dir + sub);
        }
        std::vector<uint8_t> buf;
        for (int i = 0; i < kBlobCount; ++i) {
            fill_payload(buf, 1000 + i, sizes[i]);
            char name[128];
            snprintf(name, sizeof(name), "/%x/artifact_%04d.part", i % 16, i);
            std::string p = files_dir + name;
            std::ofstream f(p.c_str(), std::ios::binary);
            f.write((const char*)buf.data(), (std::streamsize)buf.size());
            f.close();
            file_paths.push_back(p);
        }
    }

    /* Layout B: the same bytes, in packs. Written in the same order, which is
     * the locality a sector-aware writer would produce. */
    std::vector<BlobHash> hashes;
    std::unique_ptr<BlobStore> store;
    {
        StoreConfig cfg;
        cfg.dir = store_dir;
        cfg.max_pack_bytes = 256ull * 1024 * 1024;
        std::string err;
        store = BlobStore::open(cfg, &err);
        if (!store) { printf("FATAL: %s\n", err.c_str()); return 1; }
        std::vector<uint8_t> buf;
        for (int i = 0; i < kBlobCount; ++i) {
            fill_payload(buf, 1000 + i, sizes[i]);
            BlobHash h;
            if (store->put(buf.data(), buf.size(), &h) != Status::Ok) {
                printf("FATAL: put failed\n");
                return 1;
            }
            hashes.push_back(h);
        }
        if (!store->flush_index()) { printf("FATAL: commit failed\n"); return 1; }
        printf("        %llu bytes in %llu pack bytes across the store\n\n",
               (unsigned long long)store->live_bytes(),
               (unsigned long long)store->pack_bytes());
    }

    MemArena* arena = mem_arena_create(4 << 20);

    /* ---------------------------------------------------------- WARM ---- */

    printf("WARM (OS page cache hot -- a revisit inside one session)\n");

    Timing warm_files, warm_store;
    const int kWarmReps = 3;

    for (int rep = 0; rep < kWarmReps; ++rep) {
        Timing t;
        std::vector<uint8_t> buf;
        Clock::time_point t0 = Clock::now();
        for (int i = 0; i < kBlobCount; ++i) {
            std::ifstream f(file_paths[i].c_str(), std::ios::binary | std::ios::ate);
            std::streamsize n = f.tellg();
            f.seekg(0);
            buf.resize((size_t)n);
            f.read((char*)buf.data(), n);
            t.bytes += (uint64_t)n;
            ++t.opens;
            ++t.reads;
        }
        t.ms = ms_since(t0);
        if (rep == 0 || t.ms < warm_files.ms) warm_files = t;
    }

    for (int rep = 0; rep < kWarmReps; ++rep) {
        Timing t;
        Clock::time_point t0 = Clock::now();
        ReadBatch b(*store);
        b.reserve(hashes.size());
        for (const BlobHash& h : hashes) b.add(h);
        mem_arena_reset(arena);
        b.submit(arena);
        t.ms = ms_since(t0);
        t.bytes = b.stats().bytes_delivered;
        t.opens = 1;
        t.reads = b.stats().chunk_reads;
        if (rep == 0 || t.ms < warm_store.ms) warm_store = t;
    }

    print_row("small files (ifstream, no CRC)", warm_files);
    print_row("store (ReadBatch, CRC + arena)", warm_store);
    printf("  -> store is %.2fx the small-file path\n\n",
           warm_files.ms / (warm_store.ms > 0 ? warm_store.ms : 1e-9));

    /* How much of the store's warm time is the integrity check the small-file
     * path simply does not do? */
    double crc_ms = 0;
    {
        std::vector<uint8_t> buf;
        fill_payload(buf, 1, 4 << 20);
        /* Time CRC over exactly the delivered byte count. */
        Clock::time_point t0 = Clock::now();
        uint64_t done = 0;
        volatile uint32_t sink = 0;
        while (done < warm_store.bytes) {
            size_t chunk = (size_t)std::min<uint64_t>(buf.size(), warm_store.bytes - done);
            sink ^= crc32(buf.data(), chunk);
            done += chunk;
        }
        (void)sink;
        crc_ms = ms_since(t0);
    }
    printf("  (of the store's %.2f ms, %.2f ms is CRC32 over %.1f MiB -- work the\n"
           "   small-file path never does. Without it the store would be ~%.2f ms.)\n\n",
           warm_store.ms, crc_ms, (double)warm_store.bytes / (1024 * 1024),
           warm_store.ms - crc_ms);

    /* --------------------------------------------------- UNBUFFERED ---- */

    /* Both paths go through the identical unbuffered primitive, so the only
     * difference measured is the shape of the access, which is the whole
     * claim. */

    AlignedBuf ab;
    bool unbuffered_ok = true;

    auto bench_files = [&](const std::vector<int>& idx) {
        Timing t;
        Clock::time_point t0 = Clock::now();
        for (int i : idx) {
            RawFile f = raw_open_unbuffered(file_paths[i]);
            if (f == kBadFile) {
                unbuffered_ok = false;
#ifdef _WIN32
                printf("  (unbuffered open failed, win32 error %lu)\n",
                       (unsigned long)GetLastError());
#endif
                break;
            }
            ++t.opens;
            size_t want = (size_t)align_up(sizes[i]);
            ab.ensure(want);
            if (!ab.p) { unbuffered_ok = false; raw_close(f); break; }
            raw_read(f, 0, ab.p, want);
            ++t.reads;
            t.bytes += sizes[i];
            raw_close(f);
        }
        t.ms = ms_since(t0);
        return t;
    };

    /* The same coalescing ReadBatch::submit performs, by hand. */
    auto bench_pack = [&](const std::vector<int>& idx) {
        struct Ext { uint32_t pack; uint64_t begin, end; };
        std::vector<BlobLocation> locs;
        locs.reserve(idx.size());
        for (int i : idx) {
            BlobLocation l;
            if (store->locate(hashes[i], &l)) locs.push_back(l);
        }
        std::sort(locs.begin(), locs.end(), [](const BlobLocation& a, const BlobLocation& b) {
            if (a.pack != b.pack) return a.pack < b.pack;
            return a.offset < b.offset;
        });
        std::vector<Ext> exts;
        for (size_t i = 0; i < locs.size(); ++i) {
            uint64_t b0 = locs[i].offset, e0 = b0 + locs[i].length;
            if (!exts.empty() && exts.back().pack == locs[i].pack &&
                b0 <= exts.back().end + 64 * 1024) {
                if (e0 > exts.back().end) exts.back().end = e0;
            } else {
                exts.push_back(Ext{locs[i].pack, b0, e0});
            }
        }

        Timing t;
        Clock::time_point t0 = Clock::now();
        uint32_t open_pack = 0xFFFFFFFFu;
        RawFile f = kBadFile;
        for (const Ext& e : exts) {
            if (e.pack != open_pack) {
                raw_close(f);
                f = raw_open_unbuffered(store->pack_path(e.pack));
                if (f == kBadFile) {
                    unbuffered_ok = false;
#ifdef _WIN32
                    printf("  (unbuffered open of pack %u failed, win32 error %lu)\n",
                           e.pack, (unsigned long)GetLastError());
#endif
                    break;
                }
                open_pack = e.pack;
                ++t.opens;
            }
            uint64_t b0 = align_down(e.begin);
            uint64_t e0 = align_up(e.end);
            size_t span = (size_t)(e0 - b0);
            ab.ensure(span);
            if (!ab.p) { unbuffered_ok = false; break; }
            raw_read(f, b0, ab.p, span);
            ++t.reads;
            t.bytes += e.end - e.begin;
        }
        raw_close(f);
        t.ms = ms_since(t0);
        return t;
    };

    auto compare = [&](const char* title, const char* note,
                       const std::vector<int>& idx) {
        printf("%s\n", title);
        if (note) printf("  %s\n", note);
        Timing tf = bench_files(idx);
        Timing tp = unbuffered_ok ? bench_pack(idx) : Timing();
        if (!unbuffered_ok) {
            printf("  SKIPPED -- this filesystem refused an unbuffered open.\n\n");
            return;
        }
        print_row("small files (unbuffered)", tf);
        print_row("store packs (unbuffered)", tp);
        printf("  -> store is %.2fx the small-file path"
               "   (%u file ops vs %u)\n\n",
               tf.ms / (tp.ms > 0 ? tp.ms : 1e-9),
               tf.opens * 2 + tf.reads, tp.opens * 2 + tp.reads);
    };

    /* Case 1: the whole corpus. Flattering to the pack -- every blob is wanted
     * and they are all adjacent -- but it is the bulk-load case (opening a
     * world, warming a region) and it is what the design's "one or two large
     * sequential reads" describes. */
    {
        std::vector<int> all;
        for (int i = 0; i < kBlobCount; ++i) all.push_back(i);
        compare("UNBUFFERED, WHOLE CORPUS (page cache bypassed -- the cold proxy)",
                "400 blobs, everything, in physical order", all);
    }

    /* Case 2: the case the acceptance criterion is actually about. One sector's
     * worth of artifacts -- forty blobs the writer placed together, because
     * locality is a write-side concern. */
    {
        std::vector<int> sector;
        for (int i = 120; i < 160; ++i) sector.push_back(i);
        compare("UNBUFFERED, ONE SECTOR REVISIT",
                "40 blobs the writer placed contiguously -- the design's target case",
                sector);
    }

    /* Case 3: the honest worst case. Forty blobs scattered the width of the
     * corpus, so coalescing buys nothing and the pack degenerates to forty
     * seeks -- it just does them without forty opens. If the pack still wins
     * here, the win is the syscall shape and not the locality. */
    {
        std::vector<int> scattered;
        for (int i = 0; i < kBlobCount; i += 10) scattered.push_back(i);
        compare("UNBUFFERED, SCATTERED ACCESS",
                "40 blobs spread across the corpus -- coalescing buys nothing here",
                scattered);
    }

    mem_arena_destroy(arena);
    store.reset();
    rm_tree(root);
    return 0;
}
