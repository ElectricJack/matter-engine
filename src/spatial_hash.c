#include "../include/spatial_hash.h"
#include "../include/object_allocator.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define INITIAL_BUCKET_CAPACITY 8
#define HASH_TABLE_SIZE 1024
#define HASH_MASK (HASH_TABLE_SIZE - 1)

// 3D grid coordinates
typedef struct {
    int x, y, z;
} GridCoord;

// Bucket entry - stores objects at a specific position
typedef struct BucketEntry {
    void* object;
    float x, y, z;
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

// Hash function for 3D grid coordinates
static unsigned int hash_coord(GridCoord coord) {
    // Simple hash function for 3D coordinates
    unsigned int hash = 0;
    hash ^= coord.x * 73856093;
    hash ^= coord.y * 19349663;
    hash ^= coord.z * 83492791;
    return hash & HASH_MASK;
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
    
    hash->buckets = (Bucket*)calloc(HASH_TABLE_SIZE, sizeof(Bucket));
    if (!hash->buckets) {
        free(hash);
        return NULL;
    }
    
    hash->bucketCount = HASH_TABLE_SIZE;
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
    unsigned int bucketIndex = hash_coord(coord);
    
    // Create new entry
    BucketEntry* entry = (BucketEntry*)oa_alloc(hash->entryAllocator);
    if (!entry) return false;
    
    entry->object = object;
    entry->x = x;
    entry->y = y;
    entry->z = z;
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
    unsigned int bucketIndex = hash_coord(coord);
    
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
    
    // Calculate the range of grid cells to check
    int cellRange = (int)ceilf(radius / hash->cellSize);
    GridCoord centerCoord = world_to_grid(x, y, z, hash->cellSize);
    
    // Check all cells in the range
    for (int dx = -cellRange; dx <= cellRange; dx++) {
        for (int dy = -cellRange; dy <= cellRange; dy++) {
            for (int dz = -cellRange; dz <= cellRange; dz++) {
                GridCoord coord = {centerCoord.x + dx, centerCoord.y + dy, centerCoord.z + dz};
                unsigned int bucketIndex = hash_coord(coord);
                
                // Check all objects in this bucket
                BucketEntry* entry = hash->buckets[bucketIndex].head;
                while (entry && found < maxResults) {
                    float dx = entry->x - x;
                    float dy = entry->y - y;
                    float dz = entry->z - z;
                    float distSq = dx*dx + dy*dy + dz*dz;
                    
                    if (distSq <= radiusSq) {
                        results[found++] = entry->object;
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
    
    // Calculate the range of grid cells to check
    GridCoord minCoord = world_to_grid(minX, minY, minZ, hash->cellSize);
    GridCoord maxCoord = world_to_grid(maxX, maxY, maxZ, hash->cellSize);
    
    // Check all cells in the bounding box
    for (int x = minCoord.x; x <= maxCoord.x; x++) {
        for (int y = minCoord.y; y <= maxCoord.y; y++) {
            for (int z = minCoord.z; z <= maxCoord.z; z++) {
                GridCoord coord = {x, y, z};
                unsigned int bucketIndex = hash_coord(coord);
                
                // Check all objects in this bucket
                BucketEntry* entry = hash->buckets[bucketIndex].head;
                while (entry && found < maxResults) {
                    // Check if object is within the bounding box
                    if (entry->x >= minX && entry->x <= maxX &&
                        entry->y >= minY && entry->y <= maxY &&
                        entry->z >= minZ && entry->z <= maxZ) {
                        results[found++] = entry->object;
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
    unsigned int bucketIndex = hash_coord(coord);
    
    // Check all objects in this exact bucket
    BucketEntry* entry = hash->buckets[bucketIndex].head;
    while (entry && found < maxResults) {
        float dx = entry->x - x;
        float dy = entry->y - y;
        float dz = entry->z - z;
        float distSq = dx*dx + dy*dy + dz*dz;
        
        if (distSq <= toleranceSq) {
            results[found++] = entry->object;
        }
        
        entry = entry->next;
    }
    
    return found;
}

// Query for the first object within a radius (optimized for single-object lookups)
void* sh_query_first(SpatialHash* hash, float x, float y, float z, float radius) {
    if (!hash) return NULL;
    
    float radiusSq = radius * radius;
    
    // Calculate the range of grid cells to check
    int cellRange = (int)ceilf(radius / hash->cellSize);
    GridCoord centerCoord = world_to_grid(x, y, z, hash->cellSize);
    
    // Check all cells in the range
    for (int dx = -cellRange; dx <= cellRange; dx++) {
        for (int dy = -cellRange; dy <= cellRange; dy++) {
            for (int dz = -cellRange; dz <= cellRange; dz++) {
                GridCoord coord = {centerCoord.x + dx, centerCoord.y + dy, centerCoord.z + dz};
                unsigned int bucketIndex = hash_coord(coord);
                
                // Check all objects in this bucket
                BucketEntry* entry = hash->buckets[bucketIndex].head;
                while (entry) {
                    float dx = entry->x - x;
                    float dy = entry->y - y;
                    float dz = entry->z - z;
                    float distSq = dx*dx + dy*dy + dz*dz;
                    
                    if (distSq <= radiusSq) {
                        return entry->object; // Return first match
                    }
                    
                    entry = entry->next;
                }
            }
        }
    }
    
    return NULL; // No object found
}