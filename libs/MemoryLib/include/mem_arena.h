#ifndef MEM_ARENA_H
#define MEM_ARENA_H

/* libs/MemoryLib/include/mem_arena.h
 *
 * Bump ("arena") allocator: an allocation is a pointer bump inside a malloc'd
 * block, and memory is reclaimed only in bulk by mem_arena_reset() or
 * mem_arena_destroy(). There is deliberately no per-allocation free.
 *
 * Where it fits
 * -------------
 * MemoryLib sits at the bottom of the dependency chain
 * (MatterEditor -> MatterEngine3 -> MatterSurfaceLib -> SpatialQueryLib ->
 * MemoryLib) and depends on nothing but libc. Consumers do not link an
 * archive; they add -I to this directory and compile
 * libs/MemoryLib/src/mem_arena.c from where it lives (see
 * libs/AssetStoreLib/tests/Makefile). libs/AssetStoreLib/include/asset_store.h
 * includes this header: an AssetStore ReadBatch lands its decoded blobs in a
 * caller-supplied arena. memory.hpp wraps the handle as the move-only RAII
 * type mem::Arena.
 *
 * Intended use is one arena per work cycle -- a bake, a rebuild, a frame.
 * Fill it with temporaries, then mem_arena_reset() at the end of the cycle.
 * Reset retains the largest block, so after the first few cycles the steady
 * state is zero malloc calls per cycle.
 *
 * Considerations
 * --------------
 * - Thread-confined. Nothing here takes a lock; one arena belongs to one
 *   thread. Workers should own their own arenas.
 * - Alignment is 8 bytes, and only 8 bytes. Do not place a type with a
 *   stricter alignment requirement in arena memory.
 * - Destructors are never run on arena memory, so it is for trivially
 *   destructible ("POD-ish") data only. mem::Arena::allocArray<T> enforces
 *   both this and the alignment rule at compile time.
 * - Pointers stay valid until reset or destroy. Growing the arena appends a
 *   new block; existing blocks are never moved or realloc'd, so nothing
 *   already handed out is invalidated.
 * - Nothing is zeroed. Allocations return whatever bytes were there before.
 * - Every entry point tolerates a NULL arena. mem_arena_alloc() also returns
 *   NULL for size 0 and on out-of-memory, so check the result.
 */

#include <stddef.h>
#include "mem_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bump allocator with bulk reset — no per-allocation free.
 * Thread-confined: one instance = one thread. */
typedef struct MemArena MemArena;

/* Returns NULL if initialCapacity is 0 or the first block cannot be malloc'd.
 * initialCapacity is a byte count and sizes the first block only; later
 * blocks grow 1.5x from the previous one. mem_arena_destroy() accepts NULL
 * and frees every block, live allocations included. */
MemArena* mem_arena_create(size_t initialCapacity);
void mem_arena_destroy(MemArena* a);
/* `size` is rounded up to a multiple of 8 before the bump, so the arena's
 * accounting charges the rounded size, not the requested one. Returns NULL
 * for a NULL arena, for size 0, and when a new block is needed but malloc
 * fails. The returned memory is uninitialized. */
void* mem_arena_alloc(MemArena* a, size_t size);   /* 8-byte aligned */
/* Bulk free. Every pointer previously returned by mem_arena_alloc() becomes
 * dangling. The single largest block is retained (and NOT zeroed) so the next
 * cycle usually runs without touching malloc. liveBytes goes back to 0;
 * totalAllocs and peakBytes are lifetime counters and survive the reset. */
void mem_arena_reset(MemArena* a);                 /* keeps largest block */
void mem_arena_get_stats(MemArena* a, MemStats* out);

#ifdef __cplusplus
}
#endif

#endif /* MEM_ARENA_H */
