/* libs/MemoryLib/src/mem_arena.c
 *
 * Implementation of the bump arena declared in ../include/mem_arena.h. See
 * that header for the public contract (alignment, thread-confinement, the
 * "POD-ish only" rule); this file documents how it is built.
 *
 * Structure
 * ---------
 * An arena is a singly-linked list of blocks with the NEWEST block at the
 * head. Each block is one malloc of BLOCK_HEADER_SIZE + capacity bytes: the
 * ArenaBlock header sits at the front and the payload follows it immediately,
 * so a block is one allocation, not two. BLOCK_HEADER_SIZE is sizeof
 * (ArenaBlock) rounded up to ARENA_ALIGN, which is what keeps the payload --
 * and therefore every pointer handed out -- 8-byte aligned.
 *
 * Allocation only ever bumps the head block. When the head cannot fit a
 * request, a new block is pushed on the front sized max(1.5x the head's
 * capacity, the request), so blocks grow geometrically and a single huge
 * request always succeeds in a block of its own. Older blocks are never
 * revisited even if they still have room -- that slack is the price of the
 * O(1) allocation path.
 *
 * This is why the header can promise that pointers stay valid across further
 * allocations: growing pushes a new block and never realloc's or moves an
 * existing one.
 *
 * Reset keeps the single largest block and frees the rest, so a workload with
 * a stable peak converges to one block and zero mallocs per cycle. The
 * retained block is not zeroed; only its `used` cursor is rewound.
 *
 * Compiled directly from here by consumers (-I ../include plus this .c) --
 * see libs/AssetStoreLib/tests/Makefile. No locking anywhere: one arena
 * belongs to one thread.
 */
#include "../include/mem_arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ARENA_ALIGN 8

/* One malloc'd block. The payload is not a member: it starts BLOCK_HEADER_SIZE
 * bytes past the start of this header and runs for `capacity` bytes.
 *   next     - the block allocated BEFORE this one (list is newest-first)
 *   capacity - payload bytes, excluding the header
 *   used     - bump cursor, in bytes from the start of the payload; always
 *              a multiple of ARENA_ALIGN and always <= capacity */
typedef struct ArenaBlock {
    struct ArenaBlock* next;
    size_t capacity;
    size_t used;
} ArenaBlock;

/* payload follows the header; pad header size so payload stays aligned */
#define BLOCK_HEADER_SIZE (((sizeof(ArenaBlock) + ARENA_ALIGN - 1) / ARENA_ALIGN) * ARENA_ALIGN)

/* The opaque handle behind MemArena. Never null-blocks: mem_arena_create
 * fails outright rather than returning an arena with no block, so every other
 * function may assume `blocks` is non-NULL.
 *
 * The three counters feed mem_arena_get_stats and count ROUNDED-UP request
 * sizes, not raw ones, and not the malloc'd block bytes -- per-block headers
 * and unused block tail slack are invisible to them. totalAllocs and
 * peakBytes are lifetime counters that survive mem_arena_reset; only
 * liveBytes tracks the current cycle. */
struct MemArena {
    ArenaBlock* blocks;      /* head = current block */
    size_t blockCount;
    size_t totalAllocs;
    size_t liveBytes;
    size_t peakBytes;
};

/* One malloc for header + payload. Returns NULL on out-of-memory; the caller
 * is responsible for linking the block into the arena and bumping blockCount.
 * `capacity` is payload bytes and is used verbatim -- rounding to ARENA_ALIGN
 * happens in mem_arena_alloc, before this is reached. */
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

/* The hot path: round the request up to ARENA_ALIGN, bump the head block, and
 * only touch malloc when the head cannot fit it. Returns NULL for a NULL
 * arena, for size 0, when the round-up would overflow size_t, and on
 * out-of-memory -- so a NULL return is not always "out of memory".
 *
 * On the grow path the new block is sized max(1.5 * head capacity, size), so
 * an oversized request never wedges the arena. The new block becomes the head
 * and the previous head keeps whatever slack it had; that space is never
 * reused before a reset. */
void* mem_arena_alloc(MemArena* a, size_t size) {
    if (!a || size == 0) {
        return NULL;
    }
    if (size > SIZE_MAX - (ARENA_ALIGN - 1)) {
        return NULL;   /* round-up would wrap */
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

/* Two passes: find the largest block, then free every other one. Retaining
 * the largest (rather than the first or the newest) is what makes the steady
 * state zero-malloc -- the block that grew to fit the peak is the one worth
 * keeping. Every pointer previously returned by mem_arena_alloc dangles after
 * this. The retained block's bytes are left as they were; only `used` is
 * rewound to 0. */
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
    /* totalAllocs and peakBytes are lifetime counters and survive reset;
       only liveBytes tracks the current cycle. */
    a->liveBytes = 0;
}

/* Reports pageCount as the BLOCK count (MemStats reuses one field name across
 * allocators; see ../include/mem_stats.h). Pool-only fields stay 0 thanks to
 * the memset. Note the early return: a NULL arena or out pointer leaves *out
 * completely untouched rather than zeroed, so callers must not read `out`
 * after passing a NULL arena. */
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
