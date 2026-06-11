# Object Allocator Design

## Overview

The Object Allocator is a memory management system designed to efficiently allocate and deallocate objects of a fixed size. It uses a paged allocation strategy that grows as needed, minimizing fragmentation and improving performance for frequently allocated objects of the same type.

## Key Features

1. Fixed-size object allocation
2. Memory growth via pages
3. Fast allocation and deallocation
4. Minimal fragmentation
5. Thread-safety (optional)

## Data Structures

### ObjectAllocator

The main allocator structure that manages pages and tracks available objects.

```c
typedef struct ObjectAllocator {
    size_t objectSize;         // Size of each object
    size_t objectsPerPage;     // Number of objects in each page
    size_t pageCount;          // Number of pages allocated
    size_t totalObjects;       // Total number of objects across all pages
    size_t freeObjects;        // Number of free objects available
    void* freeList;            // Linked list of free objects
    void** pages;              // Array of allocated pages
} ObjectAllocator;
```

### Object Header

Each allocated object contains a header used for managing the free list.

```c
typedef struct ObjectHeader {
    struct ObjectHeader* next; // Next free object in the free list
} ObjectHeader;
```

## API

### Initialization and Cleanup

```c
// Initialize an object allocator with specified object size and objects per page
ObjectAllocator* oa_create(size_t objectSize, size_t objectsPerPage);

// Destroy an object allocator and free all allocated memory
void oa_destroy(ObjectAllocator* allocator);
```

### Memory Management

```c
// Allocate an object from the allocator
void* oa_alloc(ObjectAllocator* allocator);

// Free an object back to the allocator
void oa_free(ObjectAllocator* allocator, void* object);

// Get statistics about the allocator's current state
void oa_get_stats(ObjectAllocator* allocator, size_t* pageCount, size_t* totalObjects, size_t* freeObjects);
```

## Implementation Details

### Initialization

1. Allocate and initialize the `ObjectAllocator` structure
2. Set initial values for `objectSize`, `objectsPerPage`
3. Initialize empty `pages` array and `freeList`

### Allocation

1. Check if `freeList` is empty
   - If empty, allocate a new page and add objects to `freeList`
2. Remove the first object from `freeList`
3. Decrement `freeObjects`
4. Return the object to the caller

### Page Allocation

1. Allocate a new page of memory of size `objectSize * objectsPerPage`
2. Add the page to the `pages` array
3. Initialize each object in the page and add it to the `freeList`
4. Update `pageCount`, `totalObjects`, and `freeObjects`

### Deallocation

1. Add the object back to the `freeList`
2. Increment `freeObjects`

### Cleanup

1. Free all allocated pages
2. Free the `pages` array
3. Free the `ObjectAllocator` structure

## Usage Example

```c
// Create an allocator for 32-byte objects, 100 objects per page
ObjectAllocator* allocator = oa_create(32, 100);

// Allocate an object
void* obj1 = oa_alloc(allocator);
void* obj2 = oa_alloc(allocator);

// Use the objects...

// Free the objects when done
oa_free(allocator, obj1);
oa_free(allocator, obj2);

// Get statistics
size_t pageCount, totalObjects, freeObjects;
oa_get_stats(allocator, &pageCount, &totalObjects, &freeObjects);

// Clean up when finished
oa_destroy(allocator);
```

## Performance Considerations

1. The allocator is most efficient when:
   - Objects are of the same size
   - Allocation and deallocation happen frequently
   - Memory usage patterns involve many temporary objects

2. The page size (objectsPerPage) can be tuned:
   - Larger pages reduce fragmentation and allocation overhead
   - Smaller pages reduce memory waste for infrequently used allocators

3. Thread safety:
   - The basic implementation is not thread-safe
   - Thread safety can be added with mutexes around allocation/deallocation operations