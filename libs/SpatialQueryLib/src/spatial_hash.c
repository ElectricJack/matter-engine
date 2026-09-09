// libs/SpatialQueryLib/src/spatial_hash.c
//
// Implementation of `include/spatial_hash.h`. That header owns the public
// contract -- what is stored, what is owned, what the query functions promise
// and where they truncate. This file documents the internal structure.
//
// Structure: a fixed array of `Bucket`s, each a singly-linked list of
// `BucketEntry`. An entry holds the caller's `void*`, the exact float position
// it was inserted at, AND the `GridCoord` cell derived from that position.
// Storing the cell is what makes the dedup filter in every query possible: when
// two distinct cells hash to the same bucket, scanning that bucket for cell C
// only emits entries whose stored coord IS C, so no object is ever reported
// twice and no foreign-cell object leaks into a cell's result.
//
// The bucket count is always a power of two, established in `sh_create`, so
// `hash_coord` can mask instead of taking a modulo. Everything downstream
// assumes that -- `mask = bucketCount - 1` is computed at each call site.
//
// Growth: the entry `MemPool` grows on demand, but the BUCKET TABLE NEVER
// RESIZES AND NOTHING IS EVER REHASHED. A hash loaded far past its
// `initialCapacity` degrades gracefully into long chains rather than failing;
// `sh_get_stats`' `maxBucketSize` is the way to notice that has happened.
//
// Insertion is at the HEAD of a bucket, so chain order is reverse insertion
// order. That is what makes `sh_query_first` return the most recently inserted
// matching object within a cell, and it is why "first" is not a stable notion
// across a differently-ordered fill.
//
// Cells are half-open: `floorf(x / cellSize)` puts a point exactly on a
// boundary into the higher cell, and negative coordinates floor toward minus
// infinity (not toward zero), so the grid is uniform across the origin.
//
// Dependencies: `mem_pool.h` from `libs/MemoryLib` and the C standard library.
// Nothing else -- this is the bottom of the dependency chain.
//
// No locking anywhere. Concurrent readers are safe only while no writer runs.
#include "../include/spatial_hash.h"
#include "mem_pool.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define INITIAL_BUCKET_CAPACITY 8
#define MIN_BUCKET_COUNT 1024

// 3D grid coordinates
typedef struct {
    int x, y, z;
} GridCoord;

// Bucket entry - stores objects at a specific position
// One stored object. `x/y/z` is the position EXACTLY as passed to sh_insert and
// is authoritative for both the distance tests and for `sh_remove`'s match (see
// the header on exact-float matching). `coord` is that position's cell,
// remembered rather than recomputed so queries can filter out entries that
// merely collided into the same bucket from a different cell.
typedef struct BucketEntry {
    void* object;
    float x, y, z;
    GridCoord coord;       /* grid cell this entry was inserted into */
    struct BucketEntry* next;
} BucketEntry;

// Bucket - contains linked list of objects
typedef struct {
    BucketEntry* head;
    int count;
} Bucket;

// SpatialHash structure
// The opaque handle. `bucketCount` is always a power of two (see sh_create) and
// is fixed for the object's lifetime; `cellSize` is likewise fixed and is
// guaranteed > 0 by sh_create, which is what makes the division in
// `world_to_grid` safe everywhere. `totalObjects` counts entries, not distinct
// pointers -- the same pointer inserted twice counts twice.
struct SpatialHash {
    Bucket* buckets;
    int bucketCount;
    float cellSize;
    int totalObjects;
    MemPool* entryAllocator;
};

/* Compute the next power of two >= n (n must be > 0) */
static unsigned int next_pow2(unsigned int n) {
    if (n <= 1) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

/* Hash function for 3D grid coordinates.
 * All multiplications are performed in unsigned int to avoid signed-integer
 * overflow UB (C11 6.5p5). */
static unsigned int hash_coord(GridCoord coord, unsigned int mask) {
    unsigned int h = ((unsigned int)coord.x * 73856093u)
                   ^ ((unsigned int)coord.y * 19349663u)
                   ^ ((unsigned int)coord.z * 83492791u);
    return h & mask;
}

// Convert world position to grid coordinates
static GridCoord world_to_grid(float x, float y, float z, float cellSize) {
    GridCoord coord;
    coord.x = (int)floorf(x / cellSize);
    coord.y = (int)floorf(y / cellSize);
    coord.z = (int)floorf(z / cellSize);
    return coord;
}

// Create a new spatial hash table
// Allocate the handle, the bucket table and the entry pool.
//
// The bucket count is derived, not given: next_pow2(initialCapacity / 4),
// floored at 1024. So the table is sized for roughly four objects per bucket at
// the stated capacity, and a small hash still pays for 1024 buckets. It is
// fixed from here on -- see the file header on growth.
//
// Every allocation failure unwinds what was already taken and returns NULL, so
// a failed create leaks nothing. NULL is also returned for cellSize <= 0 or
// initialCapacity <= 0, which is what lets every other function assume a
// positive cell size.
SpatialHash* sh_create(float cellSize, int initialCapacity) {
    if (cellSize <= 0.0f || initialCapacity <= 0) {
        return NULL;
    }

    SpatialHash* hash = (SpatialHash*)malloc(sizeof(SpatialHash));
    if (!hash) return NULL;

    /* Size the bucket table from initialCapacity:
     * next power-of-two >= capacity/4, with a minimum of MIN_BUCKET_COUNT. */
    unsigned int desired = next_pow2((unsigned int)initialCapacity / 4u);
    if (desired < MIN_BUCKET_COUNT) desired = MIN_BUCKET_COUNT;

    hash->buckets = (Bucket*)calloc(desired, sizeof(Bucket));
    if (!hash->buckets) {
        free(hash);
        return NULL;
    }

    hash->bucketCount = (int)desired;
    hash->cellSize = cellSize;
    hash->totalObjects = 0;

    // Create allocator for bucket entries
    hash->entryAllocator = mem_pool_create(sizeof(BucketEntry), initialCapacity);
    if (!hash->entryAllocator) {
        free(hash->buckets);
        free(hash);
        return NULL;
    }

    return hash;
}

// Destroy a spatial hash table and free all memory
// Frees the pool, the bucket table and the handle. The `sh_clear` call first is
// bookkeeping, not a requirement for correctness: the entries live inside the
// MemPool and `mem_pool_destroy` releases their storage wholesale. The stored
// objects themselves are never touched -- the hash has only ever held pointers.
void sh_destroy(SpatialHash* hash) {
    if (!hash) return;

    // Clear all buckets first
    sh_clear(hash);

    // Destroy allocator
    if (hash->entryAllocator) {
        mem_pool_destroy(hash->entryAllocator);
    }

    // Free buckets array
    free(hash->buckets);
    free(hash);
}

// Clear all objects from the hash table
void sh_clear(SpatialHash* hash) {
    if (!hash) return;

    // Free all bucket entries by clearing each bucket
    for (int i = 0; i < hash->bucketCount; i++) {
        BucketEntry* entry = hash->buckets[i].head;
        while (entry) {
            BucketEntry* next = entry->next;
            mem_pool_free(hash->entryAllocator, entry);
            entry = next;
        }
        hash->buckets[i].head = NULL;
        hash->buckets[i].count = 0;
    }

    hash->totalObjects = 0;
}

// Insert an object at the given position
// O(1): derive the cell, take an entry from the pool, prepend it to the bucket.
// Returns false for a NULL hash or object, or when the pool cannot allocate.
//
// No duplicate detection and no position update: inserting the same pointer
// again adds a second, independent entry. Moving an object means `sh_remove` at
// its OLD position followed by `sh_insert` at the new one -- there is no
// reposition operation, and an entry left at a stale position will keep being
// returned from queries around that stale point.
bool sh_insert(SpatialHash* hash, float x, float y, float z, void* object) {
    if (!hash || !object) return false;

    // Convert position to grid coordinates
    GridCoord coord = world_to_grid(x, y, z, hash->cellSize);

    // Calculate bucket index
    unsigned int mask = (unsigned int)hash->bucketCount - 1u;
    unsigned int bucketIndex = hash_coord(coord, mask);

    // Create new entry
    BucketEntry* entry = (BucketEntry*)mem_pool_alloc(hash->entryAllocator);
    if (!entry) return false;

    entry->object = object;
    entry->x = x;
    entry->y = y;
    entry->z = z;
    entry->coord = coord;
    entry->next = hash->buckets[bucketIndex].head;

    // Insert at head of bucket
    hash->buckets[bucketIndex].head = entry;
    hash->buckets[bucketIndex].count++;
    hash->totalObjects++;

    return true;
}

// Remove an object from the given position
// Remove at most ONE entry matching both the pointer and all three coordinates
// by exact float equality. The passed position also selects which bucket is
// searched, so a position that differs from the inserted one -- even by a
// single ulp from being recomputed rather than remembered -- usually looks in
// the wrong bucket and always fails the equality test. The entry then survives
// until `sh_clear`. Callers that cannot guarantee bit-identical positions
// should rebuild the hash instead of removing from it.
//
// Returns false for "not found", which is indistinguishable from a NULL
// argument. O(chain length).
bool sh_remove(SpatialHash* hash, float x, float y, float z, void* object) {
    if (!hash || !object) return false;

    // Convert position to grid coordinates
    GridCoord coord = world_to_grid(x, y, z, hash->cellSize);

    // Calculate bucket index
    unsigned int mask = (unsigned int)hash->bucketCount - 1u;
    unsigned int bucketIndex = hash_coord(coord, mask);

    // Search for the object in the bucket
    BucketEntry** current = &hash->buckets[bucketIndex].head;
    while (*current) {
        BucketEntry* entry = *current;
        if (entry->object == object &&
            entry->x == x && entry->y == y && entry->z == z) {
            // Remove from linked list
            *current = entry->next;
            mem_pool_free(hash->entryAllocator, entry);
            hash->buckets[bucketIndex].count--;
            hash->totalObjects--;
            return true;
        }
        current = &entry->next;
    }

    return false; // Object not found
}

// Query objects within a radius of the given position
// Sphere query. Walks every grid cell overlapping the query AABB, filters each
// bucket to the cell being scanned (the dedup strategy documented below), and
// keeps points inside the radius by exact centre distance.
//
// It BAILS as soon as `maxResults` is reached -- the three `break`s unwind all
// three loops -- so a truncated result is the first N in grid-scan order, not
// the nearest N. That early exit is the only difference in cost profile from
// `sh_query_radius_nearest`, which cannot take it.
//
// Cost is the cell count of the AABB, i.e. roughly (2*radius/cellSize)^3
// buckets touched, so a radius well above `cellSize` is expensive even when it
// finds nothing.
int sh_query_radius(SpatialHash* hash, float x, float y, float z, float radius,
                    void** results, int maxResults) {
    if (!hash || !results || maxResults <= 0) return 0;

    int found = 0;
    float radiusSq = radius * radius;
    unsigned int mask = (unsigned int)hash->bucketCount - 1u;

    /* Iterate exactly the grid cells overlapping the query AABB [p-r, p+r].
     * Flooring the corner coords (rather than centerCoord +/- ceil(r/cellSize))
     * covers the same cells for any cell size while scanning the tightest
     * possible range. */
    GridCoord lo = world_to_grid(x - radius, y - radius, z - radius, hash->cellSize);
    GridCoord hi = world_to_grid(x + radius, y + radius, z + radius, hash->cellSize);

    /*
     * Dedup strategy: each BucketEntry stores the GridCoord it was inserted
     * into.  When scanning a bucket for logical cell C we only process entries
     * whose stored coord equals C.  This means:
     *   - When cell A and cell B both hash to bucket B0, scanning bucket B0
     *     for cell A only emits entries that genuinely belong to A; scanning
     *     it again for cell B only emits entries that genuinely belong to B.
     *   - No object is counted twice regardless of how many cells collide into
     *     the same bucket.
     */
    for (int gx = lo.x; gx <= hi.x; gx++) {
        for (int gy = lo.y; gy <= hi.y; gy++) {
            for (int gz = lo.z; gz <= hi.z; gz++) {
                GridCoord coord = {gx, gy, gz};
                unsigned int bucketIndex = hash_coord(coord, mask);

                // Check all objects in this bucket; only include entries whose
                // stored cell coord matches the currently-queried cell C.
                BucketEntry* entry = hash->buckets[bucketIndex].head;
                while (entry && found < maxResults) {
                    if (entry->coord.x == coord.x &&
                        entry->coord.y == coord.y &&
                        entry->coord.z == coord.z) {
                        float fdx = entry->x - x;
                        float fdy = entry->y - y;
                        float fdz = entry->z - z;
                        float distSq = fdx*fdx + fdy*fdy + fdz*fdz;

                        if (distSq <= radiusSq) {
                            results[found++] = entry->object;
                        }
                    }

                    entry = entry->next;
                }

                if (found >= maxResults) break;
            }
            if (found >= maxResults) break;
        }
        if (found >= maxResults) break;
    }

    return found;
}

// Query the NEAREST objects within a radius of the given position
// K-nearest-within-radius. Same cell walk and same dedup as `sh_query_radius`,
// but it CANNOT stop early -- it must see every candidate to know which are
// closest -- so it is strictly the more expensive of the two.
//
// The kept set is an unsorted array of at most `maxResults` with a
// `worstSlot` cursor: once full, a closer candidate evicts the current farthest
// and the cursor is re-found by a linear rescan. That is O(k) per eviction,
// fine for the k in the tens this is used with, quadratic-ish if k ever grows.
// Results are therefore the nearest k but in NO particular order.
//
// The parallel distance array lives on the stack up to 256 entries and heap
// beyond. Note the failure mode: a failed heap allocation returns 0, which is
// indistinguishable from "nothing found".
int sh_query_radius_nearest(SpatialHash* hash, float x, float y, float z, float radius,
                            void** results, int maxResults) {
    if (!hash || !results || maxResults <= 0) return 0;

    float radiusSq = radius * radius;
    unsigned int mask = (unsigned int)hash->bucketCount - 1u;

    /* Same tightest-AABB cell range as sh_query_radius. */
    GridCoord lo = world_to_grid(x - radius, y - radius, z - radius, hash->cellSize);
    GridCoord hi = world_to_grid(x + radius, y + radius, z + radius, hash->cellSize);

    /* Keep the maxResults nearest candidates. distSq[i] parallels results[i].
     * worstSlot tracks the kept entry with the largest distSq so a closer
     * candidate can evict it once the buffer is full. maxResults is small
     * (tens), so the linear worst-slot rescan on eviction is cheap. */
    float distSqStack[256];
    float* distSq = distSqStack;
    float* distSqHeap = NULL;
    if (maxResults > 256) {
        distSqHeap = (float*)malloc((size_t)maxResults * sizeof(float));
        if (!distSqHeap) return 0;
        distSq = distSqHeap;
    }
    int found = 0;
    int worstSlot = 0;

    /* Dedup via the stored per-entry GridCoord, exactly as sh_query_radius
     * does: cells that collide into one bucket never emit each other's
     * objects, so no candidate is weighed twice against the nearest set. */
    for (int gx = lo.x; gx <= hi.x; gx++) {
        for (int gy = lo.y; gy <= hi.y; gy++) {
            for (int gz = lo.z; gz <= hi.z; gz++) {
                GridCoord coord = {gx, gy, gz};
                unsigned int bucketIndex = hash_coord(coord, mask);

                BucketEntry* entry = hash->buckets[bucketIndex].head;
                while (entry) {
                    if (entry->coord.x == coord.x &&
                        entry->coord.y == coord.y &&
                        entry->coord.z == coord.z) {
                        float ex = entry->x - x;
                        float ey = entry->y - y;
                        float ez = entry->z - z;
                        float d2 = ex*ex + ey*ey + ez*ez;

                        if (d2 <= radiusSq) {
                            if (found < maxResults) {
                                results[found] = entry->object;
                                distSq[found] = d2;
                                found++;
                                if (found == maxResults) {
                                    /* Buffer just filled: find the farthest. */
                                    worstSlot = 0;
                                    for (int i = 1; i < found; i++)
                                        if (distSq[i] > distSq[worstSlot]) worstSlot = i;
                                }
                            } else if (d2 < distSq[worstSlot]) {
                                /* Replace the farthest kept entry, re-find it. */
                                results[worstSlot] = entry->object;
                                distSq[worstSlot] = d2;
                                worstSlot = 0;
                                for (int i = 1; i < maxResults; i++)
                                    if (distSq[i] > distSq[worstSlot]) worstSlot = i;
                            }
                        }
                    }

                    entry = entry->next;
                }
            }
        }
    }

    if (distSqHeap) free(distSqHeap);
    return found;
}

// Query objects within a bounding box
// Axis-aligned box query, bounds INCLUSIVE on both ends. Same cell walk and
// dedup as the radius queries, and the same early bail at `maxResults` -- so a
// truncated result is an arbitrary subset in grid-scan order. A caller that
// needs the whole set must size `results` for the worst case and check
// `found < maxResults` to know it got everything.
//
// The cell range comes from flooring the two corners, which covers every cell
// the box touches without needing a ceil on the far side because cells are
// half-open.
int sh_query_box(SpatialHash* hash, float minX, float minY, float minZ,
                 float maxX, float maxY, float maxZ, void** results, int maxResults) {
    if (!hash || !results || maxResults <= 0) return 0;

    int found = 0;
    unsigned int mask = (unsigned int)hash->bucketCount - 1u;

    // Calculate the range of grid cells to check
    GridCoord minCoord = world_to_grid(minX, minY, minZ, hash->cellSize);
    GridCoord maxCoord = world_to_grid(maxX, maxY, maxZ, hash->cellSize);

    // Check all cells in the bounding box
    for (int cx = minCoord.x; cx <= maxCoord.x; cx++) {
        for (int cy = minCoord.y; cy <= maxCoord.y; cy++) {
            for (int cz = minCoord.z; cz <= maxCoord.z; cz++) {
                GridCoord coord = {cx, cy, cz};
                unsigned int bucketIndex = hash_coord(coord, mask);

                // Check all objects in this bucket; coord filter deduplicates
                // entries that collide into this bucket from other cells.
                BucketEntry* entry = hash->buckets[bucketIndex].head;
                while (entry && found < maxResults) {
                    if (entry->coord.x == coord.x &&
                        entry->coord.y == coord.y &&
                        entry->coord.z == coord.z) {
                        // Check if object is within the bounding box
                        if (entry->x >= minX && entry->x <= maxX &&
                            entry->y >= minY && entry->y <= maxY &&
                            entry->z >= minZ && entry->z <= maxZ) {
                            results[found++] = entry->object;
                        }
                    }

                    entry = entry->next;
                }

                if (found >= maxResults) break;
            }
            if (found >= maxResults) break;
        }
        if (found >= maxResults) break;
    }

    return found;
}

// Get statistics about the hash table
// Diagnostics. Every out-parameter is optional; a NULL `hash` writes NOTHING at
// all, so initialise your locals before calling.
//
// `maxBucketSize` scans the entire bucket table (at least 1024 entries), so it
// is the expensive one -- pass NULL for it unless you are actually checking
// chain-length degradation, which is what it is for: the table never rehashes,
// so a high max against a modest `loadFactor` means the hash is clustered, not
// merely full.
void sh_get_stats(SpatialHash* hash, int* bucketCount, int* objectCount,
                  int* maxBucketSize, float* loadFactor) {
    if (!hash) return;

    if (bucketCount) *bucketCount = hash->bucketCount;
    if (objectCount) *objectCount = hash->totalObjects;

    if (maxBucketSize) {
        int max = 0;
        for (int i = 0; i < hash->bucketCount; i++) {
            if (hash->buckets[i].count > max) {
                max = hash->buckets[i].count;
            }
        }
        *maxBucketSize = max;
    }

    if (loadFactor) {
        *loadFactor = (float)hash->totalObjects / (float)hash->bucketCount;
    }
}

// Query for the first object within a radius (optimized for single-object lookups)
// Return any one object within `radius`, or NULL. "First" is grid-scan order
// across cells and then bucket-chain order within one, which is REVERSE
// insertion order -- so it is neither the nearest nor stable if the same
// objects are inserted in a different sequence.
//
// It short-circuits on the first hit, so for a radius under one cell this is
// close to a single bucket walk. That is exactly the intended use: an occupancy
// probe, not a query whose answer you care about the identity of.
//
// NULL means both "nothing in range" and "NULL hash".
void* sh_query_first(SpatialHash* hash, float x, float y, float z, float radius) {
    if (!hash) return NULL;

    float radiusSq = radius * radius;
    unsigned int mask = (unsigned int)hash->bucketCount - 1u;

    /* Tightest AABB cell range, as in sh_query_radius. */
    GridCoord lo = world_to_grid(x - radius, y - radius, z - radius, hash->cellSize);
    GridCoord hi = world_to_grid(x + radius, y + radius, z + radius, hash->cellSize);

    for (int gx = lo.x; gx <= hi.x; gx++) {
        for (int gy = lo.y; gy <= hi.y; gy++) {
            for (int gz = lo.z; gz <= hi.z; gz++) {
                GridCoord coord = {gx, gy, gz};
                unsigned int bucketIndex = hash_coord(coord, mask);

                // Check all objects in this bucket; coord filter skips
                // foreign-cell entries that collide into this bucket.
                BucketEntry* entry = hash->buckets[bucketIndex].head;
                while (entry) {
                    if (entry->coord.x == coord.x &&
                        entry->coord.y == coord.y &&
                        entry->coord.z == coord.z) {
                        float fdx = entry->x - x;
                        float fdy = entry->y - y;
                        float fdz = entry->z - z;
                        float distSq = fdx*fdx + fdy*fdy + fdz*fdz;

                        if (distSq <= radiusSq) {
                            return entry->object; // Return first match
                        }
                    }

                    entry = entry->next;
                }
            }
        }
    }

    return NULL; // No object found
}
