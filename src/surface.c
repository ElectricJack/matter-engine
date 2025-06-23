#include "../include/surface.h"
#include "../include/spatial_hash.h"
#include "mc_tables.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

// Performance timing macros
#define ENABLE_PERFORMANCE_TIMING 1

#if ENABLE_PERFORMANCE_TIMING
    static double performance_timer() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
    }
    
    #define TIMER_START(name) double timer_##name = performance_timer()
    #define TIMER_END(name, description) \
        do { \
            double elapsed = performance_timer() - timer_##name; \
            printf("PERF [%s]: %.3f ms\n", description, elapsed * 1000.0); \
        } while(0)
#else
    #define TIMER_START(name)
    #define TIMER_END(name, description)
#endif

// Local structures needed by our algorithm

// Combined scalar field and material result to eliminate duplicate calculations
typedef struct {
    float scalarValue;
    int materialId;
} ScalarMaterialPair;

// Triangle face structure
typedef struct {
    int indices[3];  // Indices of the three vertices
} Triangle;

// Memory pool for reusing buffers across mesh generations
typedef struct {
    // Scalar field buffers
    float* scalarField;
    int* materialField;
    size_t fieldCapacity;
    
    // Mesh buffers  
    Vector3* vertices;
    Vector3* normals;
    int* materials;
    Triangle* triangles;
    size_t vertexCapacity;
    size_t triangleCapacity;
    
    // Edge deduplication buffers
    unsigned long long* edgeKeys;
    int* globalEdgeVertexIndices;
    size_t hashTableCapacity;
} MemoryPool;

// Global memory pool for reuse
static MemoryPool g_memoryPool = {0};

// Memory pool management functions
static void EnsureFieldCapacity(size_t requiredCells) {
    if (g_memoryPool.fieldCapacity < requiredCells) {
        // Grow by 50% or to required size, whichever is larger
        size_t newCapacity = (g_memoryPool.fieldCapacity * 3) / 2;
        if (newCapacity < requiredCells) newCapacity = requiredCells;
        
        g_memoryPool.scalarField = (float*)realloc(g_memoryPool.scalarField, newCapacity * sizeof(float));
        g_memoryPool.materialField = (int*)realloc(g_memoryPool.materialField, newCapacity * sizeof(int));
        g_memoryPool.fieldCapacity = newCapacity;
    }
}

static void EnsureMeshCapacity(size_t requiredVertices, size_t requiredTriangles) {
    if (g_memoryPool.vertexCapacity < requiredVertices) {
        size_t newCapacity = (g_memoryPool.vertexCapacity * 3) / 2;
        if (newCapacity < requiredVertices) newCapacity = requiredVertices;
        
        g_memoryPool.vertices = (Vector3*)realloc(g_memoryPool.vertices, newCapacity * sizeof(Vector3));
        g_memoryPool.normals = (Vector3*)realloc(g_memoryPool.normals, newCapacity * sizeof(Vector3));
        g_memoryPool.materials = (int*)realloc(g_memoryPool.materials, newCapacity * sizeof(int));
        g_memoryPool.vertexCapacity = newCapacity;
    }
    
    if (g_memoryPool.triangleCapacity < requiredTriangles) {
        size_t newCapacity = (g_memoryPool.triangleCapacity * 3) / 2;
        if (newCapacity < requiredTriangles) newCapacity = requiredTriangles;
        
        g_memoryPool.triangles = (Triangle*)realloc(g_memoryPool.triangles, newCapacity * sizeof(Triangle));
        g_memoryPool.triangleCapacity = newCapacity;
    }
}

static void EnsureHashTableCapacity(size_t requiredSize) {
    if (g_memoryPool.hashTableCapacity < requiredSize) {
        size_t newCapacity = (g_memoryPool.hashTableCapacity * 3) / 2;
        if (newCapacity < requiredSize) newCapacity = requiredSize;
        
        g_memoryPool.edgeKeys = (unsigned long long*)realloc(g_memoryPool.edgeKeys, newCapacity * sizeof(unsigned long long));
        g_memoryPool.globalEdgeVertexIndices = (int*)realloc(g_memoryPool.globalEdgeVertexIndices, newCapacity * sizeof(int));
        g_memoryPool.hashTableCapacity = newCapacity;
    }
}

static void CleanupMemoryPool(void) {
    free(g_memoryPool.scalarField);
    free(g_memoryPool.materialField);
    free(g_memoryPool.vertices);
    free(g_memoryPool.normals);
    free(g_memoryPool.materials);
    free(g_memoryPool.triangles);
    free(g_memoryPool.edgeKeys);
    free(g_memoryPool.globalEdgeVertexIndices);
    g_memoryPool = (MemoryPool){0};
}

// Triangle face structure (defined above in forward declarations)

// Grid cell structure for marching cubes
typedef struct {
    Vector3 corners[8]; // Positions of the 8 corners of the cube
    float   scalars[8]; // Scalar field values at the corners
} GridCell;

// Volume data structure for marching cubes algorithm
typedef struct {
    int     gridSize;       // Number of grid cells in each dimension
    int     totalCells;     // Total number of cells in the volume
    Vector3 minBound;       // Minimum bound of the volume
    Vector3 cellSize;       // Size of each cell
    float*  scalarField;    // Scalar field values at grid points
    int*    materialField;  // Material IDs at grid points
} VolumeData;

// Vertex structure for isosurface mesh
typedef struct {
    Vector3 position;
    Vector3 normal;
    int     materialId;
} IsosurfaceVertex;

// Local function declarations
static ScalarMaterialPair CalculateScalarAndMaterial(Vector3 position, SpatialHash* spatialHash, float particleRadius);
static int     CalculateCubeIndex(GridCell cell, float isovalue);
static Vector3 VertexInterpolation(Vector3 v1, float val1, Vector3 v2, float val2, float isovalue);
static Mesh    GenerateMeshInternal(Particle* particles, float particleRadius, int particleCount, Bounds volume, MeshGenerationConfig config);


// Utility function to convert grid cell coordinates to index in the scalar field array
static int GetScalarFieldIndex(int x, int y, int z, int gridSize) {
    return x + y * gridSize + z * gridSize * gridSize;
}

// Create default mesh generation configuration
MeshGenerationConfig GetDefaultMeshConfig(void) {
    MeshGenerationConfig config;
    config.enableEdgeDeduplication = false;  // Default: enabled for better mesh quality
    config.enableMemoryReuse       = true;   // Default: enabled for better performance
    return config;
}

// Public API wrapper function using default configuration
Mesh GenerateMesh(Particle* particles, float particleRadius, int particleCount, Bounds volume) {
    MeshGenerationConfig config = GetDefaultMeshConfig();
    return GenerateMeshInternal(particles, particleRadius, particleCount, volume, config);
}

// Public API function with custom configuration
Mesh GenerateMeshWithConfig(Particle* particles, float particleRadius, int particleCount, Bounds volume, MeshGenerationConfig config) {
    return GenerateMeshInternal(particles, particleRadius, particleCount, volume, config);
}

// Cleanup function to release memory pool resources
void SurfaceLibCleanup(void) {
    CleanupMemoryPool();
}

// Internal mesh generation function with configuration
static Mesh GenerateMeshInternal(Particle* particles, float particleRadius, int particleCount, Bounds volume, MeshGenerationConfig config) {
    TIMER_START(total);
    
    // Initialize mesh
    Mesh mesh = {0};
    
    // Calculate grid dimensions based on divisionPow (2^divisionPow divisions per axis)
    int gridSize = 1 << volume.divisionPow;
    
    // Set up volume data
    VolumeData data = {0};
    data.gridSize = gridSize;
    data.totalCells = gridSize * gridSize * gridSize;
    data.cellSize = (Vector3){
        volume.size.x / (gridSize - 1),
        volume.size.y / (gridSize - 1),
        volume.size.z / (gridSize - 1)
    };
    data.minBound = (Vector3){
        volume.center.x - volume.size.x * 0.5f,
        volume.center.y - volume.size.y * 0.5f,
        volume.center.z - volume.size.z * 0.5f
    };
    
    // Allocate memory for scalar field and material field using memory pool if enabled
    if (config.enableMemoryReuse) {
        EnsureFieldCapacity(data.totalCells);
        data.scalarField = g_memoryPool.scalarField;
        data.materialField = g_memoryPool.materialField;
    } else {
        data.scalarField = (float*)malloc(data.totalCells * sizeof(float));
        data.materialField = (int*)malloc(data.totalCells * sizeof(int));
        
        if (!data.scalarField || !data.materialField) {
            printf("Failed to allocate memory for scalar field\n");
            free(data.scalarField);
            free(data.materialField);
            return mesh;
        }
    }
    
    TIMER_START(spatial_hash);
    
    // Create spatial hash for efficient particle queries
    // Optimized cell size: smaller cells (1.5x radius) for better spatial locality
    // This reduces the number of particles per cell, improving query performance
    float spatialCellSize = particleRadius * 1.5f;
    SpatialHash* spatialHash = sh_create(spatialCellSize, particleCount);
    
    if (!spatialHash) {
        printf("Failed to create spatial hash\n");
        free(data.scalarField);
        free(data.materialField);
        return mesh;
    }
    
    // Insert all particles into the spatial hash
    for (int i = 0; i < particleCount; i++) {
        sh_insert(spatialHash, particles[i].position.x, particles[i].position.y, particles[i].position.z, &particles[i]);
    }
    
    TIMER_END(spatial_hash, "Spatial Hash Setup");
    
    TIMER_START(scalar_field);
    
    // Fill scalar field with implicit function values (now using combined calculation)
    for (int z = 0; z < gridSize; z++) {
        for (int y = 0; y < gridSize; y++) {
            for (int x = 0; x < gridSize; x++) {
                Vector3 position = {
                    data.minBound.x + x * data.cellSize.x,
                    data.minBound.y + y * data.cellSize.y,
                    data.minBound.z + z * data.cellSize.z
                };
                
                int index = GetScalarFieldIndex(x, y, z, gridSize);
                
                // Use combined calculation to eliminate duplicate distance calculations
                ScalarMaterialPair result = CalculateScalarAndMaterial(position, spatialHash, particleRadius);
                data.scalarField[index] = result.scalarValue;
                data.materialField[index] = result.materialId;
            }
        }
    }
    
    TIMER_END(scalar_field, "Scalar Field Computation");
    
    // Create temporary buffers for storing mesh data using memory pool if enabled
    int maxVertices = data.totalCells * 3; // Maximum possible vertices per cell is typically 3-5
    int maxTriangles = data.totalCells * 2; // Maximum possible triangles per cell is typically 1-5
    
    Vector3*  vertices;
    Vector3*  normals;
    int*      materials;
    Triangle* triangles;
    
    if (config.enableMemoryReuse) {
        EnsureMeshCapacity(maxVertices, maxTriangles);
        vertices = g_memoryPool.vertices;
        normals = g_memoryPool.normals;
        materials = g_memoryPool.materials;
        triangles = g_memoryPool.triangles;
    } else {
        vertices = (Vector3*)malloc(maxVertices * sizeof(Vector3));
        normals = (Vector3*)malloc(maxVertices * sizeof(Vector3));
        materials = (int*)malloc(maxVertices * sizeof(int));
        triangles = (Triangle*)malloc(maxTriangles * sizeof(Triangle));
        
        if (!vertices || !normals || !materials || !triangles) {
            printf("Failed to allocate memory for mesh buffers\n");
            if (!config.enableMemoryReuse) {
                free(data.scalarField);
                free(data.materialField);
            }
            sh_destroy(spatialHash);
            if (!config.enableMemoryReuse) {
                free(vertices);
                free(normals);
                free(materials);
                free(triangles);
            }
            return mesh;
        }
    }
    
    // Use a hash table approach for faster edge lookup (only if edge deduplication is enabled)
    unsigned long long* edgeKeys = NULL;
    int* globalEdgeVertexIndices = NULL;
    int hashTableSize = 0;
    
    if (config.enableEdgeDeduplication) {
        hashTableSize = 1024 * 1024; // 1M entries in hash table
        
        if (config.enableMemoryReuse) {
            EnsureHashTableCapacity(hashTableSize);
            edgeKeys = g_memoryPool.edgeKeys;
            globalEdgeVertexIndices = g_memoryPool.globalEdgeVertexIndices;
        } else {
            edgeKeys = (unsigned long long*)malloc(hashTableSize * sizeof(unsigned long long));
            globalEdgeVertexIndices = (int*)malloc(hashTableSize * sizeof(int));
        }
        
        // Initialize hash table to indicate no entries
        for (int i = 0; i < hashTableSize; i++) {
            edgeKeys[i] = 0;  // 0 means no edge stored
            globalEdgeVertexIndices[i] = -1;  // -1 means no vertex assigned
        }
    }
    
    int vertexCount = 0;
    int triangleCount = 0;
    
    // Value for isosurface threshold
    const float isovalue = 0.0f; // Surface at zero level
    
    TIMER_START(marching_cubes);
    
    // Run marching cubes algorithm on each grid cell
    for (int z = 0; z < gridSize - 1; z++) {
        for (int y = 0; y < gridSize - 1; y++) {
            for (int x = 0; x < gridSize - 1; x++) {
                // Create a grid cell for marching cubes
                GridCell cell;
                
                // Get the 8 corners of the cube
                cell.corners[0] = (Vector3){ 
                    data.minBound.x + x * data.cellSize.x,
                    data.minBound.y + y * data.cellSize.y, 
                    data.minBound.z + z * data.cellSize.z 
                };
                cell.corners[1] = (Vector3){ 
                    data.minBound.x + (x+1) * data.cellSize.x,
                    data.minBound.y + y * data.cellSize.y, 
                    data.minBound.z + z * data.cellSize.z 
                };
                cell.corners[2] = (Vector3){ 
                    data.minBound.x + (x+1) * data.cellSize.x,
                    data.minBound.y + (y+1) * data.cellSize.y, 
                    data.minBound.z + z * data.cellSize.z 
                };
                cell.corners[3] = (Vector3){ 
                    data.minBound.x + x * data.cellSize.x,
                    data.minBound.y + (y+1) * data.cellSize.y, 
                    data.minBound.z + z * data.cellSize.z 
                };
                cell.corners[4] = (Vector3){ 
                    data.minBound.x + x * data.cellSize.x,
                    data.minBound.y + y * data.cellSize.y, 
                    data.minBound.z + (z+1) * data.cellSize.z 
                };
                cell.corners[5] = (Vector3){ 
                    data.minBound.x + (x+1) * data.cellSize.x,
                    data.minBound.y + y * data.cellSize.y, 
                    data.minBound.z + (z+1) * data.cellSize.z 
                };
                cell.corners[6] = (Vector3){ 
                    data.minBound.x + (x+1) * data.cellSize.x,
                    data.minBound.y + (y+1) * data.cellSize.y, 
                    data.minBound.z + (z+1) * data.cellSize.z 
                };
                cell.corners[7] = (Vector3){ 
                    data.minBound.x + x * data.cellSize.x,
                    data.minBound.y + (y+1) * data.cellSize.y, 
                    data.minBound.z + (z+1) * data.cellSize.z 
                };
                
                // Get scalar values at each corner
                cell.scalars[0] = data.scalarField[GetScalarFieldIndex(x, y, z, gridSize)];
                cell.scalars[1] = data.scalarField[GetScalarFieldIndex(x+1, y, z, gridSize)];
                cell.scalars[2] = data.scalarField[GetScalarFieldIndex(x+1, y+1, z, gridSize)];
                cell.scalars[3] = data.scalarField[GetScalarFieldIndex(x, y+1, z, gridSize)];
                cell.scalars[4] = data.scalarField[GetScalarFieldIndex(x, y, z+1, gridSize)];
                cell.scalars[5] = data.scalarField[GetScalarFieldIndex(x+1, y, z+1, gridSize)];
                cell.scalars[6] = data.scalarField[GetScalarFieldIndex(x+1, y+1, z+1, gridSize)];
                cell.scalars[7] = data.scalarField[GetScalarFieldIndex(x, y+1, z+1, gridSize)];
                
                // Get materials at each corner
                int materials_at_corners[8];
                materials_at_corners[0] = data.materialField[GetScalarFieldIndex(x, y, z, gridSize)];
                materials_at_corners[1] = data.materialField[GetScalarFieldIndex(x+1, y, z, gridSize)];
                materials_at_corners[2] = data.materialField[GetScalarFieldIndex(x+1, y+1, z, gridSize)];
                materials_at_corners[3] = data.materialField[GetScalarFieldIndex(x, y+1, z, gridSize)];
                materials_at_corners[4] = data.materialField[GetScalarFieldIndex(x, y, z+1, gridSize)];
                materials_at_corners[5] = data.materialField[GetScalarFieldIndex(x+1, y, z+1, gridSize)];
                materials_at_corners[6] = data.materialField[GetScalarFieldIndex(x+1, y+1, z+1, gridSize)];
                materials_at_corners[7] = data.materialField[GetScalarFieldIndex(x, y+1, z+1, gridSize)];
                
                // Calculate cube index
                int cubeIndex = CalculateCubeIndex(cell, isovalue);
                
                // Skip empty cells
                if (cubeIndex == 0 || cubeIndex == 255) continue;
                
                // Edge to vertex indices pairs
                int edgeVertexPairs[12][2] = {
                    {0, 1}, {1, 2}, {2, 3}, {3, 0},
                    {4, 5}, {5, 6}, {6, 7}, {7, 4},
                    {0, 4}, {1, 5}, {2, 6}, {3, 7}
                };
                
                // Find intersection points along edges
                Vector3 intersections[12];
                int intersectionMaterials[12];
                int cellEdgeVertexIndices[12]; // Local array for this cell's edge vertex indices
                
                // Initialize local edge vertex indices
                for (int e = 0; e < 12; e++) {
                    cellEdgeVertexIndices[e] = -1;
                }
                
                for (int edge = 0; edge < 12; edge++) {
                    // Check if this edge is intersected
                    if (edgeTable[cubeIndex] & (1 << edge)) {
                        int v1 = edgeVertexPairs[edge][0];
                        int v2 = edgeVertexPairs[edge][1];
                        
                        // Calculate intersection point
                        intersections[edge] = VertexInterpolation(
                            cell.corners[v1], cell.scalars[v1], 
                            cell.corners[v2], cell.scalars[v2], 
                            isovalue
                        );
                        
                        // Determine material for this intersection (use the dominant material)
                        if (fabs(cell.scalars[v1]) < fabs(cell.scalars[v2])) {
                            intersectionMaterials[edge] = materials_at_corners[v1];
                        } else {
                            intersectionMaterials[edge] = materials_at_corners[v2];
                        }
                        
                        if (config.enableEdgeDeduplication) {
                            // Check if this vertex already exists (using an edge key)
                            unsigned long long edgeKey = GetEdgeKey(x, y, z, edge);
                            
                            // Skip if the key is 0 (invalid edge)
                            if (edgeKey == 0) {
                                continue;
                            }
                            
                            // Hash the key to find its position in the hash table
                            unsigned int hashPos = (unsigned int)(edgeKey % hashTableSize);
                            int existingVertexIndex = -1;
                            
                            // Linear probing to handle hash collisions
                            int maxProbes = 100;  // Limit probing to avoid infinite loops
                            for (int probe = 0; probe < maxProbes; probe++) {
                                unsigned int pos = (hashPos + probe) % hashTableSize;
                                
                                // If we found our key or an empty slot
                                if (edgeKeys[pos] == edgeKey) {
                                    existingVertexIndex = globalEdgeVertexIndices[pos];
                                    break;
                                }
                                else if (edgeKeys[pos] == 0) {
                                    // Found an empty slot, so this edge doesn't exist yet
                                    hashPos = pos;  // Remember this position for insertion
                                    break;
                                }
                            }
                            
                            // If vertex doesn't exist, add it
                            if (existingVertexIndex == -1) {
                                if (vertexCount >= maxVertices) {
                                    printf("Warning: Exceeded maximum vertex count\n");
                                    continue;
                                }
                                
                                // Store the vertex
                                vertices[vertexCount] = intersections[edge];
                                materials[vertexCount] = intersectionMaterials[edge];
                                
                                // Store the edge key and vertex index in the hash table
                                edgeKeys[hashPos] = edgeKey;
                                globalEdgeVertexIndices[hashPos] = vertexCount;
                                
                                // Store the vertex index for this edge in the current cell
                                cellEdgeVertexIndices[edge] = vertexCount;
                                vertexCount++;
                            } else {
                                // Use existing vertex
                                cellEdgeVertexIndices[edge] = existingVertexIndex;
                            }
                        } else {
                            // No edge deduplication - always create new vertex
                            if (vertexCount >= maxVertices) {
                                printf("Warning: Exceeded maximum vertex count\n");
                                continue;
                            }
                            
                            // Store the vertex
                            vertices[vertexCount] = intersections[edge];
                            materials[vertexCount] = intersectionMaterials[edge];
                            
                            // Store the vertex index for this edge in the current cell
                            cellEdgeVertexIndices[edge] = vertexCount;
                            vertexCount++;
                        }
                    }
                }
                
                // Create triangles
                for (int i = 0; triTable[cubeIndex][i] != -1; i += 3) {
                    if (triangleCount >= maxTriangles) {
                        printf("Warning: Exceeded maximum triangle count\n");
                        break;
                    }
                    
                    Triangle triangle;
                    
                    // Get the vertex indices for each triangle
                    int edge1 = triTable[cubeIndex][i];
                    int edge2 = triTable[cubeIndex][i+1];
                    int edge3 = triTable[cubeIndex][i+2];
                    
                    triangle.indices[2] = cellEdgeVertexIndices[edge1];
                    triangle.indices[1] = cellEdgeVertexIndices[edge2];
                    triangle.indices[0] = cellEdgeVertexIndices[edge3];
                    
                    // Add the triangle
                    triangles[triangleCount] = triangle;
                    triangleCount++;
                }
            }
        }
    }
    
    TIMER_END(marching_cubes, "Marching Cubes Algorithm");
    
    TIMER_START(mesh_assembly);
    
    // Calculate normals
    for (int i = 0; i < vertexCount; i++) {
        normals[i] = (Vector3){0.0f, 0.0f, 0.0f};
    }
    
    // For each triangle, calculate its normal and add it to each vertex normal
    for (int i = 0; i < triangleCount; i++) {
        int idx1 = triangles[i].indices[0];
        int idx2 = triangles[i].indices[1];
        int idx3 = triangles[i].indices[2];
        
        Vector3 v1 = vertices[idx1];
        Vector3 v2 = vertices[idx2];
        Vector3 v3 = vertices[idx3];
        
        // Calculate triangle edges
        Vector3 edge1 = {v2.x - v1.x, v2.y - v1.y, v2.z - v1.z};
        Vector3 edge2 = {v3.x - v1.x, v3.y - v1.y, v3.z - v1.z};
        
        // Calculate triangle normal using cross product
        Vector3 normal = {
            edge1.y * edge2.z - edge1.z * edge2.y,
            edge1.z * edge2.x - edge1.x * edge2.z,
            edge1.x * edge2.y - edge1.y * edge2.x
        };
        
        // Add to each vertex normal
        normals[idx1].x += normal.x;
        normals[idx1].y += normal.y;
        normals[idx1].z += normal.z;
        
        normals[idx2].x += normal.x;
        normals[idx2].y += normal.y;
        normals[idx2].z += normal.z;
        
        normals[idx3].x += normal.x;
        normals[idx3].y += normal.y;
        normals[idx3].z += normal.z;
    }
    
    // Normalize all normals
    for (int i = 0; i < vertexCount; i++) {
        float length = sqrtf(
            normals[i].x * normals[i].x + 
            normals[i].y * normals[i].y + 
            normals[i].z * normals[i].z
        );
        
        if (length > 0.0001f) {
            normals[i].x /= length;
            normals[i].y /= length;
            normals[i].z /= length;
        }
    }
    
    // Create the final mesh
    mesh.vertexCount = vertexCount;
    mesh.triangleCount = triangleCount;
    
    // Allocate memory for mesh data
    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    mesh.normals = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    mesh.indices = (unsigned short*)RL_MALLOC(triangleCount * 3 * sizeof(unsigned short));
    
    // Fill mesh data
    for (int i = 0; i < vertexCount; i++) {
        mesh.vertices[i*3] = vertices[i].x;
        mesh.vertices[i*3+1] = vertices[i].y;
        mesh.vertices[i*3+2] = vertices[i].z;
        
        mesh.normals[i*3] = normals[i].x;
        mesh.normals[i*3+1] = normals[i].y;
        mesh.normals[i*3+2] = normals[i].z;
    }
    
    for (int i = 0; i < triangleCount; i++) {
        mesh.indices[i*3] = triangles[i].indices[0];
        mesh.indices[i*3+1] = triangles[i].indices[1];
        mesh.indices[i*3+2] = triangles[i].indices[2];
    }
    
    // Set material IDs as vertex colors
    mesh.colors = (unsigned char*)RL_MALLOC(vertexCount * 4 * sizeof(unsigned char));
    for (int i = 0; i < vertexCount; i++) {
        Color color = GetMaterialColor(materials[i]);
        mesh.colors[i*4+0] = color.r;
        mesh.colors[i*4+1] = color.g;
        mesh.colors[i*4+2] = color.b;
        mesh.colors[i*4+3] = color.a;
    }
    
    TIMER_END(mesh_assembly, "Mesh Assembly");
    
    // Clean up temporary data (only free if not using memory pool)
    if (!config.enableMemoryReuse) {
        free(data.scalarField);
        free(data.materialField);
        free(vertices);
        free(normals);
        free(materials);
        free(triangles);
        if (config.enableEdgeDeduplication) {
            free(edgeKeys);
            free(globalEdgeVertexIndices);
        }
    }
    sh_destroy(spatialHash);
    
    TIMER_END(total, "Total Mesh Generation");
    
    return mesh;
}

// Combined calculation to eliminate duplicate distance calculations
static ScalarMaterialPair CalculateScalarAndMaterial(Vector3 position, SpatialHash* spatialHash, float particleRadius) {
    ScalarMaterialPair result;
    result.scalarValue = INFINITY;
    result.materialId = 0;
    
    // Query nearby particles using spatial hash instead of checking all particles
    // Optimized search radius: reduced from 4x to 2.5x for better performance
    // This reduces the search volume while still capturing relevant particles
    float searchRadius = particleRadius * 2.5f;
    
    Particle* nearbyParticles[32]; // Optimized buffer size: reduced from 64 to 32
    int foundCount = sh_query_radius(spatialHash, position.x, position.y, position.z, searchRadius, 
                                     (void**)nearbyParticles, 32);
    
    // Calculate distance to only the nearby particles in a single pass
    // Optimization: use squared distances for comparison to avoid expensive sqrt
    float minDistanceSquared = INFINITY;
    
    for (int i = 0; i < foundCount; i++) {
        Vector3 diff = {
            position.x - nearbyParticles[i]->position.x,
            position.y - nearbyParticles[i]->position.y,
            position.z - nearbyParticles[i]->position.z
        };
        
        float distSquared = 
            diff.x * diff.x +
            diff.y * diff.y +
            diff.z * diff.z;
        
        if (distSquared < minDistanceSquared) {
            minDistanceSquared = distSquared;
            result.materialId = nearbyParticles[i]->materialId;
        }
    }
    
    // Only compute sqrt once at the end and calculate scalar field value
    result.scalarValue = (minDistanceSquared < INFINITY) ? sqrtf(minDistanceSquared) - particleRadius : INFINITY;
    
    return result;
}

// Calculate the cube index for marching cubes algorithm
static int CalculateCubeIndex(GridCell cell, float isovalue) {
    int cubeIndex = 0;
    
    if (cell.scalars[0] < isovalue) cubeIndex |= 1;
    if (cell.scalars[1] < isovalue) cubeIndex |= 2;
    if (cell.scalars[2] < isovalue) cubeIndex |= 4;
    if (cell.scalars[3] < isovalue) cubeIndex |= 8;
    if (cell.scalars[4] < isovalue) cubeIndex |= 16;
    if (cell.scalars[5] < isovalue) cubeIndex |= 32;
    if (cell.scalars[6] < isovalue) cubeIndex |= 64;
    if (cell.scalars[7] < isovalue) cubeIndex |= 128;
    
    return cubeIndex;
}

// Interpolate between two vertices based on isovalue
static Vector3 VertexInterpolation(Vector3 v1, float val1, Vector3 v2, float val2, float isovalue) {
    Vector3 result;
    
    if (fabs(isovalue - val1) < 0.00001f) return v1;
    if (fabs(isovalue - val2) < 0.00001f) return v2;
    if (fabs(val1 - val2) < 0.00001f) return v1;
    
    float mu = (isovalue - val1) / (val2 - val1);
    
    result.x = v1.x + mu * (v2.x - v1.x);
    result.y = v1.y + mu * (v2.y - v1.y);
    result.z = v1.z + mu * (v2.z - v1.z);
    
    return result;
}

// Enumeration for edge indices in a cube
typedef enum {
    EDGE_X0Y0Z0_X1Y0Z0 = 0,  // Bottom face, x-direction (vertices 0-1)
    EDGE_X1Y0Z0_X1Y1Z0 = 1,  // Bottom face, y-direction (vertices 1-2)
    EDGE_X1Y1Z0_X0Y1Z0 = 2,  // Bottom face, x-direction (vertices 2-3)
    EDGE_X0Y1Z0_X0Y0Z0 = 3,  // Bottom face, y-direction (vertices 3-0)
    
    EDGE_X0Y0Z1_X1Y0Z1 = 4,  // Top face, x-direction (vertices 4-5)
    EDGE_X1Y0Z1_X1Y1Z1 = 5,  // Top face, y-direction (vertices 5-6)
    EDGE_X1Y1Z1_X0Y1Z1 = 6,  // Top face, x-direction (vertices 6-7)
    EDGE_X0Y1Z1_X0Y0Z1 = 7,  // Top face, y-direction (vertices 7-4)
    
    EDGE_X0Y0Z0_X0Y0Z1 = 8,  // Side edges, z-direction (vertices 0-4)
    EDGE_X1Y0Z0_X1Y0Z1 = 9,  // Side edges, z-direction (vertices 1-5)
    EDGE_X1Y1Z0_X1Y1Z1 = 10, // Side edges, z-direction (vertices 2-6)
    EDGE_X0Y1Z0_X0Y1Z1 = 11  // Side edges, z-direction (vertices 3-7)
} CubeEdge;

// Define the global coordinates of an edge by its endpoints
typedef struct {
    int x1, y1, z1;  // First endpoint
    int x2, y2, z2;  // Second endpoint
} EdgeEndpoints;

// Get the global coordinates of the endpoints of an edge
static EdgeEndpoints GetEdgeEndpoints(int x, int y, int z, int edgeIndex) {
    EdgeEndpoints endpoints;
    
    // Initialize with default values
    endpoints.x1 = endpoints.x2 = x;
    endpoints.y1 = endpoints.y2 = y;
    endpoints.z1 = endpoints.z2 = z;
    
    // Set the second endpoint based on the edge direction
    switch(edgeIndex) {
        case EDGE_X0Y0Z0_X1Y0Z0: // x-direction
            endpoints.x2 = x + 1;
            break;
        case EDGE_X1Y0Z0_X1Y1Z0: // y-direction
            endpoints.x1 = endpoints.x2 = x + 1;
            endpoints.y2 = y + 1;
            break;
        case EDGE_X1Y1Z0_X0Y1Z0: // x-direction
            endpoints.x1 = x + 1;
            endpoints.y1 = endpoints.y2 = y + 1;
            break;
        case EDGE_X0Y1Z0_X0Y0Z0: // y-direction
            endpoints.y1 = y + 1;
            break;
            
        case EDGE_X0Y0Z1_X1Y0Z1: // x-direction
            endpoints.z1 = endpoints.z2 = z + 1;
            endpoints.x2 = x + 1;
            break;
        case EDGE_X1Y0Z1_X1Y1Z1: // y-direction
            endpoints.x1 = endpoints.x2 = x + 1;
            endpoints.z1 = endpoints.z2 = z + 1;
            endpoints.y2 = y + 1;
            break;
        case EDGE_X1Y1Z1_X0Y1Z1: // x-direction
            endpoints.x1 = x + 1;
            endpoints.y1 = endpoints.y2 = y + 1;
            endpoints.z1 = endpoints.z2 = z + 1;
            break;
        case EDGE_X0Y1Z1_X0Y0Z1: // y-direction
            endpoints.y1 = y + 1;
            endpoints.z1 = endpoints.z2 = z + 1;
            break;
            
        case EDGE_X0Y0Z0_X0Y0Z1: // z-direction
            endpoints.z2 = z + 1;
            break;
        case EDGE_X1Y0Z0_X1Y0Z1: // z-direction
            endpoints.x1 = endpoints.x2 = x + 1;
            endpoints.z2 = z + 1;
            break;
        case EDGE_X1Y1Z0_X1Y1Z1: // z-direction
            endpoints.x1 = endpoints.x2 = x + 1;
            endpoints.y1 = endpoints.y2 = y + 1;
            endpoints.z2 = z + 1;
            break;
        case EDGE_X0Y1Z0_X0Y1Z1: // z-direction
            endpoints.y1 = endpoints.y2 = y + 1;
            endpoints.z2 = z + 1;
            break;
    }
    
    return endpoints;
}

// Generate a unique key for an edge
unsigned long long GetEdgeKey(int x, int y, int z, int edgeIndex) {
    // Get the endpoints of the edge in global grid coordinates
    EdgeEndpoints endpoints = GetEdgeEndpoints(x, y, z, edgeIndex);
    
    // Order the endpoints to ensure consistent orientation
    int x1, y1, z1, x2, y2, z2;
    
    // Sort endpoints lexicographically (x, then y, then z)
    if (endpoints.x1 < endpoints.x2 || 
        (endpoints.x1 == endpoints.x2 && endpoints.y1 < endpoints.y2) ||
        (endpoints.x1 == endpoints.x2 && endpoints.y1 == endpoints.y2 && endpoints.z1 < endpoints.z2)) {
        x1 = endpoints.x1;
        y1 = endpoints.y1;
        z1 = endpoints.z1;
        x2 = endpoints.x2;
        y2 = endpoints.y2;
        z2 = endpoints.z2;
    } else {
        x1 = endpoints.x2;
        y1 = endpoints.y2;
        z1 = endpoints.z2;
        x2 = endpoints.x1;
        y2 = endpoints.y1;
        z2 = endpoints.z1;
    }
    
    // Create a unique 64-bit key using both endpoints
    unsigned long long key = 0;
    
    // Store endpoint coordinates (10 bits each, allowing for grids up to 1024³)
    key |= ((unsigned long long)x1 & 0x3FF);
    key |= ((unsigned long long)y1 & 0x3FF) << 10;
    key |= ((unsigned long long)z1 & 0x3FF) << 20;
    key |= ((unsigned long long)x2 & 0x3FF) << 30;
    key |= ((unsigned long long)y2 & 0x3FF) << 40;
    key |= ((unsigned long long)z2 & 0x3FF) << 50;
    
    return key;
}

// Utility function to create color based on material ID
Color GetMaterialColor(int materialId) {
    // Predefined colors for different materials
    Color colors[] = {
        (Color){ 255,   0,   0, 255 },      // Red
        (Color){ 0,   255,   0, 255 },      // Green
        (Color){ 0,     0, 255, 255 },      // Blue
        (Color){ 255, 255,   0, 255 },    // Yellow
        (Color){ 255,   0, 255, 255 },    // Magenta
        (Color){ 0,   255, 255, 255 },    // Cyan
        (Color){ 255, 128,   0, 255 },    // Orange
        (Color){ 128,   0, 255, 255 },    // Purple
    };
    
    const int colorCount = sizeof(colors) / sizeof(colors[0]);
    
    // Ensure valid index by using modulo
    int index = materialId % colorCount;
    if (index < 0) index += colorCount;
    
    return colors[index];
}
