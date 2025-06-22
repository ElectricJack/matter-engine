#include "../include/open_particle_surface.h"
#include "../include/surface.h"
#include "../include/object_allocator.h"
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
#define HASH_TABLE_SIZE       1024     // Size of hash table (must be power of 2)
#define HASH_MASK             (HASH_TABLE_SIZE - 1)  // Mask for fast modulo
#define HASH_BOUNDS_DETAIL    4        // Detail level for each bounds (2^4 = 16 divisions per cell)

// 3D grid coordinates for unbounded grid
typedef struct {
    int x;
    int y;
    int z;
} GridCoord;

// Particle structure with internal metadata
typedef struct {
    Vector3 position;
    int     materialId;
    int     cellIndex;      // Index into the spatial hash cells
    bool    active;         // Whether the particle is active
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

// Cell map structure - maps grid coordinates to cell indices
typedef struct {
    GridCoord* keys;       // Array of grid coordinates
    int* values;           // Array of cell indices
    bool* occupied;        // Whether each slot is occupied
    int size;              // Current number of entries
    int capacity;          // Maximum number of entries
} CellMap;

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
static CellMap cellMap = {0};

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
static void InitializeCellMap(int initialCapacity);
static int GetCellIndex(GridCoord coord);
static GridCoord GetGridCoordFromPosition(Vector3 position);
static int GetSpatialHashIndex(Vector3 position);
static Bounds GetSpatialHashBounds(GridCoord coord);
static int UpdateDirtyCells(int maxUpdates);

// Get a hash value for grid coordinates
static unsigned int HashGridCoord(GridCoord coord) {
    // Simple hash function for 3D coordinates
    // Convert to unsigned to handle negative coordinates properly
    unsigned int ux = (unsigned int)(coord.x * 73856093);
    unsigned int uy = (unsigned int)(coord.y * 19349663);
    unsigned int uz = (unsigned int)(coord.z * 83492791);
    
    // XOR the values together
    unsigned int hash = ux ^ uy ^ uz;
    return hash & HASH_MASK;
}

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
static void InitializeCellMap(int initialCapacity) {
    DEBUG_LOG("Initializing cell map with capacity %d", initialCapacity);
    cellMap.capacity = initialCapacity;
    cellMap.size = 0;
    cellMap.keys = (GridCoord*)malloc(initialCapacity * sizeof(GridCoord));
    cellMap.values = (int*)malloc(initialCapacity * sizeof(int));
    cellMap.occupied = (bool*)malloc(initialCapacity * sizeof(bool));
    
    // Initialize all slots as unoccupied
    for (int i = 0; i < initialCapacity; i++) {
        cellMap.occupied[i] = false;
    }
}

// Find or create a cell for the given grid coordinates
static int GetCellIndex(GridCoord coord) {
    // If no cell map yet, initialize it
    if (cellMap.keys == NULL) {
        InitializeCellMap(HASH_TABLE_SIZE);
    }
    
    // Compute initial hash position
    unsigned int hash = HashGridCoord(coord);
    unsigned int index = hash;
    
    // Linear probing to find the key or an empty slot
    while (cellMap.occupied[index]) {
        // Check if this is the coord we're looking for
        if (cellMap.keys[index].x == coord.x && 
            cellMap.keys[index].y == coord.y && 
            cellMap.keys[index].z == coord.z) {
            return cellMap.values[index]; // Found it
        }
        
        // Move to next slot (linear probing)
        index = (index + 1) & HASH_MASK;
        
        // If we've gone all the way around, the table is full
        if (index == hash) {
            // Resize the hash table
            int oldCapacity = cellMap.capacity;
            GridCoord* oldKeys = cellMap.keys;
            int* oldValues = cellMap.values;
            bool* oldOccupied = cellMap.occupied;
            
            // Keep track of old size
            int oldSize = cellMap.size;
            
            // Double capacity
            InitializeCellMap(oldCapacity * 2);
            cellMap.size = oldSize; // Restore the size count
            
            // Reinsert all existing entries
            for (int i = 0; i < oldCapacity; i++) {
                if (oldOccupied[i]) {
                    unsigned int newHash = HashGridCoord(oldKeys[i]);
                    unsigned int newIndex = newHash;
                    
                    while (cellMap.occupied[newIndex]) {
                        newIndex = (newIndex + 1) & HASH_MASK;
                    }
                    
                    cellMap.keys[newIndex] = oldKeys[i];
                    cellMap.values[newIndex] = oldValues[i];
                    cellMap.occupied[newIndex] = true;
                }
            }
            
            // Free old arrays
            free(oldKeys);
            free(oldValues);
            free(oldOccupied);
            
            // Try again
            return GetCellIndex(coord);
        }
    }
    
    // Not found, this is a new grid cell
    cellMap.keys[index] = coord;
    cellMap.values[index] = cellMap.size;
    cellMap.occupied[index] = true;
    cellMap.size++;
    
    // Make sure spatialHashCells has room for this index
    if (cellMap.size > spatialHashCellsCapacity) {
        int newCapacity = cellMap.size + 1000; // Add some buffer
        spatialHashCells = (SpatialHashCell*)realloc(spatialHashCells, newCapacity * sizeof(SpatialHashCell));
        
        // Initialize new cells
        for (int i = spatialHashCellsCapacity; i < newCapacity; i++) {
            spatialHashCells[i].particleCount = 0;
            spatialHashCells[i].particleIndices = NULL; // Will allocate on demand
            spatialHashCells[i].dirty = false;
            spatialHashCells[i].hasMesh = false;
        }
        
        spatialHashCellsCapacity = newCapacity;
    }
    
    return cellMap.values[index];
}

// Get spatial hash cell index from position
static int GetSpatialHashIndex(Vector3 position) {
    GridCoord coord = GetGridCoordFromPosition(position);
    int index = GetCellIndex(coord);
    return index;
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
    
    // Initialize cell map for hash table
    InitializeCellMap(HASH_TABLE_SIZE);
    
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
    
    // Free cell map
    if (cellMap.keys != NULL) {
        free(cellMap.keys);
        free(cellMap.values);
        free(cellMap.occupied);
        cellMap.keys = NULL;
        cellMap.values = NULL;
        cellMap.occupied = NULL;
    }
    
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
    
    // Reset state
    maxParticleCount = 0;
    currentParticleCount = 0;
    particleRadius = 0.0f;
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
    
    // Calculate spatial hash index
    int cellIndex = GetSpatialHashIndex(position);
    particles[particleIndex].cellIndex = cellIndex;
    
    // Set the cell's grid coordinates and bounds if it's a new cell
    if (spatialHashCells[cellIndex].particleCount == 0) {
        GridCoord coord = GetGridCoordFromPosition(position);
        spatialHashCells[cellIndex].coord = coord;
        spatialHashCells[cellIndex].bounds = GetSpatialHashBounds(coord);
        
        // First particle - need to allocate the indices array
        if (spatialHashCells[cellIndex].particleIndices == NULL) {
            spatialHashCells[cellIndex].particleIndices = (int*)malloc(10000 * sizeof(int));
            if (spatialHashCells[cellIndex].particleIndices == NULL) {
                printf("Failed to allocate particle indices array for cell %d\n", cellIndex);
                return -1;
            }
        }
    }
    
    // Safety check before adding particle to cell
    if (spatialHashCells[cellIndex].particleIndices == NULL) {
        printf("Error: particleIndices is NULL for cell %d\n", cellIndex);
        return -1;
    }
    
    // Add particle to cell
    spatialHashCells[cellIndex].particleIndices[spatialHashCells[cellIndex].particleCount] = particleIndex;
    spatialHashCells[cellIndex].particleCount++;
    spatialHashCells[cellIndex].dirty = true; // Mark as dirty
    
    // Track cell as active
    AddActiveCellIfNeeded(cellIndex);
    
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
    
    // Get current cell index
    int oldCellIndex = particles[handle].cellIndex;
    
    // Calculate new cell index
    int newCellIndex = GetSpatialHashIndex(newPosition);
    
    // Update position
    particles[handle].position = newPosition;
    
    // If cell has changed, update
    if (newCellIndex != oldCellIndex) {
        // Remove from old cell
        int oldCellCount = spatialHashCells[oldCellIndex].particleCount;
        int* oldIndices = spatialHashCells[oldCellIndex].particleIndices;
        
        // Find and remove the particle from the old cell
        for (int i = 0; i < oldCellCount; i++) {
            if (oldIndices[i] == handle) {
                // Replace with the last element and decrement count
                oldIndices[i] = oldIndices[oldCellCount - 1];
                spatialHashCells[oldCellIndex].particleCount--;
                break;
            }
        }
        
        // Set the new cell's grid coordinates and bounds if it's a new cell
        if (spatialHashCells[newCellIndex].particleCount == 0) {
            GridCoord coord = GetGridCoordFromPosition(newPosition);
            spatialHashCells[newCellIndex].coord = coord;
            spatialHashCells[newCellIndex].bounds = GetSpatialHashBounds(coord);
        }
        
        // Add to new cell
        spatialHashCells[newCellIndex].particleIndices[spatialHashCells[newCellIndex].particleCount] = handle;
        spatialHashCells[newCellIndex].particleCount++;
        
        // Update particle's cell index
        particles[handle].cellIndex = newCellIndex;
        
        // Mark both cells as dirty
        spatialHashCells[oldCellIndex].dirty = true;
        spatialHashCells[newCellIndex].dirty = true;
        
        // Track new cell as active
        AddActiveCellIfNeeded(newCellIndex);
    }
    
    return true;
}

bool DeleteParticle(ParticleHandle handle) {
    // Validate handle
    if (handle < 0 || handle >= maxParticleCount || !particles[handle].active) {
        return false;
    }
    
    // Get cell index
    int cellIndex = particles[handle].cellIndex;
    
    // Remove from cell
    int cellCount = spatialHashCells[cellIndex].particleCount;
    int* indices = spatialHashCells[cellIndex].particleIndices;
    
    // Find and remove the particle from the cell
    for (int i = 0; i < cellCount; i++) {
        if (indices[i] == handle) {
            // Replace with the last element and decrement count
            indices[i] = indices[cellCount - 1];
            spatialHashCells[cellIndex].particleCount--;
            break;
        }
    }
    
    // Mark cell as dirty
    spatialHashCells[cellIndex].dirty = true;
    
    // Mark particle as inactive
    particles[handle].active = false;
    
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
    
    // Cap max updates to a reasonable value
    if (maxUpdates > 100) maxUpdates = 100;
    
    // First pass: find cells with highest particle counts - ONLY from active cells
    for (int i = 0; i < activeCells.count && dirtyFound < maxUpdates; i++) {
        int cellIndex = activeCells.indices[i];
        if (cellIndex < 0 || cellIndex >= spatialHashCellsCapacity) {
            continue;
        }
        
        if (spatialHashCells[cellIndex].dirty && spatialHashCells[cellIndex].particleCount > 0) {
            // Find position to insert based on particle count (highest first)
            int pos = 0;
            while (pos < dirtyFound && 
                   spatialHashCells[cellIndex].particleCount < dirtyCounts[pos]) {
                pos++;
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
            }
        }
    }
    
    // Second pass: update the cells we found, in priority order
    for (int idx = 0; idx < dirtyFound; idx++) {
        int cellIndex = dirtyIndices[idx];
        
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
            spatialHashCells[cellIndex].hasMesh = false;
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
    
    printf("Drawing debug bounds for %d active cells\n", activeCells.count);
    
    // Draw bounds for all active cells
    for (int i = 0; i < activeCells.count; i++) {
        int cellIndex = activeCells.indices[i];
        if (cellIndex >= 0 && cellIndex < spatialHashCellsCapacity && 
            spatialHashCells[cellIndex].particleCount > 0) {
            
            Bounds bounds = spatialHashCells[cellIndex].bounds;
            Color boundsColor = spatialHashCells[cellIndex].dirty ? RED : GREEN;
            
            printf("Cell %d: center=(%.2f,%.2f,%.2f) size=(%.2f,%.2f,%.2f) particles=%d\n", 
                   cellIndex, bounds.center.x, bounds.center.y, bounds.center.z,
                   bounds.size.x, bounds.size.y, bounds.size.z,
                   spatialHashCells[cellIndex].particleCount);
                   
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