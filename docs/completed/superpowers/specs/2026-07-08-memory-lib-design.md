# MemoryLib Design

**Date:** 2026-07-08
**Status:** Approved (brainstorm with Jack)
**Branch:** `feature/memory-lib`

## Motivation

The engine has reinvented the same three or four memory-management ideas per-project:

1. **Fixed-size pool** — ObjectAllocatorLib (`oa_*`), used by the spatial hashes
   (SpatialQueryLib/src/spatial_hash.c, OpenParticleSurfaceLib/src/open_particle_surface.c;
   MatterSurfaceLib carries vendored copies)
2. **Growable realloc arrays** — `MemoryPool`/`Ensure*Capacity` in MatterSurfaceLib/src/surface.c
   (1.5x growth), `ParticleIndexArray` in OpenParticleSurfaceLib (2x growth), cell storage
   (+1000 linear growth) — three hand-rolled idioms with three growth policies
3. **Scratch/reuse contexts** — `SurfaceScratch` bundling buffers reused across bakes
4. **Unmanaged std::vector churn** in MatterEngine3 C++ (temporary buffers built and thrown
   away in bake/flatten paths)

Goal: one architectural home for memory management — a small set of allocators used
everywhere going forward, instead of custom-rolled solutions scattered across the engine.

## Decisions (settled during brainstorm)

- **Adoption model: forward-only + cheap cleanups.** New code uses MemoryLib. Existing
  code migrates only where trivially cheap (the `oa_*` rename and Makefile retargeting).
  No rewriting of working pipelines. The bake path is mid-rework (Phase B) and is not
  touched.
- **v1 contents: pool + arena + growable array in C, plus a minimal C++ RAII wrapper.**
  No general-purpose heap replacement, no tagged allocations, no thread-local pools.
- **Stats included, kept simple.** Per-allocator counters, queryable; no globals or registry.
- **Thread-confined.** One allocator instance = one thread. No internal locking. Workers
  own their own arenas (matches existing `SurfaceScratch` / worker-pool usage).
- **Home: new `MemoryLib` that absorbs ObjectAllocatorLib** (via `git mv`, history preserved).
- **API style: C-first (`mem_*`), matching existing repo conventions, with a thin C++
  RAII header.** No `std::pmr`.

## Library layout

```
MemoryLib/
├── Makefile
├── README.md
├── main.c                 # demo/smoke binary (repo convention)
├── include/
│   ├── mem_stats.h        # shared MemStats struct
│   ├── mem_pool.h         # renamed object_allocator.h
│   ├── mem_arena.h
│   ├── mem_array.h
│   └── memory.hpp         # C++ RAII wrappers
├── src/
│   ├── mem_pool.c
│   ├── mem_arena.c
│   └── mem_array.c
└── tests/
    └── memory_tests.c
```

C99, libc-only (no dependencies), builds on all targets including Windows.

## Components

### Pool (`mem_pool.h`)

Mechanical rename of ObjectAllocatorLib — no behavior change:

- `MemPool* mem_pool_create(size_t objectSize, size_t objectsPerPage);`
- `void mem_pool_destroy(MemPool* pool);`
- `void* mem_pool_alloc(MemPool* pool);`
- `void mem_pool_free(MemPool* pool, void* object);`
- `void mem_pool_get_stats(MemPool* pool, MemStats* out);`

Same page-based freelist internals with page-array doubling.

### Arena (`mem_arena.h`)

Bump allocator with bulk reset — for per-bake/per-rebuild/per-frame temporaries:

- `MemArena* mem_arena_create(size_t initialCapacity);`
- `void* mem_arena_alloc(MemArena* a, size_t size);` — 8-byte aligned bump pointer;
  chains a new block at 1.5x the previous block size when full
- `void mem_arena_reset(MemArena* a);` — retains the largest block, frees the rest;
  steady state is zero mallocs per cycle
- `void mem_arena_destroy(MemArena* a);`
- `void mem_arena_get_stats(MemArena* a, MemStats* out);`

No per-allocation free — that contract is what makes it fast.

### Growable array (`mem_array.h`)

One blessed idiom replacing the hand-rolled realloc patterns:

```c
typedef struct MemArray {
    void*  data;
    size_t count;
    size_t capacity;   // in elements
    size_t elemSize;
    size_t growCount;  // number of reallocs (reported as stats totalAllocs)
} MemArray;
```

- `void mem_array_init(MemArray* arr, size_t elemSize);`
- `int  mem_array_ensure(MemArray* arr, size_t minCapacity);` — grows to
  `max(minCapacity, capacity * 3 / 2, 16)`; temp-pointer realloc safety (no data loss
  on OOM, returns 0 on failure)
- `void* mem_array_push(MemArray* arr);` — returns pointer to new element slot, NULL on OOM
- `void mem_array_clear(MemArray* arr);` — count = 0, capacity retained
- `void mem_array_free(MemArray* arr);`
- `void mem_array_get_stats(const MemArray* arr, MemStats* out);`

Heap-backed only; arena-backed arrays are an explicit non-goal for v1.

### Stats

```c
typedef struct MemStats {
    size_t liveBytes;
    size_t peakBytes;
    size_t totalAllocs;
    // type-specific extras:
    size_t pageCount;    // pool: pages; arena: blocks
    size_t totalObjects; // pool only
    size_t freeObjects;  // pool only
} MemStats;
```

One `*_get_stats()` per allocator type. No tagging, no global registry.
All C headers carry `extern "C"` guards so `memory.hpp` and C++ consumers link cleanly.

### C++ wrapper (`memory.hpp`)

Header-only, ~a page of code:

```cpp
namespace mem {
  class Arena {                        // RAII over MemArena, move-only
    explicit Arena(size_t initialCap);
    ~Arena();
    void* alloc(size_t n);
    template <typename T> T* allocArray(size_t count);  // POD only
    void reset();
    MemStats stats() const;
  };
  class Pool { /* same shape over MemPool */ };
}
```

- `allocArray<T>` enforces `static_assert(std::is_trivially_destructible_v<T>)` —
  arenas never run destructors.
- **Deliberately excluded:** a `std::allocator`/container adapter. Arena + vector's
  grow-and-free pattern is a lifetime footgun (dangling after `reset()`); the engine's
  real need is "build a temporary POD buffer, throw it away", which `allocArray` covers.
  Arena-backed containers are an additive follow-up if a genuine need appears.

## Migration (the cheap cleanups)

- `git mv ObjectAllocatorLib MemoryLib` + rename files/symbols (`oa_*` → `mem_pool_*`).
  No compatibility aliases.
- Update direct consumers' call sites and includes:
  - SpatialQueryLib: `src/spatial_hash.c`, `main.c`, `src/bvh.c` (include only)
  - OpenParticleSurfaceLib: `src/open_particle_surface.c`
- Update Makefile `-I` paths in: SpatialQueryLib, OpenParticleSurfaceLib, SurfaceLib,
  GPURayTraceExample, ParticleDynamicsExample.
- Update `build-all.sh`, README.md, CLAUDE.md references, and the shell scripts that
  mention ObjectAllocatorLib (`ParticleDynamicsExample/build.sh`,
  `OpenParticleSurfaceLib/build.sh`, `OpenParticleSurfaceLib/install_dependencies.sh`).
- **MatterSurfaceLib: explicitly untouched.** Its vendored `object_allocator.[ch]` /
  `spatial_hash.c` copies stay as-is (MSL is read-only except genuine bugs; a rename
  is not one). Scope decision, surfaced deliberately.
- **Not migrating** the growable-array idioms in surface.c / open_particle_surface.c —
  working pipelines are not rewritten for uniformity. `mem_array` is for new code.

## Testing

- `tests/memory_tests.c`, headless asserts:
  - Pool: alloc/free/reuse cycles, page growth, stats
  - Arena: alignment, block chaining, reset-retains-largest-block, zero-malloc steady
    state (via stats), stats correctness
  - Array: init/push/growth/clear/free, ensure-failure behavior, growth policy
- Wired into `make test` and `./build-all.sh test` like the other libs.
- Verification gate: full `./build-all.sh test` green (the rename touches several
  projects' builds).

## Non-goals (v1)

- Arena-backed std containers / allocator adapters
- Thread-safe (locked) allocator variants
- Allocation tagging, leak tracking, global registries
- Migrating MatterSurfaceLib or any bake-path code
- Replacing existing working realloc idioms
