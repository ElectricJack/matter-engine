#ifndef OBJECT_ALLOCATOR_H
#define OBJECT_ALLOCATOR_H

#include <stddef.h>

/**
 * ObjectAllocator - A memory management system for fixed-size objects
 */
typedef struct ObjectAllocator ObjectAllocator;

/**
 * Creates a new object allocator
 * 
 * @param objectSize Size of each object to allocate (in bytes)
 * @param objectsPerPage Number of objects to allocate per page
 * @return A new object allocator, or NULL if creation failed
 */
ObjectAllocator* oa_create(size_t objectSize, size_t objectsPerPage);

/**
 * Destroys an object allocator and frees all associated memory
 * 
 * @param allocator The allocator to destroy
 */
void oa_destroy(ObjectAllocator* allocator);

/**
 * Allocates an object from the allocator
 * 
 * @param allocator The allocator to use
 * @return A pointer to a new object, or NULL if allocation failed
 */
void* oa_alloc(ObjectAllocator* allocator);

/**
 * Frees an object back to the allocator
 * 
 * @param allocator The allocator the object was allocated from
 * @param object The object to free
 */
void oa_free(ObjectAllocator* allocator, void* object);

/**
 * Gets statistics about the allocator's current state
 * 
 * @param allocator The allocator to query
 * @param pageCount Output parameter for the number of pages allocated
 * @param totalObjects Output parameter for the total number of objects
 * @param freeObjects Output parameter for the number of free objects
 */
void oa_get_stats(ObjectAllocator* allocator, size_t* pageCount, size_t* totalObjects, size_t* freeObjects);

#endif /* OBJECT_ALLOCATOR_H */