#ifndef SPATIAL_HASH_H
#define SPATIAL_HASH_H

// libs/SpatialQueryLib/include/spatial_hash.h
//
// Uniform-grid spatial hash over opaque `void*` payloads. Positions are
// bucketed by flooring world coordinates into integer grid cells of `cellSize`
// and hashing the triple with the standard 73856093/19349663/83492791 prime
// mix, masked to a power-of-two bucket count.
//
// Who uses it
// -----------
// The meshing side of MatterSurfaceLib: `surface.c` and
// `marching_cubes_algorithm.cpp` sample nearest particles per field evaluation
// / per triangle, and `cluster.cpp` uses the box query to gather a cell's
// additive and carve particles. It is the hot inner structure of a bake.
//
// Model and lifetime
// ------------------
// - The hash stores POINTERS ONLY. It never copies, owns or frees the objects
//   you insert; they must outlive the hash or be removed first.
// - Positions are captured at insert time and are never re-derived. Moving an
//   object does NOT re-bucket it: you must `sh_remove` with the position the
//   object was inserted at and then `sh_insert` at the new one.
// - Insertion does not deduplicate. The same pointer may be inserted at
//   several positions and each occurrence is a separate entry.
// - Entries come from a `MemPool` sized from `initialCapacity` but free to
//   grow; the bucket table itself never resizes or rehashes, so a hash loaded
//   far past its initial capacity degrades to long bucket chains rather than
//   failing.
//
// Concurrency: none. There are no locks anywhere in the implementation.
// Concurrent readers are fine only if no writer is active.
//
// Units and spaces: `cellSize`, all coordinates and all radii are in the
// caller's world units — the hash imposes no convention of its own. Choose
// `cellSize` near the typical query radius: every query iterates the full grid
// range covering its AABB, so a radius much larger than `cellSize` costs
// cubically in cells visited.
//
// All functions tolerate a NULL `hash` (returning 0/false/NULL) except where
// noted. Result arrays are caller-provided and never grown.

#include <stdbool.h>

typedef struct SpatialHash SpatialHash;

// Create a new spatial hash table
// - cellSize: edge length of one grid cell, in world units. Must be > 0.
//   Pick it near the typical query radius (see the header note above).
// - initialCapacity: expected object count. It sizes the entry MemPool, and
//   the bucket table is derived from it as next_pow2(initialCapacity / 4)
//   clamped to a minimum of 1024 buckets. It is NOT a bucket count and it is
//   NOT a hard limit — the pool grows, the bucket table does not.
// Returns NULL on invalid arguments (cellSize <= 0 or initialCapacity <= 0)
// or on allocation failure. Free with sh_destroy.
SpatialHash* sh_create(float cellSize, int initialCapacity);

// Destroy a spatial hash table and free all memory
void sh_destroy(SpatialHash* hash);

// Clear all objects from the hash table without destroying it
// O(objects): walks every bucket chain returning entries to the pool. The
// bucket table and the entry pool keep their memory, so a cleared hash is
// cheap to refill — this is the intended way to reuse one across bakes.
void sh_clear(SpatialHash* hash);

// Insert an object at the given position
// Returns true on success, false on failure
// O(1). Stores the pointer plus the exact (x,y,z) and the grid cell derived
// from it; the object itself is never dereferenced or copied. A NULL `object`
// is rejected (false) — NULL cannot be stored. Duplicates are permitted: the
// same pointer may be inserted repeatedly and each insert is its own entry.
// The recorded position is authoritative for later removal and for the
// distance tests in the query functions, so re-insert (after removing at the
// OLD position) whenever an object moves.
bool sh_insert(SpatialHash* hash, float x, float y, float z, void* object);

// Remove an object from the given position
// Returns true if object was found and removed, false otherwise
// Matches on the pointer AND on exact float equality of all three
// coordinates, so the position passed here must be bit-identical to the one
// passed to sh_insert — a recomputed position that differs in the last bit
// will not match and the entry will leak into the hash until sh_clear.
// Removes at most one entry per call. O(chain length).
bool sh_remove(SpatialHash* hash, float x, float y, float z, void* object);

// Query objects within a radius of the given position
// Results are stored in the provided array, up to maxResults
// Returns the actual number of objects found
// Results are UNORDERED and, once `maxResults` is reached, ARBITRARY: the scan
// stops at the first maxResults hits in grid-scan order, which are not the
// nearest ones. Use sh_query_radius_nearest when that matters.
// Iterates every grid cell overlapping the query AABB, so cost grows with
// (2*radius/cellSize)^3 — a radius far above cellSize is expensive.
// Entries carry the cell they were inserted into and are filtered against the
// cell currently being scanned, so hash collisions between distinct cells
// never produce duplicates in the output.
int sh_query_radius(SpatialHash* hash, float x, float y, float z, float radius,
                    void** results, int maxResults);

// Query the NEAREST objects within a radius of the given position. Unlike
// sh_query_radius (which returns the first maxResults it encounters in grid-scan
// order and bails early), this scans every candidate in range and keeps the
// maxResults closest by center distance. Required where local density can exceed
// maxResults: the field/SDF sampler must never miss the actually-nearest
// particle, or the isosurface fragments. Results are unordered.
// Returns the actual number of objects found (<= maxResults).
int sh_query_radius_nearest(SpatialHash* hash, float x, float y, float z, float radius,
                            void** results, int maxResults);

// Query objects within a bounding box
// Returns the actual number of objects found
// The box test is inclusive on both bounds. Results are unordered, and as with
// sh_query_radius the scan bails at `maxResults` in grid-scan order rather
// than choosing among candidates — a truncated result is an arbitrary subset.
// Callers that need every object in the box must size `results` for the worst
// case and check `found < maxResults` to know they got them all.
int sh_query_box(SpatialHash* hash, float minX, float minY, float minZ,
                 float maxX, float maxY, float maxZ, void** results, int maxResults);


// Query for the first object within a radius (optimized for single-object lookups)
// Returns the object pointer if found, NULL otherwise
// "First" means first encountered in grid-scan order within the query AABB,
// then bucket-chain order (which is reverse insertion order) — NOT the
// nearest, and not stable if the same objects are inserted in a different
// order. Use it only where any one object in range will do, e.g.
// `cluster.cpp`'s occupancy probe with a radius well under one cell.
// NULL is returned both for "no object in range" and for a NULL hash.
void* sh_query_first(SpatialHash* hash, float x, float y, float z, float radius);

// Get statistics about the hash table
// Every out-parameter is optional — pass NULL for any you do not want. With a
// NULL `hash` nothing is written at all, so initialise your locals first.
// `loadFactor` is objects/buckets. Computing `maxBucketSize` scans the whole
// bucket table (at least 1024 entries, often far more), so it is not a
// per-frame call; pass NULL for it on a hot path.
void sh_get_stats(SpatialHash* hash, int* bucketCount, int* objectCount, 
                  int* maxBucketSize, float* loadFactor);

#endif // SPATIAL_HASH_H