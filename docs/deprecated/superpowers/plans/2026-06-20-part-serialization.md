# Part Serialization (`.part` deep cache) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Serialize a fully baked part (BLAS geometry+BVH, TLAS instances, materials) to a `.part` file and load it on startup instead of regenerating, with format versioning and regenerate-on-mismatch.

**Architecture:** A new part-kind-agnostic module (`part_asset.h/.cpp`) walks the existing `BLASManager` and `TLASManager` CPU structures, writes them as raw POD arrays behind a versioned header, and reconstructs them on load — adding one new `BLASManager::register_prebuilt` path that installs a saved BVH without rebuilding it. The TLAS tree and GPU data textures are *not* serialized; they are cheaply rebuilt/re-packed on load. A loaded part is render-only.

**Tech Stack:** C++17, g++, raylib (linked, but the loader/saver are GL-free and headless-testable), the repo's hand-rolled `CHECK`-macro test pattern.

**Spec:** `docs/superpowers/specs/2026-06-20-part-serialization-design.md`

**Conventions discovered (follow exactly):**
- Headless tests live in `MatterSurfaceLib/tests/`, use `static int failures; #define CHECK(cond,msg)`, and `main()` returns `failures==0?0:1`. They never call GL (`UploadMesh`/`ensure_gpu_textures_ready`).
- Test build rules live in `MatterSurfaceLib/tests/Makefile`; C sources are compiled with `gcc` (to keep `extern "C"` symbols unmangled), C++ with `g++`.
- The app build is `MatterSurfaceLib/Makefile` with explicit `SRC`/`OBJ` lists and one rule per object.
- After struct/header changes, clean-rebuild the app (`make clean && make`) — the app Makefile has no header dependency tracking.

**Key facts the code relies on:**
- `BLASManager::generate_triangle_data` emits triangles in BVH order via `entry->triangles[ entry->bvh->triIdx[i] ]`; `generate_node_data` emits `entry->bvh->bvhNode[0..nodesUsed)` with offset-adjusted indices (`src/blas_manager.cpp:311-367`). So a faithful restore needs `entry->triangles` (original order), `entry->mesh->triEx`, `entry->mesh->triCount`, `entry->bvh->bvhNode`, `entry->bvh->nodesUsed`, `entry->bvh->triIdx`.
- `BVH(BvhMesh*)` allocates `bvhNode = MALLOC64(sizeof(BVHNode)*triCount*2+64)`, `triIdx = new uint[triCount]`, then `Build()` (`src/bvh.cpp:74-80`). We add a sibling constructor that `memcpy`s saved arrays instead of building.
- `TLASManager::draw_batch` → per instance `push_matrix; load_matrix; draw; pop_matrix`, and `build()` consumes `draw_records_`, recomputing `invTransform`/`bounds` (`src/tlas_manager.cpp:229-329`). So serializing forward transforms only is sufficient.
- `Tri` is `ALIGN(64)` with `__m128` unions; **always `memcpy` to/from file buffers** (never element-wise assignment from an unaligned buffer pointer) to avoid aligned-load faults.

---

## File Structure

- **Create** `MatterSurfaceLib/include/part_asset.h` — `PartGenParams`, format constants, `fnv1a64`/`compute_param_hash`/`cache_path`, `save`/`load` declarations.
- **Create** `MatterSurfaceLib/src/part_asset.cpp` — implementation (GL-free).
- **Create** `MatterSurfaceLib/tests/part_asset_tests.cpp` — headless round-trip + guard tests.
- **Modify** `MatterSurfaceLib/include/bvh.h` — add prebuilt `BVH` constructor declaration.
- **Modify** `MatterSurfaceLib/src/bvh.cpp` — implement prebuilt `BVH` constructor.
- **Modify** `MatterSurfaceLib/include/blas_manager.hpp` — declare `register_prebuilt`.
- **Modify** `MatterSurfaceLib/src/blas_manager.cpp` — implement `register_prebuilt`.
- **Modify** `MatterSurfaceLib/tests/Makefile` — add `part_asset_tests` target + `run-part`.
- **Modify** `MatterSurfaceLib/Makefile` — add `part_asset.cpp` to `SRC`/`OBJ` + object rule.
- **Modify** `MatterSurfaceLib/main.cpp` — extract `PartGenParams`, wire auto-cache load/save around `setup_lattice_scene()`.

---

## Task 1: Part format module skeleton — hashing + path helpers

**Files:**
- Create: `MatterSurfaceLib/include/part_asset.h`
- Create: `MatterSurfaceLib/src/part_asset.cpp`
- Create: `MatterSurfaceLib/tests/part_asset_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile`

- [ ] **Step 1: Create the header**

Create `MatterSurfaceLib/include/part_asset.h`:

```cpp
#pragma once

#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"

#include <cstddef>
#include <cstdint>
#include <string>

// Part-kind-agnostic serialization of a fully baked part (BLAS geometry + BVH,
// TLAS instances, materials). See docs/superpowers/specs/2026-06-20-part-serialization-design.md
namespace part_asset {

constexpr uint32_t kMagic = 0x50415254u;   // 'PART'
constexpr uint32_t kFormatVersion = 1u;

// Generator parameters for the brick part (the only part kind today). All fields
// are 4 bytes so the struct is padding-free and hashes deterministically by bytes.
struct PartGenParams {
    int      dimX, dimY, dimZ;
    float    spacing, baseRadius;
    float    posJitter, radiusVar, voidAmt;
    float    veinFreq, veinThresh;
    int      matOpaqueA, matOpaqueB, matGlass;
    float    simplifyRatio;
    uint32_t seed;
};
static_assert(sizeof(PartGenParams) == 60,
              "PartGenParams must be padding-free for stable byte hashing");

// FNV-1a 64-bit over a byte range.
uint64_t fnv1a64(const void* data, size_t len);

// Cache key: FNV-1a of the params XOR the format version.
uint64_t compute_param_hash(const PartGenParams& p);

// "parts/<16-hex>.part"
std::string cache_path(uint64_t hash);

// Serialize the baked managers to path (atomic temp+rename). Returns false on
// any I/O failure. GL-free.
bool save(const std::string& path, const BLASManager& blas,
          const TLASManager& tlas, uint64_t param_hash);

// Reconstruct managers from path. Returns false (caller should regenerate) on any
// header/layout/material/corruption mismatch or I/O failure. GL-free: the caller
// triggers GPU texture (re)upload via the normal render path. expected_hash must
// equal the param hash the file was written with.
bool load(const std::string& path, uint64_t expected_hash,
          BLASManager& blas, TLASManager& tlas);

} // namespace part_asset
```

- [ ] **Step 2: Create the implementation with hashing + path only**

Create `MatterSurfaceLib/src/part_asset.cpp`:

```cpp
#include "../include/part_asset.h"

#include <cstdio>

namespace part_asset {

uint64_t fnv1a64(const void* data, size_t len) {
    uint64_t h = 1469598103934665603ull;            // FNV offset basis
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t compute_param_hash(const PartGenParams& p) {
    return fnv1a64(&p, sizeof(p)) ^ static_cast<uint64_t>(kFormatVersion);
}

std::string cache_path(uint64_t hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string("parts/") + buf + ".part";
}

} // namespace part_asset
```

- [ ] **Step 3: Add the test target to the tests Makefile**

In `MatterSurfaceLib/tests/Makefile`, append after the `run-cube` rule (end of file):

```makefile
# Part serialization round-trip + format-guard tests (headless, GL-free).
# material_registry.c via gcc to keep its extern "C" symbols unmangled.
PART_TARGET = part_asset_tests
PART_CPP = part_asset_tests.cpp ../src/part_asset.cpp ../src/blas_manager.cpp \
           ../src/bvh.cpp ../src/tlas_manager.cpp ../src/vertex_ao.cpp \
           ../src/occupancy.cpp
PART_C   = ../src/material_registry.c
PART_C_OBJ = material_registry.o

$(PART_TARGET): $(PART_CPP) $(PART_C)
	gcc -c $(PART_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(PART_CPP) $(PART_C_OBJ) -o $(PART_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(PART_C_OBJ)

run-part: $(PART_TARGET)
	./$(PART_TARGET)
```

Also add `$(PART_TARGET)` to the `clean` rule's `rm -f` line, and add `run-part` to the `.PHONY` list.

- [ ] **Step 4: Write the failing test**

Create `MatterSurfaceLib/tests/part_asset_tests.cpp`:

```cpp
#include "../include/part_asset.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static part_asset::PartGenParams sample_params() {
    part_asset::PartGenParams p{};
    p.dimX = 20; p.dimY = 20; p.dimZ = 20;
    p.spacing = 0.8f; p.baseRadius = 0.62f;
    p.posJitter = 0.1f; p.radiusVar = 0.1f; p.voidAmt = 0.05f;
    p.veinFreq = 1.5f; p.veinThresh = 0.3f;
    p.matOpaqueA = 8; p.matOpaqueB = 9; p.matGlass = 4;
    p.simplifyRatio = 0.65f; p.seed = 1234u;
    return p;
}

int main() {
    using namespace part_asset;

    // fnv1a64 is deterministic and order-sensitive.
    const char* a = "hello"; const char* b = "hellp";
    CHECK(fnv1a64(a, 5) == fnv1a64(a, 5), "fnv deterministic");
    CHECK(fnv1a64(a, 5) != fnv1a64(b, 5), "fnv distinguishes input");

    // compute_param_hash: same params -> same hash; changed field -> different.
    PartGenParams p1 = sample_params();
    PartGenParams p2 = sample_params();
    CHECK(compute_param_hash(p1) == compute_param_hash(p2), "same params same hash");
    p2.seed = 9999u;
    CHECK(compute_param_hash(p1) != compute_param_hash(p2), "seed change rehashes");
    p2 = sample_params(); p2.simplifyRatio = 0.5f;
    CHECK(compute_param_hash(p1) != compute_param_hash(p2), "ratio change rehashes");

    // cache_path format.
    CHECK(cache_path(0x1ull) == "parts/0000000000000001.part", "cache_path zero-padded hex");

    if (failures == 0) printf("All part_asset tests passed\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 5: Run the test to verify it builds and passes**

Run: `cd MatterSurfaceLib/tests && make run-part`
Expected: compiles, prints `All part_asset tests passed`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/part_asset.h MatterSurfaceLib/src/part_asset.cpp \
        MatterSurfaceLib/tests/part_asset_tests.cpp MatterSurfaceLib/tests/Makefile
git commit -m "feat: part_asset module skeleton (params hash, cache path) + tests"
```

---

## Task 2: `register_prebuilt` — install a saved BVH without rebuilding

**Files:**
- Modify: `MatterSurfaceLib/include/bvh.h:91` (BVH constructors area)
- Modify: `MatterSurfaceLib/src/bvh.cpp:74-80` (after `BVH(BvhMesh*)`)
- Modify: `MatterSurfaceLib/include/blas_manager.hpp:106` (after the `register_triangles` overloads)
- Modify: `MatterSurfaceLib/src/blas_manager.cpp:199` (after `register_triangles`)
- Test: `MatterSurfaceLib/tests/part_asset_tests.cpp`

- [ ] **Step 1: Write the failing parity test**

Add this function above `main()` in `tests/part_asset_tests.cpp`, and call `test_prebuilt_parity();` as the first line inside `main()`:

```cpp
#include "../include/blas_manager.hpp"

static Tri ptri(float ox, float oy) {
    Tri t;
    t.vertex0 = make_float3(ox + 0.0f, oy + 0.0f, 0.0f);
    t.vertex1 = make_float3(ox + 1.0f, oy + 0.0f, 0.0f);
    t.vertex2 = make_float3(ox + 0.0f, oy + 1.0f, 0.0f);
    t.centroid = make_float3(ox + 0.333f, oy + 0.333f, 0.0f);
    return t;
}

static void test_prebuilt_parity() {
    // Build geometry the normal way (builds a BVH).
    BLASManager built;
    Tri tris[3] = { ptri(0,0), ptri(5,0), ptri(0,5) };
    TriEx ex[3] = {};
    for (int i = 0; i < 3; ++i) {
        ex[i].materialId = 8;
        ex[i].N0 = ex[i].N1 = ex[i].N2 = make_float3(0,0,1);
        ex[i].tint = make_float4(1,1,1,0);
    }
    BLASHandle h = built.register_triangles(tris, 3, ex);
    CHECK(h != INVALID_BLAS_HANDLE, "built register ok");

    const BLASManager::BLASEntry* e = built.get_entry(h);
    CHECK(e != nullptr, "built entry exists");

    // Re-register the SAME baked arrays via register_prebuilt (no BVH build).
    BLASManager prebuilt;
    BLASHandle h2 = prebuilt.register_prebuilt(
        e->triangles.data(), e->mesh->triEx, (int)e->triangles.size(),
        e->bvh->bvhNode, e->bvh->nodesUsed, e->bvh->triIdx,
        e->hash, e->ref_count);
    CHECK(h2 != INVALID_BLAS_HANDLE, "prebuilt register ok");

    // The GPU-facing CPU data must be byte-identical between the two paths.
    std::vector<Tri> ta, tb;
    built.generate_triangle_data(ta);
    prebuilt.generate_triangle_data(tb);
    CHECK(ta.size() == tb.size() && ta.size() == 3, "prebuilt triangle count matches");
    CHECK(ta.size() == tb.size() &&
          memcmp(ta.data(), tb.data(), ta.size()*sizeof(Tri)) == 0,
          "prebuilt triangle bytes match built");

    std::vector<LegacyBVHNode> na, nb;
    built.generate_node_data(na);
    prebuilt.generate_node_data(nb);
    CHECK(na.size() == nb.size(), "prebuilt node count matches");
    CHECK(na.size() == nb.size() &&
          memcmp(na.data(), nb.data(), na.size()*sizeof(LegacyBVHNode)) == 0,
          "prebuilt node bytes match built");
}
```

Add `#include <cstring>` and `#include <vector>` near the top of the test file.

- [ ] **Step 2: Run to verify it fails to compile**

Run: `cd MatterSurfaceLib/tests && make run-part`
Expected: FAIL — `'register_prebuilt' is not a member of 'BLASManager'`.

- [ ] **Step 3: Add the prebuilt BVH constructor declaration**

In `MatterSurfaceLib/include/bvh.h`, in the `public:` section of `class BVH` right after `BVH( BvhMesh* mesh );` (line 92), add:

```cpp
	// Install a previously-built BVH (from disk) without rebuilding. nodes/triIdx
	// are copied; nodes_used is the live node count. mesh must outlive this BVH.
	BVH( BvhMesh* mesh, const BVHNode* nodes, uint nodes_used, const uint* tri_idx );
```

- [ ] **Step 4: Implement the prebuilt BVH constructor**

In `MatterSurfaceLib/src/bvh.cpp`, immediately after the `BVH::BVH( BvhMesh* triMesh )` body (after line 80), add:

```cpp
BVH::BVH( BvhMesh* triMesh, const BVHNode* nodes, uint nodes_used, const uint* tri_idx )
{
	mesh = triMesh;
	// Same allocation shape as the building constructor so all consumers agree.
	bvhNode = (BVHNode*)MALLOC64( sizeof( BVHNode ) * mesh->triCount * 2 + 64 );
	triIdx = new uint[mesh->triCount];
	nodesUsed = nodes_used;
	memcpy( bvhNode, nodes, sizeof( BVHNode ) * nodes_used );
	memcpy( triIdx, tri_idx, sizeof( uint ) * mesh->triCount );
}
```

Ensure `#include <cstring>` is present at the top of `src/bvh.cpp` (add it if missing).

- [ ] **Step 5: Declare `register_prebuilt`**

In `MatterSurfaceLib/include/blas_manager.hpp`, after the last `register_triangles` overload (after line 110), add:

```cpp
    // Register a fully baked BLAS loaded from disk: installs the saved BVH arrays
    // directly (no BVH build, no dedup lookup). Used by part_asset::load. tris,
    // triex (may be null), nodes, and tri_idx are copied; the entry takes the
    // provided hash and ref_count.
    BLASHandle register_prebuilt(const Tri* tris, const TriEx* triex, int tri_count,
                                 const BVHNode* nodes, uint nodes_used, const uint* tri_idx,
                                 uint32_t hash, uint32_t ref_count);
```

- [ ] **Step 6: Implement `register_prebuilt`**

In `MatterSurfaceLib/src/blas_manager.cpp`, after the closing brace of `register_triangles(Tri*, int, const TriEx*)` (after line 199), add:

```cpp
BLASHandle BLASManager::register_prebuilt(const Tri* tris, const TriEx* triex, int tri_count,
                                          const BVHNode* nodes, uint nodes_used, const uint* tri_idx,
                                          uint32_t hash, uint32_t ref_count) {
    if (!tris || tri_count <= 0 || !nodes || nodes_used == 0 || !tri_idx) {
        return INVALID_BLAS_HANDLE;
    }

    // Copy triangles (memcpy: Tri is __m128-aligned and the source may be an
    // unaligned file buffer). std::vector range-construct is a memmove for the
    // trivially-copyable Tri, which is alignment-safe.
    std::vector<Tri> triangle_copy(tris, tris + tri_count);

    auto mesh = std::make_unique<BvhMesh>();
    mesh->triCount = tri_count;
    mesh->tri = static_cast<Tri*>(MALLOC64(tri_count * sizeof(Tri)));
    std::memcpy(mesh->tri, tris, tri_count * sizeof(Tri));
    if (triex) {
        mesh->triEx = static_cast<TriEx*>(MALLOC64(tri_count * sizeof(TriEx)));
        std::memcpy(mesh->triEx, triex, tri_count * sizeof(TriEx));
    }

    auto bvh = std::make_unique<BVH>(mesh.get(), nodes, nodes_used, tri_idx);

    BLASHandle handle = next_handle_++;
    auto entry = std::make_unique<BLASEntry>(handle, std::move(mesh), std::move(bvh),
                                             std::move(triangle_copy), hash);
    entry->ref_count = ref_count;

    size_t entry_index = entries_.size();
    hash_to_entry_.emplace(hash, entry_index);
    entries_.push_back(std::move(entry));

    mark_dirty();
    return handle;
}
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-part`
Expected: PASS — `All part_asset tests passed`.

- [ ] **Step 8: Commit**

```bash
git add MatterSurfaceLib/include/bvh.h MatterSurfaceLib/src/bvh.cpp \
        MatterSurfaceLib/include/blas_manager.hpp MatterSurfaceLib/src/blas_manager.cpp \
        MatterSurfaceLib/tests/part_asset_tests.cpp
git commit -m "feat: BLASManager::register_prebuilt installs saved BVH without rebuild"
```

---

## Task 3: `save` — serialize managers to a `.part` file

**Files:**
- Modify: `MatterSurfaceLib/src/part_asset.cpp`
- Test: `MatterSurfaceLib/tests/part_asset_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/part_asset_tests.cpp` (function above `main`, and call `test_save_header();` in `main`):

```cpp
#include "../include/tlas_manager.hpp"
#include <cstdint>

// Builds a tiny baked scene: 2 BLAS, 3 instances.
static void build_scene(BLASManager& blas, TLASManager& tlas,
                        BLASHandle& hA, BLASHandle& hB) {
    Tri triA[3] = { ptri(0,0), ptri(5,0), ptri(0,5) };
    Tri triB[2] = { ptri(20,0), ptri(25,5) };
    TriEx exA[3] = {}; TriEx exB[2] = {};
    for (auto& e : exA) { e.materialId = 8; e.N0=e.N1=e.N2=make_float3(0,0,1); e.tint=make_float4(1,1,1,0); }
    for (auto& e : exB) { e.materialId = 9; e.N0=e.N1=e.N2=make_float3(0,0,1); e.tint=make_float4(1,1,1,0); }
    hA = blas.register_triangles(triA, 3, exA);
    hB = blas.register_triangles(triB, 2, exB);

    std::vector<TLASManager::DrawInstance> insts(3);
    insts[0].blas_handle = hA; insts[0].material_id = 8; insts[0].transform = Matrix4x4();
    insts[1].blas_handle = hB; insts[1].material_id = 9; insts[1].transform = Matrix4x4();
    insts[1].transform.m[3] = 10.0f; // translate x
    insts[2].blas_handle = hA; insts[2].material_id = 8; insts[2].transform = Matrix4x4();
    insts[2].transform.m[7] = 7.0f;  // translate y
    tlas.draw_batch(insts);
    tlas.build(blas);
}

static uint32_t rd_u32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v; memcpy(&v, b.data()+off, 4); return v;
}
static uint64_t rd_u64(const std::vector<uint8_t>& b, size_t off) {
    uint64_t v; memcpy(&v, b.data()+off, 8); return v;
}
static std::vector<uint8_t> read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f,0,SEEK_END); long n = ftell(f); fseek(f,0,SEEK_SET);
    std::vector<uint8_t> b(n);
    size_t got = fread(b.data(),1,n,f); fclose(f);
    b.resize(got);
    return b;
}

static void test_save_header() {
    using namespace part_asset;
    BLASManager blas; TLASManager tlas(64);
    BLASHandle hA, hB; build_scene(blas, tlas, hA, hB);

    const char* path = "test_save.part";
    remove(path);
    bool ok = save(path, blas, tlas, 0xABCDEF12u);
    CHECK(ok, "save returns true");

    std::vector<uint8_t> b = read_file(path);
    CHECK(b.size() >= 36, "file has at least a header");
    CHECK(rd_u32(b, 0) == kMagic, "magic written");
    CHECK(rd_u32(b, 4) == kFormatVersion, "version written");
    CHECK(rd_u64(b, 8) == 0xABCDEF12ull, "param hash written");
    CHECK(rd_u32(b, 16) == (uint32_t)sizeof(Tri), "sizeof Tri written");
    CHECK(rd_u32(b, 20) == (uint32_t)sizeof(TriEx), "sizeof TriEx written");
    CHECK(rd_u32(b, 24) == (uint32_t)sizeof(BVHNode), "sizeof BVHNode written");
    // content hash covers the body (everything after the 36-byte header).
    uint64_t stored = rd_u64(b, 28);
    uint64_t recomputed = fnv1a64(b.data()+36, b.size()-36);
    CHECK(stored == recomputed, "content hash covers body");

    remove(path);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-part`
Expected: FAIL — link error `undefined reference to part_asset::save`.

- [ ] **Step 3: Implement `save` (with serialization helpers)**

In `MatterSurfaceLib/src/part_asset.cpp`, add includes and an anonymous-namespace writer helper at the top (below the existing `#include`s):

```cpp
#include <cstring>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <sys/stat.h>

namespace {
template <class T>
void put(std::vector<uint8_t>& b, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
void put_bytes(std::vector<uint8_t>& b, const void* d, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(d);
    b.insert(b.end(), p, p + n);
}
void ensure_parent_dir(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return;
    mkdir(path.substr(0, pos).c_str(), 0755); // ignore EEXIST
}
} // namespace
```

Then add the `save` function inside `namespace part_asset { ... }`:

```cpp
bool save(const std::string& path, const BLASManager& blas,
          const TLASManager& tlas, uint64_t param_hash) {
    std::vector<uint8_t> body;

    // --- Materials ---
    const uint32_t mcount = static_cast<uint32_t>(MaterialRegistryCount());
    put<uint32_t>(body, mcount);
    for (uint32_t i = 0; i < mcount; ++i)
        put_bytes(body, MaterialRegistryGet(static_cast<int>(i)), sizeof(MaterialDef));

    // --- BLAS table (index == position in entries_) ---
    const auto& entries = blas.get_entries();
    put<uint32_t>(body, static_cast<uint32_t>(entries.size()));
    std::unordered_map<BLASHandle, uint32_t> handle_to_index;
    for (uint32_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        handle_to_index[e->handle] = i;
        const uint32_t tri_count  = static_cast<uint32_t>(e->mesh->triCount);
        const uint32_t nodes_used = e->bvh->nodesUsed;
        const uint32_t has_triex  = e->mesh->triEx ? 1u : 0u;
        put<uint32_t>(body, e->hash);
        put<uint32_t>(body, e->ref_count);
        put<uint32_t>(body, tri_count);
        put<uint32_t>(body, nodes_used);
        put<uint32_t>(body, has_triex);
        put_bytes(body, e->triangles.data(), tri_count * sizeof(Tri));
        if (has_triex) put_bytes(body, e->mesh->triEx, tri_count * sizeof(TriEx));
        put_bytes(body, e->bvh->bvhNode, nodes_used * sizeof(BVHNode));
        put_bytes(body, e->bvh->triIdx,  tri_count  * sizeof(uint));
    }

    // --- Instances ---
    const auto& recs = tlas.get_draw_records();
    put<uint32_t>(body, static_cast<uint32_t>(recs.size()));
    for (const auto& r : recs) {
        auto it = handle_to_index.find(r.blas_handle);
        const uint32_t blas_index = (it == handle_to_index.end()) ? 0u : it->second;
        put<uint32_t>(body, blas_index);
        put<uint32_t>(body, r.material_id);
        put_bytes(body, r.transform.m, 16 * sizeof(float));
    }

    // --- Header (36 bytes) ---
    const uint64_t content_hash = fnv1a64(body.data(), body.size());
    std::vector<uint8_t> head;
    put<uint32_t>(head, kMagic);
    put<uint32_t>(head, kFormatVersion);
    put<uint64_t>(head, param_hash);
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(Tri)));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(TriEx)));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(BVHNode)));
    put<uint64_t>(head, content_hash);

    // --- Atomic write ---
    ensure_parent_dir(path);
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
              std::fwrite(body.data(), 1, body.size(), f) == body.size();
    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-part`
Expected: PASS — `All part_asset tests passed`.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/part_asset.cpp MatterSurfaceLib/tests/part_asset_tests.cpp
git commit -m "feat: part_asset::save serializes baked BLAS/TLAS/materials"
```

---

## Task 4: `load` — deserialize and reconstruct managers (full round-trip)

**Files:**
- Modify: `MatterSurfaceLib/src/part_asset.cpp`
- Test: `MatterSurfaceLib/tests/part_asset_tests.cpp`

- [ ] **Step 1: Write the failing round-trip test**

Add to `tests/part_asset_tests.cpp` (function above `main`, and call `test_round_trip();` in `main`):

```cpp
static void test_round_trip() {
    using namespace part_asset;

    // Source scene.
    BLASManager blasA; TLASManager tlasA(64);
    BLASHandle hA, hB; build_scene(blasA, tlasA, hA, hB);

    std::vector<Tri> triA; blasA.generate_triangle_data(triA);
    std::vector<LegacyBVHNode> nodeA; blasA.generate_node_data(nodeA);
    const auto recsA = tlasA.get_draw_records();

    const char* path = "test_round.part";
    remove(path);
    CHECK(save(path, blasA, tlasA, 0x55AA55AAu), "round-trip save ok");

    // Load into fresh managers.
    BLASManager blasB; TLASManager tlasB(64);
    bool ok = load(path, 0x55AA55AAu, blasB, tlasB);
    CHECK(ok, "round-trip load ok");

    // BLAS CPU data byte-identical.
    std::vector<Tri> triB; blasB.generate_triangle_data(triB);
    std::vector<LegacyBVHNode> nodeB; blasB.generate_node_data(nodeB);
    CHECK(triA.size() == triB.size(), "round-trip triangle count");
    CHECK(triA.size() == triB.size() &&
          memcmp(triA.data(), triB.data(), triA.size()*sizeof(Tri)) == 0,
          "round-trip triangle bytes");
    CHECK(nodeA.size() == nodeB.size(), "round-trip node count");
    CHECK(nodeA.size() == nodeB.size() &&
          memcmp(nodeA.data(), nodeB.data(), nodeA.size()*sizeof(LegacyBVHNode)) == 0,
          "round-trip node bytes");

    // Instances: same count, material ids, and transforms (handles may differ).
    const auto recsB = tlasB.get_draw_records();
    CHECK(recsA.size() == recsB.size() && recsB.size() == 3, "round-trip instance count");
    bool inst_ok = recsA.size() == recsB.size();
    for (size_t i = 0; inst_ok && i < recsA.size(); ++i) {
        if (recsA[i].material_id != recsB[i].material_id) inst_ok = false;
        if (memcmp(recsA[i].transform.m, recsB[i].transform.m, 16*sizeof(float)) != 0) inst_ok = false;
    }
    CHECK(inst_ok, "round-trip instance material+transform");

    // Wrong expected hash must be rejected.
    BLASManager blasC; TLASManager tlasC(64);
    CHECK(!load(path, 0xDEADBEEFu, blasC, tlasC), "load rejects wrong param hash");

    remove(path);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-part`
Expected: FAIL — link error `undefined reference to part_asset::load`.

- [ ] **Step 3: Implement `load`**

In `MatterSurfaceLib/src/part_asset.cpp`, add a reader helper to the existing anonymous namespace (after `ensure_parent_dir`):

```cpp
namespace {
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (p + sizeof(T) > end) { ok = false; return v; }
        std::memcpy(&v, p, sizeof(T)); p += sizeof(T);
        return v;
    }
    const uint8_t* take(size_t n) {
        if (p + n > end) { ok = false; return nullptr; }
        const uint8_t* r = p; p += n; return r;
    }
};
} // namespace
```

Then add `load` inside `namespace part_asset { ... }`:

```cpp
bool load(const std::string& path, uint64_t expected_hash,
          BLASManager& blas, TLASManager& tlas) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 36) { std::fclose(f); return false; }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    bool read_ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!read_ok) return false;

    Reader r{ buf.data(), buf.data() + buf.size() };

    // --- Header + validation ---
    const uint32_t magic    = r.get<uint32_t>();
    const uint32_t version  = r.get<uint32_t>();
    const uint64_t phash    = r.get<uint64_t>();
    const uint32_t s_tri    = r.get<uint32_t>();
    const uint32_t s_triex  = r.get<uint32_t>();
    const uint32_t s_node   = r.get<uint32_t>();
    const uint64_t content  = r.get<uint64_t>();
    if (!r.ok) return false;
    if (magic != kMagic) return false;
    if (version != kFormatVersion) return false;
    if (s_tri   != sizeof(Tri))     return false;
    if (s_triex != sizeof(TriEx))   return false;
    if (s_node  != sizeof(BVHNode)) return false;
    if (phash   != expected_hash)   return false;
    if (fnv1a64(r.p, static_cast<size_t>(r.end - r.p)) != content) return false;

    // --- Materials (validate against the live code-defined registry) ---
    const uint32_t mcount = r.get<uint32_t>();
    if (!r.ok) return false;
    if (static_cast<int>(mcount) != MaterialRegistryCount()) return false;
    for (uint32_t i = 0; i < mcount; ++i) {
        const uint8_t* md = r.take(sizeof(MaterialDef));
        if (!r.ok) return false;
        if (std::memcmp(md, MaterialRegistryGet(static_cast<int>(i)), sizeof(MaterialDef)) != 0)
            return false;
    }

    // --- BLAS table ---
    const uint32_t blas_count = r.get<uint32_t>();
    if (!r.ok) return false;
    std::vector<BLASHandle> handles(blas_count, INVALID_BLAS_HANDLE);
    for (uint32_t i = 0; i < blas_count; ++i) {
        const uint32_t hash       = r.get<uint32_t>();
        const uint32_t ref_count  = r.get<uint32_t>();
        const uint32_t tri_count  = r.get<uint32_t>();
        const uint32_t nodes_used = r.get<uint32_t>();
        const uint32_t has_triex  = r.get<uint32_t>();
        if (!r.ok) return false;
        const Tri*    tris  = reinterpret_cast<const Tri*>(r.take(tri_count * sizeof(Tri)));
        const TriEx*  triex = has_triex
                              ? reinterpret_cast<const TriEx*>(r.take(tri_count * sizeof(TriEx)))
                              : nullptr;
        const BVHNode* nodes  = reinterpret_cast<const BVHNode*>(r.take(nodes_used * sizeof(BVHNode)));
        const uint*    triIdx = reinterpret_cast<const uint*>(r.take(tri_count * sizeof(uint)));
        if (!r.ok) return false;
        handles[i] = blas.register_prebuilt(tris, triex, static_cast<int>(tri_count),
                                            nodes, nodes_used, triIdx, hash, ref_count);
    }

    // --- Instances ---
    const uint32_t inst_count = r.get<uint32_t>();
    if (!r.ok) return false;
    std::vector<TLASManager::DrawInstance> insts;
    insts.reserve(inst_count);
    for (uint32_t i = 0; i < inst_count; ++i) {
        const uint32_t blas_index = r.get<uint32_t>();
        const uint32_t material   = r.get<uint32_t>();
        const uint8_t* tf         = r.take(16 * sizeof(float));
        if (!r.ok) return false;
        if (blas_index >= blas_count) return false;
        TLASManager::DrawInstance di;
        di.blas_handle = handles[blas_index];
        di.material_id = material;
        std::memcpy(di.transform.m, tf, 16 * sizeof(float));
        insts.push_back(di);
    }

    if (!insts.empty()) {
        tlas.draw_batch(insts);
        tlas.build(blas);
    }
    return true;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-part`
Expected: PASS — `All part_asset tests passed`.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/part_asset.cpp MatterSurfaceLib/tests/part_asset_tests.cpp
git commit -m "feat: part_asset::load reconstructs managers + full round-trip test"
```

---

## Task 5: Format guards — layout + corruption rejection

**Files:**
- Test: `MatterSurfaceLib/tests/part_asset_tests.cpp`

- [ ] **Step 1: Write the failing guard test**

Add to `tests/part_asset_tests.cpp` (function above `main`, and call `test_guards();` in `main`). It uses the `read_file`/`rd_u32` helpers from Task 3:

```cpp
static void write_file(const char* path, const std::vector<uint8_t>& b) {
    FILE* f = fopen(path, "wb"); fwrite(b.data(),1,b.size(),f); fclose(f);
}

static void test_guards() {
    using namespace part_asset;
    BLASManager blasA; TLASManager tlasA(64);
    BLASHandle hA, hB; build_scene(blasA, tlasA, hA, hB);

    const char* path = "test_guard.part";
    remove(path);
    CHECK(save(path, blasA, tlasA, 0x1234u), "guard save ok");
    std::vector<uint8_t> good = read_file(path);

    // Sanity: the unmodified file loads.
    { BLASManager b; TLASManager t(64);
      CHECK(load(path, 0x1234u, b, t), "unmodified file loads"); }

    // Layout guard: corrupt sizeof_Tri (offset 16).
    { auto bad = good; uint32_t v = rd_u32(bad,16) + 1; memcpy(bad.data()+16,&v,4);
      write_file(path, bad);
      BLASManager b; TLASManager t(64);
      CHECK(!load(path, 0x1234u, b, t), "rejects sizeof_Tri mismatch"); }

    // Version guard: bump format_version (offset 4).
    { auto bad = good; uint32_t v = rd_u32(bad,4) + 1; memcpy(bad.data()+4,&v,4);
      write_file(path, bad);
      BLASManager b; TLASManager t(64);
      CHECK(!load(path, 0x1234u, b, t), "rejects version mismatch"); }

    // Corruption guard: flip a byte in the body (offset 40, inside materials).
    { auto bad = good; bad[40] ^= 0xFF;
      write_file(path, bad);
      BLASManager b; TLASManager t(64);
      CHECK(!load(path, 0x1234u, b, t), "rejects body corruption"); }

    // Magic guard.
    { auto bad = good; bad[0] ^= 0xFF;
      write_file(path, bad);
      BLASManager b; TLASManager t(64);
      CHECK(!load(path, 0x1234u, b, t), "rejects bad magic"); }

    remove(path);
}
```

- [ ] **Step 2: Run the test**

Run: `cd MatterSurfaceLib/tests && make run-part`
Expected: PASS — `All part_asset tests passed`. (These guards are already implemented in Task 4's `load`; this task proves them. If any CHECK fails, fix `load`'s validation rather than the test.)

- [ ] **Step 3: Commit**

```bash
git add MatterSurfaceLib/tests/part_asset_tests.cpp
git commit -m "test: part_asset format layout/version/corruption guards"
```

---

## Task 6: Wire `part_asset.cpp` into the app build

**Files:**
- Modify: `MatterSurfaceLib/Makefile:129` (SRC), `:130` (OBJ), and object-rules region (~`:234`)

- [ ] **Step 1: Add the source to `SRC`**

In `MatterSurfaceLib/Makefile` line 129, add `src/part_asset.cpp` to the `SRC =` list (e.g. right after `src/tlas_manager.cpp`).

- [ ] **Step 2: Add the object to `OBJ`**

In line 130, add `$(OBJ_DIR)/part_asset.o` to the `OBJ =` list (right after `$(OBJ_DIR)/tlas_manager.o`).

- [ ] **Step 3: Add the object build rule**

After the `tlas_manager.o` rule (line 234-235), add:

```makefile
$(OBJ_DIR)/part_asset.o: src/part_asset.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@
```

- [ ] **Step 4: Verify the app compiles and links (no integration yet)**

Run: `cd MatterSurfaceLib && make clean && make 2>&1 | tail -20`
Expected: build succeeds, produces the app binary, no errors referencing `part_asset`.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/Makefile
git commit -m "build: compile part_asset.cpp into MatterSurfaceLib app"
```

---

## Task 7: Auto-cache integration in `main.cpp`

**Files:**
- Modify: `MatterSurfaceLib/main.cpp` (include; constructor init flow ~`:328`; `setup_lattice_scene` params ~`:579-597`)

Context: The app class constructs `blas_manager_`, `tlas_manager_`, `test_cluster_` (`main.cpp:301-304`) then calls `setup_lattice_scene()` (`:328`). `setup_lattice_scene` declares lattice tunables as locals (`:579-597`) and populates the managers via `test_cluster_`. We extract those tunables into a `PartGenParams` helper, then make startup try the cache before generating.

- [ ] **Step 1: Include the module**

Near the other includes at the top of `main.cpp` (after line 28 `#include "include/tlas_manager.hpp"`), add:

```cpp
#include "include/part_asset.h"
```

- [ ] **Step 2: Add a gen-params helper**

Inside the app class (near `setup_lattice_scene`), add a method returning the brick params. Use the SAME literal values currently hard-coded in `setup_lattice_scene` (`main.cpp:579-597`) — open that range and copy each value across:

```cpp
    part_asset::PartGenParams brick_gen_params() const {
        part_asset::PartGenParams p{};
        p.dimX = 20; p.dimY = 20; p.dimZ = 20;     // DIM_X/Y/Z
        p.spacing = 0.8f;                           // SPACING
        p.baseRadius = 0.62f;                       // BASE_RADIUS
        p.posJitter = /* POS_JITTER literal */ 0.0f;
        p.radiusVar = /* RADIUS_VAR literal */ 0.0f;
        p.voidAmt   = /* VOID_AMT literal   */ 0.0f;
        p.veinFreq  = /* vein freq literal  */ 0.0f;
        p.veinThresh= /* vein thresh literal*/ 0.0f;
        p.matOpaqueA = 8; p.matOpaqueB = 9; p.matGlass = 4; // MAT_OPAQUE_A/B, MAT_GLASS
        p.simplifyRatio = 0.65f;                    // set_simplification_ratio(0.65f)
        p.seed = /* RNG seed literal */ 0u;
        return p;
    }
```

Replace each `/* ... literal */` with the actual constant read from `main.cpp:579-597` and the simplification/seed call sites. Keep `setup_lattice_scene`'s own locals as-is for now (Step 4 makes them consistent).

- [ ] **Step 3: Make startup try the cache before generating**

At the `setup_lattice_scene();` call site in the constructor (`main.cpp:328`), replace that single line with:

```cpp
        {
            part_asset::PartGenParams gp = brick_gen_params();
            uint64_t h = part_asset::compute_param_hash(gp);
            std::string part_path = part_asset::cache_path(h);
            if (part_asset::load(part_path, h, *blas_manager_, *tlas_manager_)) {
                printf("Loaded part from cache: %s (render-only)\n", part_path.c_str());
            } else {
                setup_lattice_scene();
                if (part_asset::save(part_path, *blas_manager_, *tlas_manager_, h))
                    printf("Saved part to cache: %s\n", part_path.c_str());
                else
                    printf("WARNING: failed to save part cache: %s\n", part_path.c_str());
            }
        }
```

Ensure `#include <string>` is available (it is via other headers; add if the compiler complains).

- [ ] **Step 4: Make `setup_lattice_scene` use the shared params (consistency)**

In `setup_lattice_scene` (`main.cpp:579-597`), replace the hard-coded local tunables with values pulled from `brick_gen_params()` so generation and the cache key can never diverge. Immediately after the function's opening brace add:

```cpp
        const part_asset::PartGenParams gp = brick_gen_params();
        const int   DIM_X = gp.dimX, DIM_Y = gp.dimY, DIM_Z = gp.dimZ;
        const float SPACING = gp.spacing;
        const float BASE_RADIUS = gp.baseRadius;
        const float POS_JITTER = gp.posJitter;
        const float RADIUS_VAR = gp.radiusVar;
        const float VOID_AMT = gp.voidAmt;
        const int   MAT_OPAQUE_A = gp.matOpaqueA, MAT_OPAQUE_B = gp.matOpaqueB, MAT_GLASS = gp.matGlass;
```

Then delete the now-duplicate original `const` declarations of those same names in `:579-597` (keep any locals that are NOT part of `PartGenParams`). Map vein/seed/simplification usages to `gp.veinFreq`, `gp.veinThresh`, `gp.seed`, and `gp.simplifyRatio` at their existing call sites (e.g. replace the literal in `set_simplification_ratio(0.65f)` at `main.cpp:746` with `set_simplification_ratio(gp.simplifyRatio)`).

- [ ] **Step 5: Clean-rebuild the app**

Headers changed (`bvh.h`, `blas_manager.hpp`) and the app Makefile has no header dependency tracking, so a clean rebuild is required.

Run: `cd MatterSurfaceLib && make clean && make 2>&1 | tail -20`
Expected: build succeeds with no errors.

- [ ] **Step 6: Verify the cache end-to-end (manual run — GUI)**

This step renders, so the **user** runs it (the harness reaps backgrounded GUI children). Ask the user to:

1. Delete any stale cache: `rm -rf MatterSurfaceLib/parts`
2. First run (generates + saves): launch the app and confirm the console prints `Saved part to cache: parts/<hash>.part`, and `MatterSurfaceLib/parts/<hash>.part` now exists. Note the rendered brick.
3. Second run (loads): relaunch and confirm the console prints `Loaded part from cache: ... (render-only)`, startup is noticeably faster (no meshing), and the rendered brick looks identical to the first run.

Suggested launch (user types in the prompt with the `!` prefix):
`! cd "MatterSurfaceLib" && ./<app-binary>`  (use the actual `$(BIN)` name from the Makefile).

Expected: second launch loads from cache and renders the same brick. If the render differs or is empty, do NOT mark complete — investigate (likely a triEx/instance/material mismatch in save/load) before proceeding.

- [ ] **Step 7: Commit**

```bash
git add MatterSurfaceLib/main.cpp
git commit -m "feat: auto-cache parts to disk; load on startup instead of regenerating"
```

---

## Self-Review

**Spec coverage:**
- §1 Code layout (part_asset in MatterSurfaceLib include/src) → Task 1, Task 6.
- §2 On-disk format (header, materials, BLAS table, instances) → Task 3 (write), Task 4 (read). Note: a per-BLAS `has_triex u32` field was added vs. the spec's unconditional `TriEx[tri_count]`, to losslessly handle a null `mesh->triEx`; everything else matches §2 field-for-field. **Spec should be updated to mention `has_triex`** (minor, additive to the section description; does not change the versioning story).
- §2 validation (magic/version/sizeof/content_hash/param_hash) → Task 4 `load`, Task 5 guards.
- §3 cache key (PartGenParams + FNV ^ version, startup flow) → Task 1 (hash), Task 7 (flow).
- §4 register_prebuilt / TLAS rebuilt / textures re-packed → Task 2 (prebuilt), Task 4 (`draw_batch`+`build`), Task 7 (GPU upload via normal render path; `load` is GL-free per the conventions note).
- §5 render-only → Task 7 (load path skips `setup_lattice_scene`; console says "render-only").
- §6 save/load walkthrough → Tasks 3, 4.
- §7 testing (round-trip, layout guard, corruption guard, prebuilt-vs-built parity) → Tasks 2, 4, 5.
- Versioning section → enforced by Task 4's strict `version != kFormatVersion` rejection.

**Placeholder scan:** The only intentional fill-ins are the literal values in Task 7 Step 2/4, which must be read from `main.cpp:579-597` at implementation time (they are project-specific constants that live in code, not the plan). Every code block elsewhere is complete.

**Type consistency:** `register_prebuilt`, the prebuilt `BVH(BvhMesh*, const BVHNode*, uint, const uint*)` ctor, `PartGenParams` (15 fields, 60 bytes), `fnv1a64`/`compute_param_hash`/`cache_path`/`save`/`load` signatures, and `TLASManager::DrawInstance{blas_handle,transform,material_id}` / `Matrix4x4::m[16]` / `DrawRecord{material_id,transform}` usages are consistent across all tasks and match the current headers (`bvh.h`, `blas_manager.hpp`, `tlas_manager.hpp`).
