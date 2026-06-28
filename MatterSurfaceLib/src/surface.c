#include "../include/surface.h"
#include "../include/spatial_hash.h"
#include "../include/fat_primitive.h"   // FatPrim, primitive_sdf (typed iso-primitives)
#include "../include/csg_stages.h"      // FieldStages (ordered CSG)
#include "mc_tables.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

// Platform-specific includes for timing
#ifdef _WIN32
    // Minimal Windows includes to avoid conflicts with raylib
    #define WIN32_LEAN_AND_MEAN
    #define NOGDI        // Exclude GDI (avoids Rectangle conflict)
    #define NOUSER       // Exclude User32 (avoids CloseWindow/ShowCursor conflicts)
    #include <windows.h>
    #undef WIN32_LEAN_AND_MEAN
    #undef NOGDI
    #undef NOUSER
#else
    #include <time.h>
#endif

// Performance timing macros
#define ENABLE_PERFORMANCE_TIMING 0

#if ENABLE_PERFORMANCE_TIMING
    static double performance_timer() {
        #ifdef _WIN32
            // Windows-specific high-resolution timer
            static LARGE_INTEGER frequency = {0};
            static int frequency_initialized = 0;
            
            if (!frequency_initialized) {
                QueryPerformanceFrequency(&frequency);
                frequency_initialized = 1;
            }
            
            LARGE_INTEGER counter;
            QueryPerformanceCounter(&counter);
            return (double)counter.QuadPart / (double)frequency.QuadPart;
        #else
            // POSIX systems (Linux, macOS, etc.)
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return ts.tv_sec + ts.tv_nsec / 1e9;
        #endif
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
    float*   scalarField;
    int*     materialField;
    size_t   fieldCapacity;
    
    // Mesh buffers  
    Vector3*  vertices;
    Vector3*  normals;
    int*      materials;
    Triangle* triangles;
    size_t    vertexCapacity;
    size_t    triangleCapacity;
    
    // Edge deduplication buffers
    unsigned long long* edgeKeys;
    int* globalEdgeVertexIndices;
    size_t hashTableCapacity;
} MemoryPool;

struct SurfaceScratch {
    MemoryPool   pool;
    SpatialHash* hash;          // reused across cells; NULL until first use (Task 2)
    Particle*    hashParticles; // identity guard for hash reuse (Task 2)
    int          hashCount;     // (Task 2)
    float        hashCellSize;  // cellSize the hash was created with (Task 2)
};

// Legacy single-threaded API delegates here. Lazily created; freed by SurfaceLibCleanup.
static SurfaceScratch* g_defaultScratch = NULL;

// Memory pool management functions
static void EnsureFieldCapacity(MemoryPool* pool, size_t requiredCells) {
    if (pool->fieldCapacity < requiredCells) {
        // Grow by 50% or to required size, whichever is larger
        size_t newCapacity = (pool->fieldCapacity * 3) / 2;
        if (newCapacity < requiredCells) newCapacity = requiredCells;

        pool->scalarField = (float*)realloc(pool->scalarField, newCapacity * sizeof(float));
        pool->materialField = (int*)realloc(pool->materialField, newCapacity * sizeof(int));
        pool->fieldCapacity = newCapacity;
    }
}

static void EnsureMeshCapacity(MemoryPool* pool, size_t requiredVertices, size_t requiredTriangles) {
    if (pool->vertexCapacity < requiredVertices) {
        size_t newCapacity = (pool->vertexCapacity * 3) / 2;
        if (newCapacity < requiredVertices) newCapacity = requiredVertices;

        pool->vertices = (Vector3*)realloc(pool->vertices, newCapacity * sizeof(Vector3));
        pool->normals = (Vector3*)realloc(pool->normals, newCapacity * sizeof(Vector3));
        pool->materials = (int*)realloc(pool->materials, newCapacity * sizeof(int));
        pool->vertexCapacity = newCapacity;
    }

    if (pool->triangleCapacity < requiredTriangles) {
        size_t newCapacity = (pool->triangleCapacity * 3) / 2;
        if (newCapacity < requiredTriangles) newCapacity = requiredTriangles;

        pool->triangles = (Triangle*)realloc(pool->triangles, newCapacity * sizeof(Triangle));
        pool->triangleCapacity = newCapacity;
    }
}

static void EnsureHashTableCapacity(MemoryPool* pool, size_t requiredSize) {
    if (pool->hashTableCapacity < requiredSize) {
        size_t newCapacity = (pool->hashTableCapacity * 3) / 2;
        if (newCapacity < requiredSize) newCapacity = requiredSize;

        pool->edgeKeys = (unsigned long long*)realloc(pool->edgeKeys, newCapacity * sizeof(unsigned long long));
        pool->globalEdgeVertexIndices = (int*)realloc(pool->globalEdgeVertexIndices, newCapacity * sizeof(int));
        pool->hashTableCapacity = newCapacity;
    }
}

static void CleanupMemoryPool(MemoryPool* pool) {
    free(pool->scalarField);
    free(pool->materialField);
    free(pool->vertices);
    free(pool->normals);
    free(pool->materials);
    free(pool->triangles);
    free(pool->edgeKeys);
    free(pool->globalEdgeVertexIndices);
    *pool = (MemoryPool){0};
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
static ScalarMaterialPair CalculateScalarAndMaterial(Vector3 position, SpatialHash* spatialHash, float refRadius, float blendWidth, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend);
static ScalarMaterialPair CalculateScalarStaged(Vector3 position, SpatialHash* spatialHash, float refRadius, float blendWidth, const FieldStages* stages, const FatPrim* fat, int fatCount, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend);
static int     CalculateCubeIndex(GridCell cell, float isovalue);
static Vector3 VertexInterpolation(Vector3 v1, float val1, Vector3 v2, float val2, float isovalue);
static Mesh    GenerateMeshInternal(SurfaceScratch* scratch, Particle* particles, float particleRadius, int particleCount, Bounds volume, float blendWidth, MeshGenerationConfig config, const FieldStages* stages, const FatPrim* fat, int fatCount, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend);

// True when the staged/fat field path must run; otherwise the legacy
// single-union path is used (byte-identical to before this feature).
static int field_needs_staging(const FieldStages* stages, int fatCount) {
    if (fatCount > 0) return 1;
    if (stages && stages->stageCount > 1) return 1;
    return 0;
}
static SpatialHash* scratch_ensure_hash(SurfaceScratch* s, Particle* particles, int count, float cellSize);
static void    compute_surface_normals_impl(SpatialHash* hash, Mesh* mesh, Particle* particles, float particleRadius, int particleCount, float blendWidth, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend);


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

SurfaceScratch* CreateSurfaceScratch(void) {
    SurfaceScratch* s = (SurfaceScratch*)calloc(1, sizeof(SurfaceScratch));
    return s; // pool zero-initialized; hash created lazily
}

void DestroySurfaceScratch(SurfaceScratch* s) {
    if (!s) return;
    CleanupMemoryPool(&s->pool);
    if (s->hash) sh_destroy(s->hash);
    free(s);
}

static SurfaceScratch* DefaultScratch(void) {
    if (!g_defaultScratch) g_defaultScratch = CreateSurfaceScratch();
    return g_defaultScratch;
}

// Public API wrapper function using default configuration
Mesh GenerateMesh(Particle* particles, float particleRadius, int particleCount, Bounds volume, float blendWidth, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend) {
    MeshGenerationConfig config = GetDefaultMeshConfig();
    return GenerateMeshInternal(DefaultScratch(), particles, particleRadius, particleCount, volume, blendWidth, config, NULL, NULL, 0, clipParticles, clipCount, carveParticles, carveCount, carveBlend);
}

// Public API function with custom configuration
Mesh GenerateMeshWithConfig(Particle* particles, float particleRadius, int particleCount, Bounds volume, float blendWidth, MeshGenerationConfig config, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend) {
    return GenerateMeshInternal(DefaultScratch(), particles, particleRadius, particleCount, volume, blendWidth, config, NULL, NULL, 0, clipParticles, clipCount, carveParticles, carveCount, carveBlend);
}

// Scratch-aware mesh generation: lets a caller supply (and reuse) its own
// SurfaceScratch so the spatial hash built here can be reused for downstream
// per-triangle nearest-particle lookups (see SurfaceScratchHash).
Mesh GenerateMeshWithScratch(SurfaceScratch* scratch, Particle* particles, float particleRadius,
        int particleCount, Bounds volume, float blendWidth, Particle* clipParticles, int clipCount,
        Particle* carveParticles, int carveCount, float carveBlend) {
    return GenerateMeshInternal(scratch, particles, particleRadius, particleCount, volume, blendWidth,
        GetDefaultMeshConfig(), NULL, NULL, 0, clipParticles, clipCount, carveParticles, carveCount, carveBlend);
}

// Typed iso-primitives + ordered CSG variant. Threads the stage list + fat array
// into the field-fill; everything else (normals, simplify-free path) matches
// GenerateMeshWithScratch.
Mesh GenerateMeshStaged(SurfaceScratch* scratch, Particle* particles, float particleRadius,
        int particleCount, Bounds volume, float blendWidth,
        const FieldStages* stages, const FatPrim* fat, int fatCount,
        Particle* clipParticles, int clipCount,
        Particle* carveParticles, int carveCount, float carveBlend) {
    return GenerateMeshInternal(scratch, particles, particleRadius, particleCount, volume, blendWidth,
        GetDefaultMeshConfig(), stages, fat, fatCount, clipParticles, clipCount, carveParticles, carveCount, carveBlend);
}

// Single-point field probe through the production staged eval (test/QA seam).
float ProbeFieldScalar(SurfaceScratch* scratch, Particle* particles, float particleRadius,
        int particleCount, float blendWidth,
        const FieldStages* stages, const FatPrim* fat, int fatCount,
        Particle* carveParticles, int carveCount, float carveBlend, Vector3 point) {
    float cellSize = particleRadius * 2.5f + blendWidth * 4.0f;
    if (cellSize <= 0.0f) cellSize = 1.0f;
    SpatialHash* hash = scratch_ensure_hash(scratch, particles, particleCount, cellSize);
    ScalarMaterialPair r;
    if (field_needs_staging(stages, fatCount)) {
        r = CalculateScalarStaged(point, hash, particleRadius, blendWidth, stages, fat, fatCount,
                                  NULL, 0, carveParticles, carveCount, carveBlend);
    } else {
        r = CalculateScalarAndMaterial(point, hash, particleRadius, blendWidth,
                                       NULL, 0, carveParticles, carveCount, carveBlend);
    }
    return r.scalarValue;
}

// Scratch-aware normal recomputation: reuses the hash GenerateMeshWithScratch
// just built when it still matches (same particles/count/cellSize), otherwise
// rebuilds it in the scratch.
void ComputeSurfaceNormalsWithScratch(SurfaceScratch* scratch, Mesh* mesh, Particle* particles,
        float particleRadius, int particleCount, float blendWidth, Particle* clipParticles,
        int clipCount, Particle* carveParticles, int carveCount, float carveBlend) {
    if (!mesh || !mesh->vertices || !mesh->normals || mesh->vertexCount <= 0 ||
        !particles || particleCount <= 0) return;
    float cellSize = particleRadius * 2.5f + blendWidth * 4.0f;
    SpatialHash* hash;
    if (scratch->hash && scratch->hashParticles == particles && scratch->hashCount == particleCount
        && scratch->hashCellSize == cellSize) {
        hash = scratch->hash;                 // reuse the hash GenerateMesh just built
    } else {
        hash = scratch_ensure_hash(scratch, particles, particleCount, cellSize);
    }
    compute_surface_normals_impl(hash, mesh, particles, particleRadius, particleCount, blendWidth,
        clipParticles, clipCount, carveParticles, carveCount, carveBlend);
}

SpatialHash* SurfaceScratchHash(SurfaceScratch* scratch) { return scratch ? scratch->hash : NULL; }

// Cleanup function to release memory pool resources
void SurfaceLibCleanup(void) {
    if (g_defaultScratch) { DestroySurfaceScratch(g_defaultScratch); g_defaultScratch = NULL; }
}

// Ensure scratch->hash holds exactly `particles` at `cellSize`, reusing storage
// when possible. Recreates only when cellSize changes (the AABB query is correct
// at any cellSize, but matching it to the search radius keeps queries cheap).
static SpatialHash* scratch_ensure_hash(SurfaceScratch* s, Particle* particles,
                                        int count, float cellSize) {
    if (!s->hash || s->hashCellSize != cellSize) {
        if (s->hash) sh_destroy(s->hash);
        s->hash = sh_create(cellSize, count > 0 ? count : 1);
        s->hashCellSize = cellSize;
    } else {
        sh_clear(s->hash);
    }
    for (int i = 0; i < count; i++)
        sh_insert(s->hash, particles[i].position.x, particles[i].position.y,
                  particles[i].position.z, &particles[i]);
    s->hashParticles = particles;
    s->hashCount = count;
    return s->hash;
}

static void compute_surface_normals_impl(SpatialHash* hash, Mesh* mesh, Particle* particles,
        float particleRadius, int particleCount, float blendWidth,
        Particle* clipParticles, int clipCount,
        Particle* carveParticles, int carveCount, float carveBlend) {
    // particles/particleCount are accepted for signature parity with the public
    // API; the particle set is already resident in the passed hash.
    (void)particles; (void)particleCount;
    float gradSearch = particleRadius * 2.5f + blendWidth * 4.0f;
    Particle* nearby[128];
    for (int i = 0; i < mesh->vertexCount; i++) {
        float vx = mesh->vertices[i*3+0];
        float vy = mesh->vertices[i*3+1];
        float vz = mesh->vertices[i*3+2];

        int found = sh_query_radius_nearest(hash, vx, vy, vz, gradSearch, (void**)nearby, 128);

        // Per-particle signed distance f_j = |v - c_j| - r_j (matches the field).
        // gx/gy/gz accumulates the analytic gradient direction.
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        bool haveGrad = false;
        float fmin = INFINITY;
        int   fminIdx = -1;
        float fj[128];
        for (int j = 0; j < found; j++) {
            float dx = vx - nearby[j]->position.x;
            float dy = vy - nearby[j]->position.y;
            float dz = vz - nearby[j]->position.z;
            float d2 = dx*dx + dy*dy + dz*dz;
            fj[j] = sqrtf(d2) - nearby[j]->radius;
            if (fj[j] < fmin) { fmin = fj[j]; fminIdx = j; }
        }

        if (fminIdx >= 0) {
            if (blendWidth <= 1e-5f) {
                // Hard union: gradient of the single dominant sphere = unit(v - c).
                float dx = vx - nearby[fminIdx]->position.x;
                float dy = vy - nearby[fminIdx]->position.y;
                float dz = vz - nearby[fminIdx]->position.z;
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 > 1e-12f) {
                    float inv = 1.0f / sqrtf(d2);
                    gx = dx*inv; gy = dy*inv; gz = dz*inv; haveGrad = true;
                }
            } else {
                // Smooth-min gradient: softmax-weighted blend of each sphere's
                // outward unit direction (weights w_j = exp(-(f_j - fmin)/k)).
                float k = blendWidth;
                for (int j = 0; j < found; j++) {
                    float dx = vx - nearby[j]->position.x;
                    float dy = vy - nearby[j]->position.y;
                    float dz = vz - nearby[j]->position.z;
                    float d2 = dx*dx + dy*dy + dz*dz;
                    if (d2 <= 1e-12f) continue; // vertex on this center: no direction
                    float inv = 1.0f / sqrtf(d2);
                    float w = expf(-(fj[j] - fmin) / k);
                    gx += w * dx * inv;
                    gy += w * dy * inv;
                    gz += w * dz * inv;
                }
                float gl2 = gx*gx + gy*gy + gz*gz;
                if (gl2 > 1e-12f) {
                    float ginv = 1.0f / sqrtf(gl2);
                    gx *= ginv; gy *= ginv; gz *= ginv; haveGrad = true;
                }
            }
        }

        // Carve gradient: blend the additive gradient with the inward directions
        // of nearby subtractive particles, weighted by exp((v_j - m)/k_c), to
        // match ApplySubtractField. No-op when carveCount==0.
        if (carveParticles && carveCount > 0 && haveGrad) {
            float f_add = fmin;
            if (blendWidth > 1e-5f && found > 1) {
                float k = blendWidth, sumf = 0.0f;
                for (int j = 0; j < found; j++) sumf += expf(-(fj[j] - fmin) / k);
                f_add = fmin - k * logf(sumf);
            }
            float k_c = (carveBlend > 1e-5f) ? carveBlend : 1e-5f;
            float m = f_add;
            for (int c = 0; c < carveCount; ++c) {
                float dx = vx - carveParticles[c].position.x;
                float dy = vy - carveParticles[c].position.y;
                float dz = vz - carveParticles[c].position.z;
                float v = -(sqrtf(dx*dx+dy*dy+dz*dz) - carveParticles[c].radius);
                if (v > m) m = v;
            }
            float wadd = expf((f_add - m) / k_c);
            float ax = gx*wadd, ay = gy*wadd, az = gz*wadd, wsum = wadd;
            for (int c = 0; c < carveCount; ++c) {
                float dx = vx - carveParticles[c].position.x;
                float dy = vy - carveParticles[c].position.y;
                float dz = vz - carveParticles[c].position.z;
                float dist = sqrtf(dx*dx+dy*dy+dz*dz);
                if (dist < 1e-6f) continue;
                float v = -(dist - carveParticles[c].radius);
                float w = expf((v - m) / k_c);
                float inv = 1.0f / dist;          // grad(-s) = -unit(v - c)
                ax += w * (-dx*inv); ay += w * (-dy*inv); az += w * (-dz*inv);
                wsum += w;
            }
            if (wsum > 1e-12f) {
                gx = ax/wsum; gy = ay/wsum; gz = az/wsum;
                float gl = sqrtf(gx*gx+gy*gy+gz*gz);
                if (gl > 1e-12f) { float n=1.0f/gl; gx*=n; gy*=n; gz*=n; }
            }
        }

        // Clip field (material-aware surfacing): where a foreign surface is nearer
        // than this group's own field value, the clipped field becomes -fO, whose
        // gradient is -unit(v - c_clip). Override the group gradient there so the
        // recomputed normals match the carved (clipped) surface. This MUST trigger
        // on exactly the locus the scalar clip carves, so we mirror
        // CalculateScalarAndMaterial's final field value (the BLENDED value in the
        // smooth-min case, not the hard min). The scalar clip compares against:
        //   hard union (blendWidth<=1e-5 || found==1): scalarValue = fmin
        //   smooth-min:                                scalarValue = fmin - k*ln(sum)
        // where sum = sum_j exp(-(f_j - fmin)/k), k = blendWidth. Triggering on the
        // hard fmin instead would leave a band where the scalar surface is carved
        // (blendedValue <= fmin) but the normal override never fires, shading the
        // carved wall with the stale group gradient. Skipped entirely when
        // clipCount == 0 so the unclipped path is byte-identical to before.
        if (clipParticles && clipCount > 0 && fminIdx >= 0) {
            // Mirror the scalar path's field value (pre-clip). With blendWidth==0
            // (or a single neighbor) sum's single exp(0)=1 term gives ln(1)=0, so
            // fieldValue == fmin and behavior matches the old -fO > fmin trigger.
            float fieldValue = fmin;
            if (blendWidth > 1e-5f && found > 1) {
                float k = blendWidth;
                float sum = 0.0f;
                for (int j = 0; j < found; j++) {
                    sum += expf(-(fj[j] - fmin) / k);
                }
                fieldValue = fmin - k * logf(sum);
            }

            float fO = INFINITY;
            int   clipIdx = -1;
            for (int c = 0; c < clipCount; ++c) {
                float dx = vx - clipParticles[c].position.x;
                float dy = vy - clipParticles[c].position.y;
                float dz = vz - clipParticles[c].position.z;
                float f = sqrtf(dx*dx + dy*dy + dz*dz) - clipParticles[c].radius;
                if (f < fO) { fO = f; clipIdx = c; }
            }
            if (clipIdx >= 0 && -fO > fieldValue) {
                float dx = vx - clipParticles[clipIdx].position.x;
                float dy = vy - clipParticles[clipIdx].position.y;
                float dz = vz - clipParticles[clipIdx].position.z;
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 > 1e-12f) {
                    float inv = 1.0f / sqrtf(d2);
                    // grad(-fO) = -unit(v - c_clip)
                    gx = -dx*inv; gy = -dy*inv; gz = -dz*inv; haveGrad = true;
                }
            }
        }

        if (haveGrad) {
            mesh->normals[i*3+0] = gx;
            mesh->normals[i*3+1] = gy;
            mesh->normals[i*3+2] = gz;
        } else {
            // Degenerate vertex (on a center, or none in range): normalize the
            // incoming normal, else fall back to a stable up vector.
            float fx = mesh->normals[i*3+0], fy = mesh->normals[i*3+1], fz = mesh->normals[i*3+2];
            float length = sqrtf(fx*fx + fy*fy + fz*fz);
            if (length > 0.0001f) {
                mesh->normals[i*3+0] = fx/length;
                mesh->normals[i*3+1] = fy/length;
                mesh->normals[i*3+2] = fz/length;
            } else {
                mesh->normals[i*3+0] = 0.0f;
                mesh->normals[i*3+1] = 0.0f;
                mesh->normals[i*3+2] = 1.0f;
            }
        }
    }
}

void ComputeSurfaceNormals(Mesh* mesh, Particle* particles, float particleRadius, int particleCount, float blendWidth, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend) {
    ComputeSurfaceNormalsWithScratch(DefaultScratch(), mesh, particles, particleRadius, particleCount, blendWidth, clipParticles, clipCount, carveParticles, carveCount, carveBlend);
}

// Internal mesh generation function with configuration
static Mesh GenerateMeshInternal(SurfaceScratch* scratch, Particle* particles, float particleRadius, int particleCount, Bounds volume, float blendWidth, MeshGenerationConfig config, const FieldStages* stages, const FatPrim* fat, int fatCount, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend) {
    TIMER_START(total);
    const int useStaged = field_needs_staging(stages, fatCount);
    
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
        EnsureFieldCapacity(&scratch->pool, data.totalCells);
        data.scalarField = scratch->pool.scalarField;
        data.materialField = scratch->pool.materialField;
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
    
    // Build (or reuse) the shared spatial hash for efficient particle queries.
    // Size the cell to the field query radius (r*2.5 + blend*4) so each voxel
    // query scans a ~3^3 bucket neighborhood instead of 7^3 mostly-empty
    // buckets. The scratch owns the hash and reuses its storage across cells.
    float spatialCellSize = particleRadius * 2.5f + blendWidth * 4.0f;
    SpatialHash* spatialHash = scratch_ensure_hash(scratch, particles, particleCount, spatialCellSize);

    if (!spatialHash) {
        printf("Failed to create spatial hash\n");
        if (!config.enableMemoryReuse) {
            free(data.scalarField);
            free(data.materialField);
        }
        return mesh;
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

                // Use combined calculation to eliminate duplicate distance calculations.
                // Staged path runs only when ordered CSG stages / fat primitives are
                // present; otherwise the legacy union-then-carve call is byte-identical.
                ScalarMaterialPair result = useStaged
                    ? CalculateScalarStaged(position, spatialHash, particleRadius, blendWidth,
                                            stages, fat, fatCount, clipParticles, clipCount,
                                            carveParticles, carveCount, carveBlend)
                    : CalculateScalarAndMaterial(position, spatialHash, particleRadius, blendWidth,
                                            clipParticles, clipCount, carveParticles, carveCount, carveBlend);
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
        EnsureMeshCapacity(&scratch->pool, maxVertices, maxTriangles);
        vertices = scratch->pool.vertices;
        normals = scratch->pool.normals;
        materials = scratch->pool.materials;
        triangles = scratch->pool.triangles;
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
            EnsureHashTableCapacity(&scratch->pool, hashTableSize);
            edgeKeys = scratch->pool.edgeKeys;
            globalEdgeVertexIndices = scratch->pool.globalEdgeVertexIndices;
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
    
    // Adaptive isosurface threshold based on cell size relative to particle radius
    // At higher LOD levels (larger cells), use a more negative isovalue to expand the surface
    // This prevents meshes from becoming too thin when cell resolution decreases
    float cellSizeRatio = fmaxf(data.cellSize.x, fmaxf(data.cellSize.y, data.cellSize.z)) / particleRadius;
    float adaptiveOffset = fmaxf(0.0f, (cellSizeRatio - 1.0f) * particleRadius * 0.3f);
    const float isovalue = -adaptiveOffset; // More negative = larger surface at low LOD
    
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
    
    // The per-cell face normals above are only a fallback. The real shading
    // normal is the analytic SDF gradient (computed below, after the mesh
    // arrays are filled): for a union of spheres it is the unit vector from the
    // nearest particle to the vertex, which depends only on world position and
    // is therefore continuous across independently-meshed cells. See
    // ComputeSurfaceNormals.

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

    // Overwrite the per-cell face normals with the cross-cell-continuous SDF
    // gradient (face normals remain only as the degenerate-vertex fallback).
    // Reuse the spatial hash we just built rather than rebuilding it.
    compute_surface_normals_impl(spatialHash, &mesh, particles, particleRadius, particleCount, blendWidth, clipParticles, clipCount, carveParticles, carveCount, carveBlend);

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
    // The scratch owns spatialHash; it is cleared/reused on the next call and
    // freed by DestroySurfaceScratch, so we do NOT destroy it here.

    TIMER_END(total, "Total Mesh Generation");
    
    return mesh;
}

// Apply the clip field (material-aware surfacing): where a foreign clip surface
// is nearer than this group's own field, force the group OUTSIDE so its
// isosurface terminates on the equidistant locus f_G == fO. Sign convention:
// scalarValue = signed distance, NEGATIVE = inside, POSITIVE = outside (marching
// cubes contours at isovalue <= 0 with corners < isovalue treated as inside), so
// raising scalarValue toward/above 0 pushes the surface back. No-op when
// clipCount == 0, keeping the unclipped path byte-identical.
static inline void ApplyClipField(ScalarMaterialPair* result, Vector3 position,
                                  Particle* clipParticles, int clipCount) {
    if (!clipParticles || clipCount <= 0) return;
    float fO = INFINITY;
    for (int i = 0; i < clipCount; ++i) {
        float dx = position.x - clipParticles[i].position.x;
        float dy = position.y - clipParticles[i].position.y;
        float dz = position.z - clipParticles[i].position.z;
        float f = sqrtf(dx*dx + dy*dy + dz*dz) - clipParticles[i].radius;
        if (f < fO) fO = f;
    }
    // Where a foreign surface is nearer (fO < f_G), force this group outside so
    // its isosurface terminates on the equidistant locus f_G == fO. We touch only
    // scalarValue, never materialId: the carved wall keeps this group's own
    // material so distinct types meet at a clean interface instead of fusing.
    if (-fO > result->scalarValue) result->scalarValue = -fO;
}

// Smooth-CSG subtraction: carve inward where a subtractive particle is near.
// f_carved = smooth_max(f_add, -(|p-c_j|-r_j)) via log-sum-exp with a max-shift
// for numerical stability. k_c<=0 collapses to the hard max. materialId is left
// untouched (a divot exposes the surrounding material). No-op when carveCount==0,
// so the uncarved path is byte-identical.
static inline void ApplySubtractField(ScalarMaterialPair* result, Vector3 position,
                                      Particle* carveParticles, int carveCount, float k_c) {
    if (!carveParticles || carveCount <= 0) return;
    float f_add = result->scalarValue;
    if (k_c <= 1e-5f) {
        float best = f_add;
        for (int j = 0; j < carveCount; ++j) {
            float dx = position.x - carveParticles[j].position.x;
            float dy = position.y - carveParticles[j].position.y;
            float dz = position.z - carveParticles[j].position.z;
            float v = -(sqrtf(dx*dx + dy*dy + dz*dz) - carveParticles[j].radius);
            if (v > best) best = v;
        }
        result->scalarValue = best;
        return;
    }
    float m = f_add;
    for (int j = 0; j < carveCount; ++j) {
        float dx = position.x - carveParticles[j].position.x;
        float dy = position.y - carveParticles[j].position.y;
        float dz = position.z - carveParticles[j].position.z;
        float v = -(sqrtf(dx*dx + dy*dy + dz*dz) - carveParticles[j].radius);
        if (v > m) m = v;
    }
    float sum = expf((f_add - m) / k_c);
    for (int j = 0; j < carveCount; ++j) {
        float dx = position.x - carveParticles[j].position.x;
        float dy = position.y - carveParticles[j].position.y;
        float dz = position.z - carveParticles[j].position.z;
        float v = -(sqrtf(dx*dx + dy*dy + dz*dz) - carveParticles[j].radius);
        sum += expf((v - m) / k_c);
    }
    result->scalarValue = m + k_c * logf(sum);
}

// Combined calculation to eliminate duplicate distance calculations
static ScalarMaterialPair CalculateScalarAndMaterial(Vector3 position, SpatialHash* spatialHash, float refRadius, float blendWidth, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend) {
    ScalarMaterialPair result;
    result.scalarValue = INFINITY;
    result.materialId = 0;

    // Query nearby particles. The search must reach the surface of the largest
    // particle (refRadius is the max radius in the set) plus the smooth-min
    // fillet, so distant blend partners are not missed.
    float searchRadius = refRadius * 2.5f + blendWidth * 4.0f;

    Particle* nearbyParticles[128];
    int foundCount = sh_query_radius_nearest(spatialHash, position.x, position.y, position.z, searchRadius,
                                             (void**)nearbyParticles, 128);
    if (foundCount <= 0) {
        return result; // INFINITY scalar, material 0
    }

    // Per-particle signed distance f_i = |p - c_i| - r_i, using each particle's
    // own radius. Track the hard-min (dominant particle) for the material id and
    // as the stable base point for the smooth-min.
    float perDist[128];
    float fmin = INFINITY;
    int   fminIdx = 0;
    for (int i = 0; i < foundCount; i++) {
        float dx = position.x - nearbyParticles[i]->position.x;
        float dy = position.y - nearbyParticles[i]->position.y;
        float dz = position.z - nearbyParticles[i]->position.z;
        float f = sqrtf(dx*dx + dy*dy + dz*dz) - nearbyParticles[i]->radius;
        perDist[i] = f;
        if (f < fmin) { fmin = f; fminIdx = i; }
    }

    result.materialId = nearbyParticles[fminIdx]->materialId;

    if (blendWidth <= 1e-5f || foundCount == 1) {
        // Hard union: exact min. With a single uniform radius this is identical
        // to the original sqrt(minDistSq) - particleRadius behavior.
        result.scalarValue = fmin;
        ApplySubtractField(&result, position, carveParticles, carveCount, carveBlend);
        ApplyClipField(&result, position, clipParticles, clipCount);
        return result;
    }

    // Exponential smooth-min (metaball union) via the log-sum-exp trick:
    //   f = fmin - k * ln( sum_i exp(-(f_i - fmin)/k) )
    // k = blendWidth sets the fillet size; k -> 0 recovers the hard min. The
    // result is <= fmin, so neighboring spheres bulge together where they meet.
    float k = blendWidth;
    float sum = 0.0f;
    for (int i = 0; i < foundCount; i++) {
        sum += expf(-(perDist[i] - fmin) / k);
    }
    result.scalarValue = fmin - k * logf(sum);

    ApplySubtractField(&result, position, carveParticles, carveCount, carveBlend);
    ApplyClipField(&result, position, clipParticles, clipCount);
    return result;
}

// Smooth-min (metaball union) of a set of signed distances via log-sum-exp:
//   f = fmin - k*ln( sum_i exp(-(f_i - fmin)/k) ).  k<=0 => hard min.
// `vals[0..n)` are the per-element signed distances; fmin must be their min.
static float smin_set(const float* vals, int n, float fmin, float k) {
    if (n <= 0) return INFINITY;
    if (k <= 1e-5f || n == 1) return fmin;
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += expf(-(vals[i] - fmin) / k);
    return fmin - k * logf(sum);
}

// Ordered-CSG staged field evaluation (Phase 1). Folds the authored stages in
// order so add->subtract->add is correct. The sphere stream is queried ONCE from
// the hash and bucketed by stage (stages->particleStage[i]); fat primitives are
// linear-scanned into their stage. Carve and clip are applied after the stages as
// special cases (legacy behavior preserved). Material id is taken from the nearest
// additive sphere overall, as in the legacy path.
//
// This path runs only when ordered stages and/or fat primitives are present; the
// common single-union case stays on CalculateScalarAndMaterial (byte-identical).
static ScalarMaterialPair CalculateScalarStaged(
        Vector3 position, SpatialHash* spatialHash, float refRadius, float blendWidth,
        const FieldStages* stages, const FatPrim* fat, int fatCount,
        Particle* clipParticles, int clipCount,
        Particle* carveParticles, int carveCount, float carveBlend) {
    ScalarMaterialPair result;
    result.scalarValue = INFINITY;
    result.materialId = 0;

    int stageCount = stages ? stages->stageCount : 1;
    if (stageCount < 1) stageCount = 1;

    float searchRadius = refRadius * 2.5f + blendWidth * 4.0f;
    Particle* nearby[128];
    int foundCount = sh_query_radius_nearest(spatialHash, position.x, position.y, position.z,
                                             searchRadius, (void**)nearby, 128);

    // Per-stage accumulation of additive-sphere distances (bucketed by stage).
    // STAGE_CAP bounds stack use; deeper stages fold via the field below.
    enum { STAGE_CAP = 64 };
    if (stageCount > STAGE_CAP) stageCount = STAGE_CAP;
    float stageVals[STAGE_CAP][128];
    int   stageN[STAGE_CAP];
    float stageMin[STAGE_CAP];
    for (int s = 0; s < stageCount; ++s) { stageN[s] = 0; stageMin[s] = INFINITY; }

    // Material from the globally-nearest additive sphere (matches legacy).
    float bestF = INFINITY;
    for (int i = 0; i < foundCount; ++i) {
        Particle* pp = nearby[i];
        float dx = position.x - pp->position.x;
        float dy = position.y - pp->position.y;
        float dz = position.z - pp->position.z;
        float f = sqrtf(dx*dx + dy*dy + dz*dz) - pp->radius;
        if (f < bestF) { bestF = f; result.materialId = pp->materialId; }

        // Resolve this particle's stage. particleStage is parallel to the hash's
        // backing Particle array, so index = pp - particles base. We instead read
        // it via the index encoded by the hash lookup is unavailable; the caller
        // guarantees particleStage indexing matches pointer order from a single
        // contiguous array, so recover the index by pointer arithmetic against the
        // first found is unsafe. Use stage tag from the FieldStages map keyed by
        // pointer offset supplied by the caller's particle base.
        int st = 0;
        if (stages && stages->particleStage && stages->stageCount > 1) {
            // Pointer-difference against the caller's contiguous particle array.
            // The hash stores &particles[i]; particleStage is indexed identically.
            long idx = (long)(pp - (Particle*)stages->_particleBase);
            if (idx >= 0 && idx < stages->_particleCount) st = stages->particleStage[idx];
        }
        if (st < 0 || st >= stageCount) st = 0;
        if (stageN[st] < 128) {
            stageVals[st][stageN[st]] = f;
            if (f < stageMin[st]) stageMin[st] = f;
            stageN[st]++;
        }
    }

    // Fold the ordered stages. Each stage's SDF = smin over its spheres unioned
    // with its fat primitives (also via smin so fat blends with stage spheres).
    float field = INFINITY;
    int haveAny = 0;
    for (int s = 0; s < stageCount; ++s) {
        // Collect this stage's fat-primitive distances alongside its spheres.
        float vals[128 + 64];
        int   n = 0;
        float fmin = stageMin[s];
        for (int i = 0; i < stageN[s] && n < (int)(sizeof(vals)/sizeof(vals[0])); ++i)
            vals[n++] = stageVals[s][i];
        for (int j = 0; j < fatCount; ++j) {
            if (fat[j].stage != s) continue;
            float d = primitive_sdf(&fat[j], position);
            if (n < (int)(sizeof(vals)/sizeof(vals[0]))) {
                vals[n] = d;
                if (d < fmin) fmin = d;
                n++;
            }
        }
        if (n == 0) continue; // empty stage contributes nothing
        float d = smin_set(vals, n, fmin, blendWidth);

        CsgStageOp op = stages ? stages->stageOp[s] : CSG_STAGE_UNION;
        if (!haveAny) {
            // First non-empty stage seeds the field regardless of its op (an
            // opening Difference/Intersection has nothing to act on yet).
            field = d; haveAny = 1;
        } else {
            switch (op) {
                case CSG_STAGE_UNION:        field = fminf(field, d); break;
                case CSG_STAGE_DIFFERENCE:   field = fmaxf(field, -d); break;
                case CSG_STAGE_INTERSECTION: field = fmaxf(field, d); break;
            }
        }
    }

    result.scalarValue = haveAny ? field : INFINITY;
    // Legacy carve + clip as trailing special cases (unchanged semantics).
    ApplySubtractField(&result, position, carveParticles, carveCount, carveBlend);
    ApplyClipField(&result, position, clipParticles, clipCount);
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

// Convert raylib Mesh to BVH Triangle array with per-vertex normals
BVHTriangle* ConvertMeshToBVHTriangles(Mesh mesh, int* triangleCount) {
    if (!mesh.vertices || !mesh.normals || !mesh.indices || mesh.triangleCount == 0) {
        *triangleCount = 0;
        return NULL;
    }
    
    *triangleCount = mesh.triangleCount;
    BVHTriangle* bvhTriangles = (BVHTriangle*)malloc(mesh.triangleCount * sizeof(BVHTriangle));
    if (!bvhTriangles) {
        *triangleCount = 0;
        return NULL;
    }
    
    // Convert each triangle
    for (int i = 0; i < mesh.triangleCount; i++) {
        BVHTriangle* tri = &bvhTriangles[i];
        
        // Get vertex indices for this triangle
        int idx0 = mesh.indices[i * 3 + 0];
        int idx1 = mesh.indices[i * 3 + 1];
        int idx2 = mesh.indices[i * 3 + 2];
        
        // Set vertices
        tri->v0.x = mesh.vertices[idx0 * 3 + 0];
        tri->v0.y = mesh.vertices[idx0 * 3 + 1];
        tri->v0.z = mesh.vertices[idx0 * 3 + 2];
        
        tri->v1.x = mesh.vertices[idx1 * 3 + 0];
        tri->v1.y = mesh.vertices[idx1 * 3 + 1];
        tri->v1.z = mesh.vertices[idx1 * 3 + 2];
        
        tri->v2.x = mesh.vertices[idx2 * 3 + 0];
        tri->v2.y = mesh.vertices[idx2 * 3 + 1];
        tri->v2.z = mesh.vertices[idx2 * 3 + 2];
        
        // Set per-vertex normals
        tri->n0.x = mesh.normals[idx0 * 3 + 0];
        tri->n0.y = mesh.normals[idx0 * 3 + 1];
        tri->n0.z = mesh.normals[idx0 * 3 + 2];
        
        tri->n1.x = mesh.normals[idx1 * 3 + 0];
        tri->n1.y = mesh.normals[idx1 * 3 + 1];
        tri->n1.z = mesh.normals[idx1 * 3 + 2];
        
        tri->n2.x = mesh.normals[idx2 * 3 + 0];
        tri->n2.y = mesh.normals[idx2 * 3 + 1];
        tri->n2.z = mesh.normals[idx2 * 3 + 2];
        
        // Compute centroid
        tri->centroid.x = (tri->v0.x + tri->v1.x + tri->v2.x) / 3.0f;
        tri->centroid.y = (tri->v0.y + tri->v1.y + tri->v2.y) / 3.0f;
        tri->centroid.z = (tri->v0.z + tri->v1.z + tri->v2.z) / 3.0f;
        
        // Compute face normal using cross product
        Vec3 edge1 = {tri->v1.x - tri->v0.x, tri->v1.y - tri->v0.y, tri->v1.z - tri->v0.z};
        Vec3 edge2 = {tri->v2.x - tri->v0.x, tri->v2.y - tri->v0.y, tri->v2.z - tri->v0.z};
        
        tri->normal.x = edge1.y * edge2.z - edge1.z * edge2.y;
        tri->normal.y = edge1.z * edge2.x - edge1.x * edge2.z;
        tri->normal.z = edge1.x * edge2.y - edge1.y * edge2.x;
        
        // Normalize face normal
        float length = sqrtf(tri->normal.x * tri->normal.x + 
                            tri->normal.y * tri->normal.y + 
                            tri->normal.z * tri->normal.z);
        if (length > 0.0001f) {
            tri->normal.x /= length;
            tri->normal.y /= length;
            tri->normal.z /= length;
        }
        
        // Set default material ID
        tri->material_id = 0;
    }
    
    return bvhTriangles;
}

// Free BVH triangle array
void FreeBVHTriangles(BVHTriangle* triangles) {
    if (triangles) {
        free(triangles);
    }
}
