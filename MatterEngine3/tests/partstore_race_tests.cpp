// partstore_race_tests.cpp — headless two-thread stress harness for the
// stage_load()/commit_staged() data race (claude/stage-thread-experiment).
//
// Production symptom: staging sector loads on the streaming worker while the
// app thread pumps GPU jobs reproducibly loses the Vulkan device. Holding one
// mutex across BOTH the worker's stage_load and the app thread's pump is
// healthy; locking only the snapshot half or only the bake half stays broken.
// Leading hypothesis: a latent heap-safety defect (overrun / use-after-free)
// in the decode/mesh path whose victim under concurrency is the other
// thread's live allocation.
//
// This binary needs no GPU and no window. It synthesizes real .part v2
// fixtures in a temp cache, computes golden content signatures single-threaded,
// then runs the production threading topology:
//   thread A ("worker"): loop PartStore::stage_load(h) over set A
//   thread B ("app")   : commit_staged() of A's results + get_or_load()/release()
//                        over set B (the pump's own full loads)
// with four independent corruption detectors:
//   1. hard crash (a vectored handler logs fatal exception codes first)
//   2. HeapValidate(GetProcessHeap()) — heap metadata smashes
//   3. canary blocks (0xC5-filled, churned every iteration on both threads so
//      they interleave with engine allocations) — silent scribbles on free mem
//   4. golden signature re-verification of STAGED, COMMITTED and RESIDENT
//      parts — silent scribbles on live engine data (the production victim)
//
// Env knobs:
//   MATTER_RACE_SECONDS  wall budget for the MT phase (default 40)
//   MATTER_RACE_MAX_ITERS  per-thread iteration cap (default 200000)
//   MATTER_RACE_BIG=1    sector-scale fixtures (~9-16k tris/part)
//   MATTER_RACE_CACHE=<dir>  stress a REAL cache read-only (e.g.
//                        projects/world_demo/.cache/StreamMeadow)
//   MATTER_RACE_WORKERS=N    number of staging threads (default 1)
//   MATTER_RACE_PAGEHEAP=1   guard-page allocator (see Detector 0)
//   MATTER_RACE_DIFF=<hash>  single-threaded divergence localizer
//   MATTER_RACE_PIPE=<hash>  per-stage pipeline bisection
//   MATTER_STAGE_LOCK    unset = production-broken config (no locks).
//                        "all" | "snapshot" | "bake" narrow the experiment
//                        mutex exactly like matter_engine.cpp does; when set,
//                        thread B also wraps its work in the same mutex the
//                        way pump_gpu_jobs() does.
//
// FINDINGS (2026-07-28, this harness):
//   * ~60k racing loads across small/sector-scale/real-cache fixtures, plus
//     36.5M page-guarded allocations, found NO overrun, NO use-after-free and
//     NO cross-thread content corruption in decode/bake/commit.
//   * What it DID find, single-threaded, on the real StreamMeadow cache:
//     BLAS geometry identity is broken three ways in
//     libs/MatterSurfaceLib/src/blas_manager.cpp —
//       (1) calculate_hash reads 9 CONSECUTIVE floats from &vertex0, but Tri's
//           union slots are 16-byte strided: the hash covers the two padding
//           words at +12/+28 and NEVER sees vertex2.y/z (line ~43);
//       (2) the tint fold reads the float4 local through
//           reinterpret_cast<const uint32_t*> — strict-aliasing UB; GCC -O2
//           elides the member stores and the hash folds 16 UNINITIALIZED stack
//           bytes per triangle, making geometry identity nondeterministic
//           (line ~59-64, proven by disassembly and proofs A/C);
//       (3) triangles_equal memcmps sizeof(float3)*3 == 36 contiguous bytes —
//           same wrong window: padding compared, vertex2.y/z ignored, so a
//           garbage-hash collision DEDUPES DIFFERENT GEOMETRY (proof B).
//     See run_dedup_identity_proofs() below; these make the test exit red.
//   * With (1)-(3) fixed, the REAL-cache MT phase then caught a fourth defect:
//     BLASManager::adopt_from installed adopted entries with ref_count=1,
//     discarding the staged entry's multiplicity (>=2 whenever ladder rungs
//     deduplicated in-staging, routine for small assets whose QEM run is an
//     identity) while owned_blas lists the handle once per rung and release()
//     decrements per occurrence -> under-count -> premature erasure ->
//     DANGLING lod_blas handles on other resident parts ("null BLAS entry"
//     on StreamMeadow grass). Pre-fix this fired only when the garbage hash
//     happened to collide, i.e. nondeterministically and dependent on the
//     STAGING THREAD's stack contents -- the thread-coupled trigger.
//   * Remaining real-cache signature deltas (meadow grass/rock families) are
//     residency-order TriEx adoption: identity deliberately excludes
//     normals/uv/AO, but consumers take TriEx from the deduped entry, so a
//     part can render with a sibling's shading. Identical failures under
//     MATTER_STAGE_LOCK=all prove they are order effects, not a race.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#define NOUSER
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <new>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "render/part_store.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

namespace fs = std::filesystem;

static std::atomic<int> g_failures{0};
static std::mutex g_log_mutex;

#define CHECKF(cond, ...)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::lock_guard<std::mutex> lk_(g_log_mutex);                     \
            std::printf("  FAIL: ");                                          \
            std::printf(__VA_ARGS__);                                         \
            std::printf("\n");                                                \
            std::fflush(stdout);                                              \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

// The TriEx byte-fold below assumes the named members are contiguous in
// [0, 92) with only tail padding — same contract part_asset_v2.cpp pins.
static_assert(sizeof(TriEx) == 96, "TriEx layout changed");
static_assert(offsetof(TriEx, ao2) == 88, "TriEx trailing member moved");

// ---------------------------------------------------------------------------
// Detector 0 (opt-in, MATTER_RACE_PAGEHEAP=1): page-guard allocator. Overrides
// global operator new/delete for the WHOLE binary, so every std::vector /
// make_unique buffer in the engine gets:
//   * its end pushed against a PAGE_NOACCESS tail guard page -> any overrun
//     write/read past size() faults AT THE GUILTY INSTRUCTION, and
//   * MEM_DECOMMIT on delete with the reservation kept -> any use-after-free
//     faults at the stale access.
// This is ASan-lite for the heap objects the decode/bake pipeline actually
// carries its data in (Tri/TriEx/BVHNode vectors, RasterMeshData channels,
// weld maps). MALLOC64 buffers (BvhMesh::tri/triEx, BVH::bvhNode) still go
// through _aligned_malloc and are NOT covered. Slack from alignment rounding
// (< align bytes between the block end and the guard page) is filled with a
// canary pattern verified on free, so even sub-alignment overruns are caught.
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace guardalloc {

static std::atomic<bool> g_enabled{false};
static std::atomic<uint64_t> g_live{0}, g_total{0};
static constexpr size_t kPage = 4096;
static constexpr uint64_t kMagic = 0x47554152445F4D45ull;  // "GUARD_ME"

struct Header {
    uint64_t magic;
    void* user;
    size_t size;
    size_t total;
};

static void* guarded_alloc(size_t n, size_t align) {
    if (align < 16) align = 16;
    if (align > kPage) return nullptr;  // not expected; fall back
    if (n == 0) n = 1;
    // [base: Header ... slack][user .. user+n .. slack<align][GUARD PAGE]
    const size_t payload = sizeof(Header) + 8 /*backptr*/ + n + align;
    const size_t total = ((payload + kPage - 1) / kPage) * kPage + kPage;
    unsigned char* base = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) return nullptr;
    DWORD old = 0;
    VirtualProtect(base + total - kPage, kPage, PAGE_NOACCESS, &old);
    // Place the user block as close to the guard page as alignment allows.
    unsigned char* guard = base + total - kPage;
    unsigned char* user = guard - n;
    user = reinterpret_cast<unsigned char*>(
        reinterpret_cast<uintptr_t>(user) & ~(uintptr_t)(align - 1));
    // Slack between user+n and the guard page (< align): canary-filled.
    std::memset(user + n, 0xC7, (size_t)(guard - (user + n)));
    Header* h = reinterpret_cast<Header*>(base);
    h->magic = kMagic;
    h->user = user;
    h->size = n;
    h->total = total;
    *reinterpret_cast<void**>(user - 8) = base;
    g_live.fetch_add(1, std::memory_order_relaxed);
    g_total.fetch_add(1, std::memory_order_relaxed);
    return user;
}

static bool guarded_free(void* p) {
    if (!p) return true;
    // Our blocks carry a back-pointer 8 bytes before the user pointer. A
    // foreign block's bytes there are readable heap data; the magic check
    // rejects them.
    unsigned char* user = static_cast<unsigned char*>(p);
    void* base_candidate = *reinterpret_cast<void**>(user - 8);
    if (!base_candidate) return false;
    if (reinterpret_cast<uintptr_t>(base_candidate) & (kPage - 1)) return false;
    if (base_candidate > p ||
        static_cast<unsigned char*>(p) - static_cast<unsigned char*>(base_candidate) >
            (ptrdiff_t)(1u << 30))
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(base_candidate, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT || mbi.AllocationBase != base_candidate) return false;
    Header* h = static_cast<Header*>(base_candidate);
    if (h->magic != kMagic || h->user != p) return false;
    // Tail-slack canary: a sub-alignment overrun lands here instead of the
    // guard page; report it at free time.
    unsigned char* guard = static_cast<unsigned char*>(base_candidate) + h->total - kPage;
    for (unsigned char* q = user + h->size; q < guard; ++q) {
        if (*q != 0xC7) {
            std::fprintf(stderr,
                         "*** guardalloc: tail-slack overrun of block %p (size %zu): "
                         "byte at +%zd is 0x%02X\n",
                         p, h->size, q - (user + h->size), *q);
            std::fflush(stderr);
            abort();
        }
    }
    h->magic = 0;
    // Decommit everything and KEEP the reservation: any later touch of this
    // block (use-after-free) faults, and the address range can never be
    // recycled into a fresh allocation. Address space is the only cost.
    VirtualFree(base_candidate, h->total, MEM_DECOMMIT);
    g_live.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

}  // namespace guardalloc

void* operator new(size_t n) {
    if (guardalloc::g_enabled.load(std::memory_order_relaxed)) {
        if (void* p = guardalloc::guarded_alloc(n, 16)) return p;
    }
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return ::operator new(n); }
void* operator new(size_t n, std::align_val_t al) {
    if (guardalloc::g_enabled.load(std::memory_order_relaxed)) {
        if (void* p = guardalloc::guarded_alloc(n, (size_t)al)) return p;
    }
    void* p = _aligned_malloc(n ? n : 1, (size_t)al);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n, std::align_val_t al) { return ::operator new(n, al); }

void operator delete(void* p) noexcept {
    if (!p) return;
    if (guardalloc::guarded_free(p)) return;
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }
void operator delete(void* p, std::align_val_t) noexcept {
    if (!p) return;
    if (guardalloc::guarded_free(p)) return;
    _aligned_free(p);
}
void operator delete[](void* p, std::align_val_t al) noexcept { ::operator delete(p, al); }
void operator delete(void* p, size_t, std::align_val_t al) noexcept { ::operator delete(p, al); }
void operator delete[](void* p, size_t, std::align_val_t al) noexcept { ::operator delete(p, al); }

// MALLOC64/FREE64 seam (precomp.h, MATTER_MALLOC64_HOOK — defined only by the
// race flavor): guards the raw BVH/mesh buffers too — BvhMesh::tri/triEx,
// BVH::bvhNode, TLAS::tlasNode — completing coverage of every buffer the
// decode/bake pipeline writes.
extern "C" void* matter_malloc64_hook(size_t bytes) {
    if (guardalloc::g_enabled.load(std::memory_order_relaxed)) {
        if (void* p = guardalloc::guarded_alloc(bytes, 64)) return p;
    }
    return _aligned_malloc(bytes, 64);
}
extern "C" void matter_free64_hook(void* p) {
    if (!p) return;
    if (guardalloc::guarded_free(p)) return;
    _aligned_free(p);
}
#endif  // _WIN32

// ---------------------------------------------------------------------------
// Detector 1: log fatal exceptions before the process dies, so the make log
// shows WHERE instead of a bare exit code.
// ---------------------------------------------------------------------------
#ifdef _WIN32
static LONG WINAPI fatal_logger(EXCEPTION_POINTERS* xp) {
    const DWORD code = xp->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == 0xC0000374u /*heap corruption*/ ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_IN_PAGE_ERROR ||
        code == EXCEPTION_STACK_OVERFLOW) {
        // Async-signal safety is moot; we are crashing anyway.
        std::fprintf(stderr, "\n*** FATAL exception 0x%08lX at %p",
                     (unsigned long)code, xp->ExceptionRecord->ExceptionAddress);
        if (code == EXCEPTION_ACCESS_VIOLATION && xp->ExceptionRecord->NumberParameters >= 2) {
            std::fprintf(stderr, " (%s %p)",
                         xp->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                         (void*)xp->ExceptionRecord->ExceptionInformation[1]);
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// Detector 2: whole-process-heap walk. UCRT malloc/_aligned_malloc/operator
// new all land on the process heap, so this covers every engine allocation.
static bool heap_ok() {
#ifdef _WIN32
    return HeapValidate(GetProcessHeap(), 0, nullptr) != 0;
#else
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Detector 3: canary pool. Blocks in engine-typical size classes, pattern
// filled, churned every iteration so fresh canaries keep landing next to the
// other thread's live allocations (LFH buckets are shared across threads).
// ---------------------------------------------------------------------------
struct CanaryPool {
    struct Block { unsigned char* p; size_t n; };
    std::vector<Block> blocks;
    uint32_t rng;

    CanaryPool(uint32_t seed, size_t count) : rng(seed | 1u) {
        blocks.reserve(count);
        for (size_t i = 0; i < count; ++i) blocks.push_back(alloc_one());
    }
    ~CanaryPool() {
        for (Block& b : blocks) std::free(b.p);
    }
    uint32_t next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
    Block alloc_one() {
        static const size_t sizes[] = {32, 48, 64, 80, 96, 128, 160, 256,
                                       384, 512, 1024, 2048, 4096, 16384};
        const size_t n = sizes[next() % (sizeof(sizes) / sizeof(sizes[0]))];
        unsigned char* p = static_cast<unsigned char*>(std::malloc(n));
        std::memset(p, 0xC5, n);
        return {p, n};
    }
    void verify(const char* who, uint64_t iter) {
        for (Block& b : blocks) {
            for (size_t i = 0; i < b.n; ++i) {
                if (b.p[i] != 0xC5) {
                    CHECKF(false,
                           "%s iter %llu: CANARY SMASH block=%p size=%zu offset=%zu byte=0x%02X",
                           who, (unsigned long long)iter, (void*)b.p, b.n, i, b.p[i]);
                    std::memset(b.p, 0xC5, b.n);  // rearm so one smash logs once
                    break;
                }
            }
        }
    }
    void churn(size_t k) {
        for (size_t i = 0; i < k; ++i) {
            const size_t j = next() % blocks.size();
            std::free(blocks[j].p);
            blocks[j] = alloc_one();
        }
    }
};

// ---------------------------------------------------------------------------
// Fixture synthesis: sector-like static v2 parts. Multiple BLAS groups per
// part (like a sector's mesh groups), full TriEx (materials/tints/normals/AO)
// so the stage path runs QEM decimation AND SampleSource reprojection, plus a
// couple of ugly groups (exact-degenerate corners, slivers, one lone tiny
// triangle, mixed scales) to widen the input space.
// ---------------------------------------------------------------------------
struct GroupSpec {
    int grid;            // (grid x grid) cells -> 2*grid^2 triangles
    float cell;          // cell world size
    float ox, oz;        // origin
    uint32_t mat_base;
    uint32_t seed;
    bool degens;         // append degenerate + sliver triangles
};
struct PartSpec {
    uint64_t hash;
    std::vector<GroupSpec> groups;
    bool lone_triangle_group = false;  // extra 1-triangle group
};

static void push_tri(std::vector<Tri>& tris, std::vector<TriEx>& triex,
                     float3 a, float3 b, float3 c,
                     float3 na, float3 nb, float3 nc,
                     int material, float4 tint, float ao0, float ao1, float ao2) {
    Tri t{};
    t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
    t.centroid = make_float3((a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f,
                             (a.z + b.z + c.z) / 3.0f);
    tris.push_back(t);
    TriEx e{};
    e.N0 = na; e.N1 = nb; e.N2 = nc;
    e.uv0 = make_float2(a.x * 0.13f, a.z * 0.13f);
    e.uv1 = make_float2(b.x * 0.13f, b.z * 0.13f);
    e.uv2 = make_float2(c.x * 0.13f, c.z * 0.13f);
    e.materialId = material;
    e.tint = tint;
    e.ao0 = ao0; e.ao1 = ao1; e.ao2 = ao2;
    triex.push_back(e);
}

static void make_group(const GroupSpec& g, std::vector<Tri>& tris, std::vector<TriEx>& triex) {
    const int G = g.grid;
    std::vector<float> h((size_t)(G + 1) * (G + 1));
    uint32_t s = g.seed | 1u;
    auto rnd = [&s]() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(s & 0xFFFF) / 65536.0f;
    };
    for (int z = 0; z <= G; ++z)
        for (int x = 0; x <= G; ++x) {
            const float fx = x * g.cell, fz = z * g.cell;
            h[(size_t)z * (G + 1) + x] =
                0.8f * std::sin(fx * 0.9f + (float)(g.seed % 13u)) * std::cos(fz * 0.6f) +
                0.35f * rnd();
        }
    auto H = [&](int x, int z) { return h[(size_t)z * (G + 1) + x]; };
    auto P = [&](int x, int z) {
        return make_float3(g.ox + x * g.cell, H(x, z), g.oz + z * g.cell);
    };
    auto N = [&](int x, int z) {
        const int x0 = x > 0 ? x - 1 : x, x1 = x < G ? x + 1 : x;
        const int z0 = z > 0 ? z - 1 : z, z1 = z < G ? z + 1 : z;
        const float dhx = (H(x1, z) - H(x0, z)) / ((float)(x1 - x0) * g.cell + 1e-9f);
        const float dhz = (H(x, z1) - H(x, z0)) / ((float)(z1 - z0) * g.cell + 1e-9f);
        float3 n = make_float3(-dhx, 1.0f, -dhz);
        return normalize(n);
    };
    for (int z = 0; z < G; ++z)
        for (int x = 0; x < G; ++x) {
            const float3 p00 = P(x, z), p10 = P(x + 1, z), p01 = P(x, z + 1), p11 = P(x + 1, z + 1);
            const float3 n00 = N(x, z), n10 = N(x + 1, z), n01 = N(x, z + 1), n11 = N(x + 1, z + 1);
            const int material = (int)((g.mat_base + (uint32_t)(x + z) % 5u) % 250u);
            const float tv = 0.25f + 0.75f * (float)((x * 7 + z * 3) % 16) / 16.0f;
            const float4 tint = make_float4(tv, 1.0f - tv * 0.5f, 0.4f + 0.6f * tv,
                                            ((x + z) % 3) ? 0.35f : 0.0f);
            const float a0 = 0.35f + 0.65f * (float)(x % 8) / 8.0f;
            const float a1 = 0.35f + 0.65f * (float)(z % 8) / 8.0f;
            const float a2 = 0.35f + 0.65f * (float)((x + z) % 8) / 8.0f;
            push_tri(tris, triex, p00, p10, p11, n00, n10, n11, material, tint, a0, a1, a2);
            push_tri(tris, triex, p00, p11, p01, n00, n11, n01, material, tint, a0, a2, a1);
        }
    if (g.degens) {
        const float3 up = make_float3(0, 1, 0);
        for (int i = 0; i < 8; ++i) {
            // exact duplicate corner: welding must drop it; BVH must survive it
            const float3 a = P(i % G, (i * 3) % G);
            const float3 c = P((i + 1) % G, (i * 3) % G);
            push_tri(tris, triex, a, a, c, up, up, up, (int)(g.mat_base % 250u),
                     make_float4(1, 1, 1, 0), 1, 1, 1);
        }
        for (int i = 0; i < 8; ++i) {
            // near-zero-area sliver, tall and thin
            const float3 a = P((2 * i) % G, i % G);
            const float3 b = make_float3(a.x + 1e-6f, a.y, a.z);
            const float3 c = make_float3(a.x, a.y + 2.0f, a.z + 1e-6f);
            push_tri(tris, triex, a, b, c, up, up, up, (int)((g.mat_base + 1) % 250u),
                     make_float4(0.9f, 0.2f, 0.1f, 0.5f), 0.8f, 0.8f, 0.8f);
        }
    }
}

static bool write_part_fixture(const fs::path& root, const PartSpec& spec) {
    std::error_code ec;
    fs::create_directories(root / "parts", ec);
    if (ec) return false;

    BLASManager blas;
    TLASManager tlas(256);
    part_asset::LodLevels lods;
    part_asset::LodLevel L0;
    L0.screen_size_threshold = 0.2f;
    std::vector<TLASManager::DrawInstance> instances;

    auto register_group = [&](std::vector<Tri>& tris, std::vector<TriEx>& triex,
                              uint32_t material) -> bool {
        const uint32_t idx = (uint32_t)blas.get_entries().size();
        const BLASHandle handle =
            blas.register_triangles(tris.data(), (int)tris.size(), triex.data());
        if (handle == INVALID_BLAS_HANDLE) return false;
        L0.blas_indices.push_back(idx);
        TLASManager::DrawInstance di{};
        di.blas_handle = handle;
        di.material_id = material;
        instances.push_back(di);
        return true;
    };

    for (const GroupSpec& g : spec.groups) {
        std::vector<Tri> tris;
        std::vector<TriEx> triex;
        make_group(g, tris, triex);
        if (!register_group(tris, triex, g.mat_base)) return false;
    }
    if (spec.lone_triangle_group) {
        std::vector<Tri> tris;
        std::vector<TriEx> triex;
        const float3 up = make_float3(0, 1, 0);
        push_tri(tris, triex, make_float3(90, 0, 90), make_float3(90.02f, 0, 90),
                 make_float3(90, 0.02f, 90.02f), up, up, up, 5,
                 make_float4(1, 1, 1, 0), 1, 1, 1);
        if (!register_group(tris, triex, 5)) return false;
    }

    lods.push_back(L0);
    tlas.draw_batch(instances);
    tlas.build(blas);
    const std::string path = (root / part_asset::cache_path_resolved(spec.hash)).string();
    return part_asset::save_v2(path, blas, tlas, nullptr, 0, lods, spec.hash);
}

// ---------------------------------------------------------------------------
// Detector 4: content signatures. Deterministic FNV fold of everything the
// stage/commit pipeline produces. Folds member-by-member (never raw structs
// with padding) so the value is stable across runs and across the
// staged->adopted copy.
// ---------------------------------------------------------------------------
static void fold_bytes(uint64_t& h, const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
}
template <class T>
static void fold_pod(uint64_t& h, const T& v) { fold_bytes(h, &v, sizeof v); }
static void fold_f3(uint64_t& h, const float3& v) {
    fold_pod(h, v.x); fold_pod(h, v.y); fold_pod(h, v.z);
}

// Validate one BLAS entry's full self-consistency and fold its content.
static bool validate_entry(const BLASManager::BLASEntry* e, uint64_t& sig,
                           const char* ctx, uint64_t hash, size_t level) {
    if (!e) { CHECKF(false, "%s %016llx L%zu: null BLAS entry", ctx, (unsigned long long)hash, level); return false; }
    const size_t n = e->triangles.size();
    if (n == 0 || n > 20000000u) {
        CHECKF(false, "%s %016llx L%zu: bad triangle count %zu", ctx, (unsigned long long)hash, level, n);
        return false;
    }
    if (!e->mesh || e->mesh->triCount != (int)n || !e->mesh->tri) {
        CHECKF(false, "%s %016llx L%zu: mesh/triCount mismatch (mesh=%p triCount=%d n=%zu)",
               ctx, (unsigned long long)hash, level, (void*)e->mesh.get(),
               e->mesh ? e->mesh->triCount : -1, n);
        return false;
    }
    if (!e->bvh || !e->bvh->bvhNode || !e->bvh->triIdx) {
        CHECKF(false, "%s %016llx L%zu: missing BVH arrays", ctx, (unsigned long long)hash, level);
        return false;
    }
    const uint nu = e->bvh->nodesUsed;
    if (nu < 1 || nu > 2 * n + 1) {
        CHECKF(false, "%s %016llx L%zu: nodesUsed %u out of range for %zu tris",
               ctx, (unsigned long long)hash, level, nu, n);
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (e->bvh->triIdx[i] >= n) {
            CHECKF(false, "%s %016llx L%zu: triIdx[%zu]=%u out of range (%zu tris)",
                   ctx, (unsigned long long)hash, level, i, e->bvh->triIdx[i], n);
            return false;
        }
    }
    for (uint i = 0; i < nu; ++i) {
        const BVHNode& node = e->bvh->bvhNode[i];
        if (node.triCount != 0) {
            if (node.leftFirst > n || node.triCount > n - node.leftFirst) {
                CHECKF(false, "%s %016llx L%zu: leaf node %u bad range (first=%u count=%u tris=%zu)",
                       ctx, (unsigned long long)hash, level, i, node.leftFirst, node.triCount, n);
                return false;
            }
        } else if (nu < 2 || node.leftFirst > nu - 2) {
            CHECKF(false, "%s %016llx L%zu: interior node %u bad child index %u (nodesUsed=%u)",
                   ctx, (unsigned long long)hash, level, i, node.leftFirst, nu);
            return false;
        }
    }
    if (!e->tri_extra.empty() && e->tri_extra.size() != n) {
        CHECKF(false, "%s %016llx L%zu: tri_extra size %zu != %zu",
               ctx, (unsigned long long)hash, level, e->tri_extra.size(), n);
        return false;
    }
    // The entry keeps TWO independent copies of the triangles (entry.triangles
    // and mesh->tri). They were written from the same source, so any
    // disagreement means somebody scribbled on one of them.
    for (size_t i = 0; i < n; i += (n > 64 ? 17 : 1)) {
        const Tri& a = e->triangles[i];
        const Tri& b = e->mesh->tri[i];
        if (std::memcmp(&a.vertex0, &b.vertex0, 12) || std::memcmp(&a.vertex1, &b.vertex1, 12) ||
            std::memcmp(&a.vertex2, &b.vertex2, 12)) {
            CHECKF(false, "%s %016llx L%zu: triangles[%zu] disagrees with mesh->tri[%zu] (scribble)",
                   ctx, (unsigned long long)hash, level, i, i);
            return false;
        }
    }

    fold_pod(sig, (uint64_t)n);
    for (size_t i = 0; i < n; ++i) {
        fold_f3(sig, e->triangles[i].vertex0);
        fold_f3(sig, e->triangles[i].vertex1);
        fold_f3(sig, e->triangles[i].vertex2);
    }
    if (!e->tri_extra.empty()) {
        // Named members occupy [0,92) contiguously (static_assert above).
        for (size_t i = 0; i < n; ++i) fold_bytes(sig, &e->tri_extra[i], 92);
    }
    fold_pod(sig, nu);
    for (uint i = 0; i < nu; ++i) {
        const BVHNode& node = e->bvh->bvhNode[i];
        fold_f3(sig, node.aabbMin);
        fold_pod(sig, node.leftFirst);
        fold_f3(sig, node.aabbMax);
        fold_pod(sig, node.triCount);
    }
    fold_bytes(sig, e->bvh->triIdx, n * sizeof(uint));
    return true;
}

static bool validate_mesh_data(const viewer::RasterMeshData& m, uint64_t& sig,
                               const char* ctx, uint64_t hash, size_t level) {
    const size_t v = (size_t)m.vertex_count;
    if (m.vertex_count <= 0 || m.indices.empty() || m.indices.size() % 3 != 0 ||
        m.vertices.size() != v * 3 || m.normals.size() != v * 3 || m.colors.size() != v * 4 ||
        m.texcoords.size() != v * 2 || m.surface_uvs.size() != v * 2 ||
        m.material_ids.size() != v || m.baked_ao.size() != v) {
        CHECKF(false, "%s %016llx L%zu: lod_mesh_data channel sizes inconsistent (v=%d idx=%zu)",
               ctx, (unsigned long long)hash, level, m.vertex_count, m.indices.size());
        return false;
    }
    for (uint32_t idx : m.indices) {
        if (idx >= v) {
            CHECKF(false, "%s %016llx L%zu: mesh index %u out of range (v=%zu)",
                   ctx, (unsigned long long)hash, level, idx, v);
            return false;
        }
    }
    fold_pod(sig, viewer::indexed_part_geometry_signature(m, (uint32_t)level));
    return true;
}

// Validate a LoadedPart whose BLAS entries live in `lookup` (shared manager
// after commit, staging manager before) and fold the whole content signature.
// Handles both shapes the store produces:
//   compositional (stage_load / coherent get_or_load): lod_blas == thresholds
//     == lod_mesh_data == owned_blas, clusters empty;
//   flat v2/v3 (get_or_load with a .flat.part): owned_blas covers legacy +
//     per-cluster registrations, lod_mesh_data has legacy entries first then
//     per-cluster entries, clusters index into both.
// A geometry-less assembler (children only) legitimately has an empty ladder.
template <class EntryLookup>
static bool validate_part_content(const viewer::LoadedPart& lp, EntryLookup&& lookup,
                                  uint64_t& sig, const char* ctx, uint64_t hash) {
    sig = 1469598103934665603ull;
    if (lp.lod_blas.empty()) {
        if (!lp.lod_mesh_data.empty() || !lp.owned_blas.empty() || !lp.clusters.empty()) {
            CHECKF(false, "%s %016llx: empty ladder but mesh=%zu owned=%zu clusters=%zu",
                   ctx, (unsigned long long)hash, lp.lod_mesh_data.size(),
                   lp.owned_blas.size(), lp.clusters.size());
            return false;
        }
        fold_pod(sig, (uint64_t)0xE117ull);  // assembler marker
        fold_pod(sig, (uint64_t)lp.children.size());
        return true;
    }
    if (lp.lod_blas.size() != lp.thresholds.size() ||
        lp.lod_mesh_data.size() < lp.lod_blas.size() ||
        lp.owned_blas.size() < lp.lod_blas.size()) {
        CHECKF(false, "%s %016llx: ladder arrays inconsistent (blas=%zu thr=%zu mesh=%zu owned=%zu)",
               ctx, (unsigned long long)hash, lp.lod_blas.size(), lp.thresholds.size(),
               lp.lod_mesh_data.size(), lp.owned_blas.size());
        return false;
    }
    fold_pod(sig, lp.bound_radius);
    bool ok = true;
    for (size_t i = 0; i < lp.lod_blas.size(); ++i) {
        fold_pod(sig, lp.thresholds[i]);
        const BLASManager::BLASEntry* e = lookup(lp.lod_blas[i]);
        ok = validate_entry(e, sig, ctx, hash, i) && ok;
    }
    // Every owned registration must resolve; fold cluster handles' content too
    // (dedup can alias them onto ladder handles — content folds are idempotent
    // per handle order, which is deterministic).
    for (size_t i = lp.lod_blas.size(); i < lp.owned_blas.size(); ++i) {
        const BLASManager::BLASEntry* e = lookup(lp.owned_blas[i]);
        ok = validate_entry(e, sig, ctx, hash, 1000 + i) && ok;
    }
    for (size_t i = 0; i < lp.lod_mesh_data.size(); ++i) {
        const auto& mesh = lp.lod_mesh_data[i];
        if (mesh.vertex_count == 0 && mesh.indices.empty()) {
            fold_pod(sig, (uint64_t)0xE0E0ull);  // legitimately-empty mesh slot
            continue;
        }
        ok = validate_mesh_data(mesh, sig, ctx, hash, i) && ok;
    }
    fold_pod(sig, (uint64_t)lp.clusters.size());
    for (const viewer::LoadedCluster& cluster : lp.clusters) {
        fold_bytes(sig, cluster.aabb_min, sizeof cluster.aabb_min);
        fold_bytes(sig, cluster.aabb_max, sizeof cluster.aabb_max);
        fold_pod(sig, cluster.radius);
        if (cluster.lod_blas.size() != cluster.thresholds.size() ||
            cluster.lod_blas.size() != cluster.lod_mesh.size()) {
            CHECKF(false, "%s %016llx: cluster arrays inconsistent", ctx,
                   (unsigned long long)hash);
            ok = false;
            continue;
        }
        for (size_t i = 0; i < cluster.lod_blas.size(); ++i) {
            fold_pod(sig, cluster.thresholds[i]);
            if (!lookup(cluster.lod_blas[i])) {
                CHECKF(false, "%s %016llx: cluster BLAS handle %u unresolvable", ctx,
                       (unsigned long long)hash, cluster.lod_blas[i]);
                ok = false;
            }
            if (cluster.lod_mesh[i] < 0 ||
                (size_t)cluster.lod_mesh[i] >= lp.lod_mesh_data.size()) {
                CHECKF(false, "%s %016llx: cluster lod_mesh index %d out of range (%zu)",
                       ctx, (unsigned long long)hash, cluster.lod_mesh[i],
                       lp.lod_mesh_data.size());
                ok = false;
            }
        }
    }
    fold_pod(sig, (uint64_t)lp.children.size());
    for (const part_asset::ChildInstance& child : lp.children) {
        fold_pod(sig, child.child_resolved_hash);
        fold_bytes(sig, child.transform, sizeof child.transform);
    }
    fold_pod(sig, (uint64_t)lp.flat_refs.size());
    fold_pod(sig, lp.inline_cutover);
    fold_pod(sig, (uint64_t)lp.fine_cluster_count);
    return ok;
}

static bool validate_staged(viewer::PartStore::StagedPart& sp, uint64_t& sig, const char* ctx) {
    if (!sp.ok || !sp.staging) {
        CHECKF(false, "%s %016llx: staged part not ok", ctx, (unsigned long long)sp.part_hash);
        return false;
    }
    return validate_part_content(
        sp.lp, [&](BLASHandle handle) { return sp.staging->get_entry(handle); }, sig, ctx,
        sp.part_hash);
}

static bool validate_loaded(viewer::PartStore& store, uint64_t hash,
                            const viewer::LoadedPart* lp, uint64_t& sig, const char* ctx) {
    if (!lp) {
        CHECKF(false, "%s %016llx: null LoadedPart", ctx, (unsigned long long)hash);
        return false;
    }
    return validate_part_content(
        *lp, [&](BLASHandle handle) { return store.blas().get_entry(handle); }, sig, ctx, hash);
}

// ---------------------------------------------------------------------------
// MATTER_RACE_DIFF=<16-hex-hash> : single-threaded divergence localizer. Loads
// the hash repeatedly through both flavors and prints the FIRST structural
// divergence field-by-field (level, entry, triangle index, byte offset).
// ---------------------------------------------------------------------------
struct PartSnapshot {
    float bound_radius = 0;
    std::vector<float> thresholds;
    std::vector<BLASHandle> handles;                // lod_blas as-is (alias probe)
    struct Entry {
        std::vector<Tri> tris;
        std::vector<TriEx> triex;
        std::vector<BVHNode> nodes;
        std::vector<uint> tri_idx;
    };
    std::vector<Entry> entries;                     // one per lod_blas handle
    std::vector<viewer::RasterMeshData> meshes;     // lod_mesh_data copies
};

template <class EntryLookup>
static PartSnapshot snap_part(const viewer::LoadedPart& lp, EntryLookup&& lookup) {
    PartSnapshot s;
    s.bound_radius = lp.bound_radius;
    s.thresholds = lp.thresholds;
    s.handles = lp.lod_blas;
    for (BLASHandle h : lp.lod_blas) {
        PartSnapshot::Entry se;
        if (const BLASManager::BLASEntry* e = lookup(h)) {
            se.tris = e->triangles;
            se.triex = e->tri_extra;
            if (e->bvh && e->bvh->bvhNode)
                se.nodes.assign(e->bvh->bvhNode, e->bvh->bvhNode + e->bvh->nodesUsed);
            if (e->bvh && e->bvh->triIdx)
                se.tri_idx.assign(e->bvh->triIdx, e->bvh->triIdx + e->triangles.size());
        }
        s.entries.push_back(std::move(se));
    }
    s.meshes = lp.lod_mesh_data;
    return s;
}

static void print_handles(const PartSnapshot& s, const char* tag) {
    std::printf("    %s handles:", tag);
    for (BLASHandle h : s.handles) std::printf(" %u", h);
    std::printf("\n");
}

static void diff_snapshots(const PartSnapshot& a, const PartSnapshot& b, const char* what) {
    auto p = [&](const char* fmt, auto... args) {
        std::printf("  [diff %s] ", what);
        std::printf(fmt, args...);
        std::printf("\n");
    };
    if (std::memcmp(&a.bound_radius, &b.bound_radius, 4))
        p("bound_radius %.9g vs %.9g", a.bound_radius, b.bound_radius);
    if (a.thresholds != b.thresholds) p("thresholds differ");
    if (a.handles != b.handles) {
        p("HANDLE PATTERN differs (dedup flip?)");
        print_handles(a, "a");
        print_handles(b, "b");
    }
    if (a.entries.size() != b.entries.size()) {
        p("entry count %zu vs %zu", a.entries.size(), b.entries.size());
        return;
    }
    for (size_t e = 0; e < a.entries.size(); ++e) {
        const auto& ea = a.entries[e];
        const auto& eb = b.entries[e];
        if (ea.tris.size() != eb.tris.size()) {
            p("L%zu tri count %zu vs %zu", e, ea.tris.size(), eb.tris.size());
            continue;
        }
        for (size_t t = 0; t < ea.tris.size(); ++t) {
            const auto* ba = reinterpret_cast<const unsigned char*>(&ea.tris[t]);
            const auto* bb = reinterpret_cast<const unsigned char*>(&eb.tris[t]);
            // compare the three vertex float3s only (12 bytes at offsets 0/16/32)
            for (int corner = 0; corner < 3; ++corner) {
                if (std::memcmp(ba + corner * 16, bb + corner * 16, 12)) {
                    const float* fa = reinterpret_cast<const float*>(ba + corner * 16);
                    const float* fb = reinterpret_cast<const float*>(bb + corner * 16);
                    p("L%zu tri %zu vertex%d differs: a=(%.9g,%.9g,%.9g) b=(%.9g,%.9g,%.9g)",
                      e, t, corner, fa[0], fa[1], fa[2], fb[0], fb[1], fb[2]);
                    goto tris_done;
                }
            }
        }
    tris_done:
        if (ea.triex.size() != eb.triex.size()) {
            p("L%zu triex count %zu vs %zu", e, ea.triex.size(), eb.triex.size());
        } else {
            for (size_t t = 0; t < ea.triex.size(); ++t) {
                const auto* xa = reinterpret_cast<const unsigned char*>(&ea.triex[t]);
                const auto* xb = reinterpret_cast<const unsigned char*>(&eb.triex[t]);
                for (size_t off = 0; off < 92; ++off) {
                    if (xa[off] != xb[off]) {
                        static const char* fields =
                            "uv0@0 uv1@8 uv2@16 N0@24 N1@36 N2@48 materialId@60 tint@64 ao@80";
                        p("L%zu triex %zu first byte diff at offset %zu (%s)", e, t, off, fields);
                        goto triex_done;
                    }
                }
            }
        }
    triex_done:
        if (ea.nodes.size() != eb.nodes.size()) {
            p("L%zu nodesUsed %zu vs %zu", e, ea.nodes.size(), eb.nodes.size());
        } else {
            for (size_t n = 0; n < ea.nodes.size(); ++n) {
                if (std::memcmp(&ea.nodes[n], &eb.nodes[n], sizeof(BVHNode))) {
                    p("L%zu BVH node %zu differs: a(leftFirst=%u triCount=%u aabbMin=%g,%g,%g) "
                      "b(leftFirst=%u triCount=%u aabbMin=%g,%g,%g)",
                      e, n, ea.nodes[n].leftFirst, ea.nodes[n].triCount, ea.nodes[n].aabbMin.x,
                      ea.nodes[n].aabbMin.y, ea.nodes[n].aabbMin.z, eb.nodes[n].leftFirst,
                      eb.nodes[n].triCount, eb.nodes[n].aabbMin.x, eb.nodes[n].aabbMin.y,
                      eb.nodes[n].aabbMin.z);
                    break;
                }
            }
        }
        if (ea.tri_idx != eb.tri_idx) {
            for (size_t i = 0; i < ea.tri_idx.size() && i < eb.tri_idx.size(); ++i)
                if (ea.tri_idx[i] != eb.tri_idx[i]) {
                    p("L%zu triIdx[%zu] %u vs %u", e, i, ea.tri_idx[i], eb.tri_idx[i]);
                    break;
                }
        }
    }
    if (a.meshes.size() != b.meshes.size()) {
        p("mesh count %zu vs %zu", a.meshes.size(), b.meshes.size());
        return;
    }
    for (size_t m = 0; m < a.meshes.size(); ++m) {
        const auto& ma = a.meshes[m];
        const auto& mb = b.meshes[m];
        if (ma.vertex_count != mb.vertex_count)
            p("mesh %zu vertex_count %d vs %d", m, ma.vertex_count, mb.vertex_count);
        auto diff_arr = [&](const char* name, const auto& va, const auto& vb) {
            if (va.size() != vb.size()) {
                p("mesh %zu %s size %zu vs %zu", m, name, va.size(), vb.size());
                return;
            }
            for (size_t i = 0; i < va.size(); ++i)
                if (std::memcmp(&va[i], &vb[i], sizeof va[i])) {
                    p("mesh %zu %s[%zu] differs", m, name, i);
                    return;
                }
        };
        diff_arr("vertices", ma.vertices, mb.vertices);
        diff_arr("normals", ma.normals, mb.normals);
        diff_arr("colors", ma.colors, mb.colors);
        diff_arr("texcoords", ma.texcoords, mb.texcoords);
        diff_arr("surface_uvs", ma.surface_uvs, mb.surface_uvs);
        diff_arr("material_ids", ma.material_ids, mb.material_ids);
        diff_arr("baked_ao", ma.baked_ao, mb.baked_ao);
        diff_arr("indices", ma.indices, mb.indices);
    }
}

static int run_diff_mode(const fs::path& root, uint64_t hash, int rounds) {
    std::printf("DIFF MODE: %016llx, %d rounds\n", (unsigned long long)hash, rounds);
    // Staged flavor: repeated stage_load in ONE store (restage divergence).
    {
        viewer::PartStore store(root.string());
        PartSnapshot first;
        bool have_first = false;
        for (int r = 0; r < rounds; ++r) {
            viewer::PartStore::StagedPart sp = store.stage_load(hash);
            if (!sp.ok) { std::printf("  stage_load !ok on round %d\n", r); break; }
            PartSnapshot s = snap_part(
                sp.lp, [&](BLASHandle h) { return sp.staging->get_entry(h); });
            if (!have_first) {
                first = std::move(s);
                have_first = true;
            } else {
                char what[64];
                std::snprintf(what, sizeof what, "stage round %d vs 0", r);
                diff_snapshots(first, s, what);
            }
        }
    }
    // Loaded flavor: get_or_load / release cycles in ONE store.
    {
        viewer::PartStore store(root.string());
        PartSnapshot first;
        bool have_first = false;
        for (int r = 0; r < rounds; ++r) {
            const viewer::LoadedPart* lp = store.get_or_load(hash);
            if (!lp) { std::printf("  get_or_load null on round %d\n", r); break; }
            PartSnapshot s =
                snap_part(*lp, [&](BLASHandle h) { return store.blas().get_entry(h); });
            if (!have_first) {
                first = std::move(s);
                have_first = true;
            } else {
                char what[64];
                std::snprintf(what, sizeof what, "load round %d vs 0", r);
                diff_snapshots(first, s, what);
            }
            store.release(hash);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// MATTER_RACE_PIPE=<16-hex-hash> : pipeline bisection. Decodes the part ONCE,
// freezes the gathered full-res Tri/TriEx in memory, then repeatedly runs each
// downstream stage in isolation and reports which stage produces divergent
// output for identical input:
//   stage A: lod_bake::bake_lods into a fresh private BLASManager
//   stage B: decimate_tris alone (per configured rung ratio)
//   stage C: from_tri(source) + ReprojectSource build + reproject_triex onto a
//            frozen decimated target — fresh index per round
//   stage D: same but ONE prebuilt index queried repeatedly
// ---------------------------------------------------------------------------
#include "lod_bake.h"
#include "mesh_indexed.hpp"
#include "mesh_transform.hpp"

static uint64_t sig_tris(const std::vector<Tri>& tris) {
    uint64_t h = 1469598103934665603ull;
    fold_pod(h, (uint64_t)tris.size());
    for (const Tri& t : tris) { fold_f3(h, t.vertex0); fold_f3(h, t.vertex1); fold_f3(h, t.vertex2); }
    return h;
}
static uint64_t sig_triex(const std::vector<TriEx>& triex) {
    uint64_t h = 1469598103934665603ull;
    fold_pod(h, (uint64_t)triex.size());
    for (const TriEx& e : triex) fold_bytes(h, &e, 92);
    return h;
}
static size_t first_triex_diff(const std::vector<TriEx>& a, const std::vector<TriEx>& b,
                               size_t& byte_off) {
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const auto* pa = reinterpret_cast<const unsigned char*>(&a[i]);
        const auto* pb = reinterpret_cast<const unsigned char*>(&b[i]);
        for (size_t o = 0; o < 92; ++o)
            if (pa[o] != pb[o]) { byte_off = o; return i; }
    }
    byte_off = SIZE_MAX;
    return SIZE_MAX;
}

static int run_pipe_mode(const fs::path& root, uint64_t hash, int rounds) {
    std::printf("PIPE MODE: %016llx, %d rounds\n", (unsigned long long)hash, rounds);
    // Decode once via a throwaway store staging; freeze full-res tris/triex.
    std::vector<Tri> tris;
    std::vector<TriEx> triex;
    {
        viewer::PartStore store(root.string());
        viewer::PartStore::StagedPart sp = store.stage_load(hash);
        if (!sp.ok) { std::printf("  stage_load !ok\n"); return 1; }
        // Recover the FULL-RES level (level 0 of the ladder is undecimated).
        const BLASManager::BLASEntry* e0 =
            sp.lp.lod_blas.empty() ? nullptr : sp.staging->get_entry(sp.lp.lod_blas[0]);
        if (!e0) { std::printf("  no level-0 entry\n"); return 1; }
        tris = e0->triangles;
        triex = e0->tri_extra;
        std::printf("  frozen source: %zu tris, %zu triex\n", tris.size(), triex.size());
    }

    // Stage A: whole ladder bake, fresh manager per round.
    {
        std::vector<uint64_t> tri_sigs, ex_sigs;
        std::vector<std::vector<TriEx>> kept_ex;
        for (int r = 0; r < rounds; ++r) {
            BLASManager priv;
            std::vector<BLASHandle> handles;
            lod_bake::LodLevels lods = lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, priv,
                                                           triex.empty() ? nullptr : &triex,
                                                           nullptr, &handles);
            uint64_t ts = 1469598103934665603ull, xs = ts;
            std::vector<TriEx> all_ex;
            for (BLASHandle h : handles) {
                if (const auto* e = priv.get_entry(h)) {
                    fold_pod(ts, sig_tris(e->triangles));
                    fold_pod(xs, sig_triex(e->tri_extra));
                    all_ex.insert(all_ex.end(), e->tri_extra.begin(), e->tri_extra.end());
                }
            }
            tri_sigs.push_back(ts);
            ex_sigs.push_back(xs);
            kept_ex.push_back(std::move(all_ex));
        }
        int tri_div = 0, ex_div = 0;
        for (int r = 1; r < rounds; ++r) {
            tri_div += tri_sigs[r] != tri_sigs[0];
            ex_div += ex_sigs[r] != ex_sigs[0];
            if (ex_sigs[r] != ex_sigs[0]) {
                size_t off = 0;
                size_t idx = first_triex_diff(kept_ex[0], kept_ex[r], off);
                std::printf("  [A bake_lods] round %d triex diverges at concat-index %zu offset %zu\n",
                            r, idx, off);
            }
        }
        std::printf("  stage A (bake_lods x%d): tri divergences %d, triex divergences %d\n",
                    rounds, tri_div, ex_div);
    }

    // Stage B: decimation alone.
    std::vector<Tri> target_geo;
    {
        std::vector<uint64_t> sigs;
        for (int r = 0; r < rounds; ++r) {
            std::vector<Tri> d = lod_bake::decimate_tris(tris, 0.1f);
            if (r == 0) target_geo = d;
            sigs.push_back(sig_tris(d));
        }
        int div = 0;
        for (int r = 1; r < rounds; ++r) div += sigs[r] != sigs[0];
        std::printf("  stage B (decimate_tris 0.1 x%d): divergences %d (out tris %zu)\n", rounds,
                    div, target_geo.size());
    }

    if (!triex.empty() && triex.size() == tris.size() && !target_geo.empty()) {
        // Stage C: fresh ReprojectSource per round, frozen target.
        MeshIndexed tgt_frozen = from_tri(target_geo, nullptr);
        std::vector<std::vector<TriEx>> outs;
        {
            std::vector<uint64_t> sigs;
            for (int r = 0; r < rounds; ++r) {
                MeshIndexed src_m = from_tri(tris, &triex);
                ReprojectSource index(src_m, ReprojectNormals::SampleSource);
                MeshIndexed tgt = tgt_frozen;
                reproject_triex(index, tgt);
                sigs.push_back(sig_triex(tgt.triex));
                outs.push_back(tgt.triex);
            }
            int div = 0;
            for (int r = 1; r < rounds; ++r) {
                if (sigs[r] != sigs[0]) {
                    ++div;
                    size_t off = 0;
                    size_t idx = first_triex_diff(outs[0], outs[r], off);
                    std::printf("  [C fresh-index] round %d diverges at tri %zu offset %zu\n", r,
                                idx, off);
                }
            }
            std::printf("  stage C (fresh ReprojectSource x%d): divergences %d\n", rounds, div);
        }
        // Stage D: one index, repeated queries.
        {
            MeshIndexed src_m = from_tri(tris, &triex);
            ReprojectSource index(src_m, ReprojectNormals::SampleSource);
            std::vector<uint64_t> sigs;
            for (int r = 0; r < rounds; ++r) {
                MeshIndexed tgt = tgt_frozen;
                reproject_triex(index, tgt);
                sigs.push_back(sig_triex(tgt.triex));
            }
            int div = 0;
            for (int r = 1; r < rounds; ++r) div += sigs[r] != sigs[0];
            std::printf("  stage D (one ReprojectSource, %d queries): divergences %d\n", rounds,
                        div);
        }
    }

    // Stage E: full per-round emulation WITH a fresh decode each round — the
    // configuration that actually flips. Signatures of every intermediate name
    // the first diverging stage.
    {
        struct RoundSig {
            uint64_t gather_tris = 0, gather_triex = 0;
            std::vector<uint64_t> decimated;      // per decimated rung
            std::vector<uint64_t> reprojected;    // per decimated rung
            std::vector<BLASHandle> handles;      // ladder handle pattern
        };
        std::vector<RoundSig> rs;
        const lod_bake::BakeTargets targets{};
        for (int r = 0; r < rounds; ++r) {
            RoundSig s;
            // fresh decode (mirrors read_coherent_snapshot)
            BLASManager scratch;
            TLASManager scratch_tlas(65536);
            std::vector<part_asset::ChildInstance> children;
            part_asset::LodLevels lods_in;
            std::vector<part_asset::VolumeEmitter> emitters;
            std::optional<part_asset::PartAnimationLink> link;
            const std::string path =
                (root / part_asset::cache_path_resolved(hash)).string();
            if (!part_asset::load_v2(path, hash, scratch, scratch_tlas, children, lods_in,
                                     emitters, link)) {
                std::printf("  [E] round %d: load_v2 failed\n", r);
                break;
            }
            std::vector<Tri> g_tris;
            std::vector<TriEx> g_triex;
            for (const auto& e : scratch.get_entries()) {
                g_tris.insert(g_tris.end(), e->triangles.begin(), e->triangles.end());
                g_triex.insert(g_triex.end(), e->tri_extra.begin(), e->tri_extra.end());
            }
            s.gather_tris = sig_tris(g_tris);
            s.gather_triex = sig_triex(g_triex);
            const bool usable = g_triex.size() == g_tris.size() && !g_triex.empty();
            std::unique_ptr<MeshIndexed> src_m;
            std::unique_ptr<ReprojectSource> src_index;
            BLASManager staging;
            for (size_t lvl = 0; lvl < targets.keep_ratio.size(); ++lvl) {
                const float keep = targets.keep_ratio[lvl];
                const bool full = keep >= 0.999f;
                std::vector<Tri> decimated;
                if (!full) {
                    decimated = lod_bake::decimate_tris(g_tris, keep);
                    if (decimated.empty()) decimated = g_tris;
                    s.decimated.push_back(sig_tris(decimated));
                }
                const std::vector<Tri>& geo = full ? g_tris : decimated;
                std::vector<TriEx> reprojected;
                if (!full && usable) {
                    if (!src_index) {
                        src_m = std::make_unique<MeshIndexed>(from_tri(g_tris, &g_triex));
                        src_index = std::make_unique<ReprojectSource>(
                            *src_m, ReprojectNormals::SampleSource);
                    }
                    MeshIndexed tgt_m = from_tri(geo, nullptr);
                    reproject_triex(*src_index, tgt_m);
                    std::vector<Tri> unused;
                    to_tri(tgt_m, unused, reprojected);
                    s.reprojected.push_back(sig_triex(reprojected));
                }
                const TriEx* ex = nullptr;
                if (full && usable && g_triex.size() == geo.size()) ex = g_triex.data();
                else if (!full && reprojected.size() == geo.size()) ex = reprojected.data();
                const BLASHandle h = staging.register_triangles(
                    const_cast<Tri*>(geo.data()), (int)geo.size(), ex);
                s.handles.push_back(h);
            }
            rs.push_back(std::move(s));
        }
        int gt = 0, gx = 0, dec = 0, rep = 0, hp = 0;
        for (size_t r = 1; r < rs.size(); ++r) {
            gt += rs[r].gather_tris != rs[0].gather_tris;
            gx += rs[r].gather_triex != rs[0].gather_triex;
            dec += rs[r].decimated != rs[0].decimated;
            rep += rs[r].reprojected != rs[0].reprojected;
            if (rs[r].handles != rs[0].handles) {
                ++hp;
                std::printf("  [E] round %zu handle pattern:", r);
                for (BLASHandle h : rs[r].handles) std::printf(" %u", h);
                std::printf("  (round 0:");
                for (BLASHandle h : rs[0].handles) std::printf(" %u", h);
                std::printf(")\n");
            }
        }
        std::printf("  stage E (full emulation x%d): gather_tris div %d, gather_triex div %d, "
                    "decimated div %d, reprojected div %d, handle-pattern div %d\n",
                    rounds, gt, gx, dec, rep, hp);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Targeted proofs for the BLAS dedup identity defect.
//
// Tri is ALIGN(64) with union{float3 vertexN; __m128 vN} slots: vertex0 at 0,
// vertex1 at 16, vertex2 at 32 — 4 padding bytes after every float3. But
// BLASManager::calculate_hash reads 9 consecutive floats from &vertex0
// (bytes 0..35) and triangles_equal memcmps sizeof(float3)*3 == 36 contiguous
// bytes. That range is: vertex0 | PAD | vertex1 | PAD | vertex2.x — so
//   (a) the two padding words PARTICIPATE in geometry identity, and
//   (b) vertex2.y and vertex2.z are IGNORED by it.
// Proof A: logically identical triangles with different pad bytes must dedup
//          but do not (false negative — nondeterministic ladders, duplicate
//          entries, unstable content).
// Proof B: triangles differing ONLY in vertex2.y/z must NOT dedup but DO
//          (false positive — a part silently adopts ANOTHER mesh's geometry
//          and BVH: wrong triangles reach every consumer of the entry).
// ---------------------------------------------------------------------------
static_assert(sizeof(Tri) == 64, "Tri layout changed");
static_assert(offsetof(Tri, vertex1) == 16, "Tri union stride assumption");
static_assert(offsetof(Tri, vertex2) == 32, "Tri union stride assumption");

static void set_tri_probe(Tri& t) {
    t.vertex0 = make_float3(1, 2, 3);
    t.vertex1 = make_float3(4, 5, 6);
    t.vertex2 = make_float3(7, 8, 9);
    t.centroid = make_float3(4, 5, 6);
}

static void run_dedup_identity_proofs() {
    auto set_tri = [](Tri& t, float ax, float ay, float az, float bx, float by, float bz,
                      float cx, float cy, float cz) {
        t.vertex0 = make_float3(ax, ay, az);
        t.vertex1 = make_float3(bx, by, bz);
        t.vertex2 = make_float3(cx, cy, cz);
        t.centroid = make_float3((ax + bx + cx) / 3, (ay + by + cy) / 3, (az + bz + cz) / 3);
    };

    // --- Proof A: padding participates in identity -------------------------
    {
        BLASManager m;
        Tri a[1], b[1];
        std::memset(a, 0x00, sizeof a);
        std::memset(b, 0xA5, sizeof b);  // different padding garbage
        set_tri(a[0], 0, 0, 0, 1, 0, 0, 0, 1, 0);
        set_tri(b[0], 0, 0, 0, 1, 0, 0, 0, 1, 0);  // identical logical triangle
        const BLASHandle ha = m.register_triangles(a, 1, nullptr);
        const BLASHandle hb = m.register_triangles(b, 1, nullptr);
        if (ha != hb) {
            CHECKF(false,
                   "PROOF A (pad sensitivity): logically identical triangles did NOT dedup "
                   "(handles %u vs %u, %zu entries) — calculate_hash/triangles_equal read the "
                   "union padding bytes at Tri+12 and Tri+28",
                   ha, hb, m.get_entries().size());
        } else {
            std::printf("  proof A: identical-geometry/different-padding deduped OK "
                        "(padding does not affect identity)\n");
        }
    }

    // --- Proof B: vertex2.y/z ignored by identity --------------------------
    // The hash ALSO folds 16 uninitialized stack bytes per triangle (see
    // proof C below), so the two registrations only land in the same bucket
    // when the garbage happens to match. Loop until the false-positive dedup
    // fires: any wrong merge of distinct geometry is one too many.
    {
        int wrong_dedup_rounds = 0, rounds_run = 0;
        float stored_y = 0, stored_z = 0;
        for (int round = 0; round < 400 && wrong_dedup_rounds == 0; ++round) {
            BLASManager m;
            Tri a[1], b[1];
            std::memset(a, 0, sizeof a);
            std::memset(b, 0, sizeof b);
            set_tri(a[0], 0, 0, 0, 1, 0, 0, 2, 5, 9);
            set_tri(b[0], 0, 0, 0, 1, 0, 0, 2, -7, 42);  // SAME v2.x, DIFFERENT v2.y/z
            const BLASHandle ha = m.register_triangles(a, 1, nullptr);
            const BLASHandle hb = m.register_triangles(b, 1, nullptr);
            ++rounds_run;
            if (ha == hb) {
                ++wrong_dedup_rounds;
                const auto* e = m.get_entry(ha);
                if (e) { stored_y = e->triangles[0].vertex2.y; stored_z = e->triangles[0].vertex2.z; }
            }
        }
        if (wrong_dedup_rounds) {
            CHECKF(false,
                   "PROOF B (missed geometry): triangles differing ONLY in vertex2.y/z were "
                   "DEDUPED onto one entry after %d rounds (stored v2.y/z=(%g,%g); second mesh "
                   "was (-7,42) or (5,9)) — a mesh silently adopts ANOTHER mesh's geometry/BVH",
                   rounds_run, stored_y, stored_z);
        } else {
            std::printf("  proof B: vertex2.y/z differences kept distinct entries in %d rounds "
                        "(false-positive dedup did not fire this run — it is gated on the "
                        "uninitialized-tint hash collision, see proof C)\n",
                        rounds_run);
        }
    }

    // --- Proof C: identity depends on uninitialized stack bytes ------------
    // Register the SAME triangle bytes into fresh managers via call paths with
    // different stack residue; the stored entry hash flips. Also probe which
    // 4-byte offsets of Tri affect the hash — the true identity window.
    {
        Tri base{};
        std::memset(&base, 0, sizeof base);
        set_tri_probe(base);
        auto hash_once = [&](int flavor) -> uint32_t {
            BLASManager probe;
            if (flavor) {
                // dirty the stack with a different call pattern first
                volatile double sink[24];
                for (int i = 0; i < 24; ++i) sink[i] = i * 1.618033988749895 + flavor;
                (void)sink;
            }
            Tri t = base;
            probe.register_triangles(&t, 1, nullptr);
            return probe.get_entries()[0]->hash;
        };
        uint32_t h0 = hash_once(0);
        int flips = 0;
        for (int flavor = 1; flavor <= 8; ++flavor) flips += hash_once(flavor) != h0;
        if (flips) {
            CHECKF(false,
                   "PROOF C (uninitialized identity): the SAME triangle bytes produced %d/8 "
                   "different content hashes depending on prior stack contents — "
                   "calculate_hash's tint fold reads the never-initialized float4 local "
                   "through reinterpret_cast<uint32_t*> (strict-aliasing UB; GCC elides the "
                   "stores; disassembly reads 16 bytes at rsp)",
                   flips);
        } else {
            std::printf("  proof C: hash stable across stack contexts this run "
                        "(garbage happened to repeat)\n");
        }
        uint32_t base_hash = hash_once(0);
        std::printf("  hash-affecting Tri offsets (true identity window):");
        for (size_t off = 0; off < sizeof(Tri); off += 4) {
            Tri t = base;
            const float poison = 999.25f;
            std::memcpy(reinterpret_cast<char*>(&t) + off, &poison, 4);
            BLASManager probe;
            probe.register_triangles(&t, 1, nullptr);
            if (probe.get_entries()[0]->hash != base_hash) std::printf(" %zu", off);
        }
        std::printf("  (vertex floats are 0-8,16-24,32-40; 12/28 are padding; 48+ centroid)\n");
    }
}

// ---------------------------------------------------------------------------
// Staged-part handoff queue (worker -> app), mirrors the GpuJob closure.
// ---------------------------------------------------------------------------
struct StagedQueue {
    std::mutex m;
    std::deque<viewer::PartStore::StagedPart> q;
    void push(viewer::PartStore::StagedPart&& sp) {
        std::lock_guard<std::mutex> lk(m);
        q.push_back(std::move(sp));
    }
    std::optional<viewer::PartStore::StagedPart> try_pop() {
        std::lock_guard<std::mutex> lk(m);
        if (q.empty()) return std::nullopt;
        viewer::PartStore::StagedPart sp = std::move(q.front());
        q.pop_front();
        return sp;
    }
    size_t size() {
        std::lock_guard<std::mutex> lk(m);
        return q.size();
    }
};

// ---------------------------------------------------------------------------
int main() {
#ifdef _WIN32
    AddVectoredExceptionHandler(1, fatal_logger);
#endif
    const double budget_s = [] {
        const char* v = std::getenv("MATTER_RACE_SECONDS");
        return v ? std::atof(v) : 40.0;
    }();
    const uint64_t max_iters = [] {
        const char* v = std::getenv("MATTER_RACE_MAX_ITERS");
        return v ? (uint64_t)std::atoll(v) : 200000ull;
    }();
    const char* lock_env = std::getenv("MATTER_STAGE_LOCK");
    const bool app_takes_lock = lock_env != nullptr;  // mirrors pump_gpu_jobs
    const bool pageheap = [] {
        const char* v = std::getenv("MATTER_RACE_PAGEHEAP");
        return v && *v && *v != '0';
    }();
    // MATTER_RACE_CACHE=<dir>: read a REAL cache (e.g.
    // projects/world_demo/.cache/StreamMeadow) instead of synthesizing
    // fixtures. Read-only: PartStore never writes the cache, and the cleanup
    // guard is disabled in this mode.
    const char* cache_env = std::getenv("MATTER_RACE_CACHE");
    const bool real_cache = cache_env && *cache_env;
#ifdef _WIN32
    if (pageheap) guardalloc::g_enabled.store(true);
#endif

    std::printf("partstore_race_tests: budget=%.0fs max_iters=%llu MATTER_STAGE_LOCK=%s "
                "pageheap=%d cache=%s\n",
                budget_s, (unsigned long long)max_iters, lock_env ? lock_env : "(unset)",
                pageheap ? 1 : 0, real_cache ? cache_env : "(synthetic)");

    const fs::path root =
        real_cache ? fs::path(cache_env) : (fs::temp_directory_path() / "me3_partstore_race");
    std::error_code ec;
    if (!real_cache) {
        fs::remove_all(root, ec);
        fs::create_directories(root / "parts", ec);
    } else if (!fs::is_directory(root / "parts", ec)) {
        std::printf("partstore_race_tests: no parts/ under %s\n", root.string().c_str());
        return 1;
    }
    struct Cleanup {
        fs::path p;
        bool armed;
        ~Cleanup() {
            if (!armed) return;
            std::error_code ignored;
            fs::remove_all(p, ignored);
        }
    } cleanup{root, !real_cache};

    std::printf("dedup identity proofs:\n");
    run_dedup_identity_proofs();
    // The proofs above demonstrate KNOWN defects in BLAS identity
    // (blas_manager.cpp calculate_hash/triangles_equal); their failures make
    // this test red but must not gate the stress phases below.
    const int proof_failures = g_failures.load();

    if (const char* diff_env = std::getenv("MATTER_RACE_DIFF")) {
        const uint64_t diff_hash = std::strtoull(diff_env, nullptr, 16);
        if (diff_hash) return run_diff_mode(root, diff_hash, 6);
    }
    if (const char* pipe_env = std::getenv("MATTER_RACE_PIPE")) {
        const uint64_t pipe_hash = std::strtoull(pipe_env, nullptr, 16);
        if (pipe_hash) return run_pipe_mode(root, pipe_hash, 24);
    }
    // MATTER_RACE_DIFF2=<h1>,<h2>: single-threaded residency-order probe.
    // Loads h2 alone, then h1-then-h2 in a fresh store, and diffs h2's two
    // states. A difference proves cross-part dedup adopts the FIRST resident
    // part's TriEx (normals/uv/AO are outside the identity), no threads
    // involved.
    if (const char* d2 = std::getenv("MATTER_RACE_DIFF2")) {
        uint64_t h1 = 0, h2 = 0;
        if (std::sscanf(d2, "%llx,%llx", (unsigned long long*)&h1,
                        (unsigned long long*)&h2) == 2 && h1 && h2) {
            PartSnapshot alone, after;
            {
                viewer::PartStore store(root.string());
                const viewer::LoadedPart* lp = store.get_or_load(h2);
                if (!lp) { std::printf("DIFF2: %016llx unloadable\n",
                                       (unsigned long long)h2); return 1; }
                alone = snap_part(*lp, [&](BLASHandle h) { return store.blas().get_entry(h); });
            }
            {
                viewer::PartStore store(root.string());
                store.get_or_load(h1);
                const viewer::LoadedPart* lp = store.get_or_load(h2);
                if (!lp) { std::printf("DIFF2: %016llx unloadable after %016llx\n",
                                       (unsigned long long)h2, (unsigned long long)h1); return 1; }
                after = snap_part(*lp, [&](BLASHandle h) { return store.blas().get_entry(h); });
            }
            std::printf("DIFF2: %016llx alone vs after-%016llx:\n", (unsigned long long)h2,
                        (unsigned long long)h1);
            diff_snapshots(alone, after, "residency-order");
            std::printf("DIFF2 done (no output above the header = identical)\n");
            return 0;
        }
    }

    // --- fixtures ---------------------------------------------------------
    // Worker set (A): the sectors the streaming worker stages.
    // App set (B): the parts the app thread's own get_or_load path churns.
    std::vector<PartSpec> parts;
    auto add_part = [&](uint64_t hash, std::initializer_list<GroupSpec> groups, bool lone) {
        PartSpec p;
        p.hash = hash;
        p.groups = groups;
        p.lone_triangle_group = lone;
        parts.push_back(std::move(p));
    };
    const bool big = [] {
        const char* v = std::getenv("MATTER_RACE_BIG");
        return v && *v && *v != '0';
    }();
    if (big) {
        // Sector-scale parts (~9-16k tris) — matches the 20-40 ms production
        // stage cost profile; fewer hashes so residency churn stays realistic.
        add_part(0xA000000000000001ull, {{56, 0.50f, 0, 0, 10, 0xA1u, false},
                                         {40, 0.75f, 29, 0, 40, 0xA2u, false}}, false);
        add_part(0xA000000000000002ull, {{64, 0.45f, 0, 0, 70, 0xB3u, true}}, false);
        add_part(0xA000000000000003ull, {{48, 0.60f, 0, 0, 130, 0xC5u, false},
                                         {32, 4.00f, -140, -140, 160, 0xC6u, false}}, true);
        add_part(0xA000000000000004ull, {{40, 0.02f, 0, 0, 80, 0xF1u, false},
                                         {40, 8.00f, 400, 400, 110, 0xF2u, false}}, false);
        add_part(0xB000000000000001ull, {{56, 0.52f, 0, 0, 15, 0x11u, false}}, false);
        add_part(0xB000000000000002ull, {{48, 0.44f, 0, 0, 75, 0x23u, true},
                                         {32, 0.62f, 24, 0, 45, 0x12u, false}}, false);
        add_part(0xB000000000000003ull, {{40, 0.58f, 0, 0, 105, 0x34u, false},
                                         {40, 0.31f, 0, 24, 135, 0x35u, false}}, true);
        add_part(0xB000000000000004ull, {{64, 0.47f, 0, 0, 165, 0x46u, false}}, false);
    } else {
    // A set — sector-like: 2-3 groups, ~700-1300 tris each part.
    add_part(0xA000000000000001ull, {{16, 0.50f, 0, 0, 10, 0xA1u, false},
                                     {12, 0.75f, 9, 0, 40, 0xA2u, false}}, false);
    add_part(0xA000000000000002ull, {{18, 0.45f, 0, 0, 70, 0xB3u, false},
                                     {10, 0.30f, 0, 9, 100, 0xB4u, true}}, false);
    add_part(0xA000000000000003ull, {{16, 0.60f, 0, 0, 130, 0xC5u, false},
                                     {8, 4.00f, -40, -40, 160, 0xC6u, false}}, true);
    add_part(0xA000000000000004ull, {{20, 0.40f, 0, 0, 190, 0xD7u, false}}, false);
    add_part(0xA000000000000005ull, {{14, 0.55f, 0, 0, 20, 0xE8u, true},
                                     {14, 0.35f, 8, 8, 50, 0xE9u, false}}, false);
    add_part(0xA000000000000006ull, {{12, 0.02f, 0, 0, 80, 0xF1u, false},   // tiny scale
                                     {12, 8.00f, 100, 100, 110, 0xF2u, false}}, false); // huge scale
    // B set — the app thread's own loads.
    add_part(0xB000000000000001ull, {{14, 0.52f, 0, 0, 15, 0x11u, false},
                                     {10, 0.62f, 8, 0, 45, 0x12u, false}}, false);
    add_part(0xB000000000000002ull, {{16, 0.44f, 0, 0, 75, 0x23u, true}}, false);
    add_part(0xB000000000000003ull, {{12, 0.58f, 0, 0, 105, 0x34u, false},
                                     {12, 0.31f, 0, 8, 135, 0x35u, false}}, true);
    add_part(0xB000000000000004ull, {{18, 0.47f, 0, 0, 165, 0x46u, false}}, false);
    add_part(0xB000000000000005ull, {{10, 0.66f, 0, 0, 195, 0x57u, true},
                                     {14, 0.41f, 7, 7, 25, 0x58u, false}}, false);
    add_part(0xB000000000000006ull, {{16, 0.36f, 0, 0, 55, 0x69u, false}}, false);
    }

    std::vector<uint64_t> all_hashes;
    if (real_cache) {
        for (const auto& de : fs::directory_iterator(root / "parts", ec)) {
            const std::string name = de.path().filename().string();
            // <16 hex>.part only; the .flat.part sibling is loaded implicitly
            // by get_or_load's flat-preferred probe.
            if (name.size() != 21 || name.compare(16, 5, ".part") != 0) continue;
            const uint64_t h = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
            if (h) all_hashes.push_back(h);
        }
        std::sort(all_hashes.begin(), all_hashes.end());
        std::printf("real cache: %zu canonical parts under %s\n", all_hashes.size(),
                    root.string().c_str());
        if (all_hashes.empty()) return 1;
    } else {
        for (const PartSpec& p : parts) {
            CHECKF(write_part_fixture(root, p), "fixture write failed for %016llx",
                   (unsigned long long)p.hash);
            all_hashes.push_back(p.hash);
        }
        if (g_failures.load() > proof_failures) {
            std::printf("partstore_race_tests: fixture phase FAILED\n");
            return 1;
        }
        std::printf("fixtures: %zu synthetic parts under %s\n", all_hashes.size(),
                    root.string().c_str());
    }

    // --- golden phase (single-threaded) ----------------------------------
    // Two golden flavors per hash: what stage_load produces (coherent decode +
    // ladder re-bake) and what get_or_load produces (flat-preferred; identical
    // to the staged flavor when no .flat.part exists). In the MT phase, thread
    // A's staged results and B's commits check the staged flavor; B's own
    // loads check the loaded flavor.
    std::map<uint64_t, uint64_t> golden_staged, golden_loaded;
    std::vector<uint64_t> stageable, loadable;
    {
        viewer::PartStore store(root.string());
        for (uint64_t h : all_hashes) {
            viewer::PartStore::StagedPart sp = store.stage_load(h);
            if (!sp.ok) {
                // Real caches legitimately contain non-stageable parts
                // (animated bundles); synthetic fixtures must all stage.
                CHECKF(real_cache, "golden stage_load(%016llx) failed",
                       (unsigned long long)h);
                continue;
            }
            uint64_t staged_sig = 0;
            if (!validate_staged(sp, staged_sig, "golden-staged")) continue;
            const viewer::LoadedPart* lp = store.commit_staged(std::move(sp));
            uint64_t committed_sig = 0;
            if (!validate_loaded(store, h, lp, committed_sig, "golden-commit")) continue;
            CHECKF(staged_sig == committed_sig,
                   "golden %016llx: staged sig != committed sig (%016llx vs %016llx)",
                   (unsigned long long)h, (unsigned long long)staged_sig,
                   (unsigned long long)committed_sig);
            golden_staged[h] = staged_sig;
            stageable.push_back(h);
            store.release(h);
        }
        for (uint64_t h : all_hashes) {
            const viewer::LoadedPart* lp = store.get_or_load(h);
            if (!lp) {
                CHECKF(real_cache, "golden get_or_load(%016llx) failed",
                       (unsigned long long)h);
                continue;
            }
            uint64_t sig = 0;
            if (!validate_loaded(store, h, lp, sig, "golden-load")) continue;
            golden_loaded[h] = sig;
            loadable.push_back(h);
        }
        // Determinism gate: reload every loadable hash and compare; the MT
        // phase's golden comparisons are meaningless if this fails. (Children
        // recursively loaded by flats stay resident — the parent signature
        // folds child hashes, not child content, so this is stable.)
        for (uint64_t h : all_hashes) store.release(h);
        for (uint64_t h : loadable) {
            const viewer::LoadedPart* lp = store.get_or_load(h);
            uint64_t sig = 0;
            if (validate_loaded(store, h, lp, sig, "golden-reload"))
                CHECKF(sig == golden_loaded[h],
                       "golden %016llx: reload signature differs (pipeline nondeterministic?) "
                       "%016llx vs %016llx",
                       (unsigned long long)h, (unsigned long long)sig,
                       (unsigned long long)golden_loaded[h]);
        }
        // And the staged flavor must be reproducible too.
        for (uint64_t h : stageable) {
            viewer::PartStore::StagedPart sp = store.stage_load(h);
            uint64_t sig = 0;
            if (sp.ok && validate_staged(sp, sig, "golden-restage"))
                CHECKF(sig == golden_staged[h],
                       "golden %016llx: restage signature differs %016llx vs %016llx",
                       (unsigned long long)h, (unsigned long long)sig,
                       (unsigned long long)golden_staged[h]);
        }
    }
    if (g_failures.load() > proof_failures) {
        std::printf("partstore_race_tests: golden phase FAILED (%d) — aborting MT phase.\n"
                    "NOTE: nondeterministic golden signatures are the DEDUP IDENTITY defect "
                    "(see proofs above) manifesting through register_triangles' ladder dedup.\n",
                    g_failures.load() - proof_failures);
        return 1;
    }
    std::printf("golden phase OK (%zu stageable, %zu loadable, deterministic)\n",
                stageable.size(), loadable.size());

    // --- A/B hash split ---------------------------------------------------
    std::vector<uint64_t> a_hashes, b_hashes;
    if (real_cache) {
        // Alternate stageable hashes between the worker and the app; every
        // loadable hash the worker does not own goes to the app's own loads.
        size_t k = 0;
        for (uint64_t h : stageable) ((k++ % 2) ? b_hashes : a_hashes).push_back(h);
        for (uint64_t h : loadable)
            if (std::find(a_hashes.begin(), a_hashes.end(), h) == a_hashes.end() &&
                std::find(b_hashes.begin(), b_hashes.end(), h) == b_hashes.end())
                b_hashes.push_back(h);
    } else {
        for (uint64_t h : all_hashes) ((h >> 60) == 0xA ? a_hashes : b_hashes).push_back(h);
    }
    b_hashes.erase(std::remove_if(b_hashes.begin(), b_hashes.end(),
                                  [&](uint64_t h) { return golden_loaded.find(h) == golden_loaded.end(); }),
                   b_hashes.end());
    if (a_hashes.empty() || b_hashes.empty()) {
        std::printf("partstore_race_tests: degenerate A/B split (%zu/%zu)\n", a_hashes.size(),
                    b_hashes.size());
        return 1;
    }
    std::printf("split: %zu worker hashes, %zu app hashes\n", a_hashes.size(), b_hashes.size());

    // Thread-safe const lookup: both threads read the golden maps concurrently,
    // and map::operator[] would insert on a miss.
    const auto gval = [](const std::map<uint64_t, uint64_t>& m, uint64_t h) -> uint64_t {
        const auto found = m.find(h);
        return found == m.end() ? 0ull : found->second;
    };

    // --- multithreaded phase ---------------------------------------------
    viewer::PartStore store(root.string());
    StagedQueue queue;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> a_iters{0}, b_iters{0}, commits{0}, own_loads{0};

    // Production runs ONE streaming worker; MATTER_RACE_WORKERS>1 widens the
    // interleaving space (stage_load is documented safe from any thread).
    const int worker_count = [] {
        const char* v = std::getenv("MATTER_RACE_WORKERS");
        const int n = v ? std::atoi(v) : 1;
        return n < 1 ? 1 : (n > 8 ? 8 : n);
    }();

    auto worker_body = [&](uint32_t worker_id) {
        CanaryPool canary(0xA11CEu + worker_id * 0x1111u, 320);
        uint64_t it = worker_id;  // stagger the hash sequence per worker
        uint64_t steps = 0;
        while (!stop.load(std::memory_order_relaxed) && steps < max_iters &&
               g_failures.load() <= 50) {
            const uint64_t h = a_hashes[it % a_hashes.size()];
            viewer::PartStore::StagedPart sp = store.stage_load(h);
            CHECKF(sp.ok, "worker%u iter %llu: stage_load(%016llx) failed", worker_id,
                   (unsigned long long)it, (unsigned long long)h);
            if (sp.ok) {
                const uint64_t expect = gval(golden_staged, h);
                uint64_t sig = 0;
                if (validate_staged(sp, sig, "worker-staged") && expect)
                    CHECKF(sig == expect,
                           "worker%u iter %llu: STAGED CONTENT CORRUPTED %016llx sig %016llx != "
                           "golden %016llx",
                           worker_id, (unsigned long long)it, (unsigned long long)h,
                           (unsigned long long)sig, (unsigned long long)expect);
                if ((it % 2) == 0 && queue.size() < 16)
                    queue.push(std::move(sp));
                // else: StagedPart destroyed here (worker-side teardown)
            }
            canary.verify("worker", it);
            canary.churn(6);
            if ((it & 3u) == 0)
                CHECKF(heap_ok(), "worker%u iter %llu: HeapValidate FAILED (heap corrupt)",
                       worker_id, (unsigned long long)it);
            ++it;
            ++steps;
            a_iters.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> workers;
    for (int w = 0; w < worker_count; ++w)
        workers.emplace_back(worker_body, (uint32_t)w);

    {
        CanaryPool canary(0xB0Bu, 320);
        // Hashes B decided to keep loaded, with the golden flavor the resident
        // copy must match (staged flavor after a commit, loaded flavor after a
        // get_or_load — they differ for flat-backed parts in real caches).
        std::vector<std::pair<uint64_t, uint64_t>> resident;  // (hash, expected sig)
        auto note_resident = [&](uint64_t h, uint64_t expect) {
            for (auto& entry : resident)
                if (entry.first == h) { entry.second = expect; return; }
            resident.push_back({h, expect});
        };
        size_t victim_rr = 0;
        uint64_t it = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const auto deadline = t0 + std::chrono::duration<double>(budget_s);
        while (std::chrono::steady_clock::now() < deadline && it < max_iters &&
               g_failures.load() <= 50) {
            // Emulate pump_gpu_jobs: in lock-experiment configs the app thread
            // serializes its publish work on the same mutex.
            std::unique_lock<std::mutex> pump_lock(viewer::PartStore::stage_experiment_mutex(),
                                                   std::defer_lock);
            if (app_takes_lock) pump_lock.lock();

            // 1. commit staged sectors from the worker
            for (int k = 0; k < 2; ++k) {
                auto osp = queue.try_pop();
                if (!osp) break;
                const uint64_t h = osp->part_hash;
                // A hash can already be resident as a side effect (loaded as a
                // flat child during someone's expansion); commit then keeps the
                // resident copy and drops the staged one, so the expectation is
                // the loaded flavor, not the staged one.
                const bool was_resident = store.find(h) != nullptr;
                const viewer::LoadedPart* lp = store.commit_staged(std::move(*osp));
                CHECKF(lp != nullptr, "app iter %llu: commit_staged(%016llx) null",
                       (unsigned long long)it, (unsigned long long)h);
                const uint64_t expect =
                    was_resident ? gval(golden_loaded, h) : gval(golden_staged, h);
                uint64_t sig = 0;
                if (lp && expect && validate_loaded(store, h, lp, sig, "app-commit"))
                    CHECKF(sig == expect,
                           "app iter %llu: COMMITTED CONTENT CORRUPTED %016llx sig %016llx != "
                           "golden %016llx",
                           (unsigned long long)it, (unsigned long long)h,
                           (unsigned long long)sig, (unsigned long long)expect);
                ++commits;
                if ((it % 4) != 3) {
                    store.release(h);
                } else if (expect) {
                    note_resident(h, expect);
                }
            }
            // 2. the app thread's own full loads (decode+bake+commit inline)
            {
                const uint64_t h2 = b_hashes[it % b_hashes.size()];
                const uint64_t expect2 = gval(golden_loaded, h2);
                const viewer::LoadedPart* lp2 = store.get_or_load(h2);
                CHECKF(lp2 != nullptr, "app iter %llu: get_or_load(%016llx) null",
                       (unsigned long long)it, (unsigned long long)h2);
                uint64_t sig = 0;
                if (lp2 && expect2 && validate_loaded(store, h2, lp2, sig, "app-load"))
                    CHECKF(sig == expect2,
                           "app iter %llu: LOADED CONTENT CORRUPTED %016llx sig %016llx != "
                           "golden %016llx",
                           (unsigned long long)it, (unsigned long long)h2,
                           (unsigned long long)sig, (unsigned long long)expect2);
                ++own_loads;
                if ((it % 3) == 2) {
                    store.release(h2);
                    resident.erase(std::remove_if(resident.begin(), resident.end(),
                                                  [&](const auto& e) { return e.first == h2; }),
                                   resident.end());
                } else if (expect2) {
                    note_resident(h2, expect2);
                }
            }
            // 3. victim scan: re-verify one RESIDENT part's bytes against golden
            if (!resident.empty()) {
                const auto [hv, expect] = resident[victim_rr++ % resident.size()];
                const viewer::LoadedPart* lpv = store.find(hv);
                if (lpv && expect) {
                    uint64_t sig = 0;
                    if (validate_loaded(store, hv, lpv, sig, "app-victim"))
                        CHECKF(sig == expect,
                               "app iter %llu: RESIDENT PART CORRUPTED %016llx sig %016llx != "
                               "golden %016llx",
                               (unsigned long long)it, (unsigned long long)hv,
                               (unsigned long long)sig, (unsigned long long)expect);
                    if (!real_cache)
                        CHECKF(lpv->expansion.size() == 1,
                               "app iter %llu: resident %016llx expansion size %zu != 1",
                               (unsigned long long)it, (unsigned long long)hv,
                               lpv->expansion.size());
                }
                if (resident.size() > 6) {
                    store.release(resident.front().first);
                    resident.erase(resident.begin());
                }
            }
            if (app_takes_lock) pump_lock.unlock();

            canary.verify("app", it);
            canary.churn(6);
            if ((it & 3u) == 0)
                CHECKF(heap_ok(), "app iter %llu: HeapValidate FAILED (heap corrupt)",
                       (unsigned long long)it);
            ++it;
            if ((it % 250) == 0) {
                std::lock_guard<std::mutex> lk(g_log_mutex);
                std::printf("  ... app iter %llu (worker %llu, commits %llu, failures %d)\n",
                            (unsigned long long)it, (unsigned long long)a_iters.load(),
                            (unsigned long long)commits.load(), g_failures.load());
                std::fflush(stdout);
            }
        }
        b_iters = it;
    }
    stop = true;
    for (std::thread& w : workers) w.join();
    while (auto osp = queue.try_pop()) { /* destroy remaining staged parts */ }

    CHECKF(heap_ok(), "final HeapValidate FAILED");
    std::printf(
        "MT phase done: worker iters=%llu, app iters=%llu (commits=%llu own_loads=%llu)\n",
        (unsigned long long)a_iters.load(), (unsigned long long)b_iters.load(),
        (unsigned long long)commits.load(), (unsigned long long)own_loads.load());
#ifdef _WIN32
    if (pageheap)
        std::printf("pageheap: %llu guarded allocations, %llu still live\n",
                    (unsigned long long)guardalloc::g_total.load(),
                    (unsigned long long)guardalloc::g_live.load());
#endif

    if (g_failures) {
        std::printf("partstore_race_tests: %d FAILURE(S) (%d identity-defect proofs, %d "
                    "stress-phase)\n",
                    g_failures.load(), proof_failures, g_failures.load() - proof_failures);
        return 1;
    }
    std::printf("partstore_race_tests: ALL PASS (no corruption detected)\n");
    return 0;
}
