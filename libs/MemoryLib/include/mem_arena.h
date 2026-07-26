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
