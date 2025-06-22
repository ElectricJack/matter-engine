#include "SurfaceLib/surface.h"
#include "ObjectAllocatorLib/object_allocator.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <string.h>

// Demo configuration
#define PARTICLE_COUNT         1000000  // Target 1 million particles
#define PARTICLE_RADIUS        0.5f     // Particle radius
#define VOLUME_SIZE            100.0f   // Overall volume size
#define BOUNDS_SIZE            16.0f    // Size of each spatial hash bounds
#define MAX_DIRTY_UPDATES      3        // Maximum dirty bound updates per frame
#define INITIAL_PARTICLE_COUNT 10000    // Start with fewer particles and grow

// Spatial hashing configuration
#define HASH_GRID_SIZE         25       // Size of the spatial hash grid (25x25x25)
#define HASH_BOUNDS_DETAIL     16       // Detail level for each bounds (16x16x16)

// Particle with position, material, and hash cell reference
typedef struct {
    Vector3 position;
    int     materialId;
    int     hashIndex;      // Index into the spatial hash
    bool    active;         // Whether the particle is active
} ExtendedParticle;

// Spatial hash cell structure
typedef struct {
    int            particleCount;    // Number of particles in this cell
    int*           particleIndices;  // Array of indices to particles
    bool           dirty;            // Whether this cell needs mesh regeneration
    Mesh           mesh;             // Mesh for this cell
    Bounds         bounds;           // Bounds for this cell
    bool           hasMesh;          // Whether a mesh has been generated
} SpatialHashCell;

// Global mesh that can be accessed
Mesh combinedMesh;

// Models for instanced rendering
Model particleModel;
Matrix* particleMatrices;   // Array of transformation matrices for particles
Color* particleColors;      // Array of colors for particles
int particleInstanceCount; // Number of particle instances to draw

// Global variables for the particle system
ObjectAllocator* particleAllocator;
ExtendedParticle* particles;
SpatialHashCell* spatialHashCells;
int totalParticleCount = 0;
int actualParticleCount = 0;
float timeSinceLastParticleAddition = 0.0f;

// Utility function to compute spatial hash index from position
int GetSpatialHashIndex(Vector3 position, float cellSize) {
    // Convert position to grid coordinates
    int gridX = (int)floorf((position.x + VOLUME_SIZE/2) / cellSize);
    int gridY = (int)floorf((position.y + VOLUME_SIZE/2) / cellSize);
    int gridZ = (int)floorf((position.z + VOLUME_SIZE/2) / cellSize);
    
    // Clamp to grid bounds
    gridX = (gridX < 0) ? 0 : (gridX >= HASH_GRID_SIZE) ? HASH_GRID_SIZE - 1 : gridX;
    gridY = (gridY < 0) ? 0 : (gridY >= HASH_GRID_SIZE) ? HASH_GRID_SIZE - 1 : gridY;
    gridZ = (gridZ < 0) ? 0 : (gridZ >= HASH_GRID_SIZE) ? HASH_GRID_SIZE - 1 : gridZ;
    
    // Compute 1D index from 3D coordinates
    return gridX + gridY * HASH_GRID_SIZE + gridZ * HASH_GRID_SIZE * HASH_GRID_SIZE;
}

// Utility function to get the bounds for a spatial hash cell
Bounds GetSpatialHashBounds(int hashIndex, float cellSize) {
    // Convert 1D index back to 3D coordinates
    int gridZ = hashIndex / (HASH_GRID_SIZE * HASH_GRID_SIZE);
    int remainder = hashIndex % (HASH_GRID_SIZE * HASH_GRID_SIZE);
    int gridY = remainder / HASH_GRID_SIZE;
    int gridX = remainder % HASH_GRID_SIZE;
    
    // Calculate bounds center position
    Vector3 center = {
        (gridX + 0.5f) * cellSize - VOLUME_SIZE/2,
        (gridY + 0.5f) * cellSize - VOLUME_SIZE/2,
        (gridZ + 0.5f) * cellSize - VOLUME_SIZE/2
    };
    
    // Create bounds with center, size, and division power
    Bounds bounds = {
        .center = center,
        .size = {cellSize, cellSize, cellSize},
        .divisionPow = 4  // 2^4 = 16 divisions per cell
    };
    
    return bounds;
}

// Initialize the particle system with spatial hashing
void InitializeParticleSystem() {
    // Create particle allocator (object size, objects per page)
    particleAllocator = oa_create(sizeof(ExtendedParticle), 10000);
    
    // Preallocate space for particles
    particles = (ExtendedParticle*)malloc(PARTICLE_COUNT * sizeof(ExtendedParticle));
    
    // Initialize particles as inactive
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        particles[i].active = false;
    }
    
    // Calculate the spatial hash cell size based on volume
    float cellSize = VOLUME_SIZE / HASH_GRID_SIZE;
    
    // Create spatial hash cells
    int totalCells = HASH_GRID_SIZE * HASH_GRID_SIZE * HASH_GRID_SIZE;
    spatialHashCells = (SpatialHashCell*)malloc(totalCells * sizeof(SpatialHashCell));
    
    // Initialize all cells
    for (int i = 0; i < totalCells; i++) {
        spatialHashCells[i].particleCount = 0;
        spatialHashCells[i].particleIndices = (int*)malloc(10000 * sizeof(int)); // Assume max 10000 particles per cell initially
        spatialHashCells[i].dirty = false;
        spatialHashCells[i].hasMesh = false;
        spatialHashCells[i].bounds = GetSpatialHashBounds(i, cellSize);
    }
}

// Generate noise-based particles that form weblike structures
void GenerateWeblikeParticles(int count) {
    if (totalParticleCount >= PARTICLE_COUNT) return;
    
    // Seed the random number generator
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = true;
    }
    
    float cellSize = VOLUME_SIZE / HASH_GRID_SIZE;
    int addedCount = 0;
    
    // Generate particles along strands or clusters
    for (int i = 0; i < count && totalParticleCount < PARTICLE_COUNT; i++) {
        // Choose a random starting point for a strand
        Vector3 strandCenter = {
            ((float)rand() / RAND_MAX - 0.5f) * VOLUME_SIZE * 0.8f,
            ((float)rand() / RAND_MAX - 0.5f) * VOLUME_SIZE * 0.8f,
            ((float)rand() / RAND_MAX - 0.5f) * VOLUME_SIZE * 0.8f
        };
        
        // Determine strand direction
        Vector3 strandDir = {
            ((float)rand() / RAND_MAX - 0.5f) * 2.0f,
            ((float)rand() / RAND_MAX - 0.5f) * 2.0f,
            ((float)rand() / RAND_MAX - 0.5f) * 2.0f
        };
        
        // Normalize the direction
        float magnitude = sqrtf(strandDir.x*strandDir.x + strandDir.y*strandDir.y + strandDir.z*strandDir.z);
        if (magnitude > 0) {
            strandDir.x /= magnitude;
            strandDir.y /= magnitude;
            strandDir.z /= magnitude;
        }
        
        // Create a random cluster of particles along this strand
        int clusterSize = 100 + rand() % 150; // 100-250 particles per cluster
        for (int j = 0; j < clusterSize && totalParticleCount < PARTICLE_COUNT; j++) {
            // Position along the strand with some randomness
            float t = ((float)rand() / RAND_MAX) * 15.0f; // Position along strand
            
            // Calculate position with some noise
            Vector3 offset = {
                ((float)rand() / RAND_MAX - 0.5f) * 3.0f,
                ((float)rand() / RAND_MAX - 0.5f) * 3.0f,
                ((float)rand() / RAND_MAX - 0.5f) * 3.0f
            };
            
            // Final position
            Vector3 position = {
                strandCenter.x + strandDir.x * t + offset.x,
                strandCenter.y + strandDir.y * t + offset.y,
                strandCenter.z + strandDir.z * t + offset.z
            };
            
            // Ensure position is within bounds
            if (fabsf(position.x) > VOLUME_SIZE/2 || 
                fabsf(position.y) > VOLUME_SIZE/2 || 
                fabsf(position.z) > VOLUME_SIZE/2) {
                continue; // Skip this particle if outside bounds
            }
            
            // Create the particle
            particles[totalParticleCount].position = position;
            particles[totalParticleCount].materialId = rand() % 10; // Random material
            
            // Calculate spatial hash index
            int hashIndex = GetSpatialHashIndex(position, cellSize);
            particles[totalParticleCount].hashIndex = hashIndex;
            particles[totalParticleCount].active = true;
            
            // Add to spatial hash cell
            int particleIndex = totalParticleCount;
            spatialHashCells[hashIndex].particleIndices[spatialHashCells[hashIndex].particleCount] = particleIndex;
            spatialHashCells[hashIndex].particleCount++;
            spatialHashCells[hashIndex].dirty = true; // Mark as dirty
            
            totalParticleCount++;
            actualParticleCount++;
            addedCount++;
        }
    }
    
    printf("Added %d particles, total: %d\n", addedCount, totalParticleCount);
}

// Add a chunk of new particles at a specified world space location
void AddParticlesAtLocation(Vector3 location, int count) {
    if (totalParticleCount >= PARTICLE_COUNT) return;
    
    float cellSize = VOLUME_SIZE / HASH_GRID_SIZE;
    int addedCount = 0;
    
    // Generate particles in a spherical cluster around the location
    for (int i = 0; i < count && totalParticleCount < PARTICLE_COUNT; i++) {
        // Generate a random offset in a sphere
        float radius = 5.0f * powf((float)rand() / RAND_MAX, 1.0f/3.0f);
        float theta = 2.0f * PI * ((float)rand() / RAND_MAX);
        float phi = acosf(2.0f * ((float)rand() / RAND_MAX) - 1.0f);
        
        // Calculate position
        Vector3 position = {
            location.x + radius * sinf(phi) * cosf(theta),
            location.y + radius * sinf(phi) * sinf(theta),
            location.z + radius * cosf(phi)
        };
        
        // Ensure position is within bounds
        if (fabsf(position.x) > VOLUME_SIZE/2 || 
            fabsf(position.y) > VOLUME_SIZE/2 || 
            fabsf(position.z) > VOLUME_SIZE/2) {
            continue; // Skip this particle if outside bounds
        }
        
        // Create the particle
        particles[totalParticleCount].position = position;
        particles[totalParticleCount].materialId = rand() % 10; // Random material
        
        // Calculate spatial hash index
        int hashIndex = GetSpatialHashIndex(position, cellSize);
        particles[totalParticleCount].hashIndex = hashIndex;
        particles[totalParticleCount].active = true;
        
        // Add to spatial hash cell
        int particleIndex = totalParticleCount;
        spatialHashCells[hashIndex].particleIndices[spatialHashCells[hashIndex].particleCount] = particleIndex;
        spatialHashCells[hashIndex].particleCount++;
        spatialHashCells[hashIndex].dirty = true; // Mark as dirty
        
        totalParticleCount++;
        actualParticleCount++;
        addedCount++;
    }
    
    printf("Added %d particles at location, total: %d\n", addedCount, totalParticleCount);
}

// Update the position of a particle and update its spatial hash cell if needed
void UpdateParticlePosition(int particleIndex, Vector3 newPosition) {
    if (particleIndex < 0 || particleIndex >= totalParticleCount || !particles[particleIndex].active) {
        return; // Invalid particle index or inactive particle
    }
    
    float cellSize = VOLUME_SIZE / HASH_GRID_SIZE;
    
    // Get current hash index
    int oldHashIndex = particles[particleIndex].hashIndex;
    
    // Calculate new hash index based on new position
    int newHashIndex = GetSpatialHashIndex(newPosition, cellSize);
    
    // Update position
    particles[particleIndex].position = newPosition;
    
    // If hash index has changed, update spatial hash cells
    if (newHashIndex != oldHashIndex) {
        // Remove from old cell
        int oldCellCount = spatialHashCells[oldHashIndex].particleCount;
        int* oldIndices = spatialHashCells[oldHashIndex].particleIndices;
        
        // Find and remove the particle from the old cell
        for (int i = 0; i < oldCellCount; i++) {
            if (oldIndices[i] == particleIndex) {
                // Replace with the last element and decrement count
                oldIndices[i] = oldIndices[oldCellCount - 1];
                spatialHashCells[oldHashIndex].particleCount--;
                break;
            }
        }
        
        // Add to new cell
        spatialHashCells[newHashIndex].particleIndices[spatialHashCells[newHashIndex].particleCount] = particleIndex;
        spatialHashCells[newHashIndex].particleCount++;
        
        // Update particle's hash index
        particles[particleIndex].hashIndex = newHashIndex;
        
        // Mark both cells as dirty
        spatialHashCells[oldHashIndex].dirty = true;
        spatialHashCells[newHashIndex].dirty = true;
    }
}

// Notify the system to update (sets dirty flag for specific bounds)
void NotifySystemUpdate(int hashIndex) {
    if (hashIndex >= 0 && hashIndex < HASH_GRID_SIZE * HASH_GRID_SIZE * HASH_GRID_SIZE) {
        spatialHashCells[hashIndex].dirty = true;
    }
}

// Static buffer for particle data to avoid repeated malloc/free
static Particle* cellParticleBuffer = NULL;
static int cellParticleBufferSize = 0;

// Update dirty spatial hash cells and regenerate meshes (limited per frame)
void UpdateDirtyCells(int maxUpdates) {
    int updatedCount = 0;
    int totalCells = HASH_GRID_SIZE * HASH_GRID_SIZE * HASH_GRID_SIZE;
    
    // Initialize the static buffer if needed
    if (cellParticleBuffer == NULL) {
        cellParticleBufferSize = 20000; // Pre-allocate a large buffer
        cellParticleBuffer = (Particle*)malloc(cellParticleBufferSize * sizeof(Particle));
    }
    
    // Track cells with higher particle counts first (prioritize dense areas)
    int dirtyIndices[MAX_DIRTY_UPDATES];
    int dirtyCounts[MAX_DIRTY_UPDATES];
    int dirtyFound = 0;
    
    // First pass: find cells with highest particle counts
    for (int i = 0; i < totalCells && dirtyFound < maxUpdates; i++) {
        if (spatialHashCells[i].dirty && spatialHashCells[i].particleCount > 0) {
            // Find position to insert based on particle count (highest first)
            int pos = 0;
            while (pos < dirtyFound && 
                   spatialHashCells[i].particleCount < dirtyCounts[pos]) {
                pos++;
            }
            
            // Shift elements to make room
            if (dirtyFound < maxUpdates) {
                for (int j = dirtyFound; j > pos; j--) {
                    dirtyIndices[j] = dirtyIndices[j-1];
                    dirtyCounts[j] = dirtyCounts[j-1];
                }
                
                // Insert this cell
                dirtyIndices[pos] = i;
                dirtyCounts[pos] = spatialHashCells[i].particleCount;
                dirtyFound++;
            }
        }
    }
    
    // Second pass: update the cells we found, in priority order
    for (int idx = 0; idx < dirtyFound; idx++) {
        int i = dirtyIndices[idx];
        
        // Ensure the buffer is large enough
        if (spatialHashCells[i].particleCount > cellParticleBufferSize) {
            cellParticleBufferSize = spatialHashCells[i].particleCount * 1.5;
            cellParticleBuffer = (Particle*)realloc(cellParticleBuffer, 
                                                 cellParticleBufferSize * sizeof(Particle));
        }
        
        // Copy particle data to buffer
        for (int j = 0; j < spatialHashCells[i].particleCount; j++) {
            int particleIdx = spatialHashCells[i].particleIndices[j];
            cellParticleBuffer[j].position = particles[particleIdx].position;
            cellParticleBuffer[j].materialId = particles[particleIdx].materialId;
        }
        
        // If a mesh already exists, unload it
        if (spatialHashCells[i].hasMesh) {
            UnloadMesh(spatialHashCells[i].mesh);
        }
        
        // Generate new mesh
        spatialHashCells[i].mesh = GenerateMesh(cellParticleBuffer, PARTICLE_RADIUS, 
                                              spatialHashCells[i].particleCount, 
                                              spatialHashCells[i].bounds);
        spatialHashCells[i].hasMesh = true;
        
        // Mark as clean
        spatialHashCells[i].dirty = false;
        updatedCount++;
    }
    
    if (updatedCount > 0) {
        printf("Updated %d dirty cells\n", updatedCount);
    }
}

int main(void) {
    // Initialization
    const int screenWidth = 1280;
    const int screenHeight = 800;

    InitWindow(screenWidth, screenHeight, "OpenParticleSurfaceLib - Dynamic Particle Mesh Demo");

    // Define the camera to look into our 3d world
    Camera camera = { 0 };
    camera.position = (Vector3){ 0.0f, VOLUME_SIZE*0.5f, VOLUME_SIZE*1.2f }; // Position camera in front
    camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    // Initialize particle system
    InitializeParticleSystem();
    
    // Setup the particle sphere model for instanced rendering
    particleModel = LoadModelFromMesh(GenMeshSphere(PARTICLE_RADIUS*0.1f, 4, 4));
    
    // Preallocate matrices and colors for particles (up to PARTICLE_COUNT)
    particleMatrices = (Matrix*)malloc(PARTICLE_COUNT * sizeof(Matrix));
    particleColors = (Color*)malloc(PARTICLE_COUNT * sizeof(Color));
    particleInstanceCount = 0;
    
    // Generate initial particles
    GenerateWeblikeParticles(INITIAL_PARTICLE_COUNT / 20); // Generate more clusters
    
    // Display modes
    bool wireframe = true;     // Toggle for wireframe rendering
    bool showParticles = false; // Toggle for showing particle positions (default off for performance)
    bool showBounds = false;    // Toggle for showing spatial hash bounds (default off for performance)
    bool useInstancing = true;  // Toggle for using instanced rendering
    
    SetTargetFPS(60);               // Set target to run at 60 frames-per-second
    
    // Main game loop
    while (!WindowShouldClose()) {
        // Update
        // Only update camera rotation, not position
        if (IsKeyDown(KEY_LEFT_ALT)) {
            UpdateCamera(&camera, CAMERA_ORBITAL);
        }
        
        // Calculate delta time
        float deltaTime = GetFrameTime();
        
        // Add more particles gradually
        timeSinceLastParticleAddition += deltaTime;
        if (timeSinceLastParticleAddition > 0.2f && totalParticleCount < PARTICLE_COUNT) {
            GenerateWeblikeParticles(2000); // Add more particles in clusters
            timeSinceLastParticleAddition = 0.0f;
        }
        
        // Update dirty cells (limit to MAX_DIRTY_UPDATES per frame)
        UpdateDirtyCells(MAX_DIRTY_UPDATES);
        
        // Check for key presses
        if (IsKeyPressed(KEY_SPACE)) wireframe = !wireframe;
        if (IsKeyPressed(KEY_P)) showParticles = !showParticles;
        if (IsKeyPressed(KEY_B)) showBounds = !showBounds;
        if (IsKeyPressed(KEY_I)) useInstancing = !useInstancing;
        
        // Camera control with keys
        if (IsKeyDown(KEY_W)) camera.position.z -= 2.0f;
        if (IsKeyDown(KEY_S)) camera.position.z += 2.0f;
        if (IsKeyDown(KEY_A)) camera.position.x -= 2.0f;
        if (IsKeyDown(KEY_D)) camera.position.x += 2.0f;
        if (IsKeyDown(KEY_Q)) camera.position.y -= 2.0f;
        if (IsKeyDown(KEY_E)) camera.position.y += 2.0f;
        if (IsKeyPressed(KEY_R)) {
            // Reset camera to initial position
            camera.position = (Vector3){ 0.0f, VOLUME_SIZE*0.5f, VOLUME_SIZE*1.2f };
            camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };
        }
        
        // Update particle matrices and colors for instanced rendering
        if (showParticles && useInstancing) {
            particleInstanceCount = 0;
            for (int i = 0; i < totalParticleCount && particleInstanceCount < PARTICLE_COUNT; i++) {
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
                
                // Limit the number of instances to draw for performance
                if (particleInstanceCount >= 50000) break;
            }
        }
        
        // Add particles at mouse click location
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Ray ray = GetMouseRay(GetMousePosition(), camera);
            
            // Calculate intersection with a plane at y=0
            float t = -ray.position.y / ray.direction.y;
            if (t > 0) {
                Vector3 hitPoint = {
                    ray.position.x + ray.direction.x * t,
                    ray.position.y + ray.direction.y * t,
                    ray.position.z + ray.direction.z * t
                };
                
                // Only add if within volume bounds
                if (fabsf(hitPoint.x) <= VOLUME_SIZE/2 && fabsf(hitPoint.z) <= VOLUME_SIZE/2) {
                    AddParticlesAtLocation(hitPoint, 500);
                }
            }
        }

        // Draw
        BeginDrawing();
            ClearBackground(BLACK);
            
            BeginMode3D(camera);
                // Draw the main volume bounds
                DrawCubeWires((Vector3){ 0, 0, 0 }, VOLUME_SIZE, VOLUME_SIZE, VOLUME_SIZE, DARKGRAY);
                
                // Draw particles if enabled
                if (showParticles) {
                    if (useInstancing && particleInstanceCount > 0) {
                        // Draw all particles using instanced rendering
                        // Note: raylib's DrawMeshInstanced doesn't support per-instance colors directly
                        // Using the default material for all instances
                        DrawMeshInstanced(particleModel.meshes[0], particleModel.materials[0], 
                                         particleMatrices, particleInstanceCount);
                    } else {
                        // Fallback to individual particle rendering (much slower)
                        // Only draw a limited number for performance
                        int maxDrawCount = 1000;
                        for (int i = 0; i < totalParticleCount && maxDrawCount > 0; i++) {
                            if (particles[i].active) {
                                DrawSphere(particles[i].position, PARTICLE_RADIUS*0.1f, 
                                          GetMaterialColor(particles[i].materialId));
                                maxDrawCount--;
                            }
                        }
                    }
                }
                
                // Static materials to avoid creating new ones each frame
                static Material defaultMaterial;
                static bool materialInitialized = false;
                
                if (!materialInitialized) {
                    defaultMaterial = LoadMaterialDefault();
                    materialInitialized = true;
                }
                
                // Draw all cell meshes
                int totalCells = HASH_GRID_SIZE * HASH_GRID_SIZE * HASH_GRID_SIZE;
                int drawnMeshes = 0;
                
                // Set wireframe mode once instead of toggling for each mesh
                if (wireframe) {
                    rlEnableWireMode();
                }
                
                for (int i = 0; i < totalCells; i++) {
                    if (spatialHashCells[i].hasMesh && spatialHashCells[i].particleCount > 0) {
                        DrawMesh(spatialHashCells[i].mesh, defaultMaterial, MatrixIdentity());
                        drawnMeshes++;
                    }
                    
                    // Draw cell bounds if enabled (only draw if they have particles)
                    if (showBounds && spatialHashCells[i].particleCount > 0) {
                        Bounds bounds = spatialHashCells[i].bounds;
                        Color boundsColor = spatialHashCells[i].dirty ? RED : GREEN;
                        DrawCubeWires(bounds.center, bounds.size.x, bounds.size.y, bounds.size.z, boundsColor);
                    }
                }
                
                // Disable wireframe mode if it was enabled
                if (wireframe) {
                    rlDisableWireMode();
                }
                
                DrawGrid(10, 10.0f);        // Draw a grid
            EndMode3D();
            
            // UI
            DrawFPS(10, 10);
            DrawText("OpenParticleSurfaceLib - Dynamic Particle Mesh Demo", 10, 40, 20, WHITE);
            DrawText(TextFormat("Particles: %d / %d", actualParticleCount, PARTICLE_COUNT), 10, 70, 12, WHITE);
            DrawText(TextFormat("Spatial Hash Grid: %dx%dx%d", HASH_GRID_SIZE, HASH_GRID_SIZE, HASH_GRID_SIZE), 10, 90, 12, WHITE);
            DrawText(TextFormat("Bounds Detail: %dx%dx%d", HASH_BOUNDS_DETAIL, HASH_BOUNDS_DETAIL, HASH_BOUNDS_DETAIL), 10, 110, 12, WHITE);
            DrawText(TextFormat("Max Dirty Updates Per Frame: %d", MAX_DIRTY_UPDATES), 10, 130, 12, WHITE);
            DrawText(TextFormat("FPS: %d", GetFPS()), 10, 150, 12, WHITE);
            DrawText("Press SPACE to toggle wireframe mode", 10, 160, 12, WHITE);
            DrawText("Press P to show/hide particles", 10, 180, 12, WHITE);
            DrawText("Press I to toggle instanced rendering", 10, 200, 12, WHITE);
            DrawText("Press B to show/hide spatial hash bounds", 10, 220, 12, WHITE);
            DrawText("Click to add particles at location", 10, 240, 12, WHITE);
            DrawText("Hold ALT + mouse to orbit camera", 10, 260, 12, WHITE);
            DrawText("WASD keys to move camera, QE for up/down, R to reset", 10, 280, 12, WHITE);
            
            if (showParticles) {
                DrawText(TextFormat("Instanced Rendering: %s", useInstancing ? "ON" : "OFF"), 10, 280, 12, WHITE);
                if (useInstancing) {
                    DrawText(TextFormat("Particles Rendered: %d", particleInstanceCount), 10, 300, 12, WHITE);
                }
            }
            
        EndDrawing();
    }

    // Cleanup
    int totalCells = HASH_GRID_SIZE * HASH_GRID_SIZE * HASH_GRID_SIZE;
    for (int i = 0; i < totalCells; i++) {
        if (spatialHashCells[i].hasMesh) {
            UnloadMesh(spatialHashCells[i].mesh);
        }
        free(spatialHashCells[i].particleIndices);
    }
    
    // Free the static cell particle buffer
    if (cellParticleBuffer != NULL) {
        free(cellParticleBuffer);
    }
    
    // Unload instanced rendering resources
    UnloadModel(particleModel);
    free(particleMatrices);
    free(particleColors);
    
    free(spatialHashCells);
    free(particles);
    oa_destroy(particleAllocator);

    CloseWindow();        // Close window and OpenGL context

    return 0;
}