#include "../include/mem_pool.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

// Object header used for free list management
typedef struct ObjectHeader {
    struct ObjectHeader* next;
} ObjectHeader;

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