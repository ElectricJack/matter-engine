// libs/MemoryLib/src/mem_pool.c
//
// Implementation of the fixed-size object pool declared in
// ../include/mem_pool.h. See that header for the public contract; this file
// documents the mechanism.
//
// Structure
// ---------
// The pool owns a flat array of page pointers (`pages`, doubling from an
// initial capacity of 10). Each page is one malloc of
// objectsPerPage * stride bytes, carved into equal-stride slots. Every free
// slot is threaded onto a single INTRUSIVE free list: the ObjectHeader that
// links a free slot to the next one lives inside the slot's own storage,
// which costs no side table but is exactly why the stride is forced up to at
// least sizeof(ObjectHeader) — a pool of 4-byte objects still spends a
// pointer's width per slot, and calculate_object_size then rounds that up to
// max_align_t alignment so every slot is suitably aligned for any type.
// `objectSize` in the struct is that PADDED stride, not the caller's
// requested size.
//
// Both alloc and free are O(1) pointer swaps, and reuse is LIFO (most
// recently freed comes back first), which is cache-friendly and is what
// libs/MemoryLib/main.c's test_reuse pins down.
//
// Pages are allocated lazily — a freshly created pool holds zero pages — and
// are never returned to the OS before mem_pool_destroy. Freeing every object
// restores the free list to full but keeps all the pages, so the pool's
// footprint is a high-water mark for the pool's lifetime.
//
// Safety posture: every entry point tolerates a NULL pool, out-of-memory is
// reported (NULL / 0) rather than fatal, but mem_pool_free performs NO
// ownership or double-free validation. Thread-confined — nothing here locks.
//
// Compiled directly from this path by MatterEngine3/Makefile,
// libs/SpatialQueryLib/Makefile, libs/MatterSurfaceLib/tests/Makefile and
// Prototypes/GPURayTraceExample/Makefile (the repo's no-copies rule).
#include "../include/mem_pool.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

// Intrusive free-list link. This overlays the FIRST bytes of a free slot's
// own storage rather than sitting beside it, so it costs nothing while a slot
// is in use — at the price of forcing the stride to at least sizeof(void*)
// and of mem_pool_alloc having to clear `next` before handing the slot out.
// Object header used for free list management
typedef struct ObjectHeader {
    struct ObjectHeader* next;
} ObjectHeader;

// The opaque handle behind MemPool.
//
// Field notes:
//   objectSize     - the PADDED stride in bytes, already run through
//                    calculate_object_size; NOT the size the caller asked
//                    for. All byte arithmetic in this file uses it.
//   pageCount      - pages actually malloc'd; 0 until the first alloc.
//   totalObjects   - slots carved from pages (pageCount * objectsPerPage),
//                    live and free alike — not a count of live objects.
//   freeObjects    - slots currently on the free list; live count is
//                    totalObjects - freeObjects.
//   freeList       - head of the intrusive LIFO list; NULL means the next
//                    alloc must allocate a page.
//   pages          - array of page base pointers, needed only so destroy can
//                    free them; it is never searched, which is why free()
//                    cannot validate that a pointer belongs to this pool.
//   pagesCapacity  - slots in the `pages` array, doubling from 10.
// MemPool implementation
struct MemPool {
    size_t objectSize;         /* stride (header-padded, max_align_t-aligned) */
    size_t objectsPerPage;
    size_t pageCount;
    size_t totalObjects;
    size_t freeObjects;
    size_t totalAllocs;        /* lifetime mem_pool_alloc count */
    size_t peakLiveBytes;      /* high-water mark of live bytes */
    ObjectHeader* freeList;
    void** pages;
    size_t pagesCapacity;
};

// Two-step rounding: first raise to at least sizeof(ObjectHeader) so a free
// slot can hold its own list link, then round up to max_align_t alignment so
// consecutive slots stay aligned for any type. Idempotent — feeding it an
// already-padded stride returns that stride unchanged, which is why
// allocate_page can safely re-apply it to the value create() already stored.
// Calculate the actual size needed for each object (including header)
static size_t calculate_object_size(size_t requested_size) {
    // Ensure the object is at least as large as the header
    size_t header_size = sizeof(ObjectHeader);
    size_t size = (requested_size > header_size) ? requested_size : header_size;

    // Align stride to max_align_t to ensure proper object alignment
    size_t al = _Alignof(max_align_t);
    size = (size + al - 1) / al * al;

    return size;
}

// Returns 1 on success, 0 on out-of-memory (either growing the `pages` array
// or malloc'ing the page itself); on failure the pool is left consistent and
// usable, just without the new page. Every slot in the fresh page is pushed
// onto the free list in ascending address order, so — the list being LIFO —
// the first allocations out of a new page walk it backwards. Called only from
// mem_pool_alloc, and only when the free list is empty.
// Allocate a new page of objects and add them to the free list
static int allocate_page(MemPool* allocator) {
    if (!allocator) {
        return 0;
    }
    
    // Check if we need to resize the pages array
    if (allocator->pageCount >= allocator->pagesCapacity) {
        size_t new_capacity = allocator->pagesCapacity * 2;
        void** new_pages = (void**)realloc(allocator->pages, new_capacity * sizeof(void*));
        if (!new_pages) {
            return 0;
        }
        allocator->pages = new_pages;
        allocator->pagesCapacity = new_capacity;
    }
    
    // Calculate the size of each object (including the header)
    size_t actual_size = calculate_object_size(allocator->objectSize);
    
    // Allocate a new page
    size_t page_size = actual_size * allocator->objectsPerPage;
    void* page = malloc(page_size);
    if (!page) {
        return 0;
    }
    
    // Add the page to the pages array
    allocator->pages[allocator->pageCount++] = page;
    
    // Initialize each object in the page and add it to the free list
    char* obj_ptr = (char*)page;
    for (size_t i = 0; i < allocator->objectsPerPage; i++) {
        ObjectHeader* obj = (ObjectHeader*)obj_ptr;
        
        // Add the object to the free list
        obj->next = allocator->freeList;
        allocator->freeList = obj;
        
        // Move to the next object
        obj_ptr += actual_size;
    }
    
    // Update statistics
    allocator->totalObjects += allocator->objectsPerPage;
    allocator->freeObjects += allocator->objectsPerPage;
    
    return 1;
}

// Create a new object allocator
MemPool* mem_pool_create(size_t objectSize, size_t objectsPerPage) {
    if (objectSize == 0 || objectsPerPage == 0) {
        return NULL;
    }
    
    // Allocate and initialize the object allocator
    MemPool* allocator = (MemPool*)malloc(sizeof(MemPool));
    if (!allocator) {
        return NULL;
    }
    
    // Calculate the actual object size (ensuring it's at least as large as the header)
    size_t actual_size = calculate_object_size(objectSize);
    
    // Initialize the allocator
    allocator->objectSize = actual_size;
    allocator->objectsPerPage = objectsPerPage;
    allocator->pageCount = 0;
    allocator->totalObjects = 0;
    allocator->freeObjects = 0;
    allocator->totalAllocs = 0;
    allocator->peakLiveBytes = 0;
    allocator->freeList = NULL;
    allocator->pagesCapacity = 10; // Initial capacity for pages array
    
    // Allocate pages array
    allocator->pages = (void**)malloc(allocator->pagesCapacity * sizeof(void*));
    if (!allocator->pages) {
        free(allocator);
        return NULL;
    }
    
    return allocator;
}

// Destroy an object allocator and free all associated memory
void mem_pool_destroy(MemPool* allocator) {
    if (!allocator) {
        return;
    }
    
    // Free all allocated pages
    for (size_t i = 0; i < allocator->pageCount; i++) {
        free(allocator->pages[i]);
    }
    
    // Free the pages array
    free(allocator->pages);
    
    // Free the allocator itself
    free(allocator);
}

// Pops the free list, growing by one page first if it is empty. Returns NULL
// only for a NULL pool or on out-of-memory. The returned storage is
// UNINITIALIZED apart from the first sizeof(ObjectHeader) bytes, which are
// zeroed to scrub the free-list link — the rest is deliberately left as the
// previous occupant wrote it (see the retained comment below). No constructor
// runs; this is raw storage.
// Allocate an object from the allocator
void* mem_pool_alloc(MemPool* allocator) {
    if (!allocator) {
        return NULL;
    }
    
    // If the free list is empty, allocate a new page
    if (!allocator->freeList) {
        if (!allocate_page(allocator)) {
            return NULL;
        }
    }
    
    // Remove the first object from the free list
    ObjectHeader* obj = allocator->freeList;
    allocator->freeList = obj->next;
    
    // Update statistics
    allocator->freeObjects--;
    allocator->totalAllocs++;
    {
        size_t live = (allocator->totalObjects - allocator->freeObjects) * allocator->objectSize;
        if (live > allocator->peakLiveBytes) allocator->peakLiveBytes = live;
    }

    // We want to keep the memory untouched for test_reuse to work properly
    // Clear only the header to avoid data leakage
    obj->next = NULL;
    
    return obj;
}

// Pushes the slot back onto the free list. `object` MUST be a pointer this
// same pool returned and must not already be free; neither is checked, and
// the pool has no way to check cheaply (see the `pages` note above), so a
// foreign pointer or a double free silently corrupts the list and will
// surface later as two live objects sharing one slot. NULL is a no-op. The
// page is not released, and peakLiveBytes is not revised downward.
// Free an object back to the allocator
void mem_pool_free(MemPool* allocator, void* object) {
    if (!allocator || !object) {
        return;
    }
    
    // Cast the object to an ObjectHeader
    ObjectHeader* obj = (ObjectHeader*)object;
    
    // Add the object back to the free list
    obj->next = allocator->freeList;
    allocator->freeList = obj;
    
    // Update statistics
    allocator->freeObjects++;
}

// Byte counts use the PADDED stride, so liveBytes/peakBytes exceed
// live_count * requested_objectSize whenever padding applied. peakLiveBytes
// is sampled in mem_pool_alloc only, which is sufficient because live bytes
// can only rise there. Early-returns without writing anything if `pool` or
// `out` is NULL — do not read `out` after such a call.
void mem_pool_get_stats(MemPool* pool, MemStats* out) {
    if (!pool || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->liveBytes = (pool->totalObjects - pool->freeObjects) * pool->objectSize;
    out->peakBytes = pool->peakLiveBytes;
    out->totalAllocs = pool->totalAllocs;
    out->pageCount = pool->pageCount;
    out->totalObjects = pool->totalObjects;
    out->freeObjects = pool->freeObjects;
}
