#ifndef SPATIAL_HASH_H
#define SPATIAL_HASH_H

#include <stdbool.h>

typedef struct SpatialHash SpatialHash;

// Create a new spatial hash table
// - objectSize: size of objects to store (in bytes)
// - cellSize: spatial size of each hash cell
// - initialCapacity: initial number of buckets to allocate
SpatialHash* sh_create(float cellSize, int initialCapacity);

// Destroy a spatial hash table and free all memory
void sh_destroy(SpatialHash* hash);

// Clear all objects from the hash table without destroying it
void sh_clear(SpatialHash* hash);

// Insert an object at the given position
// Returns true on success, false on failure
bool sh_insert(SpatialHash* hash, float x, float y, float z, void* object);

// Remove an object from the given position
// Returns true if object was found and removed, false otherwise
bool sh_remove(SpatialHash* hash, float x, float y, float z, void* object);

// Query objects within a radius of the given position
// Results are stored in the provided array, up to maxResults
// Returns the actual number of objects found
int sh_query_radius(SpatialHash* hash, float x, float y, float z, float radius, 
                    void** results, int maxResults);

// Query objects within a bounding box
// Returns the actual number of objects found
int sh_query_box(SpatialHash* hash, float minX, float minY, float minZ,
                 float maxX, float maxY, float maxZ, void** results, int maxResults);

// Get statistics about the hash table
void sh_get_stats(SpatialHash* hash, int* bucketCount, int* objectCount, 
                  int* maxBucketSize, float* loadFactor);

#endif // SPATIAL_HASH_H