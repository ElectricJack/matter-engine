#include "../include/open_particle_surface.h"
#include "../include/surface.h"
#include "../include/object_allocator.h"
#include "../include/spatial_hash.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

// Debug printf wrapper for tracking issues
#define DEBUG_LOG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)

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
} InternalParticle;

// Spatial hash cell structure
typedef struct {
    int            particleCount;    // Number of particles in this cell
    int*           particleIndices;  // Array of indices to particles
    bool           dirty;            // Whether this cell needs mesh regeneration
    Mesh           mesh;             // Mesh for this cell
    Bounds         bounds;           // Bounds for this cell
    bool           hasMesh;          // Whether a mesh has been generated
    GridCoord      coord;            // Grid coordinates of this cell
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

// Static buffer for particle data to avoid repeated malloc/free
static Particle* cellParticleBuffer = NULL;
static int cellParticleBufferSize = 0;

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
    
    // Create bounds with center, size, and division power
    Bounds bounds = {
        .center = center,
        .size = {CELL_SIZE, CELL_SIZE, CELL_SIZE},
        .divisionPow = HASH_BOUNDS_DETAIL
    };
    
    return bounds;
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

static int GetCellIndex(GridCoord coord) {
    // Convert grid coordinates to world position for spatial hash lookup
    Vector3 worldPos = GridCoordToWorldPos(coord);
    
    // Try to find existing cell using the spatial hash
    SpatialHashCell* existingCell = (SpatialHashCell*)sh_query_first(spatialHash, worldPos.x, worldPos.y, worldPos.z, 0.1f);
    
    if (existingCell != NULL) {
        // Found existing cell, return its index
        return (int)(existingCell - spatialHashCells);
    }
    
    // Cell doesn't exist yet, create a new one
    
    // Make sure spatialHashCells has room for a new cell
    if (totalCellCount >= spatialHashCellsCapacity) {
        int newCapacity = spatialHashCellsCapacity + 1000; // Add some buffer
        spatialHashCells = (SpatialHashCell*)realloc(spatialHashCells, newCapacity * sizeof(SpatialHashCell));
        
        // Initialize new cells
        for (int i = spatialHashCellsCapacity; i < newCapacity; i++) {
            SpatialHashCell* cell = &spatialHashCells[i];
            cell->particleCount   = 0;
            cell->particleIndices = NULL; // Will allocate on demand
            cell->dirty           = false;
            cell->hasMesh         = false;
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
    newCell->particleIndices = NULL;
    newCell->dirty = false;
    newCell->hasMesh = false;
    
    // Insert the cell into the spatial hash at its world position
    sh_insert(spatialHash, worldPos.x, worldPos.y, worldPos.z, newCell);
    
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
                        printf("Warning: Maximum overlapping cells reached (%d)\n", maxCells);
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
    activeCells.count = 0;
}

// Add a cell to the active list if not already present
static void AddActiveCellIfNeeded(int cellIndex) {
    // Check if cell is already in active list
    for (int i = 0; i < activeCells.count; i++) {
        if (activeCells.indices[i] == cellIndex) {
            return; // Already tracked
        }
    }
    
    DEBUG_LOG("Adding cell %d to active cells list", cellIndex);
    
    // Expand capacity if needed
    if (activeCells.count >= activeCells.capacity) {
        activeCells.capacity *= 2;
        activeCells.indices = (int*)realloc(activeCells.indices, 
                                          activeCells.capacity * sizeof(int));
    }
    
    // Add to active list
    activeCells.indices[activeCells.count++] = cellIndex;
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
    particles = (InternalParticle*)malloc(maxParticleCount * sizeof(InternalParticle));
    
    // Initialize particles as inactive
    for (int i = 0; i < maxParticleCount; i++) {
        particles[i].active = false;
    }
    
    // Initialize the new shared spatial hash for storing cells
    // Estimate cell capacity based on particle distribution
    int estimatedCellCount = maxParticles / 10; // Rough estimate: ~10 particles per cell
    spatialHash = sh_create(CELL_SIZE, estimatedCellCount);
    
    // Allocate initial array for spatial hash cells
    // This will grow dynamically as needed
    int initialCellCapacity = 1000;
    spatialHashCells = (SpatialHashCell*)malloc(initialCellCapacity * sizeof(SpatialHashCell));
    spatialHashCellsCapacity = initialCellCapacity;
    
    // Initialize active cell tracking
    InitializeActiveCellTracking();
    
    // Setup for instanced rendering
    Mesh sphereMesh = GenMeshSphere(particleRadius * 0.1f, 4, 4);
    particleModel = LoadModelFromMesh(sphereMesh);
    
    // Preallocate matrices and colors for particles
    particleMatrices = (Matrix*)malloc(maxParticleCount * sizeof(Matrix));
    particleColors = (Color*)malloc(maxParticleCount * sizeof(Color));
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
            if (spatialHashCells[cellIndex].particleIndices) {
                free(spatialHashCells[cellIndex].particleIndices);
                spatialHashCells[cellIndex].particleIndices = NULL;
            }
        }
    }
    
    // Free the static cell particle buffer
    if (cellParticleBuffer != NULL) {
        free(cellParticleBuffer);
        cellParticleBuffer = NULL;
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
}

ParticleHandle CreateParticle(Vector3 position, int materialId) {
    // Check if we've reached capacity
    if (currentParticleCount >= maxParticleCount) {
        DEBUG_LOG("Failed to create particle: maximum capacity reached");
        return -1;
    }
    
    // Find a free slot for the new particle
    int particleIndex = -1;
    for (int i = 0; i < maxParticleCount; i++) {
        if (!particles[i].active) {
            particleIndex = i;
            break;
        }
    }
    
    if (particleIndex == -1) {
        DEBUG_LOG("Failed to create particle: no free slots available");
        return -1;
    }
    
    // Initialize the particle
    particles[particleIndex].position = position;
    particles[particleIndex].materialId = materialId;
    particles[particleIndex].active = true;
    particles[particleIndex].cellCount = 0;
    
    // Find all cells that this particle overlaps
    GridCoord overlappingCells[MAX_OVERLAPPING_CELLS];
    int overlappingCellCount = 0;
    GetOverlappingCells(position, particleRadius, overlappingCells, &overlappingCellCount, MAX_OVERLAPPING_CELLS);
    
    // Add particle to each overlapping cell
    for (int i = 0; i < overlappingCellCount; i++) {
        // Get cell index (creates the cell if it doesn't exist yet)
        int cellIndex = GetCellIndex(overlappingCells[i]);
        
        // Store cell index in particle
        if (particles[particleIndex].cellCount < MAX_OVERLAPPING_CELLS) {
            particles[particleIndex].cellIndices[particles[particleIndex].cellCount++] = cellIndex;
        } else {
            printf("Warning: Maximum overlapping cells reached for particle %d\n", particleIndex);
            break;
        }

        SpatialHashCell* cell = &spatialHashCells[cellIndex];
        
        // Set the cell's grid coordinates and bounds if it's a new cell
        if (cell->particleCount == 0) {
            cell->coord = overlappingCells[i];
            cell->bounds = GetSpatialHashBounds(overlappingCells[i]);
            
            // First particle - need to allocate the indices array
            if (cell->particleIndices == NULL) {
                cell->particleIndices = (int*)malloc(10000 * sizeof(int));
                if (cell->particleIndices == NULL) {
                    printf("Failed to allocate particle indices array for cell %d\n", cellIndex);
                    continue; // Skip this cell but continue with others
                }
            }
        }
        
        // Safety check before adding particle to cell
        if (cell->particleIndices == NULL) {
            printf("Error: particleIndices is NULL for cell %d\n", cellIndex);
            continue; // Skip this cell but continue with others
        }
        
        // Add particle to cell
        cell->particleIndices[cell->particleCount] = particleIndex;
        cell->particleCount++;
        cell->dirty = true; // Mark as dirty
        
        // Track cell as active
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
        int cellCount = cell->particleCount;
        int* indices = cell->particleIndices;
        
        if (indices != NULL) {
            // Find and remove the particle
            for (int j = 0; j < cellCount; j++) {
                if (indices[j] == handle) {
                    // Replace with the last element and decrement count
                    indices[j] = indices[cellCount - 1];
                    cell->particleCount--;
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
        
        // Store cell index in particle
        if (particles[handle].cellCount < MAX_OVERLAPPING_CELLS) {
            particles[handle].cellIndices[particles[handle].cellCount++] = cellIndex;
        } else {
            printf("Warning: Maximum overlapping cells reached for particle %d\n", handle);
            break;
        }

        // Get the cell
        SpatialHashCell* cell = &spatialHashCells[cellIndex];
        
        // Set the cell's grid coordinates and bounds if it's a new cell
        if (cell->particleCount == 0) {
            cell->coord = newOverlappingCells[i];
            cell->bounds = GetSpatialHashBounds(newOverlappingCells[i]);
            
            // First particle - need to allocate the indices array
            if (cell->particleIndices == NULL) {
                cell->particleIndices = (int*)malloc(10000 * sizeof(int));
                if (cell->particleIndices == NULL) {
                    printf("Failed to allocate particle indices array for cell %d\n", cellIndex);
                    continue; // Skip this cell but continue with others
                }
            }
        }
        
        // Safety check before adding particle to cell
        if (cell->particleIndices == NULL) {
            printf("Error: particleIndices is NULL for cell %d\n", cellIndex);
            continue; // Skip this cell but continue with others
        }
        
        // Add particle to cell
        cell->particleIndices[cell->particleCount] = handle;
        cell->particleCount++;
        cell->dirty = true; // Mark as dirty
        
        // Track cell as active
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
        int cellParticleCount = spatialHashCells[cellIndex].particleCount;
        int* indices = spatialHashCells[cellIndex].particleIndices;
        
        if (indices != NULL) {
            // Find and remove the particle
            for (int j = 0; j < cellParticleCount; j++) {
                if (indices[j] == handle) {
                    // Replace with the last element and decrement count
                    indices[j] = indices[cellParticleCount - 1];
                    spatialHashCells[cellIndex].particleCount--;
                    break;
                }
            }
        }
        
        // Mark cell as dirty
        spatialHashCells[cellIndex].dirty = true;
    }
    
    // Note: particles are now tracked via their cells in the spatial hash
    
    // Mark particle as inactive
    particles[handle].active = false;
    
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
    if (totalDirty > 5 && maxUpdates < totalDirty) {
        printf("[INFO] %d total dirty cells found, processing up to %d\n", totalDirty, maxUpdates);
    }
    
    // Track newly created cells with a higher priority for the first few frames
    // This helps ensure new cells get their initial meshes
    static int* newCellIndices = NULL;
    static int newCellCount = 0;
    static int newCellCapacity = 0;
    
    // Initialize new cell tracking if needed
    if (newCellIndices == NULL) {
        newCellCapacity = 100;
        newCellIndices = (int*)malloc(newCellCapacity * sizeof(int));
        newCellCount = 0;
    }
    
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
                
                // Track new cells for debugging
                if (isNewCell) {
                    printf("[INFO] Prioritizing new cell %d with %d particles\n", 
                           cellIndex, spatialHashCells[cellIndex].particleCount);
                }
            }
        }
    }
    
    // Second pass: update the cells we found, in priority order
    for (int idx = 0; idx < dirtyFound; idx++) {
        int cellIndex = dirtyIndices[idx];
        
        // Log cell being processed
        printf("[DEBUG] Processing cell %d (%s): %d particles\n", 
               cellIndex, 
               spatialHashCells[cellIndex].hasMesh ? "existing" : "new",
               spatialHashCells[cellIndex].particleCount);
        
        // Ensure the buffer is large enough
        if (spatialHashCells[cellIndex].particleCount > cellParticleBufferSize) {
            cellParticleBufferSize = spatialHashCells[cellIndex].particleCount * 1.5;
            cellParticleBuffer = (Particle*)realloc(cellParticleBuffer, 
                                               cellParticleBufferSize * sizeof(Particle));
        }
        
        // Copy particle data to buffer - only copy valid particles
        int validCount = 0;
        for (int j = 0; j < spatialHashCells[cellIndex].particleCount; j++) {
            int particleIdx = spatialHashCells[cellIndex].particleIndices[j];
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
            printf("[DEBUG] Cell %d skipped: insufficient valid particles (%d)\n", cellIndex, validCount);
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

        UploadMesh(&spatialHashCells[cellIndex].mesh, false);
        spatialHashCells[cellIndex].hasMesh = true;
        
        // Mark as clean
        spatialHashCells[cellIndex].dirty = false;
        updatedCount++;
        
        printf("[DEBUG] Cell %d mesh generated successfully: %d vertices\n", 
               cellIndex, spatialHashCells[cellIndex].mesh.vertexCount);
    }
    
    // Log summary
    if (totalDirty > 0) {
        printf("[INFO] Updated %d/%d dirty cells (%.1f%%)\n", 
               updatedCount, totalDirty, 
               (100.0f * updatedCount) / totalDirty);
    }
    
    return updatedCount;
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
            
            DrawMesh(spatialHashCells[cellIndex].mesh, material, MatrixIdentity());
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
            
            // printf("Cell %d: center=(%.2f,%.2f,%.2f) size=(%.2f,%.2f,%.2f) particles=%d\n", 
            //        cellIndex, bounds.center.x, bounds.center.y, bounds.center.z,
            //        bounds.size.x, bounds.size.y, bounds.size.z,
            //        spatialHashCells[cellIndex].particleCount);
                   
            DrawCubeWires(bounds.center, bounds.size.x, bounds.size.y, bounds.size.z, boundsColor);
        }
    }
}

void DrawParticles(bool useInstancing, int maxInstancesToDraw) {
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