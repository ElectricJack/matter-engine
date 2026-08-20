/* libs/MemoryLib/src/mem_array.c
 *
 * Implementation of the growable array declared in ../include/mem_array.h.
 * Deliberately thin: it is a realloc wrapper whose value is that the growth
 * policy, the overflow check and the out-of-memory behaviour are written once
 * here instead of being re-derived in every subsystem.
 *
 * Growth policy: new capacity is max(minCapacity, capacity * 3/2, 16)
 * elements. The 1.5x term amortises repeated pushes; taking minCapacity when
 * it is larger means one big ensure() jumps straight to the requested size
 * rather than stepping there 1.5x at a time; the floor of 16 keeps tiny
 * arrays from reallocating on every one of their first few pushes.
 *
 * Out-of-memory is a reported condition, never fatal: realloc's result is
 * checked before `data` is overwritten, so a failed grow leaves the array
 * exactly as it was and returns 0 / NULL. The `newCap > SIZE_MAX / elemSize`
 * test in mem_array_ensure is a real guard, not a formality -- without it the
 * byte count could wrap and produce an undersized buffer that then overflows
 * silently.
 *
 * Caveat inherited by every caller: growing reallocs, so any pointer returned
 * by an earlier mem_array_push (and any cached copy of `data`) is invalidated
 * by a subsequent push or ensure. Store indices, not pointers.
 *
 * Thread-confined; no locking. Except for mem_array_get_stats, the functions
 * here dereference `arr` unconditionally, so a NULL array is a crash rather
 * than a no-op -- unlike mem_arena.c and mem_pool.c, which tolerate NULL
 * handles throughout.
 */
#include "../include/mem_array.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Zeroes the descriptor in place. There is no create/destroy pair -- the
 * caller owns the MemArray value. This does NOT free a previous buffer, so
 * calling it on a live array leaks it; pair it with mem_array_free instead. */
void mem_array_init(MemArray* arr, size_t elemSize) {
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
    arr->elemSize = elemSize;
    arr->growCount = 0;
}

/* Grows capacity to at least minCapacity ELEMENTS (not bytes). Returns 1 on
 * success, including the no-op case where capacity is already sufficient, and
 * 0 on refusal -- either elemSize is 0 or the byte count would overflow
 * size_t -- or on realloc failure. In every failure case `data`, `count` and
 * `capacity` are left untouched, so the array remains usable.
 *
 * Capacity never shrinks; there is no reserve-down or shrink-to-fit. A
 * successful grow bumps growCount, which is what MemStats reports as
 * totalAllocs. */
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
    /* Guard against overflow: newCap * elemSize must not wrap size_t */
    if (arr->elemSize == 0 || newCap > SIZE_MAX / arr->elemSize) {
        return 0;
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

/* Appends one UNINITIALIZED slot and returns a pointer to it; the caller must
 * write it before reading. Returns NULL on out-of-memory (and at the SIZE_MAX
 * count ceiling) with `count` unchanged, so a failed push is not a partial
 * push. May realloc, invalidating every previously returned slot pointer. */
void* mem_array_push(MemArray* arr) {
    if (arr->count == SIZE_MAX) {
        return NULL;
    }
    if (arr->count == arr->capacity && !mem_array_ensure(arr, arr->count + 1)) {
        return NULL;
    }
    return (char*)arr->data + arr->count++ * arr->elemSize;
}

void mem_array_clear(MemArray* arr) {
    arr->count = 0;
}

/* Releases the buffer and resets the array to the empty state. elemSize is
 * preserved, so the array is immediately reusable without calling
 * mem_array_init again. Idempotent -- freeing twice is safe because `data` is
 * nulled. */
void mem_array_free(MemArray* arr) {
    free(arr->data);
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
    /* growCount is a lifetime counter; survives free (consistent with arena totalAllocs) */
}

/* The only NULL-tolerant function in this file. Note what the shared MemStats
 * field names mean here (see ../include/mem_stats.h): totalAllocs is the
 * REALLOC count, not the push count, and peakBytes is derived from the
 * current capacity rather than a recorded high-water mark -- so it drops back
 * to 0 after mem_array_free rather than remembering the peak, unlike the
 * arena's and the pool's peakBytes. pageCount is left 0; arrays have no
 * paging. */
void mem_array_get_stats(const MemArray* arr, MemStats* out) {
    if (!arr || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->liveBytes = arr->count * arr->elemSize;
    out->peakBytes = arr->capacity * arr->elemSize;   /* capacity never shrinks */
    out->totalAllocs = arr->growCount;
}
