#ifndef MEM_STATS_H
#define MEM_STATS_H

/* libs/MemoryLib/include/mem_stats.h
 *
 * The common instrumentation record for every MemoryLib allocator. Each
 * allocator exposes one `*_get_stats(alloc, MemStats* out)` entry point
 * (mem_pool_get_stats, mem_arena_get_stats, mem_array_get_stats) so callers
 * can report pool/arena/array usage through a single struct.
 *
 * The first three fields mean the same thing everywhere; the last three are
 * type-specific extras and are left at 0 by the allocators they don't apply
 * to. Every getter memsets the whole struct first, so an unset field always
 * reads 0 rather than stale caller data.
 *
 * Note the per-allocator differences behind the shared names:
 * - `totalAllocs` is an alloc call count for the pool and arena, but a
 *   REALLOC count for the array (its growCount).
 * - `peakBytes` is a genuine recorded high-water mark for the pool and the
 *   arena (both survive mem_arena_reset), but for the array it is simply the
 *   current capacity in bytes, so it drops back to 0 after mem_array_free().
 * - Pool byte counts use the padded stride, not the requested object size.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MemStats {
    size_t liveBytes;    /* bytes currently handed out */
    size_t peakBytes;    /* high-water mark; array: current capacity in bytes */
    size_t totalAllocs;  /* lifetime alloc calls; array: realloc count */
    /* type-specific extras: */
    size_t pageCount;    /* pool: pages; arena: blocks; array: 0 */
    size_t totalObjects; /* pool only */
    size_t freeObjects;  /* pool only */
} MemStats;

#ifdef __cplusplus
}
#endif

#endif /* MEM_STATS_H */
