#include "../include/spatial_hash.h"
#include "../include/object_allocator.h"
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
struct SpatialHash {
    Bucket* buckets;
    int bucketCount;
    float cellSize;
    int totalObjects;
    ObjectAllocator* entryAllocator;
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
    hash->entryAllocator = oa_create(sizeof(BucketEntry), initialCapacity);
    if (!hash->entryAllocator) {
        free(hash->buckets);
        free(hash);
        return NULL;
    }

    return hash;
}

// Destroy a spatial hash table and free all memory
void sh_destroy(SpatialHash* hash) {
    if (!hash) return;

    // Clear all buckets first
    sh_clear(hash);

    // Destroy allocator
    if (hash->entryAllocator) {
        oa_destroy(hash->entryAllocator);
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
            oa_free(hash->entryAllocator, entry);
            entry = next;
        }
        hash->buckets[i].head = NULL;
        hash->buckets[i].count = 0;
    }

    hash->totalObjects = 0;
}

// Insert an object at the given position
bool sh_insert(SpatialHash* hash, float x, float y, float z, void* object) {
    if (!hash || !object) return false;

    // Convert position to grid coordinates
    GridCoord coord = world_to_grid(x, y, z, hash->cellSize);

    // Calculate bucket index
    unsigned int mask = (unsigned int)hash->bucketCount - 1u;
    unsigned int bucketIndex = hash_coord(coord, mask);

    // Create new entry
    BucketEntry* entry = (BucketEntry*)oa_alloc(hash->entryAllocator);
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
            oa_free(hash->entryAllocator, entry);
            hash->buckets[bucketIndex].count--;
            hash->totalObjects--;
            return true;
        }
        current = &entry->next;
    }

    return false; // Object not found
}

// Query objects within a radius of the given position
int sh_query_radius(SpatialHash* hash, float x, float y, float z, float radius,
                    void** results, int maxResults) {
    if (!hash || !results || maxResults <= 0) return 0;

    int found = 0;
    float radiusSq = radius * radius;
    unsigned int mask = (unsigned int)hash->bucketCount - 1u;

    // Calculate the range of grid cells to check
    int cellRange = (int)ceilf(radius / hash->cellSize);
    GridCoord centerCoord = world_to_grid(x, y, z, hash->cellSize);

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
    // Check all cells in the range
    for (int dx = -cellRange; dx <= cellRange; dx++) {
        for (int dy = -cellRange; dy <= cellRange; dy++) {
            for (int dz = -cellRange; dz <= cellRange; dz++) {
                GridCoord coord = {centerCoord.x + dx, centerCoord.y + dy, centerCoord.z + dz};
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

// Query objects within a bounding box
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

// Query objects at an exact position (within small tolerance)
int sh_query_point(SpatialHash* hash, float x, float y, float z, void** results, int maxResults) {
    if (!hash || !results || maxResults <= 0) return 0;

    // Use a very small tolerance for "exact" position matching
    const float POINT_TOLERANCE = 0.001f;

    int found = 0;
    float toleranceSq = POINT_TOLERANCE * POINT_TOLERANCE;

    // Convert position to grid coordinates and check only the exact cell
    GridCoord coord = world_to_grid(x, y, z, hash->cellSize);
    unsigned int mask = (unsigned int)hash->bucketCount - 1u;
    unsigned int bucketIndex = hash_coord(coord, mask);

    // Check all objects in this exact bucket; filter by coord to skip colliders
    BucketEntry* entry = hash->buckets[bucketIndex].head;
    while (entry && found < maxResults) {
        if (entry->coord.x == coord.x &&
            entry->coord.y == coord.y &&
            entry->coord.z == coord.z) {
            float fdx = entry->x - x;
            float fdy = entry->y - y;
            float fdz = entry->z - z;
            float distSq = fdx*fdx + fdy*fdy + fdz*fdz;

            if (distSq <= toleranceSq) {
                results[found++] = entry->object;
            }
        }

        entry = entry->next;
    }

    return found;
}

// Query for the first object within a radius (optimized for single-object lookups)
void* sh_query_first(SpatialHash* hash, float x, float y, float z, float radius) {
    if (!hash) return NULL;

    float radiusSq = radius * radius;
    unsigned int mask = (unsigned int)hash->bucketCount - 1u;

    // Calculate the range of grid cells to check
    int cellRange = (int)ceilf(radius / hash->cellSize);
    GridCoord centerCoord = world_to_grid(x, y, z, hash->cellSize);

    // Check all cells in the range
    for (int dx = -cellRange; dx <= cellRange; dx++) {
        for (int dy = -cellRange; dy <= cellRange; dy++) {
            for (int dz = -cellRange; dz <= cellRange; dz++) {
                GridCoord coord = {centerCoord.x + dx, centerCoord.y + dy, centerCoord.z + dz};
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
