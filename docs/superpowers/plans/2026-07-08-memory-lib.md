# MemoryLib Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expand ObjectAllocatorLib into MemoryLib — a shared C memory-management library (pool + arena + growable array, each with stats) with a C++ RAII wrapper — and migrate existing consumers.

**Architecture:** New top-level `MemoryLib/` sub-project absorbs ObjectAllocatorLib via `git mv` (history preserved). Three thread-confined allocators behind a C99 API (`mem_pool_*`, `mem_arena_*`, `mem_array_*`), one shared `MemStats` struct, one header-only C++ wrapper (`memory.hpp`). Consumers compile MemoryLib sources directly from the sibling dir, exactly as they do today with ObjectAllocatorLib.

**Tech Stack:** C99 (libc only), C++14 for the wrapper test, gcc/g++, make, ASan+UBSan for test binaries.

**Spec:** `docs/superpowers/specs/2026-07-08-memory-lib-design.md` (approved)

## Global Constraints

- Work in worktree `/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.claude/worktrees/memory-lib`, branch `feature/memory-lib`. All paths below are relative to the worktree root.
- The exec bit is not set on shell scripts in this checkout — invoke as `bash build-all.sh test`, never `./build-all.sh test`.
- **Never modify anything under `MatterSurfaceLib/`** (read-only policy; its vendored `object_allocator.[ch]` copies stay).
- C sources: C99, libc only, `-Wall -Wextra`. Test binaries: `-fsanitize=address,undefined`.
- Naming (exact): types `MemPool`, `MemArena`, `MemArray`, `MemStats`; functions `mem_pool_*`, `mem_arena_*`, `mem_array_*`. No `oa_*` compatibility aliases anywhere.
- All allocators are thread-confined (one instance = one thread); no locking code.
- All C headers carry `extern "C"` guards.
- Commit after every task (message prefixes shown per task).

---

### Task 1: Rename ObjectAllocatorLib → MemoryLib and land the MemStats pool API

**Files:**
- Rename: `ObjectAllocatorLib/` → `MemoryLib/` (whole dir via `git mv`), then inside it: `include/object_allocator.h` → `include/mem_pool.h`, `src/object_allocator.c` → `src/mem_pool.c`, `tests/object_allocator_tests.c` → `tests/memory_tests.c`
- Create: `MemoryLib/include/mem_stats.h`
- Modify: `MemoryLib/include/mem_pool.h`, `MemoryLib/src/mem_pool.c`, `MemoryLib/main.c`, `MemoryLib/tests/memory_tests.c`, `MemoryLib/Makefile`

**Interfaces:**
- Produces (later tasks rely on these exact signatures):
  - `MemStats` struct (mem_stats.h): fields `liveBytes, peakBytes, totalAllocs, pageCount, totalObjects, freeObjects` (all `size_t`)
  - `MemPool* mem_pool_create(size_t objectSize, size_t objectsPerPage);`
  - `void mem_pool_destroy(MemPool* pool);`
  - `void* mem_pool_alloc(MemPool* pool);`
  - `void mem_pool_free(MemPool* pool, void* object);`
  - `void mem_pool_get_stats(MemPool* pool, MemStats* out);`

- [ ] **Step 1: git mv (pure moves, no content edits), commit**

```bash
git mv ObjectAllocatorLib MemoryLib
git mv MemoryLib/include/object_allocator.h MemoryLib/include/mem_pool.h
git mv MemoryLib/src/object_allocator.c MemoryLib/src/mem_pool.c
git mv MemoryLib/tests/object_allocator_tests.c MemoryLib/tests/memory_tests.c
git commit -m "refactor(memory-lib): git mv ObjectAllocatorLib -> MemoryLib (moves only)"
```

Committing moves separately from content edits keeps `git log --follow` clean.

- [ ] **Step 2: Create `MemoryLib/include/mem_stats.h`**

```c
#ifndef MEM_STATS_H
#define MEM_STATS_H

#include <stddef.h>

typedef struct MemStats {
    size_t liveBytes;
    size_t peakBytes;
    size_t totalAllocs;
    /* type-specific extras: */
    size_t pageCount;    /* pool: pages; arena: blocks; array: 0 */
    size_t totalObjects; /* pool only */
    size_t freeObjects;  /* pool only */
} MemStats;

#endif /* MEM_STATS_H */
```

- [ ] **Step 3: Replace `MemoryLib/include/mem_pool.h` contents entirely**

```c
#ifndef MEM_POOL_H
#define MEM_POOL_H

#include <stddef.h>
#include "mem_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-size object pool. Thread-confined: one instance = one thread. */
typedef struct MemPool MemPool;

MemPool* mem_pool_create(size_t objectSize, size_t objectsPerPage);
void mem_pool_destroy(MemPool* pool);
void* mem_pool_alloc(MemPool* pool);
void mem_pool_free(MemPool* pool, void* object);
void mem_pool_get_stats(MemPool* pool, MemStats* out);

#ifdef __cplusplus
}
#endif

#endif /* MEM_POOL_H */
```

- [ ] **Step 4: Mechanical symbol rename in the three moved .c files**

```bash
cd MemoryLib
sed -i 's/ObjectAllocator\b/MemPool/g; s/\boa_create\b/mem_pool_create/g; s/\boa_destroy\b/mem_pool_destroy/g; s/\boa_alloc\b/mem_pool_alloc/g; s/\boa_free\b/mem_pool_free/g; s/\boa_get_stats\b/mem_pool_get_stats/g; s/object_allocator\.h/mem_pool.h/g' src/mem_pool.c main.c tests/memory_tests.c
cd ..
```

(String literals in printfs get renamed too — that's fine, the demo output should say MemPool.)

- [ ] **Step 5: Write the failing stats test**

Append to `MemoryLib/tests/memory_tests.c` (and add `test_stats();` to its `main()`):

```c
void test_stats() {
    printf("Testing MemStats...\n");
    MemPool* pool = mem_pool_create(32, 4);
    assert(pool != NULL);

    MemStats st;
    mem_pool_get_stats(pool, &st);
    assert(st.totalAllocs == 0);
    assert(st.liveBytes == 0);
    assert(st.pageCount == 0);

    void* a = mem_pool_alloc(pool);
    void* b = mem_pool_alloc(pool);
    assert(a && b);
    mem_pool_get_stats(pool, &st);
    assert(st.totalAllocs == 2);
    assert(st.pageCount == 1);
    assert(st.totalObjects == 4);
    assert(st.freeObjects == 2);
    assert(st.liveBytes > 0);
    size_t peakAfterTwo = st.peakBytes;
    assert(peakAfterTwo == st.liveBytes);

    mem_pool_free(pool, a);
    mem_pool_get_stats(pool, &st);
    assert(st.peakBytes == peakAfterTwo);   /* peak survives frees */
    assert(st.liveBytes < peakAfterTwo);
    assert(st.totalAllocs == 2);            /* allocs only count up */

    mem_pool_destroy(pool);
    printf("  MemStats tests passed!\n");
}
```

- [ ] **Step 6: Run the test to verify it fails**

```bash
make -C MemoryLib test
```

Expected: FAIL to compile — `mem_pool_get_stats` in `src/mem_pool.c` still has the old 4-argument signature, and the struct lacks `totalAllocs`/`peakLiveBytes`. (The Makefile still references old paths — fix it now if the failure is path-related, see Step 8, then re-run to see the signature failure.)

- [ ] **Step 7: Implement stats in `MemoryLib/src/mem_pool.c`**

Three edits to the sed-renamed file:

7a. Add two fields to `struct MemPool`:

```c
struct MemPool {
    size_t objectSize;         /* stride (header-padded, max_align_t-aligned) */
    size_t objectsPerPage;
    size_t pageCount;
    size_t totalObjects;
    size_t freeObjects;
    size_t totalAllocs;        /* lifetime mem_pool_alloc count */
    size_t peakLiveBytes;      /* high-water mark of live bytes */
    ObjectHeader* freeList;
    void** pages;
    size_t pagesCapacity;
};
```

7b. In `mem_pool_create`, after the existing field initialization add:

```c
    allocator->totalAllocs = 0;
    allocator->peakLiveBytes = 0;
```

7c. In `mem_pool_alloc`, right after `allocator->freeObjects--;` add:

```c
    allocator->totalAllocs++;
    {
        size_t live = (allocator->totalObjects - allocator->freeObjects) * allocator->objectSize;
        if (live > allocator->peakLiveBytes) allocator->peakLiveBytes = live;
    }
```

7d. Replace the old `mem_pool_get_stats` function (previously 4 out-params) entirely with:

```c
void mem_pool_get_stats(MemPool* pool, MemStats* out) {
    if (!pool || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->liveBytes = (pool->totalObjects - pool->freeObjects) * pool->objectSize;
    out->peakBytes = pool->peakLiveBytes;
    out->totalAllocs = pool->totalAllocs;
    out->pageCount = pool->pageCount;
    out->totalObjects = pool->totalObjects;
    out->freeObjects = pool->freeObjects;
}
```

- [ ] **Step 8: Fix `MemoryLib/main.c` get_stats call sites and the Makefile**

`main.c` has 4 call sites (old lines ~119, 129, 146, 159) shaped like:

```c
size_t pageCount, totalObjects, freeObjects;
mem_pool_get_stats(allocator, &pageCount, &totalObjects, &freeObjects);
printf("... %zu ... %zu ... %zu ...", pageCount, totalObjects, freeObjects);
```

Convert each to:

```c
MemStats st;
mem_pool_get_stats(allocator, &st);
printf("... %zu ... %zu ... %zu ...", st.pageCount, st.totalObjects, st.freeObjects);
```

(Keep each printf's original text; only the argument expressions change. Delete the now-unused local `size_t` declarations.)

Then replace `MemoryLib/Makefile` contents entirely:

```make
CC = gcc
CFLAGS = -Wall -Wextra -g -I./include
LDFLAGS =

SRC_DIR = src
INCLUDE_DIR = include
OBJ_DIR = obj

$(shell mkdir -p $(OBJ_DIR))

SRCS = main.c $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)
HEADERS = $(wildcard $(INCLUDE_DIR)/*.h)

TARGET = memorylib

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/main.o: main.c $(HEADERS)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	mkdir -p $(OBJ_DIR)/$(SRC_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET) memory_tests memory_hpp_tests

# Tests: ASan + UBSan (repo convention)
TEST_FLAGS = -Wall -Wextra -g -I./include -fsanitize=address,undefined
TEST_SRCS = tests/memory_tests.c $(wildcard $(SRC_DIR)/*.c)

test: $(TEST_SRCS)
	$(CC) $(TEST_FLAGS) -o memory_tests $(TEST_SRCS)
	./memory_tests

-include $(OBJS:.o=.d)

.PHONY: all clean test
```

(`wildcard src/*.c` means Tasks 3–4 add sources without touching SRCS/TEST_SRCS again.)

- [ ] **Step 9: Run tests and demo binary to verify they pass**

```bash
make -C MemoryLib test && make -C MemoryLib && ./MemoryLib/memorylib
```

Expected: `memory_tests` prints all test passes including `MemStats tests passed!`, exit 0; demo binary runs its self-tests, exit 0.

- [ ] **Step 10: Commit**

```bash
git add MemoryLib
git commit -m "feat(memory-lib): mem_pool API with MemStats (renamed from oa_*)"
```

---

### Task 2: Migrate consumers and the build system

**Files:**
- Modify: `SpatialQueryLib/Makefile`, `SpatialQueryLib/main.c`, `SpatialQueryLib/src/spatial_hash.c`, `SpatialQueryLib/src/bvh.c`
- Modify: `OpenParticleSurfaceLib/Makefile`, `OpenParticleSurfaceLib/src/open_particle_surface.c`, `OpenParticleSurfaceLib/install_dependencies.sh`, `OpenParticleSurfaceLib/build.sh`
- Modify: `SurfaceLib/Makefile`, `GPURayTraceExample/Makefile`, `ParticleDynamicsExample/Makefile`, `ParticleDynamicsExample/build.sh`
- Modify: `build-all.sh:35,142-145`, `.gitignore:16,78-83`
- Modify: `README.md:19,37,75,99,111`, `CLAUDE.md:46`, `ROADMAP.md:242,255`, `ParticleDynamicsExample/README.md:132`, `GPURayTraceExample/README.md:238`
- **Do NOT touch:** anything in `MatterSurfaceLib/` (including its README)

**Interfaces:**
- Consumes: Task 1's `mem_pool_*` API and `MemoryLib/{include/mem_pool.h,src/mem_pool.c}` paths.

- [ ] **Step 1: Migrate SpatialQueryLib**

```bash
cd SpatialQueryLib
sed -i 's|\.\./ObjectAllocatorLib|../MemoryLib|g; s/\bOA_LIB\b/MEMORY_LIB/g; s/object_allocator/mem_pool/g' Makefile
sed -i 's/ObjectAllocator\b/MemPool/g; s/\boa_create\b/mem_pool_create/g; s/\boa_destroy\b/mem_pool_destroy/g; s/\boa_alloc\b/mem_pool_alloc/g; s/\boa_free\b/mem_pool_free/g; s/\boa_get_stats\b/mem_pool_get_stats/g; s/object_allocator\.h/mem_pool.h/g' main.c src/spatial_hash.c
cd ..
```

Then in `SpatialQueryLib/src/bvh.c` delete line 2 (`#include "object_allocator.h"`) — bvh.c calls no pool functions (verified: zero `oa_*` call sites). In `SpatialQueryLib/Makefile`, remove ` $(MEMORY_LIB)/src/mem_pool.c` from `BVH_TEST_SRCS` (line ~51) since bvh tests no longer reference the pool.

- [ ] **Step 2: Build and test SpatialQueryLib**

```bash
make -C SpatialQueryLib clean && make -C SpatialQueryLib && make -C SpatialQueryLib test && ./SpatialQueryLib/spatialquerylib
```

Expected: builds clean; `spatial_hash_tests` and `bvh_tests` pass under ASan; main binary's self-tests pass (its pool integration test now exercises `mem_pool_*`), exit 0.

- [ ] **Step 3: Migrate OpenParticleSurfaceLib**

```bash
cd OpenParticleSurfaceLib
sed -i 's|\.\./ObjectAllocatorLib|../MemoryLib|g; s/\bOA_LIB\b/MEMORY_LIB/g; s/object_allocator/mem_pool/g' Makefile
sed -i 's/ObjectAllocator\b/MemPool/g; s/\boa_create\b/mem_pool_create/g; s/\boa_destroy\b/mem_pool_destroy/g; s/\boa_alloc\b/mem_pool_alloc/g; s/\boa_free\b/mem_pool_free/g; s/object_allocator\.h/mem_pool.h/g' src/open_particle_surface.c
sed -i 's|ObjectAllocatorLib|MemoryLib|g; s/object_allocator/mem_pool/g' install_dependencies.sh build.sh
cd ..
make -C OpenParticleSurfaceLib clean && make -C OpenParticleSurfaceLib
```

Expected: builds clean.

- [ ] **Step 4: Migrate the Makefile-only consumers**

```bash
for d in SurfaceLib GPURayTraceExample ParticleDynamicsExample; do
  sed -i 's|\.\./ObjectAllocatorLib|../MemoryLib|g; s/\bOA_LIB\b/MEMORY_LIB/g; s/object_allocator/mem_pool/g' "$d/Makefile"
done
sed -i 's|ObjectAllocatorLib|MemoryLib|g; s/object_allocator/mem_pool/g' ParticleDynamicsExample/build.sh
make -C SurfaceLib clean && make -C SurfaceLib
make -C GPURayTraceExample clean && make -C GPURayTraceExample
make -C ParticleDynamicsExample clean && make -C ParticleDynamicsExample
```

Expected: all three build clean. (These compile `$(MEMORY_LIB)/src/mem_pool.c` into their own obj dirs, same pattern as before.)

- [ ] **Step 5: Update build-all.sh, .gitignore**

`build-all.sh` line 35: `    ObjectAllocatorLib` → `    MemoryLib`.

`build-all.sh` lines 142-145 — the test loop's special case dies because `MemoryLib` lowercases cleanly to its binary name:

```bash
    for proj in MemoryLib SpatialQueryLib; do
        bin="$proj/$(echo "$proj" | tr '[:upper:]' '[:lower:]')"
        if [ -x "$bin" ]; then
```

`.gitignore` — four line edits (other lines untouched):

- line 16: `ObjectAllocatorLib/objectallocator` → `MemoryLib/memorylib`
- line 78 comment: `# ObjectAllocatorLib and SpatialQueryLib test binaries + object dirs` → `# MemoryLib and SpatialQueryLib test binaries + object dirs`
- line 81: `ObjectAllocatorLib/object_allocator_tests` → two lines: `MemoryLib/memory_tests` and `MemoryLib/memory_hpp_tests`
- line 83: `ObjectAllocatorLib/linux` → `MemoryLib/linux`

- [ ] **Step 6: Update doc references**

- `README.md:19`: `### \`MemoryLib/\` — memory managers: pool, arena, growable array (C)`
- `README.md:37`: `ObjectAllocatorLib` → `MemoryLib`
- `README.md:75`: comment `(ObjectAllocatorLib + SpatialQueryLib)` → `(MemoryLib + SpatialQueryLib)`
- `README.md:99`: `| \`MemoryLib\` | \`make\` | \`./memorylib\` (test runner) |`
- `README.md:111`: `6 in \`ObjectAllocatorLib\`` → `6 in \`MemoryLib\``
- `CLAUDE.md:46`: `\`ObjectAllocatorLib\`` → `\`MemoryLib\``
- `ROADMAP.md:242,255`: `ObjectAllocatorLib` → `MemoryLib`
- `ParticleDynamicsExample/README.md:132`: `**ObjectAllocatorLib**` → `**MemoryLib**`
- `GPURayTraceExample/README.md:238`: `(copied from ObjectAllocatorLib)` → `(compiled from MemoryLib)`

Then verify no stragglers outside the allowed zones:

```bash
grep -rn "ObjectAllocatorLib\|oa_create\|oa_alloc\|object_allocator" \
  --include="*.c" --include="*.h" --include="*.cpp" --include="*.hpp" \
  --include="Makefile" --include="*.sh" --include="*.md" . \
  | grep -v "^./MatterSurfaceLib/" | grep -v "^./docs/" | grep -v "^./Libraries/" | grep -v "^./Examples/"
```

Expected: no output. (MatterSurfaceLib keeps its vendored copies; docs/ history stays as-is.)

- [ ] **Step 7: Full gate**

```bash
bash build-all.sh test
```

Expected: every project builds, all headless suites pass, summary shows no FAIL. This is the proof the rename broke nothing.

- [ ] **Step 8: Commit**

```bash
git add -A ':!MatterSurfaceLib'
git commit -m "refactor: retarget all ObjectAllocatorLib consumers to MemoryLib mem_pool API"
```

---

### Task 3: mem_arena — bump allocator with bulk reset

**Files:**
- Create: `MemoryLib/include/mem_arena.h`, `MemoryLib/src/mem_arena.c`
- Modify: `MemoryLib/tests/memory_tests.c` (append tests + calls in `main()`)

**Interfaces:**
- Consumes: `MemStats` from Task 1.
- Produces (Task 5 relies on these):
  - `MemArena* mem_arena_create(size_t initialCapacity);` — NULL if `initialCapacity == 0` or OOM
  - `void mem_arena_destroy(MemArena* a);`
  - `void* mem_arena_alloc(MemArena* a, size_t size);` — 8-byte aligned; NULL if `size == 0` or OOM
  - `void mem_arena_reset(MemArena* a);` — frees all blocks except the largest; keeps capacity
  - `void mem_arena_get_stats(MemArena* a, MemStats* out);` — `pageCount` = block count

- [ ] **Step 1: Write the failing tests**

Append to `MemoryLib/tests/memory_tests.c` (add `#include "../include/mem_arena.h"` at the top, and both calls to `main()`):

```c
void test_arena_basic() {
    printf("Testing arena basic alloc + alignment...\n");
    MemArena* a = mem_arena_create(1024);
    assert(a != NULL);

    int* x = (int*)mem_arena_alloc(a, sizeof(int) * 10);   /* 40 -> 40 */
    assert(x != NULL);
    assert(((uintptr_t)x % 8) == 0);
    for (int i = 0; i < 10; i++) x[i] = i;

    char* c = (char*)mem_arena_alloc(a, 3);                /* 3 -> 8 */
    assert(c != NULL);
    assert(((uintptr_t)c % 8) == 0);

    for (int i = 0; i < 10; i++) assert(x[i] == i);        /* no overlap */

    MemStats st;
    mem_arena_get_stats(a, &st);
    assert(st.totalAllocs == 2);
    assert(st.pageCount == 1);
    assert(st.liveBytes == 48);
    assert(st.peakBytes == 48);

    assert(mem_arena_alloc(a, 0) == NULL);
    mem_arena_destroy(a);
    printf("  Arena basic tests passed!\n");
}

void test_arena_chaining_and_reset() {
    printf("Testing arena block chaining + reset...\n");
    MemArena* a = mem_arena_create(64);
    assert(a != NULL);

    void* big = mem_arena_alloc(a, 256);   /* > 64: forces a new 256-byte block */
    assert(big != NULL);
    MemStats st;
    mem_arena_get_stats(a, &st);
    assert(st.pageCount == 2);
    assert(st.liveBytes == 256);

    mem_arena_reset(a);
    mem_arena_get_stats(a, &st);
    assert(st.pageCount == 1);             /* largest (256) block retained */
    assert(st.liveBytes == 0);
    assert(st.peakBytes == 256);           /* peak survives reset */

    /* steady state: refill fits in the retained block — no new blocks */
    for (int i = 0; i < 4; i++) assert(mem_arena_alloc(a, 64) != NULL);
    mem_arena_get_stats(a, &st);
    assert(st.pageCount == 1);
    assert(st.liveBytes == 256);

    mem_arena_destroy(a);
    printf("  Arena chaining/reset tests passed!\n");
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
make -C MemoryLib test
```

Expected: FAIL to compile — `mem_arena.h: No such file or directory`.

- [ ] **Step 3: Write `MemoryLib/include/mem_arena.h`**

```c
#ifndef MEM_ARENA_H
#define MEM_ARENA_H

#include <stddef.h>
#include "mem_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bump allocator with bulk reset — no per-allocation free.
 * Thread-confined: one instance = one thread. */
typedef struct MemArena MemArena;

MemArena* mem_arena_create(size_t initialCapacity);
void mem_arena_destroy(MemArena* a);
void* mem_arena_alloc(MemArena* a, size_t size);   /* 8-byte aligned */
void mem_arena_reset(MemArena* a);                 /* keeps largest block */
void mem_arena_get_stats(MemArena* a, MemStats* out);

#ifdef __cplusplus
}
#endif

#endif /* MEM_ARENA_H */
```

- [ ] **Step 4: Write `MemoryLib/src/mem_arena.c`**

```c
#include "../include/mem_arena.h"
#include <stdlib.h>
#include <string.h>

#define ARENA_ALIGN 8

typedef struct ArenaBlock {
    struct ArenaBlock* next;
    size_t capacity;
    size_t used;
} ArenaBlock;

/* payload follows the header; pad header size so payload stays aligned */
#define BLOCK_HEADER_SIZE (((sizeof(ArenaBlock) + ARENA_ALIGN - 1) / ARENA_ALIGN) * ARENA_ALIGN)

struct MemArena {
    ArenaBlock* blocks;      /* head = current block */
    size_t blockCount;
    size_t totalAllocs;
    size_t liveBytes;
    size_t peakBytes;
};

static ArenaBlock* block_create(size_t capacity) {
    ArenaBlock* b = (ArenaBlock*)malloc(BLOCK_HEADER_SIZE + capacity);
    if (!b) {
        return NULL;
    }
    b->next = NULL;
    b->capacity = capacity;
    b->used = 0;
    return b;
}

MemArena* mem_arena_create(size_t initialCapacity) {
    if (initialCapacity == 0) {
        return NULL;
    }
    MemArena* a = (MemArena*)calloc(1, sizeof(MemArena));
    if (!a) {
        return NULL;
    }
    a->blocks = block_create(initialCapacity);
    if (!a->blocks) {
        free(a);
        return NULL;
    }
    a->blockCount = 1;
    return a;
}

void mem_arena_destroy(MemArena* a) {
    if (!a) {
        return;
    }
    ArenaBlock* b = a->blocks;
    while (b) {
        ArenaBlock* next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

void* mem_arena_alloc(MemArena* a, size_t size) {
    if (!a || size == 0) {
        return NULL;
    }
    size = (size + ARENA_ALIGN - 1) / ARENA_ALIGN * ARENA_ALIGN;

    ArenaBlock* b = a->blocks;
    if (b->capacity - b->used < size) {
        size_t newCap = b->capacity + b->capacity / 2;   /* 1.5x previous block */
        if (newCap < size) {
            newCap = size;
        }
        ArenaBlock* nb = block_create(newCap);
        if (!nb) {
            return NULL;
        }
        nb->next = a->blocks;
        a->blocks = nb;
        a->blockCount++;
        b = nb;
    }

    void* p = (char*)b + BLOCK_HEADER_SIZE + b->used;
    b->used += size;
    a->totalAllocs++;
    a->liveBytes += size;
    if (a->liveBytes > a->peakBytes) {
        a->peakBytes = a->liveBytes;
    }
    return p;
}

void mem_arena_reset(MemArena* a) {
    if (!a) {
        return;
    }
    ArenaBlock* largest = a->blocks;
    for (ArenaBlock* b = a->blocks; b; b = b->next) {
        if (b->capacity > largest->capacity) {
            largest = b;
        }
    }
    ArenaBlock* b = a->blocks;
    while (b) {
        ArenaBlock* next = b->next;
        if (b != largest) {
            free(b);
        }
        b = next;
    }
    largest->next = NULL;
    largest->used = 0;
    a->blocks = largest;
    a->blockCount = 1;
    a->liveBytes = 0;
}

void mem_arena_get_stats(MemArena* a, MemStats* out) {
    if (!a || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->liveBytes = a->liveBytes;
    out->peakBytes = a->peakBytes;
    out->totalAllocs = a->totalAllocs;
    out->pageCount = a->blockCount;
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
make -C MemoryLib test
```

Expected: PASS — all existing pool tests plus `Arena basic tests passed!` and `Arena chaining/reset tests passed!`, ASan clean, exit 0.

- [ ] **Step 6: Commit**

```bash
git add MemoryLib
git commit -m "feat(memory-lib): mem_arena bump allocator with bulk reset + stats"
```

---

### Task 4: mem_array — the one blessed growable array

**Files:**
- Create: `MemoryLib/include/mem_array.h`, `MemoryLib/src/mem_array.c`
- Modify: `MemoryLib/tests/memory_tests.c` (append tests + calls in `main()`)

**Interfaces:**
- Consumes: `MemStats` from Task 1.
- Produces:
  - `MemArray` struct (public fields): `void* data; size_t count; size_t capacity; size_t elemSize; size_t growCount;`
  - `void mem_array_init(MemArray* arr, size_t elemSize);`
  - `int mem_array_ensure(MemArray* arr, size_t minCapacity);` — 1 on success, 0 on OOM (old data intact)
  - `void* mem_array_push(MemArray* arr);` — pointer to the new element slot, NULL on OOM
  - `void mem_array_clear(MemArray* arr);` — count = 0, capacity retained
  - `void mem_array_free(MemArray* arr);`
  - `void mem_array_get_stats(const MemArray* arr, MemStats* out);`

- [ ] **Step 1: Write the failing tests**

Append to `MemoryLib/tests/memory_tests.c` (add `#include "../include/mem_array.h"`, calls in `main()`):

```c
void test_array_growth() {
    printf("Testing mem_array growth + clear + free...\n");
    MemArray arr;
    mem_array_init(&arr, sizeof(int));
    assert(arr.data == NULL && arr.count == 0 && arr.capacity == 0);

    for (int i = 0; i < 100; i++) {
        int* slot = (int*)mem_array_push(&arr);
        assert(slot != NULL);
        *slot = i;
    }
    assert(arr.count == 100);
    assert(arr.capacity >= 100);
    for (int i = 0; i < 100; i++) assert(((int*)arr.data)[i] == i);

    MemStats st;
    mem_array_get_stats(&arr, &st);
    assert(st.liveBytes == 100 * sizeof(int));
    assert(st.peakBytes == arr.capacity * sizeof(int));
    assert(st.totalAllocs == arr.growCount);
    assert(st.totalAllocs >= 2);            /* 100 ints can't fit the first grow */

    size_t capBefore = arr.capacity;
    mem_array_clear(&arr);
    assert(arr.count == 0 && arr.capacity == capBefore);   /* capacity retained */

    mem_array_free(&arr);
    assert(arr.data == NULL && arr.capacity == 0 && arr.count == 0);
    printf("  mem_array growth tests passed!\n");
}

void test_array_growth_policy() {
    printf("Testing mem_array growth policy...\n");
    MemArray arr;
    mem_array_init(&arr, 1);
    assert(mem_array_ensure(&arr, 1) == 1);
    assert(arr.capacity == 16);              /* minimum step */
    assert(mem_array_ensure(&arr, 17) == 1);
    assert(arr.capacity == 24);              /* 16 * 3 / 2 */
    assert(mem_array_ensure(&arr, 100) == 1);
    assert(arr.capacity == 100);             /* jump straight when 1.5x is short */
    mem_array_free(&arr);
    printf("  mem_array growth policy tests passed!\n");
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
make -C MemoryLib test
```

Expected: FAIL to compile — `mem_array.h: No such file or directory`.

- [ ] **Step 3: Write `MemoryLib/include/mem_array.h`**

```c
#ifndef MEM_ARRAY_H
#define MEM_ARRAY_H

#include <stddef.h>
#include "mem_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Growable array (heap-backed). Thread-confined. */
typedef struct MemArray {
    void*  data;
    size_t count;
    size_t capacity;    /* in elements */
    size_t elemSize;
    size_t growCount;   /* number of reallocs; reported as stats totalAllocs */
} MemArray;

void  mem_array_init(MemArray* arr, size_t elemSize);
int   mem_array_ensure(MemArray* arr, size_t minCapacity);  /* 1 ok, 0 OOM (data intact) */
void* mem_array_push(MemArray* arr);                        /* new slot, NULL on OOM */
void  mem_array_clear(MemArray* arr);                       /* count=0, keeps capacity */
void  mem_array_free(MemArray* arr);
void  mem_array_get_stats(const MemArray* arr, MemStats* out);

#ifdef __cplusplus
}
#endif

#endif /* MEM_ARRAY_H */
```

- [ ] **Step 4: Write `MemoryLib/src/mem_array.c`**

```c
#include "../include/mem_array.h"
#include <stdlib.h>
#include <string.h>

void mem_array_init(MemArray* arr, size_t elemSize) {
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
    arr->elemSize = elemSize;
    arr->growCount = 0;
}

int mem_array_ensure(MemArray* arr, size_t minCapacity) {
    if (minCapacity <= arr->capacity) {
        return 1;
    }
    size_t newCap = arr->capacity + arr->capacity / 2;   /* 1.5x */
    if (newCap < minCapacity) {
        newCap = minCapacity;
    }
    if (newCap < 16) {
        newCap = 16;
    }
    void* tmp = realloc(arr->data, newCap * arr->elemSize);
    if (!tmp) {
        return 0;   /* old data intact */
    }
    arr->data = tmp;
    arr->capacity = newCap;
    arr->growCount++;
    return 1;
}

void* mem_array_push(MemArray* arr) {
    if (arr->count == arr->capacity && !mem_array_ensure(arr, arr->count + 1)) {
        return NULL;
    }
    return (char*)arr->data + arr->count++ * arr->elemSize;
}

void mem_array_clear(MemArray* arr) {
    arr->count = 0;
}

void mem_array_free(MemArray* arr) {
    free(arr->data);
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
    arr->growCount = 0;
}

void mem_array_get_stats(const MemArray* arr, MemStats* out) {
    if (!arr || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->liveBytes = arr->count * arr->elemSize;
    out->peakBytes = arr->capacity * arr->elemSize;   /* capacity never shrinks */
    out->totalAllocs = arr->growCount;
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
make -C MemoryLib test
```

Expected: PASS — all pool + arena + array tests, ASan clean, exit 0.

- [ ] **Step 6: Commit**

```bash
git add MemoryLib
git commit -m "feat(memory-lib): mem_array growable array with unified growth policy + stats"
```

---

### Task 5: memory.hpp — C++ RAII wrappers

**Files:**
- Create: `MemoryLib/include/memory.hpp`, `MemoryLib/tests/memory_hpp_tests.cpp`
- Modify: `MemoryLib/Makefile` (test target gains the C++ test)

**Interfaces:**
- Consumes: `mem_arena_*` (Task 3), `mem_pool_*` (Task 1), `MemStats`.
- Produces: `mem::Arena` and `mem::Pool` RAII classes (header-only, move-only).

- [ ] **Step 1: Write the failing test `MemoryLib/tests/memory_hpp_tests.cpp`**

```cpp
#include "../include/memory.hpp"
#include <cassert>
#include <cstdio>
#include <utility>

struct Vec3 { float x, y, z; };

int main() {
    printf("Testing mem::Arena...\n");
    mem::Arena arena(1024);
    assert(arena.valid());

    Vec3* v = arena.allocArray<Vec3>(10);
    assert(v != nullptr);
    v[9].x = 1.0f; v[9].y = 2.0f; v[9].z = 3.0f;
    assert(arena.stats().totalAllocs == 1);
    assert(arena.stats().liveBytes == 10 * sizeof(Vec3));

    arena.reset();
    assert(arena.stats().liveBytes == 0);
    assert(arena.stats().peakBytes == 10 * sizeof(Vec3));

    mem::Arena moved(std::move(arena));
    assert(moved.valid() && !arena.valid());

    printf("Testing mem::Pool...\n");
    mem::Pool pool(sizeof(Vec3), 16);
    assert(pool.valid());
    void* p = pool.alloc();
    assert(p != nullptr);
    pool.free(p);
    assert(pool.stats().totalAllocs == 1);
    assert(pool.stats().freeObjects == 16);

    printf("All memory.hpp tests passed!\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
g++ -Wall -Wextra -g -std=c++14 -I MemoryLib/include -fsanitize=address,undefined -o MemoryLib/memory_hpp_tests MemoryLib/tests/memory_hpp_tests.cpp MemoryLib/src/mem_pool.c MemoryLib/src/mem_arena.c
```

Expected: FAIL — `memory.hpp: No such file or directory`.

- [ ] **Step 3: Write `MemoryLib/include/memory.hpp`**

```cpp
#ifndef MEMORY_HPP
#define MEMORY_HPP

#include <cstddef>
#include <type_traits>
#include "mem_arena.h"
#include "mem_pool.h"

namespace mem {

// RAII over MemArena. Move-only. Arena never runs destructors: POD-ish types only.
class Arena {
public:
    explicit Arena(size_t initialCap) : a_(mem_arena_create(initialCap)) {}
    ~Arena() { mem_arena_destroy(a_); }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& o) noexcept : a_(o.a_) { o.a_ = nullptr; }
    Arena& operator=(Arena&& o) noexcept {
        if (this != &o) {
            mem_arena_destroy(a_);
            a_ = o.a_;
            o.a_ = nullptr;
        }
        return *this;
    }

    void* alloc(size_t n) { return mem_arena_alloc(a_, n); }

    template <typename T>
    T* allocArray(size_t count) {
        static_assert(std::is_trivially_destructible<T>::value,
                      "arena memory is never destructed");
        static_assert(alignof(T) <= 8, "arena guarantees 8-byte alignment only");
        return static_cast<T*>(mem_arena_alloc(a_, count * sizeof(T)));
    }

    void reset() { mem_arena_reset(a_); }
    MemStats stats() const { MemStats s; mem_arena_get_stats(a_, &s); return s; }
    bool valid() const { return a_ != nullptr; }

private:
    MemArena* a_;
};

// RAII over MemPool. Move-only.
class Pool {
public:
    Pool(size_t objectSize, size_t objectsPerPage)
        : p_(mem_pool_create(objectSize, objectsPerPage)) {}
    ~Pool() { mem_pool_destroy(p_); }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Pool& operator=(Pool&& o) noexcept {
        if (this != &o) {
            mem_pool_destroy(p_);
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }

    void* alloc() { return mem_pool_alloc(p_); }
    void free(void* obj) { mem_pool_free(p_, obj); }
    MemStats stats() const { MemStats s; mem_pool_get_stats(p_, &s); return s; }
    bool valid() const { return p_ != nullptr; }

private:
    MemPool* p_;
};

} // namespace mem

#endif // MEMORY_HPP
```

Note on `stats()` in the test: `pool.stats().freeObjects == 16` holds because `alloc` then `free` returns the object to a 16-object page.

- [ ] **Step 4: Wire the C++ test into the Makefile**

In `MemoryLib/Makefile`, replace the `test:` target with:

```make
TEST_CC_OBJS = $(OBJ_DIR)/t_mem_pool.o $(OBJ_DIR)/t_mem_arena.o $(OBJ_DIR)/t_mem_array.o

$(OBJ_DIR)/t_%.o: $(SRC_DIR)/%.c $(HEADERS)
	$(CC) $(TEST_FLAGS) -c $< -o $@

test: $(TEST_CC_OBJS) tests/memory_tests.c tests/memory_hpp_tests.cpp
	$(CC) $(TEST_FLAGS) -o memory_tests tests/memory_tests.c $(TEST_CC_OBJS)
	./memory_tests
	g++ -Wall -Wextra -g -std=c++14 -I./include -fsanitize=address,undefined -o memory_hpp_tests tests/memory_hpp_tests.cpp $(TEST_CC_OBJS)
	./memory_hpp_tests
```

(C sources compiled once by gcc, linked into both test binaries; `extern "C"` guards from Tasks 1/3/4 make the C++ link work.)

- [ ] **Step 5: Run tests to verify they pass**

```bash
make -C MemoryLib test
```

Expected: PASS — `memory_tests` then `memory_hpp_tests` both run green under ASan/UBSan, exit 0.

- [ ] **Step 6: Commit**

```bash
git add MemoryLib
git commit -m "feat(memory-lib): memory.hpp RAII wrappers (mem::Arena, mem::Pool)"
```

---

### Task 6: Documentation and final gate

**Files:**
- Rename+rewrite: `MemoryLib/OBJECT_ALLOCATOR.md` → `MemoryLib/README.md`
- Modify: `build-all.sh` (run MemoryLib's full `make test` in the test phase)

**Interfaces:**
- Consumes: everything from Tasks 1–5.

- [ ] **Step 1: Replace the old doc with a README**

```bash
git mv MemoryLib/OBJECT_ALLOCATOR.md MemoryLib/README.md
```

Write `MemoryLib/README.md` with this content:

```markdown
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
```

- [ ] **Step 2: Run the full MemoryLib suite in build-all.sh's test phase**

In `build-all.sh`, immediately after the `for proj in MemoryLib SpatialQueryLib; do ... done` loop (Task 2 edited it), add:

```bash
    echo
    echo "--- MemoryLib (memory_tests + memory_hpp_tests) ---"
    make -C MemoryLib test || RESULT[MemoryLib]="FAIL (tests)"
```

(The loop runs the demo binary; this adds the ASan suites — arena/array/hpp coverage isn't in the demo.)

- [ ] **Step 3: Final full gate**

```bash
bash build-all.sh test
```

Expected: all projects build; summary shows no FAIL; the MemoryLib section shows both the demo binary and the two test suites passing.

- [ ] **Step 4: Verify MatterSurfaceLib untouched**

```bash
git diff main --stat -- MatterSurfaceLib
```

Expected: no output.

- [ ] **Step 5: Commit**

```bash
git add MemoryLib/README.md build-all.sh
git commit -m "docs(memory-lib): README for pool/arena/array + wire ASan suites into build-all test phase"
```
