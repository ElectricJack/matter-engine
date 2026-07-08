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
    /* totalAllocs and peakBytes are lifetime counters and survive reset;
       only liveBytes tracks the current cycle. */
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
