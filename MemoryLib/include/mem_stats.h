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
