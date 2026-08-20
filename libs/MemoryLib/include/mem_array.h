#ifndef MEM_ARRAY_H
#define MEM_ARRAY_H

/* libs/MemoryLib/include/mem_array.h
 *
 * Growable array of fixed-size elements over realloc -- MemoryLib's single
 * blessed "grow a buffer" idiom, so subsystems stop hand-rolling their own.
 * Element type is erased: the array stores a byte count per element and hands
 * back void*, so the caller casts.
 *
 * Where it fits
 * -------------
 * MemoryLib is the bottom of the dependency chain and depends only on libc.
 * Unlike mem_pool (compiled into MatterEngine3, SpatialQueryLib and others)
 * and mem_arena (used by AssetStoreLib), mem_array currently has no consumers
 * outside MemoryLib's own tests -- it is here as the shared idiom for new
 * code rather than as a retrofit of existing code.
 *
 * How to use it
 * -------------
 *   MemArray a;
 *   mem_array_init(&a, sizeof(Thing));   // required before anything else
 *   Thing* t = (Thing*)mem_array_push(&a);
 *   if (!t) { ... out of memory ... }
 *   *t = ...;
 *   mem_array_free(&a);
 *
 * Considerations
 * --------------
 * - Thread-confined. No locking; one array belongs to one thread.
 * - Growth is realloc-based, so ANY pointer previously returned by
 *   mem_array_push() (and `data` itself) is invalidated by a grow. Store
 *   indices, not pointers, if you keep references across pushes.
 * - Out-of-memory is reported, never fatal: mem_array_ensure() returns 0 and
 *   mem_array_push() returns NULL, and in both cases the existing contents
 *   are left intact.
 * - Growth policy is max(minCapacity, capacity * 3/2, 16) elements, so a
 *   single large ensure() jumps straight to the requested capacity instead of
 *   stepping there 1.5x at a time.
 * - New slots are NOT zeroed; mem_array_push() returns uninitialized bytes.
 * - Every function tolerates a NULL `arr`, matching the mem_arena and
 *   mem_pool APIs: the void-returning ones do nothing, mem_array_ensure()
 *   returns 0 and mem_array_push() returns NULL.
 */

#include <stddef.h>
#include "mem_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-owned value type -- declare it on the stack or embed it in your own
 * struct; there is no create/destroy pair. It is trivially copyable in the
 * language sense but NOT safe to copy in practice: two MemArrays sharing one
 * `data` pointer will double-free it. Move by assigning and then zeroing the
 * source, or just don't copy.
 *
 * Between calls the invariants are: count <= capacity, and `data` is either
 * NULL (capacity 0) or a block of capacity * elemSize bytes. */
/* Growable array (heap-backed). Thread-confined. */
typedef struct MemArray {
    void*  data;        /* NULL until the first successful grow */
    size_t count;       /* elements in use, not bytes */
    size_t capacity;    /* in elements */
    size_t elemSize;    /* bytes per element; set once by mem_array_init */
    size_t growCount;   /* number of reallocs; reported as stats totalAllocs */
} MemArray;

/* Zero-initializes the array in place. Must be called before any other
 * function; it does not free a previous buffer, so calling it on a live array
 * leaks it. elemSize is a byte count and is fixed for the array's lifetime.
 * mem_array_free() leaves the array reusable without re-init. */
void  mem_array_init(MemArray* arr, size_t elemSize);
int   mem_array_ensure(MemArray* arr, size_t minCapacity);  /* 1 ok, 0 NULL/OOM (data intact) */
/* Appends one uninitialized slot and returns a pointer to it. May realloc,
 * which invalidates every pointer previously returned by push and any cached
 * copy of `data`. Returns NULL for a NULL array and on out-of-memory (count
 * is left unchanged, so the array is still usable) -- callers must check
 * before writing. */
void* mem_array_push(MemArray* arr);                        /* new slot, NULL on OOM */
void  mem_array_clear(MemArray* arr);                       /* count=0, keeps capacity */
void  mem_array_free(MemArray* arr);
/* liveBytes = count * elemSize, peakBytes = capacity * elemSize, and
 * totalAllocs is the realloc count (growCount), not the push count. peakBytes
 * is derived from the CURRENT capacity rather than a recorded high-water
 * mark, so it is only meaningful while the buffer is alive: after
 * mem_array_free() it reports 0 -- it is a live capacity reading, not a
 * high-water mark, unlike the arena's and the pool's peakBytes. pageCount,
 * totalObjects and freeObjects are pool-only and are zeroed here. A NULL arr
 * or out leaves *out completely untouched -- do not read it after such a
 * call. */
void  mem_array_get_stats(const MemArray* arr, MemStats* out);

#ifdef __cplusplus
}
#endif

#endif /* MEM_ARRAY_H */
