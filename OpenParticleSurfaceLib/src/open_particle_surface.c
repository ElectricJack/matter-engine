#include "../include/open_particle_surface.h"
#include "../include/surface.h"
#include "object_allocator.h"
#include "spatial_hash.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

// Debug printf wrapper — only compiled in when OPS_DEBUG is defined
#ifdef OPS_DEBUG
#define DEBUG_LOG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...) ((void)0)
#endif

// Configuration
#define CELL_SIZE             16.0f    // Size of each spatial hash cell
#define HASH_BOUNDS_DETAIL    5        // Detail level for each bounds (2^4 = 16 divisions per cell)

// 3D grid coordinates for unbounded grid
typedef struct {
    int x;
    int y;
    int z;
} GridCoord;

// Maximum number of cells a particle can overlap
#define MAX_OVERLAPPING_CELLS 27

// Particle structure with internal metadata
typedef struct {
    Vector3 position;
    int     materialId;
    int     cellIndices[MAX_OVERLAPPING_CELLS]; // Indices of all cells containing this particle
    int     cellCount;     // Number of cells this particle belongs to
    bool    active;        // Whether the particle is active
    int     nextFree;      // Free-list link (-1 = end-of-list, used when !active)
} InternalParticle;

// Per-cell particle index array with tracked capacity
typedef struct {
    int* data;
    int  count;
    int  capacity;
} ParticleIndexArray;

// Spatial hash cell structure
typedef struct {
    int              particleCount;    // Number of particles in this cell
    ParticleIndexArray indices;        // Capacity-tracked array of particle indices
    bool             dirty;            // Whether this cell needs mesh regeneration
    Mesh             mesh;             // Mesh for this cell
    Bounds           bounds;           // Bounds for this cell
    bool             hasMesh;          // Whether a mesh has been generated
    GridCoord        coord;            // Grid coordinates of this cell
    bool             isActive;         // Whether the cell is in the active list
} SpatialHashCell;

// Cell counter for tracking total cells (replaces CellMap.size)
static int totalCellCount = 0;

// List to track active cells (only those with particles)
typedef struct {
    int* indices;
    int count;
    int capacity;
} ActiveCellList;

// Global state
static InternalParticle* particles = NULL;
static SpatialHashCell* spatialHashCells = NULL;
static int spatialHashCellsCapacity = 0;
static int maxParticleCount = 0;
static int currentParticleCount = 0;
static float particleRadius = 0.0f;
static ObjectAllocator* particleAllocator = NULL;
static ActiveCellList activeCells = {0};
static SpatialHash* spatialHash = NULL;

// Free-list head for O(1) free-slot lookup in CreateParticle (-1 = none)
static int freeParticleHead = -1;

// Static buffer for particle data to avoid repeated malloc/free
static Particle* cellParticleBuffer = NULL;
static int cellParticleBufferSize = 0;

// Per-cell vertex welding for smooth normals within each cell
typedef struct {
    Vector3 position;
    Vector3 normal;
    Color color;
    int originalIndex;
} CellVertex;

// Simple spatial hash for vertex welding within a cell
#define WELD_GRID_SIZE 32
typedef struct {
    int* vertexIndices;
    int count;
    int capacity;
} WeldGridCell;

static CellVertex* cellVertices = NULL;
static int cellVertexCapacity = 0;
static WeldGridCell weldGrid[WELD_GRID_SIZE][WELD_GRID_SIZE][WELD_GRID_SIZE];

// ---------------------------------------------------------------------------
// Open-addressed edge-dedup hash table for SimplifyMesh
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t key;   // packed edge key; 0 = empty slot
    int      edgeIdx;
} EdgeHashEntry;

// Mesh simplification structures
typedef struct {
    int v1, v2;           // Vertex indices
    float length;         // Edge length
    bool valid;           // Whether this edge is still valid
} Edge;

typedef struct {
    int v1, v2, v3;       // Vertex indices
    float area;           // Triangle area
    bool valid;           // Whether this triangle is still valid
} Triangle;

static Edge* meshEdges = NULL;
static int meshEdgeCount = 0;
static int meshEdgeCapacity = 0;
static Triangle* meshTriangles = NULL;
static int meshTriangleCapacity = 0;

// Open-addressed hash table for edge dedup
static EdgeHashEntry* edgeHashTable = NULL;
static int edgeHashTableSize = 0; // always a power of two

// For instanced rendering
static Model particleModel;
static Matrix* particleMatrices = NULL;
static Color* particleColors = NULL;
static int particleInstanceCount = 0;

// Forward declarations
static void AddActiveCellIfNeeded(int cellIndex);
static void InitializeActiveCellTracking(void);
static int GetCellIndex(GridCoord coord);
static GridCoord GetGridCoordFromPosition(Vector3 position);
static Bounds GetSpatialHashBounds(GridCoord coord);
static int UpdateDirtyCells(int maxUpdates);
static bool IsVertexOnCellBoundary(Vector3 vertex, Bounds cellBounds, float tolerance);
static void WeldVerticesInMesh(Mesh* mesh, Bounds cellBounds);
static void SimplifyMesh(Mesh* mesh);
static bool CellParticleArrayEnsure(SpatialHashCell* cell, int minCapacity);

// HashGridCoord function removed - now using spatial hash for cell lookups

// Convert position to grid coordinates
static GridCoord GetGridCoordFromPosition(Vector3 position) {
    GridCoord coord;
    coord.x = (int)floorf(position.x / CELL_SIZE);
    coord.y = (int)floorf(position.y / CELL_SIZE);
    coord.z = (int)floorf(position.z / CELL_SIZE);
    return coord;
}

// Get the bounds for a spatial hash cell based on its grid coordinates
static Bounds GetSpatialHashBounds(GridCoord coord) {
    // Calculate bounds center position
    Vector3 center = {
        (coord.x + 0.5f) * CELL_SIZE,
        (coord.y + 0.5f) * CELL_SIZE,
        (coord.z + 0.5f) * CELL_SIZE
    };

    // Expand bounds slightly to overlap with neighboring cells
    // This ensures vertices on cell boundaries are shared for proper normal blending
    const float overlap = CELL_SIZE * 0.1f; // 10% overlap with neighbors

    // Create bounds with center, expanded size, and division power
    Bounds bounds = {
        .center = center,
        .size = {CELL_SIZE + overlap, CELL_SIZE + overlap, CELL_SIZE + overlap},
        .divisionPow = HASH_BOUNDS_DETAIL
    };

    return bounds;
}

// Ensure a cell's particle index array has room for at least minCapacity entries.
// Returns false on allocation failure (original array is preserved).
static bool CellParticleArrayEnsure(SpatialHashCell* cell, int minCapacity) {
    if (cell->indices.capacity >= minCapacity) return true;

    int newCap = cell->indices.capacity == 0 ? 64 : cell->indices.capacity * 2;
    if (newCap < minCapacity) newCap = minCapacity;

    int* tmp = (int*)realloc(cell->indices.data, newCap * sizeof(int));
    if (!tmp) {
        fprintf(stderr, "[ERROR] Failed to grow cell particle index array to %d\n", newCap);
        return false;
    }
    cell->indices.data = tmp;
    cell->indices.capacity = newCap;
    return true;
}

// Initialize the cell map
// InitializeCellMap function removed - cell management now handled by spatial hash

// Find or create a cell for the given grid coordinates
// Convert grid coordinates to world position (cell center)
static Vector3 GridCoordToWorldPos(GridCoord coord) {
    return (Vector3){
        (coord.x + 0.5f) * CELL_SIZE,
        (coord.y + 0.5f) * CELL_SIZE,
        (coord.z + 0.5f) * CELL_SIZE
    };
}

// Step 1 (B1): store integer cell index in the spatial hash (as (void*)(intptr_t)(idx+1))
// so that NULL means "not found" and 0 is a valid index.
static int GetCellIndex(GridCoord coord) {
    // Convert grid coordinates to world position for spatial hash lookup
    Vector3 worldPos = GridCoordToWorldPos(coord);

    // Try to find existing cell using the spatial hash
    // Stored as (void*)(intptr_t)(idx+1) to distinguish idx==0 from NULL
    void* stored = sh_query_first(spatialHash, worldPos.x, worldPos.y, worldPos.z, 0.1f);

    if (stored != NULL) {
        // Decode stored index
        return (int)((intptr_t)stored - 1);
    }

    // Cell doesn't exist yet, create a new one

    // Make sure spatialHashCells has room for a new cell
    if (totalCellCount >= spatialHashCellsCapacity) {
        int newCapacity = spatialHashCellsCapacity + 1000; // Add some buffer
        SpatialHashCell* tmp = (SpatialHashCell*)realloc(spatialHashCells, newCapacity * sizeof(SpatialHashCell));
        if (!tmp) {
            fprintf(stderr, "[ERROR] Failed to grow spatialHashCells to %d\n", newCapacity);
            return -1;
        }
        spatialHashCells = tmp;

        // NOTE: realloc may have moved the array; all stored spatial-hash payloads
        // are integer indices (not pointers into this array), so no fix-up needed.

        // Initialize new cells
        for (int i = spatialHashCellsCapacity; i < newCapacity; i++) {
            SpatialHashCell* cell = &spatialHashCells[i];
            cell->particleCount   = 0;
            cell->indices.data    = NULL;
            cell->indices.count   = 0;
            cell->indices.capacity = 0;
            cell->dirty           = false;
            cell->hasMesh         = false;
            cell->isActive        = false;
        }

        spatialHashCellsCapacity = newCapacity;
    }

    // Get the index for the new cell
    int newCellIndex = totalCellCount;
    totalCellCount++;

    // Initialize the new cell
    SpatialHashCell* newCell = &spatialHashCells[newCellIndex];
    newCell->coord = coord;
    newCell->bounds = GetSpatialHashBounds(coord);
    newCell->particleCount = 0;
    newCell->indices.data = NULL;
    newCell->indices.count = 0;
    newCell->indices.capacity = 0;
    newCell->dirty = false;
    newCell->hasMesh = false;
    newCell->isActive = false;

    // Insert the cell index into the spatial hash
    // Encode as (idx+1) so that idx==0 is distinguishable from NULL
    sh_insert(spatialHash, worldPos.x, worldPos.y, worldPos.z,
              (void*)(intptr_t)(newCellIndex + 1));

    return newCellIndex;
}

// Check if a position with radius overlaps a cell
static bool PositionOverlapsCell(Vector3 position, float radius, GridCoord cellCoord) {
    // Calculate cell bounds
    float cellMinX = cellCoord.x * CELL_SIZE;
    float cellMinY = cellCoord.y * CELL_SIZE;
    float cellMinZ = cellCoord.z * CELL_SIZE;
    float cellMaxX = (cellCoord.x + 1) * CELL_SIZE;
    float cellMaxY = (cellCoord.y + 1) * CELL_SIZE;
    float cellMaxZ = (cellCoord.z + 1) * CELL_SIZE;

    // Check if the sphere overlaps the cell
    float closestX = fmaxf(cellMinX, fminf(position.x, cellMaxX));
    float closestY = fmaxf(cellMinY, fminf(position.y, cellMaxY));
    float closestZ = fmaxf(cellMinZ, fminf(position.z, cellMaxZ));

    // Calculate squared distance between the closest point and sphere center
    float distanceX = position.x - closestX;
    float distanceY = position.y - closestY;
    float distanceZ = position.z - closestZ;

    float distanceSquared = distanceX * distanceX +
                            distanceY * distanceY +
                            distanceZ * distanceZ;

    // Check if the closest point is within the sphere's radius
    return distanceSquared <= (radius * radius);
}

// Find all cells that a particle overlaps
static void GetOverlappingCells(Vector3 position, float radius, GridCoord* cells, int* cellCount, int maxCells) {
    // Get the base cell (containing the particle center)
    GridCoord baseCoord = GetGridCoordFromPosition(position);
    cells[0] = baseCoord;
    *cellCount = 1;

    // Calculate the maximum cells the particle could overlap in each direction
    int cellRadius = (int)ceilf(radius / CELL_SIZE) + 1;

    // Check all potentially overlapping cells in a cube around the base cell
    for (int z = -cellRadius; z <= cellRadius && *cellCount < maxCells; z++) {
        for (int y = -cellRadius; y <= cellRadius && *cellCount < maxCells; y++) {
            for (int x = -cellRadius; x <= cellRadius && *cellCount < maxCells; x++) {
                // Skip the base cell (already added)
                if (x == 0 && y == 0 && z == 0) continue;

                GridCoord neighborCoord = {
                    baseCoord.x + x,
                    baseCoord.y + y,
                    baseCoord.z + z
                };

                // Check if the particle actually overlaps this cell
                if (PositionOverlapsCell(position, radius, neighborCoord)) {
                    cells[*cellCount] = neighborCoord;
                    (*cellCount)++;

                    if (*cellCount >= maxCells) {
                        DEBUG_LOG("Warning: Maximum overlapping cells reached (%d)", maxCells);
                        break;
                    }
                }
            }
        }
    }
}


// Initialize active cell tracking
static void InitializeActiveCellTracking(void) {
    DEBUG_LOG("Initializing active cell tracking");
    // Start with capacity for a reasonable number of active cells
    activeCells.capacity = 1000;
    activeCells.indices = (int*)malloc(activeCells.capacity * sizeof(int));
    if (!activeCells.indices) {
        fprintf(stderr, "[ERROR] Failed to allocate activeCells.indices (%d entries)\n", activeCells.capacity);
        activeCells.capacity = 0;
    }
    activeCells.count = 0;
}

// Add a cell to the active list if not already present — O(1) via isActive flag
static void AddActiveCellIfNeeded(int cellIndex) {
    if (cellIndex < 0 || cellIndex >= spatialHashCellsCapacity) return;

    if (spatialHashCells[cellIndex].isActive) {
        return; // Already tracked
    }

    DEBUG_LOG("Adding cell %d to active cells list", cellIndex);

    // Expand capacity if needed
    if (activeCells.count >= activeCells.capacity) {
        int newCap = activeCells.capacity * 2;
        int* tmp = (int*)realloc(activeCells.indices,
                                 newCap * sizeof(int));
        if (!tmp) {
            fprintf(stderr, "[ERROR] Failed to grow activeCells to %d\n", newCap);
            return;
        }
        activeCells.indices = tmp;
        activeCells.capacity = newCap;
    }

    // Add to active list and set the flag
    activeCells.indices[activeCells.count++] = cellIndex;
    spatialHashCells[cellIndex].isActive = true;
}

// API implementation

void InitializeParticleSystem(int maxParticles, float radius) {
    DEBUG_LOG("Initializing particle system with %d particles, radius %.2f", maxParticles, radius);

    // Store parameters
    maxParticleCount = maxParticles;
    particleRadius = radius;

    // Create particle allocator (object size, objects per page)
    particleAllocator = oa_create(sizeof(InternalParticle), 10000);

    // Preallocate space for particles
    InternalParticle* pBuf = (InternalParticle*)malloc(maxParticleCount * sizeof(InternalParticle));
    if (!pBuf) {
        fprintf(stderr, "[ERROR] Failed to allocate particle array (%d entries)\n", maxParticleCount);
        maxParticleCount = 0;
        return;
    }
    particles = pBuf;

    // Initialize particles as inactive and build free list
    for (int i = 0; i < maxParticleCount; i++) {
        particles[i].active = false;
        particles[i].nextFree = (i + 1 < maxParticleCount) ? i + 1 : -1;
    }
    freeParticleHead = maxParticleCount > 0 ? 0 : -1;

    // Initialize the new shared spatial hash for storing cells
    // Estimate cell capacity based on particle distribution
    int estimatedCellCount = maxParticles / 10; // Rough estimate: ~10 particles per cell
    spatialHash = sh_create(CELL_SIZE, estimatedCellCount);

    // Allocate initial array for spatial hash cells
    // This will grow dynamically as needed
    int initialCellCapacity = 1000;
    spatialHashCells = (SpatialHashCell*)malloc(initialCellCapacity * sizeof(SpatialHashCell));
    if (!spatialHashCells) {
        fprintf(stderr, "[ERROR] Failed to allocate spatialHashCells (%d entries)\n", initialCellCapacity);
        return;
    }
    spatialHashCellsCapacity = initialCellCapacity;

    // Initialize active cell tracking
    InitializeActiveCellTracking();

    // Setup for instanced rendering
    Mesh sphereMesh = GenMeshSphere(particleRadius * 0.1f, 4, 4);
    particleModel = LoadModelFromMesh(sphereMesh);

    // Preallocate matrices and colors for particles
    Matrix* mBuf = (Matrix*)malloc(maxParticleCount * sizeof(Matrix));
    if (!mBuf) {
        fprintf(stderr, "[ERROR] Failed to allocate particleMatrices (%d entries)\n", maxParticleCount);
    }
    particleMatrices = mBuf;

    Color* cBuf = (Color*)malloc(maxParticleCount * sizeof(Color));
    if (!cBuf) {
        fprintf(stderr, "[ERROR] Failed to allocate particleColors (%d entries)\n", maxParticleCount);
    }
    particleColors = cBuf;
    particleInstanceCount = 0;
}

void ShutdownParticleSystem(void) {
    DEBUG_LOG("Shutting down particle system");

    // Cleanup cell resources
    for (int i = 0; i < activeCells.count; i++) {
        int cellIndex = activeCells.indices[i];
        if (cellIndex >= 0 && cellIndex < spatialHashCellsCapacity) {
            if (spatialHashCells[cellIndex].hasMesh) {
                UnloadMesh(spatialHashCells[cellIndex].mesh);
            }
            if (spatialHashCells[cellIndex].indices.data) {
                free(spatialHashCells[cellIndex].indices.data);
                spatialHashCells[cellIndex].indices.data = NULL;
            }
        }
    }

    // Free the static cell particle buffer
    if (cellParticleBuffer != NULL) {
        free(cellParticleBuffer);
        cellParticleBuffer = NULL;
    }

    // Free vertex welding buffer
    if (cellVertices != NULL) {
        free(cellVertices);
        cellVertices = NULL;
        cellVertexCapacity = 0;
    }

    // Free welding grid cell buffers
    for (int x = 0; x < WELD_GRID_SIZE; x++) {
        for (int y = 0; y < WELD_GRID_SIZE; y++) {
            for (int z = 0; z < WELD_GRID_SIZE; z++) {
                if (weldGrid[x][y][z].vertexIndices) {
                    free(weldGrid[x][y][z].vertexIndices);
                    weldGrid[x][y][z].vertexIndices = NULL;
                    weldGrid[x][y][z].capacity = 0;
                    weldGrid[x][y][z].count = 0;
                }
            }
        }
    }

    // Free mesh simplification buffers
    if (meshEdges != NULL) {
        free(meshEdges);
        meshEdges = NULL;
        meshEdgeCount = 0;
        meshEdgeCapacity = 0;
    }

    if (meshTriangles != NULL) {
        free(meshTriangles);
        meshTriangles = NULL;
        meshTriangleCapacity = 0;
    }

    if (edgeHashTable != NULL) {
        free(edgeHashTable);
        edgeHashTable = NULL;
        edgeHashTableSize = 0;
    }

    // Free active cell tracking
    if (activeCells.indices != NULL) {
        free(activeCells.indices);
        activeCells.indices = NULL;
    }

    // Cell map cleanup removed - now handled by spatial hash

    // Unload instanced rendering resources
    UnloadModel(particleModel);
    free(particleMatrices);
    free(particleColors);

    // Free spatial hash cells
    free(spatialHashCells);
    spatialHashCells = NULL;

    // Free particles
    free(particles);
    particles = NULL;

    // Destroy allocator
    oa_destroy(particleAllocator);

    // Destroy spatial hash
    if (spatialHash) {
        sh_destroy(spatialHash);
        spatialHash = NULL;
    }

    // Reset state
    maxParticleCount = 0;
    currentParticleCount = 0;
    particleRadius = 0.0f;
    totalCellCount = 0;
    freeParticleHead = -1;
}

// Helper: add particle to a cell's index array (Step 2 + B3)
static bool CellAddParticle(SpatialHashCell* cell, int particleIndex, int cellIndex) {
    // Grow capacity if needed
    if (cell->indices.count >= cell->indices.capacity) {
        if (!CellParticleArrayEnsure(cell, cell->indices.capacity == 0 ? 64 : cell->indices.capacity * 2)) {
            fprintf(stderr, "[ERROR] Failed to add particle %d to cell %d\n", particleIndex, cellIndex);
            return false;
        }
    }
    cell->indices.data[cell->indices.count++] = particleIndex;
    cell->particleCount = cell->indices.count;
    return true;
}

ParticleHandle CreateParticle(Vector3 position, int materialId) {
    // Check if we've reached capacity
    if (currentParticleCount >= maxParticleCount) {
        DEBUG_LOG("Failed to create particle: maximum capacity reached");
        return -1;
    }

    // Find a free slot via the free list — O(1)
    int particleIndex = freeParticleHead;
    if (particleIndex == -1) {
        DEBUG_LOG("Failed to create particle: no free slots available");
        return -1;
    }
    freeParticleHead = particles[particleIndex].nextFree;

    // Initialize the particle
    particles[particleIndex].position = position;
    particles[particleIndex].materialId = materialId;
    particles[particleIndex].active = true;
    particles[particleIndex].cellCount = 0;
    particles[particleIndex].nextFree = -1;

    // Find all cells that this particle overlaps
    GridCoord overlappingCells[MAX_OVERLAPPING_CELLS];
    int overlappingCellCount = 0;
    GetOverlappingCells(position, particleRadius, overlappingCells, &overlappingCellCount, MAX_OVERLAPPING_CELLS);

    // Add particle to each overlapping cell
    for (int i = 0; i < overlappingCellCount; i++) {
        // Get cell index (creates the cell if it doesn't exist yet)
        int cellIndex = GetCellIndex(overlappingCells[i]);
        if (cellIndex < 0) continue;

        // Store cell index in particle
        if (particles[particleIndex].cellCount < MAX_OVERLAPPING_CELLS) {
            particles[particleIndex].cellIndices[particles[particleIndex].cellCount++] = cellIndex;
        } else {
            DEBUG_LOG("Warning: Maximum overlapping cells reached for particle %d", particleIndex);
            break;
        }

        SpatialHashCell* cell = &spatialHashCells[cellIndex];

        // Set the cell's grid coordinates and bounds if it's a new cell
        if (cell->indices.count == 0) {
            cell->coord = overlappingCells[i];
            cell->bounds = GetSpatialHashBounds(overlappingCells[i]);
        }

        // Add particle to cell (bounds-checked, capacity-tracked)
        if (!CellAddParticle(cell, particleIndex, cellIndex)) {
            continue; // Skip this cell but continue with others
        }
        cell->dirty = true; // Mark as dirty

        // Track cell as active — O(1) via isActive flag
        AddActiveCellIfNeeded(cellIndex);
    }

    // Note: particles are now tracked via their cells in the spatial hash

    // Increment counter
    currentParticleCount++;

    return particleIndex;
}

int CreateParticles(Vector3* positions, int* materialIds, int count) {
    int created = 0;

    for (int i = 0; i < count; i++) {
        if (CreateParticle(positions[i], materialIds[i]) != -1) {
            created++;
        }
    }

    return created;
}

bool UpdateParticlePosition(ParticleHandle handle, Vector3 newPosition) {
    // Validate handle
    if (handle < 0 || handle >= maxParticleCount || !particles[handle].active) {
        return false;
    }

    // Note: particles are now tracked via their cells in the spatial hash

    // Update position
    particles[handle].position = newPosition;

    // Find all cells that this particle now overlaps
    GridCoord newOverlappingCells[MAX_OVERLAPPING_CELLS];
    int newOverlappingCount = 0;
    GetOverlappingCells(newPosition, particleRadius, newOverlappingCells, &newOverlappingCount, MAX_OVERLAPPING_CELLS);

    // Remove from all current cells
    for (int i = 0; i < particles[handle].cellCount; i++) {
        int cellIndex = particles[handle].cellIndices[i];

        // Skip invalid cells
        if (cellIndex < 0 || cellIndex >= spatialHashCellsCapacity) {
            continue;
        }

        // Get the cell
        SpatialHashCell* cell = &spatialHashCells[cellIndex];

        // Remove particle from this cell
        int cellCount = cell->indices.count;
        int* idxData = cell->indices.data;

        if (idxData != NULL) {
            // Find and remove the particle
            for (int j = 0; j < cellCount; j++) {
                if (idxData[j] == handle) {
                    // Replace with the last element and decrement count
                    idxData[j] = idxData[cellCount - 1];
                    cell->indices.count--;
                    cell->particleCount = cell->indices.count;
                    break;
                }
            }
        }

        // Mark cell as dirty
        cell->dirty = true;
    }

    // Reset particle's cell count
    particles[handle].cellCount = 0;

    // Add to all new overlapping cells
    for (int i = 0; i < newOverlappingCount; i++) {
        // Get cell index (creates the cell if it doesn't exist yet)
        int cellIndex = GetCellIndex(newOverlappingCells[i]);
        if (cellIndex < 0) continue;

        // Store cell index in particle
        if (particles[handle].cellCount < MAX_OVERLAPPING_CELLS) {
            particles[handle].cellIndices[particles[handle].cellCount++] = cellIndex;
        } else {
            DEBUG_LOG("Warning: Maximum overlapping cells reached for particle %d", handle);
            break;
        }

        // Get the cell
        SpatialHashCell* cell = &spatialHashCells[cellIndex];

        // Set the cell's grid coordinates and bounds if it's a new cell
        if (cell->indices.count == 0) {
            cell->coord = newOverlappingCells[i];
            cell->bounds = GetSpatialHashBounds(newOverlappingCells[i]);
        }

        // Add particle to cell (bounds-checked, capacity-tracked)
        if (!CellAddParticle(cell, handle, cellIndex)) {
            continue; // Skip this cell but continue with others
        }
        cell->dirty = true; // Mark as dirty

        // Track cell as active — O(1) via isActive flag
        AddActiveCellIfNeeded(cellIndex);
    }

    // Note: particles are now tracked via their cells in the spatial hash

    return true;
}

bool DeleteParticle(ParticleHandle handle) {
    // Validate handle
    if (handle < 0 || handle >= maxParticleCount || !particles[handle].active) {
        return false;
    }

    // Remove from all cells the particle belongs to
    for (int i = 0; i < particles[handle].cellCount; i++) {
        int cellIndex = particles[handle].cellIndices[i];

        // Skip invalid cells
        if (cellIndex < 0 || cellIndex >= spatialHashCellsCapacity) {
            continue;
        }

        // Remove particle from this cell
        int cellParticleCount = spatialHashCells[cellIndex].indices.count;
        int* idxData = spatialHashCells[cellIndex].indices.data;

        if (idxData != NULL) {
            // Find and remove the particle
            for (int j = 0; j < cellParticleCount; j++) {
                if (idxData[j] == handle) {
                    // Replace with the last element and decrement count
                    idxData[j] = idxData[cellParticleCount - 1];
                    spatialHashCells[cellIndex].indices.count--;
                    spatialHashCells[cellIndex].particleCount = spatialHashCells[cellIndex].indices.count;
                    break;
                }
            }
        }

        // Mark cell as dirty
        spatialHashCells[cellIndex].dirty = true;
    }

    // Note: particles are now tracked via their cells in the spatial hash

    // Mark particle as inactive and return it to the free list
    particles[handle].active = false;
    particles[handle].nextFree = freeParticleHead;
    freeParticleHead = handle;

    // Reset particle's cell count
    particles[handle].cellCount = 0;

    // Decrement counter
    currentParticleCount--;

    return true;
}

// Update dirty spatial hash cells and regenerate meshes
static int UpdateDirtyCells(int maxUpdates) {
    int updatedCount = 0;

    // Initialize the static buffer if needed
    if (cellParticleBuffer == NULL) {
        cellParticleBufferSize = 20000; // Pre-allocate a large buffer
        cellParticleBuffer = (Particle*)malloc(cellParticleBufferSize * sizeof(Particle));
    }

    // Track cells with higher particle counts first (prioritize dense areas)
    int dirtyIndices[100]; // Max updates that can be processed
    int dirtyCounts[100];
    int dirtyFound = 0;
    int totalDirty = 0;

    // Cap max updates to a reasonable value
    if (maxUpdates > 100) maxUpdates = 100;

    // Count total dirty cells for debug
    for (int i = 0; i < activeCells.count; i++) {
        int cellIndex = activeCells.indices[i];
        if (cellIndex >= 0 && cellIndex < spatialHashCellsCapacity &&
            spatialHashCells[cellIndex].dirty &&
            spatialHashCells[cellIndex].particleCount > 0) {
            totalDirty++;
        }
    }

    // Debug output if there are dirty cells but few being processed
#ifdef OPS_DEBUG
    if (totalDirty > 5 && maxUpdates < totalDirty) {
        printf("[INFO] %d total dirty cells found, processing up to %d\n", totalDirty, maxUpdates);
    }
#endif

    // First pass: find cells with highest particle counts - ONLY from active cells
    for (int i = 0; i < activeCells.count && dirtyFound < maxUpdates; i++) {
        int cellIndex = activeCells.indices[i];
        if (cellIndex < 0 || cellIndex >= spatialHashCellsCapacity) {
            continue;
        }

        if (spatialHashCells[cellIndex].dirty && spatialHashCells[cellIndex].particleCount > 0) {
            // Check if this is a newly created cell without a mesh
            bool isNewCell = !spatialHashCells[cellIndex].hasMesh;

            // Find position to insert based on whether it's new and particle count
            int pos = 0;
            while (pos < dirtyFound) {
                // New cells get higher priority
                bool posIsNewCell = !spatialHashCells[dirtyIndices[pos]].hasMesh;

                if (isNewCell && !posIsNewCell) {
                    // Put new cells before existing cells
                    break;
                } else if (isNewCell == posIsNewCell &&
                          spatialHashCells[cellIndex].particleCount < dirtyCounts[pos]) {
                    // Within same cell type (new or existing), order by particle count
                    pos++;
                } else {
                    break;
                }
            }

            // Shift elements to make room
            if (dirtyFound < maxUpdates) {
                for (int j = dirtyFound; j > pos; j--) {
                    dirtyIndices[j] = dirtyIndices[j-1];
                    dirtyCounts[j] = dirtyCounts[j-1];
                }

                // Insert this cell
                dirtyIndices[pos] = cellIndex;
                dirtyCounts[pos] = spatialHashCells[cellIndex].particleCount;
                dirtyFound++;

#ifdef OPS_DEBUG
                // Track new cells for debugging
                if (isNewCell) {
                    printf("[INFO] Prioritizing new cell %d with %d particles\n",
                           cellIndex, spatialHashCells[cellIndex].particleCount);
                }
#endif
            }
        }
    }

    // Second pass: update the cells we found, in priority order
    for (int idx = 0; idx < dirtyFound; idx++) {
        int cellIndex = dirtyIndices[idx];

        DEBUG_LOG("Processing cell %d (%s): %d particles",
                  cellIndex,
                  spatialHashCells[cellIndex].hasMesh ? "existing" : "new",
                  spatialHashCells[cellIndex].particleCount);

        // Ensure the buffer is large enough
        if (spatialHashCells[cellIndex].particleCount > cellParticleBufferSize) {
            int newSize = (int)(spatialHashCells[cellIndex].particleCount * 1.5f);
            if (newSize < spatialHashCells[cellIndex].particleCount + 1)
                newSize = spatialHashCells[cellIndex].particleCount + 1;
            Particle* newBuffer = (Particle*)realloc(cellParticleBuffer,
                                               newSize * sizeof(Particle));
            if (!newBuffer) {
                fprintf(stderr, "[ERROR] Failed to grow cell particle buffer to %d entries\n", newSize);
                continue; // keep old buffer/size; skip this cell
            }
            cellParticleBuffer = newBuffer;
            cellParticleBufferSize = newSize;
        }

        // Copy particle data to buffer - only copy valid particles
        int validCount = 0;
        for (int j = 0; j < spatialHashCells[cellIndex].indices.count; j++) {
            int particleIdx = spatialHashCells[cellIndex].indices.data[j];
            if (particleIdx < 0 || particleIdx >= maxParticleCount) {
                continue;
            }

            if (!particles[particleIdx].active) {
                continue;
            }

            cellParticleBuffer[validCount].position = particles[particleIdx].position;
            cellParticleBuffer[validCount].materialId = particles[particleIdx].materialId;
            validCount++;
        }

        // Only generate mesh if we have enough valid particles
        if (validCount < 5) {
            DEBUG_LOG("Cell %d skipped: insufficient valid particles (%d)", cellIndex, validCount);
            spatialHashCells[cellIndex].hasMesh = false;
            spatialHashCells[cellIndex].dirty = false; // Mark as clean even though no mesh was generated
            continue;
        }

        // If a mesh already exists, unload it
        if (spatialHashCells[cellIndex].hasMesh) {
            UnloadMesh(spatialHashCells[cellIndex].mesh);
        }

        // Generate new mesh
        spatialHashCells[cellIndex].mesh = GenerateMesh(cellParticleBuffer, particleRadius,
                                          validCount,
                                          spatialHashCells[cellIndex].bounds);

        // Simplify mesh by collapsing short edges and removing thin triangles
        SimplifyMesh(&spatialHashCells[cellIndex].mesh);

        // Weld vertices and smooth normals within this mesh
        WeldVerticesInMesh(&spatialHashCells[cellIndex].mesh, spatialHashCells[cellIndex].bounds);

        UploadMesh(&spatialHashCells[cellIndex].mesh, false);
        spatialHashCells[cellIndex].hasMesh = true;

        // Mark as clean
        spatialHashCells[cellIndex].dirty = false;
        updatedCount++;

        DEBUG_LOG("Cell %d mesh generated successfully: %d vertices",
                  cellIndex, spatialHashCells[cellIndex].mesh.vertexCount);
    }

#ifdef OPS_DEBUG
    // Log summary
    if (totalDirty > 0) {
        printf("[INFO] Updated %d/%d dirty cells (%.1f%%)\n",
               updatedCount, totalDirty,
               (100.0f * updatedCount) / totalDirty);
    }
#endif

    return updatedCount;
}

// Check if a vertex lies on or very close to a cell boundary
static bool IsVertexOnCellBoundary(Vector3 vertex, Bounds cellBounds, float tolerance) {
    // Calculate the actual cell boundaries (without overlap)
    // The original cell size is CELL_SIZE, but bounds.size includes overlap
    float actualCellSize = CELL_SIZE;
    Vector3 cellMin = {
        cellBounds.center.x - actualCellSize * 0.5f,
        cellBounds.center.y - actualCellSize * 0.5f,
        cellBounds.center.z - actualCellSize * 0.5f
    };
    Vector3 cellMax = {
        cellBounds.center.x + actualCellSize * 0.5f,
        cellBounds.center.y + actualCellSize * 0.5f,
        cellBounds.center.z + actualCellSize * 0.5f
    };

    // Check if vertex is within tolerance of any face of the cell boundary
    bool onXMin = fabsf(vertex.x - cellMin.x) <= tolerance;
    bool onXMax = fabsf(vertex.x - cellMax.x) <= tolerance;
    bool onYMin = fabsf(vertex.y - cellMin.y) <= tolerance;
    bool onYMax = fabsf(vertex.y - cellMax.y) <= tolerance;
    bool onZMin = fabsf(vertex.z - cellMin.z) <= tolerance;
    bool onZMax = fabsf(vertex.z - cellMax.z) <= tolerance;

    // Vertex is on boundary if it's close to any face
    return onXMin || onXMax || onYMin || onYMax || onZMin || onZMax;
}

// Weld nearby vertices within a single mesh using spatial binning
static void WeldVerticesInMesh(Mesh* mesh, Bounds cellBounds) {
    if (!mesh || !mesh->vertices || !mesh->normals || mesh->vertexCount == 0) {
        return;
    }

    const float WELD_DISTANCE = 0.01f; // Slightly larger threshold for better welding
    const float BOUNDARY_TOLERANCE = 0.05f; // Tolerance for detecting boundary vertices

    // Ensure we have enough capacity for vertices
    if (mesh->vertexCount > cellVertexCapacity) {
        int newCap = mesh->vertexCount;
        CellVertex* tmp = (CellVertex*)realloc(cellVertices, newCap * sizeof(CellVertex));
        if (!tmp) {
            fprintf(stderr, "[ERROR] Failed to grow cellVertices to %d\n", newCap);
            return;
        }
        cellVertices = tmp;
        cellVertexCapacity = newCap;
    }

    // Clear the spatial grid
    for (int x = 0; x < WELD_GRID_SIZE; x++) {
        for (int y = 0; y < WELD_GRID_SIZE; y++) {
            for (int z = 0; z < WELD_GRID_SIZE; z++) {
                weldGrid[x][y][z].count = 0;
            }
        }
    }

    // Find the bounding box of all vertices
    Vector3 minBounds = {INFINITY, INFINITY, INFINITY};
    Vector3 maxBounds = {-INFINITY, -INFINITY, -INFINITY};

    for (int i = 0; i < mesh->vertexCount; i++) {
        Vector3 pos = {mesh->vertices[i * 3], mesh->vertices[i * 3 + 1], mesh->vertices[i * 3 + 2]};
        if (pos.x < minBounds.x) minBounds.x = pos.x;
        if (pos.y < minBounds.y) minBounds.y = pos.y;
        if (pos.z < minBounds.z) minBounds.z = pos.z;
        if (pos.x > maxBounds.x) maxBounds.x = pos.x;
        if (pos.y > maxBounds.y) maxBounds.y = pos.y;
        if (pos.z > maxBounds.z) maxBounds.z = pos.z;
    }

    // Calculate grid cell size
    Vector3 size = {maxBounds.x - minBounds.x, maxBounds.y - minBounds.y, maxBounds.z - minBounds.z};
    Vector3 gridCellSize = {
        size.x / (WELD_GRID_SIZE - 1),
        size.y / (WELD_GRID_SIZE - 1),
        size.z / (WELD_GRID_SIZE - 1)
    };

    // Avoid division by zero
    if (gridCellSize.x < 0.001f) gridCellSize.x = 0.001f;
    if (gridCellSize.y < 0.001f) gridCellSize.y = 0.001f;
    if (gridCellSize.z < 0.001f) gridCellSize.z = 0.001f;

    // Copy vertices and place them in spatial grid
    for (int i = 0; i < mesh->vertexCount; i++) {
        CellVertex* cv = &cellVertices[i];
        cv->position = (Vector3){mesh->vertices[i * 3], mesh->vertices[i * 3 + 1], mesh->vertices[i * 3 + 2]};
        cv->normal = (Vector3){mesh->normals[i * 3], mesh->normals[i * 3 + 1], mesh->normals[i * 3 + 2]};
        cv->originalIndex = i;

        if (mesh->colors) {
            cv->color = (Color){mesh->colors[i * 4], mesh->colors[i * 4 + 1], mesh->colors[i * 4 + 2], mesh->colors[i * 4 + 3]};
        } else {
            cv->color = WHITE;
        }

        // Calculate grid position
        int gx = (int)((cv->position.x - minBounds.x) / gridCellSize.x);
        int gy = (int)((cv->position.y - minBounds.y) / gridCellSize.y);
        int gz = (int)((cv->position.z - minBounds.z) / gridCellSize.z);

        // Clamp to grid bounds
        gx = (gx < 0) ? 0 : (gx >= WELD_GRID_SIZE) ? WELD_GRID_SIZE - 1 : gx;
        gy = (gy < 0) ? 0 : (gy >= WELD_GRID_SIZE) ? WELD_GRID_SIZE - 1 : gy;
        gz = (gz < 0) ? 0 : (gz >= WELD_GRID_SIZE) ? WELD_GRID_SIZE - 1 : gz;

        // Add to grid cell
        WeldGridCell* gridCell = &weldGrid[gx][gy][gz];
        if (gridCell->count >= gridCell->capacity) {
            int newCap = gridCell->capacity == 0 ? 8 : gridCell->capacity * 2;
            int* tmp = (int*)realloc(gridCell->vertexIndices, newCap * sizeof(int));
            if (!tmp) {
                fprintf(stderr, "[ERROR] Failed to grow weldGrid cell to %d\n", newCap);
                continue;
            }
            gridCell->vertexIndices = tmp;
            gridCell->capacity = newCap;
        }
        gridCell->vertexIndices[gridCell->count++] = i;
    }

    // Weld vertices using spatial grid for fast neighbor lookup
    for (int i = 0; i < mesh->vertexCount; i++) {
        if (cellVertices[i].originalIndex == -1) continue; // Already welded

        // Check if this vertex is on a cell boundary
        bool isOnBoundary = IsVertexOnCellBoundary(cellVertices[i].position, cellBounds, BOUNDARY_TOLERANCE);

        Vector3 avgPosition = cellVertices[i].position;
        Vector3 avgNormal = cellVertices[i].normal;
        int weldCount = 1;

        // Calculate grid position for this vertex
        int gx = (int)((cellVertices[i].position.x - minBounds.x) / gridCellSize.x);
        int gy = (int)((cellVertices[i].position.y - minBounds.y) / gridCellSize.y);
        int gz = (int)((cellVertices[i].position.z - minBounds.z) / gridCellSize.z);

        // Check neighboring grid cells
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dz = -1; dz <= 1; dz++) {
                    int nx = gx + dx;
                    int ny = gy + dy;
                    int nz = gz + dz;

                    if (nx < 0 || nx >= WELD_GRID_SIZE || ny < 0 || ny >= WELD_GRID_SIZE || nz < 0 || nz >= WELD_GRID_SIZE) {
                        continue;
                    }

                    WeldGridCell* gridCell = &weldGrid[nx][ny][nz];
                    for (int k = 0; k < gridCell->count; k++) {
                        int j = gridCell->vertexIndices[k];
                        if (j <= i || cellVertices[j].originalIndex == -1) continue; // Skip already processed

                        float dist = Vector3Distance(cellVertices[i].position, cellVertices[j].position);
                        if (dist < WELD_DISTANCE) {
                            // Check if the neighbor vertex is also on a boundary
                            bool neighborOnBoundary = IsVertexOnCellBoundary(cellVertices[j].position, cellBounds, BOUNDARY_TOLERANCE);

                            // Always average the normals for smooth shading
                            avgNormal.x += cellVertices[j].normal.x;
                            avgNormal.y += cellVertices[j].normal.y;
                            avgNormal.z += cellVertices[j].normal.z;

                            // Only average positions if neither vertex is on a boundary
                            // This preserves boundary vertex positions to avoid gaps
                            if (!isOnBoundary && !neighborOnBoundary) {
                                avgPosition.x += cellVertices[j].position.x;
                                avgPosition.y += cellVertices[j].position.y;
                                avgPosition.z += cellVertices[j].position.z;
                            }

                            weldCount++;

                            // Mark as welded
                            cellVertices[j].originalIndex = -1;
                        }
                    }
                }
            }
        }

        // Normalize and apply the averaged values
        if (weldCount > 1) {
            // Always normalize and apply the averaged normal
            avgNormal.x /= weldCount;
            avgNormal.y /= weldCount;
            avgNormal.z /= weldCount;

            float len = sqrtf(avgNormal.x * avgNormal.x + avgNormal.y * avgNormal.y + avgNormal.z * avgNormal.z);
            if (len > 0.0001f) {
                avgNormal.x /= len;
                avgNormal.y /= len;
                avgNormal.z /= len;
            }

            // Only apply averaged position if the vertex is not on a boundary
            if (!isOnBoundary) {
                avgPosition.x /= weldCount;
                avgPosition.y /= weldCount;
                avgPosition.z /= weldCount;

                // Update vertex position in mesh
                mesh->vertices[i * 3] = avgPosition.x;
                mesh->vertices[i * 3 + 1] = avgPosition.y;
                mesh->vertices[i * 3 + 2] = avgPosition.z;
            }
        }

        // Apply the smoothed normal (always applied for better shading)
        mesh->normals[i * 3] = avgNormal.x;
        mesh->normals[i * 3 + 1] = avgNormal.y;
        mesh->normals[i * 3 + 2] = avgNormal.z;
    }
}

// ---------------------------------------------------------------------------
// Edge-dedup hash table helpers (open-addressed, power-of-two size)
// Key = ((uint64_t)min(v1,v2) << 32) | (uint32_t)max(v1,v2)
// Slot 0 is a valid index, so we use key==0 as the empty sentinel only when
// both v1==0 and v2==0 — which can't happen for a real edge (that would be a
// degenerate triangle). If v1==v2==0 appeared, the triangle would already be
// filtered by the degenerate check above.
// ---------------------------------------------------------------------------
static inline uint64_t EdgeKey(int v1, int v2) {
    int lo = v1 < v2 ? v1 : v2;
    int hi = v1 < v2 ? v2 : v1;
    return ((uint64_t)(unsigned int)lo << 32) | (uint32_t)(unsigned int)hi;
}

// Ensure the edge hash table is large enough for the given number of edges.
// Size is always a power of two, at least 3× the number of edges to guarantee free slots.
static bool EdgeHashEnsure(int edgeCount) {
    int needed = edgeCount * 3;
    if (needed < 16) needed = 16;
    // Round up to next power of two
    int sz = 1;
    while (sz < needed) sz <<= 1;

    if (sz <= edgeHashTableSize) return true; // already big enough

    EdgeHashEntry* tmp = (EdgeHashEntry*)calloc(sz, sizeof(EdgeHashEntry));
    if (!tmp) {
        fprintf(stderr, "[ERROR] Failed to allocate edge hash table (%d slots)\n", sz);
        return false;
    }
    free(edgeHashTable);
    edgeHashTable = tmp;
    edgeHashTableSize = sz;
    return true;
}

// Insert edge (v1,v2) into the dedup table if not already present.
// Returns true if inserted (new edge), false if duplicate.
static bool EdgeHashInsert(int v1, int v2) {
    uint64_t key = EdgeKey(v1, v2);
    int mask = edgeHashTableSize - 1;
    int slot = (int)((key ^ (key >> 32)) & (unsigned int)mask);
    for (int probe = 0; probe < edgeHashTableSize; probe++) {
        int s = (slot + probe) & mask;
        if (edgeHashTable[s].key == 0) {
            // Empty slot — use special key=1 for the (0,0) sentinel edge
            // (degenerate edges are already filtered, so this is just a guard)
            edgeHashTable[s].key = key != 0 ? key : (uint64_t)1;
            edgeHashTable[s].edgeIdx = meshEdgeCount;
            return true;
        }
        if (edgeHashTable[s].key == key ||
            (key == 0 && edgeHashTable[s].key == (uint64_t)1)) {
            return false; // duplicate
        }
    }
    // Table full (should not happen if EdgeHashEnsure was called correctly)
    return true; // insert anyway — treat as new to avoid silent loss
}

// Simplify mesh by collapsing short edges and removing thin triangles
static void SimplifyMesh(Mesh* mesh) {
    if (!mesh || !mesh->vertices || !mesh->indices || mesh->vertexCount == 0 || mesh->triangleCount == 0) {
        return;
    }

    const float MIN_EDGE_LENGTH = 0.05f;  // Collapse edges shorter than this
    const float MIN_TRIANGLE_AREA = 0.001f;  // Remove triangles smaller than this

    // Ensure capacity for edges and triangles
    int maxEdges = mesh->triangleCount * 3;  // Each triangle has 3 edges
    if (maxEdges > meshEdgeCapacity) {
        int newCap = maxEdges;
        Edge* tmp = (Edge*)realloc(meshEdges, newCap * sizeof(Edge));
        if (!tmp) {
            fprintf(stderr, "[ERROR] Failed to grow meshEdges to %d\n", newCap);
            return;
        }
        meshEdges = tmp;
        meshEdgeCapacity = newCap;
    }

    if (mesh->triangleCount > meshTriangleCapacity) {
        int newCap = mesh->triangleCount;
        Triangle* tmp = (Triangle*)realloc(meshTriangles, newCap * sizeof(Triangle));
        if (!tmp) {
            fprintf(stderr, "[ERROR] Failed to grow meshTriangles to %d\n", newCap);
            return;
        }
        meshTriangles = tmp;
        meshTriangleCapacity = newCap;
    }

    // Prepare open-addressed edge hash for O(E) dedup
    if (!EdgeHashEnsure(maxEdges)) {
        // Fall through without edge dedup would be O(E^2); bail to avoid perf cliff
        return;
    }
    // Clear the hash table (calloc gives zeros; we just clear up to table size)
    memset(edgeHashTable, 0, edgeHashTableSize * sizeof(EdgeHashEntry));

    // Build edge list from triangles
    meshEdgeCount = 0;
    for (int t = 0; t < mesh->triangleCount; t++) {
        int i1 = mesh->indices[t * 3];
        int i2 = mesh->indices[t * 3 + 1];
        int i3 = mesh->indices[t * 3 + 2];

        // Skip degenerate triangles
        if (i1 == i2 || i2 == i3 || i1 == i3) continue;

        // Add three edges (avoid duplicates via hash)
        int vPairs[3][2] = {{i1, i2}, {i2, i3}, {i1, i3}};

        for (int e = 0; e < 3; e++) {
            int v1 = vPairs[e][0];
            int v2 = vPairs[e][1];

            if (EdgeHashInsert(v1, v2)) {
                // New edge — append to edge list
                if (meshEdgeCount < meshEdgeCapacity) {
                    Vector3 pos1 = {mesh->vertices[v1 * 3], mesh->vertices[v1 * 3 + 1], mesh->vertices[v1 * 3 + 2]};
                    Vector3 pos2 = {mesh->vertices[v2 * 3], mesh->vertices[v2 * 3 + 1], mesh->vertices[v2 * 3 + 2]};
                    int lo = v1 < v2 ? v1 : v2;
                    int hi = v1 < v2 ? v2 : v1;
                    meshEdges[meshEdgeCount].v1 = lo;
                    meshEdges[meshEdgeCount].v2 = hi;
                    meshEdges[meshEdgeCount].length = Vector3Distance(pos1, pos2);
                    meshEdges[meshEdgeCount].valid = true;
                    meshEdgeCount++;
                } else {
                    fprintf(stderr, "[ERROR] meshEdges array full (capacity %d, tried to insert edge %d)\n", meshEdgeCapacity, meshEdgeCount);
                }
            }
        }
    }

    // Create vertex remap table for edge collapses
    int* vertexRemap = (int*)malloc(mesh->vertexCount * sizeof(int));
    if (!vertexRemap) {
        fprintf(stderr, "[ERROR] Failed to allocate vertexRemap (%d entries)\n", mesh->vertexCount);
        return;
    }
    for (int i = 0; i < mesh->vertexCount; i++) {
        vertexRemap[i] = i;  // Initially, each vertex maps to itself
    }

    // Collapse short edges
    int collapsedEdges = 0;
    for (int e = 0; e < meshEdgeCount; e++) {
        if (!meshEdges[e].valid || meshEdges[e].length >= MIN_EDGE_LENGTH) continue;

        int v1 = meshEdges[e].v1;
        int v2 = meshEdges[e].v2;

        // Skip if either vertex has already been remapped
        if (vertexRemap[v1] != v1 || vertexRemap[v2] != v2) continue;

        // Collapse v2 into v1 (average their positions)
        Vector3 pos1 = {mesh->vertices[v1 * 3], mesh->vertices[v1 * 3 + 1], mesh->vertices[v1 * 3 + 2]};
        Vector3 pos2 = {mesh->vertices[v2 * 3], mesh->vertices[v2 * 3 + 1], mesh->vertices[v2 * 3 + 2]};

        Vector3 avgPos = {(pos1.x + pos2.x) * 0.5f, (pos1.y + pos2.y) * 0.5f, (pos1.z + pos2.z) * 0.5f};

        mesh->vertices[v1 * 3] = avgPos.x;
        mesh->vertices[v1 * 3 + 1] = avgPos.y;
        mesh->vertices[v1 * 3 + 2] = avgPos.z;

        // Average normals if available
        if (mesh->normals) {
            Vector3 norm1 = {mesh->normals[v1 * 3], mesh->normals[v1 * 3 + 1], mesh->normals[v1 * 3 + 2]};
            Vector3 norm2 = {mesh->normals[v2 * 3], mesh->normals[v2 * 3 + 1], mesh->normals[v2 * 3 + 2]};

            Vector3 avgNorm = {(norm1.x + norm2.x) * 0.5f, (norm1.y + norm2.y) * 0.5f, (norm1.z + norm2.z) * 0.5f};
            float len = sqrtf(avgNorm.x * avgNorm.x + avgNorm.y * avgNorm.y + avgNorm.z * avgNorm.z);
            if (len > 0.0001f) {
                avgNorm.x /= len;
                avgNorm.y /= len;
                avgNorm.z /= len;
            }

            mesh->normals[v1 * 3] = avgNorm.x;
            mesh->normals[v1 * 3 + 1] = avgNorm.y;
            mesh->normals[v1 * 3 + 2] = avgNorm.z;
        }

        // Average colors if available
        if (mesh->colors) {
            Color col1 = {mesh->colors[v1 * 4], mesh->colors[v1 * 4 + 1], mesh->colors[v1 * 4 + 2], mesh->colors[v1 * 4 + 3]};
            Color col2 = {mesh->colors[v2 * 4], mesh->colors[v2 * 4 + 1], mesh->colors[v2 * 4 + 2], mesh->colors[v2 * 4 + 3]};

            mesh->colors[v1 * 4] = (col1.r + col2.r) / 2;
            mesh->colors[v1 * 4 + 1] = (col1.g + col2.g) / 2;
            mesh->colors[v1 * 4 + 2] = (col1.b + col2.b) / 2;
            mesh->colors[v1 * 4 + 3] = (col1.a + col2.a) / 2;
        }

        // Remap v2 to v1
        vertexRemap[v2] = v1;
        collapsedEdges++;

        // Invalidate all edges involving v2 — O(E) scan is now on the short
        // compact edge list (no longer scanning O(E^2) across the full list)
        for (int i = 0; i < meshEdgeCount; i++) {
            if (meshEdges[i].v1 == v2 || meshEdges[i].v2 == v2) {
                meshEdges[i].valid = false;
            }
        }
    }

    // Update triangle indices using vertex remap
    int validTriangles = 0;
    for (int t = 0; t < mesh->triangleCount; t++) {
        int i1 = vertexRemap[mesh->indices[t * 3]];
        int i2 = vertexRemap[mesh->indices[t * 3 + 1]];
        int i3 = vertexRemap[mesh->indices[t * 3 + 2]];

        // Skip degenerate triangles
        if (i1 == i2 || i2 == i3 || i1 == i3) continue;

        // Calculate triangle area to filter out thin triangles
        Vector3 v1 = {mesh->vertices[i1 * 3], mesh->vertices[i1 * 3 + 1], mesh->vertices[i1 * 3 + 2]};
        Vector3 v2 = {mesh->vertices[i2 * 3], mesh->vertices[i2 * 3 + 1], mesh->vertices[i2 * 3 + 2]};
        Vector3 v3 = {mesh->vertices[i3 * 3], mesh->vertices[i3 * 3 + 1], mesh->vertices[i3 * 3 + 2]};

        Vector3 edge1 = {v2.x - v1.x, v2.y - v1.y, v2.z - v1.z};
        Vector3 edge2 = {v3.x - v1.x, v3.y - v1.y, v3.z - v1.z};

        Vector3 cross = {
            edge1.y * edge2.z - edge1.z * edge2.y,
            edge1.z * edge2.x - edge1.x * edge2.z,
            edge1.x * edge2.y - edge1.y * edge2.x
        };

        float area = 0.5f * sqrtf(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z);

        if (area >= MIN_TRIANGLE_AREA) {
            // Keep this triangle
            mesh->indices[validTriangles * 3] = i1;
            mesh->indices[validTriangles * 3 + 1] = i2;
            mesh->indices[validTriangles * 3 + 2] = i3;
            validTriangles++;
        }
    }

    // Update triangle count
#ifdef OPS_DEBUG
    int removedTriangles = mesh->triangleCount - validTriangles;
#endif
    mesh->triangleCount = validTriangles;

    free(vertexRemap);

    DEBUG_LOG("Mesh simplified: collapsed %d edges, removed %d thin triangles",
              collapsedEdges, removedTriangles);
}

int UpdateParticleSystem(int maxUpdatesPerFrame) {
    return UpdateDirtyCells(maxUpdatesPerFrame);
}

int GetParticleCount(void) {
    return currentParticleCount;
}

int GetParticleCapacity(void) {
    return maxParticleCount;
}

void DrawParticleMeshes(Material material, bool wireframe) {
    // Set wireframe mode if requested
    if (wireframe) {
        rlEnableWireMode();
    }

    // Draw meshes for all active cells
    for (int i = 0; i < activeCells.count; i++) {
        int cellIndex = activeCells.indices[i];
        if (cellIndex >= 0 && cellIndex < spatialHashCellsCapacity &&
            spatialHashCells[cellIndex].hasMesh &&
            spatialHashCells[cellIndex].particleCount > 0) {

            Mesh* mesh = &spatialHashCells[cellIndex].mesh;

            if (wireframe) {
                // Draw wireframe mesh
                DrawMesh(*mesh, material, MatrixIdentity());

                // Draw normal vectors as lines
                if (mesh->normals != NULL && mesh->vertices != NULL) {
                    const float normalLength = 0.2f; // Length of normal lines (1/10 of original)

                    rlBegin(RL_LINES);

                    for (int v = 0; v < mesh->vertexCount; v++) {
                        // Start point: vertex position
                        Vector3 vertex = {
                            mesh->vertices[v * 3],
                            mesh->vertices[v * 3 + 1],
                            mesh->vertices[v * 3 + 2]
                        };

                        // End point: vertex + normal * length
                        Vector3 normal = {
                            mesh->normals[v * 3],
                            mesh->normals[v * 3 + 1],
                            mesh->normals[v * 3 + 2]
                        };

                        Vector3 endPoint = {
                            vertex.x + normal.x * normalLength,
                            vertex.y + normal.y * normalLength,
                            vertex.z + normal.z * normalLength
                        };

                        // Get vertex color based on material (same as face material)
                        Color vertexColor = WHITE; // Default color
                        if (mesh->colors != NULL) {
                            vertexColor = (Color){
                                mesh->colors[v * 4],
                                mesh->colors[v * 4 + 1],
                                mesh->colors[v * 4 + 2],
                                mesh->colors[v * 4 + 3]
                            };
                        }

                        // Set normal line color to match vertex/face material
                        rlColor4ub(vertexColor.r, vertexColor.g, vertexColor.b, vertexColor.a);

                        // Draw line from vertex to end point
                        rlVertex3f(vertex.x, vertex.y, vertex.z);
                        rlVertex3f(endPoint.x, endPoint.y, endPoint.z);
                    }

                    rlEnd();
                }
            } else {
                // Solid draw: use DrawMesh with the caller-supplied material
                // (which carries the shader + lighting uniforms).
                // The per-frame rlVertex3f immediate-mode path has been removed —
                // the mesh is already GPU-resident after UploadMesh().
                DrawMesh(*mesh, material, MatrixIdentity());
            }
        }
    }

    // Disable wireframe mode if enabled
    if (wireframe) {
        rlDisableWireMode();
    }
}

void DrawParticleSystemDebug(bool showBounds) {
    if (!showBounds) return;

    // Draw bounds for all active cells
    for (int i = 0; i < activeCells.count; i++) {
        int cellIndex = activeCells.indices[i];
        if (cellIndex >= 0 && cellIndex < spatialHashCellsCapacity &&
            spatialHashCells[cellIndex].particleCount > 0) {

            Bounds bounds = spatialHashCells[cellIndex].bounds;
            Color boundsColor = spatialHashCells[cellIndex].dirty ? RED : GREEN;

            DrawCubeWires(bounds.center, bounds.size.x, bounds.size.y, bounds.size.z, boundsColor);
        }
    }
}

void DrawParticles(bool useInstancing, int maxInstancesToDraw) {
    // Guard against malloc failures in InitializeParticleSystem
    if (!particleMatrices || !particleColors) {
        fprintf(stderr, "[ERROR] DrawParticles: particleMatrices or particleColors is NULL\n");
        return;
    }

    if (useInstancing) {
        // Prepare matrices and colors for instanced rendering
        particleInstanceCount = 0;

        for (int i = 0; i < maxParticleCount && particleInstanceCount < maxInstancesToDraw; i++) {
            if (particles[i].active) {
                // Set transformation matrix (translation only for particles)
                particleMatrices[particleInstanceCount] = MatrixTranslate(
                    particles[i].position.x,
                    particles[i].position.y,
                    particles[i].position.z
                );

                // Set color based on material ID
                particleColors[particleInstanceCount] = GetMaterialColor(particles[i].materialId);

                particleInstanceCount++;
            }
        }

        // Draw all particles using instanced rendering
        if (particleInstanceCount > 0) {
            DrawMeshInstanced(particleModel.meshes[0], particleModel.materials[0],
                             particleMatrices, particleInstanceCount);
        }
    } else {
        // Fallback to individual particle rendering (much slower)
        int drawn = 0;
        for (int i = 0; i < maxParticleCount && drawn < maxInstancesToDraw; i++) {
            if (particles[i].active) {
                DrawSphere(particles[i].position, particleRadius * 0.1f,
                          GetMaterialColor(particles[i].materialId));
                drawn++;
            }
        }
    }
}

void GetParticleSystemStats(int* activeCellCount, int* dirtyRegionCount, int* meshVertexCount) {
    if (activeCellCount) *activeCellCount = activeCells.count;

    if (dirtyRegionCount) {
        int dirtyCount = 0;
        for (int i = 0; i < activeCells.count; i++) {
            int cellIndex = activeCells.indices[i];
            if (cellIndex >= 0 && cellIndex < spatialHashCellsCapacity &&
                spatialHashCells[cellIndex].dirty) {
                dirtyCount++;
            }
        }
        *dirtyRegionCount = dirtyCount;
    }

    if (meshVertexCount) {
        int vertexCount = 0;
        for (int i = 0; i < activeCells.count; i++) {
            int cellIndex = activeCells.indices[i];
            if (cellIndex >= 0 && cellIndex < spatialHashCellsCapacity &&
                spatialHashCells[cellIndex].hasMesh) {
                vertexCount += spatialHashCells[cellIndex].mesh.vertexCount;
            }
        }
        *meshVertexCount = vertexCount;
    }
}
