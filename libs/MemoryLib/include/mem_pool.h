#ifndef MEM_POOL_H
#define MEM_POOL_H

/* libs/MemoryLib/include/mem_pool.h
 *
 * Fixed-size object pool: one pool serves objects of exactly one size, carved
 * out of malloc'd pages and recycled through an intrusive free list.
 * Allocation and free are both O(1) pointer swaps, which is the point -- it
 * exists for the many-small-same-sized-objects case (spatial hash entries,
 * particles, BVH nodes) where per-object malloc would dominate.
 *
 * Where it fits
 * -------------
 * MemoryLib is the bottom of the dependency chain
 * (MatterEditor -> MatterEngine3 -> MatterSurfaceLib -> SpatialQueryLib ->
 * MemoryLib) and depends on nothing but libc. This is the most widely
 * consumed allocator in the repo: MatterEngine3/Makefile,
 * libs/SpatialQueryLib/Makefile, libs/MatterSurfaceLib/tests/Makefile and
 * Prototypes/GPURayTraceExample/Makefile all compile
 * libs/MemoryLib/src/mem_pool.c directly from this directory (the house rule
 * -- no copies, no symlinks). libs/SpatialQueryLib/src/spatial_hash.c is the
 * main in-engine caller. memory.hpp wraps the handle as mem::Pool.
 *
 * Considerations
 * --------------
 * - Thread-confined. No locking; one pool belongs to one thread. Workers
 *   should own their own pools.
 * - The free list is intrusive: a free object's own storage holds the "next"
 *   pointer. That is why the effective stride is raised to at least
 *   sizeof(void*) and then padded up to max_align_t alignment, so the pool's
 *   memory footprint can exceed objectSize * objectCount.
 * - Pages are allocated lazily -- a freshly created pool owns no pages, and
 *   stats report pageCount 0 until the first mem_pool_alloc().
 * - Memory is never returned to the OS before mem_pool_destroy(). Freeing
 *   every object empties the free list back to full but keeps all pages.
 * - Objects are handed out uninitialized (except the first sizeof(void*)
 *   bytes, which alloc clears) and are not destructed -- this is plain C
 *   storage, not object lifetime management.
 * - mem_pool_free() does no ownership or double-free validation. Passing a
 *   pointer that did not come from this pool, or freeing twice, silently
 *   corrupts the free list.
 * - Every entry point tolerates NULL. mem_pool_alloc() returns NULL on
 *   out-of-memory, so check the result.
 */

#include <stddef.h>
#include "mem_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-size object pool. Thread-confined: one instance = one thread. */
typedef struct MemPool MemPool;

/* objectSize is in bytes and is the caller's object size; the pool rounds it
 * up internally (to at least sizeof(void*), then to max_align_t alignment) to
 * get the stride it actually uses. objectsPerPage is how many objects one
 * malloc'd page holds -- it is the allocation granularity, so bigger means
 * fewer mallocs and more slack. Returns NULL if either argument is 0 or on
 * out-of-memory. No pages are allocated until the first alloc.
 *
 * mem_pool_destroy() accepts NULL. It frees every page regardless of whether
 * objects in them are still live, and runs no destructors. */
MemPool* mem_pool_create(size_t objectSize, size_t objectsPerPage);
void mem_pool_destroy(MemPool* pool);
/* alloc pops the free list, allocating a fresh page first if it is empty;
 * returns NULL only on out-of-memory. Reuse is LIFO -- the most recently
 * freed object comes back first -- and the memory is otherwise left as it
 * was, so treat it as uninitialized.
 *
 * free pushes the object back onto the free list. `object` must be a pointer
 * this same pool returned and must not already be free; neither is checked.
 * The page it lives in is not released. Passing NULL is a no-op. */
void* mem_pool_alloc(MemPool* pool);
void mem_pool_free(MemPool* pool, void* object);
/* Fills every MemStats field. liveBytes and peakBytes are computed from the
 * PADDED stride, not the requested objectSize, so they exceed the caller's
 * notion of object bytes. totalObjects counts slots carved from pages
 * (pageCount * objectsPerPage), not live objects. All zero on a pool that has
 * never allocated. Tolerates NULL pool/out. */
void mem_pool_get_stats(MemPool* pool, MemStats* out);

#ifdef __cplusplus
}
#endif

#endif /* MEM_POOL_H */
