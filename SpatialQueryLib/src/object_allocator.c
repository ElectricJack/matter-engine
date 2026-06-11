#include "../include/object_allocator.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

// Object header used for free list management
typedef struct ObjectHeader {
    struct ObjectHeader* next;
} ObjectHeader;

// ObjectAllocator implementation
struct ObjectAllocator {
    size_t objectSize;         // Size of each object
    size_t objectsPerPage;     // Number of objects in each page
    size_t pageCount;          // Number of pages allocated
    size_t totalObjects;       // Total number of objects across all pages
    size_t freeObjects;        // Number of free objects available
    ObjectHeader* freeList;    // Linked list of free objects
    void** pages;              // Array of allocated pages
    size_t pagesCapacity;      // Capacity of the pages array
};

// Calculate the actual size needed for each object (including header)
static size_t calculate_object_size(size_t requested_size) {
    // Ensure the object is at least as large as the header
    size_t header_size = sizeof(ObjectHeader);
    return (requested_size > header_size) ? requested_size : header_size;
}

// Allocate a new page of objects and add them to the free list
static int allocate_page(ObjectAllocator* allocator) {
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
ObjectAllocator* oa_create(size_t objectSize, size_t objectsPerPage) {
    if (objectSize == 0 || objectsPerPage == 0) {
        return NULL;
    }
    
    // Allocate and initialize the object allocator
    ObjectAllocator* allocator = (ObjectAllocator*)malloc(sizeof(ObjectAllocator));
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
void oa_destroy(ObjectAllocator* allocator) {
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
void* oa_alloc(ObjectAllocator* allocator) {
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
    
    // We want to keep the memory untouched for test_reuse to work properly
    // Clear only the header to avoid data leakage
    obj->next = NULL;
    
    return obj;
}

// Free an object back to the allocator
void oa_free(ObjectAllocator* allocator, void* object) {
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

// Get statistics about the allocator's current state
void oa_get_stats(ObjectAllocator* allocator, size_t* pageCount, size_t* totalObjects, size_t* freeObjects) {
    if (!allocator || !pageCount || !totalObjects || !freeObjects) {
        return;
    }
    
    *pageCount = allocator->pageCount;
    *totalObjects = allocator->totalObjects;
    *freeObjects = allocator->freeObjects;
}