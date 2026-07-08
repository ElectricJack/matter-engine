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
