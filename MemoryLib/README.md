# MemoryLib

Shared memory managers for MatterEngine2. One home for allocation patterns
instead of custom-rolled solutions per project. C99, libc-only; C++ wrappers
in `memory.hpp`.

All allocators are **thread-confined**: one instance belongs to one thread.
Workers should own their own instances.

## Allocators

### `mem_pool.h` — fixed-size object pool
Page-based freelist for objects of one size (spatial hash entries, particles).
`mem_pool_create(objectSize, objectsPerPage)` / `mem_pool_alloc` /
`mem_pool_free` / `mem_pool_destroy`.

### `mem_arena.h` — bump allocator with bulk reset
For per-bake / per-rebuild / per-frame temporaries. `mem_arena_alloc` is a
bump (8-byte aligned); there is no per-allocation free. `mem_arena_reset`
drops everything at once, retaining the largest block — steady state is zero
mallocs per cycle.

### `mem_array.h` — growable array
The one blessed realloc idiom: 1.5x growth, 16-element minimum, OOM-safe
(`mem_array_ensure` returns 0 and leaves data intact).

### `memory.hpp` — C++ RAII wrappers
`mem::Arena` and `mem::Pool` (move-only). `Arena::allocArray<T>` is for
trivially-destructible types only (compile-time enforced) — arenas never run
destructors.

## Stats

Every allocator answers `*_get_stats(alloc, MemStats* out)`:
`liveBytes`, `peakBytes`, `totalAllocs`, plus type extras (`pageCount`,
pool's `totalObjects`/`freeObjects`).

## Build & test

```bash
make            # builds the demo/self-test binary ./memorylib
make test       # ASan+UBSan test suites (C + C++)
```

## Consumers

SpatialQueryLib, SurfaceLib, OpenParticleSurfaceLib, GPURayTraceExample and
ParticleDynamicsExample compile `src/mem_pool.c` directly via
`-I../MemoryLib/include` (see their Makefiles). MatterSurfaceLib keeps its
own vendored copy by design.

## History

Formerly `ObjectAllocatorLib` (fixed-size pool only). Renamed 2026-07-08 when
arena/array/stats were added; `git log --follow` traces the old history.
