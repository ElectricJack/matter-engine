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
The one blessed realloc idiom: new capacity is `max(minCapacity, capacity*3/2, 16)` so a large `ensure` jumps straight to `minCapacity`, OOM-safe
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
make            # builds the demo/self-test binary ./build/memorylib
make test       # ASan+UBSan test suites (C + C++)
```

## Consumers

Nothing copies or symlinks these sources — consumers add
`-I../MemoryLib/include` and compile the `.c` from here, per CLAUDE.md.

- **`mem_pool.c`** — the only in-tree caller is
  `libs/SpatialQueryLib/src/spatial_hash.c`, so every project that compiles
  the spatial hash also compiles the pool: `libs/SpatialQueryLib/Makefile`,
  `MatterEngine3/Makefile` (both the kernel archive and the editor's viewer
  archive, which is how MatterEditor gets it), `MatterEngine3/tests/Makefile`
  and `libs/MatterSurfaceLib/tests/Makefile`.
  `Prototypes/GPURayTraceExample/Makefile` also compiles it, but Prototypes is
  a frozen snapshot excluded from `build-all.sh`.
- **`mem_arena.c`** — `libs/AssetStoreLib` uses the arena in its public API
  (`ReadBatch` lands blobs in a caller's arena) and compiles it in
  `tests/Makefile`. `libasset_store.a` deliberately does not contain the
  arena's object, so a consumer that already compiles `mem_arena.c` cannot get
  duplicate symbols.
- **`mem_array.c` / `memory.hpp`** — no consumers outside MemoryLib's own
  tests yet; they are here as the blessed idiom for new code.

## History

Formerly `ObjectAllocatorLib` (fixed-size pool only). Renamed 2026-07-08 when
arena/array/stats were added; `git log --follow` traces the old history.

## Fixed backing banks (`mem_bank.h`)

`MemBank` reserves its backing storage and bounded lease metadata at creation.
Acquisition rounds bytes up to the configured power-of-two quantum and returns
an aligned, generation-checked range. Capacity must be a multiple of that
quantum. Release returns the range for reuse; adjacent free space is implicitly
coalesced. Neither operation allocates or frees backing storage or metadata.
Exhaustion or fragmentation fails admission; the bank never grows. Destroying a
bank with live leases fails and leaves it intact.

Owned CPU storage is aligned and zero-touched at initialization. External
storage is borrowed; external mode with a null pointer manages offsets only,
for callers owning GPU buffers. GPU fencing and resource lifetime remain the
caller's responsibility. The C API is thread-confined; AssetStoreLib's shared
`PageBank` wrapper synchronizes cross-thread lease retirement.

The current implementation keeps a bounded sorted array of live ranges:
first-fit search and insertion/release are O(live leases), without a metadata
allocation per base quantum. This supports tiny compatibility quanta without
large bitmaps. Size-class free lists are a possible later optimization if
measured contention or range-search cost warrants them.

`mem_bank_stats` exposes committed capacity, occupied/requested bytes, largest
free range, active/peak leases, failures and backing allocation count. Payload
padding is `occupied - requested`; free space can be fragmented. These figures
exclude allocator metadata and alignment slack.

The MemoryLib test target checks alignment, exhaustion, fragmentation, stale
leases and reuse. On Linux it also runs 100,000 randomized operations with
linker-wrapped libc allocation functions, asserting zero calls during bank
acquisition/release and no overlapping live ranges.
