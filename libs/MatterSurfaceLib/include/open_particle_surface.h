#ifndef OPEN_PARTICLE_SURFACE_H
#define OPEN_PARTICLE_SURFACE_H

#include "raylib.h"
#include "surface.h"

/**
 * OpenParticleSurfaceLib - Dynamic particle-based surface mesh generation
 * 
 * This library allows for creating, manipulating, and rendering dynamic
 * surface meshes based on particle positions. It efficiently handles 
 * large numbers of particles and generates meshes only in regions where
 * particles exist, using spatial hashing for optimal performance.
 */

// Handle for a particle in the system
typedef int ParticleHandle;

/**
 * Initialize the particle system
 * @param maxParticles Maximum number of particles the system can handle
 * @param particleRadius Radius of each particle's influence
 */
void InitializeParticleSystem(int maxParticles, float particleRadius);

/**
 * Shutdown and clean up the particle system
 */
void ShutdownParticleSystem(void);

/**
 * Create a new particle in the system
 * @param position Initial position of the particle
 * @param materialId Material ID for the particle (affects appearance)
 * @return Handle to the created particle, or -1 if creation failed
 */
ParticleHandle CreateParticle(Vector3 position, int materialId);

/**
 * Create multiple particles at once
 * @param positions Array of positions for new particles
 * @param materialIds Array of material IDs for new particles
 * @param count Number of particles to create
 * @return Number of particles successfully created
 */
int CreateParticles(Vector3* positions, int* materialIds, int count);

/**
 * Update the position of an existing particle
 * @param handle Handle of the particle to update
 * @param newPosition New position for the particle
 * @return true if successful, false if the handle is invalid
 */
bool UpdateParticlePosition(ParticleHandle handle, Vector3 newPosition);

/**
 * Delete a particle from the system
 * @param handle Handle of the particle to delete
 * @return true if successful, false if the handle is invalid
 */
bool DeleteParticle(ParticleHandle handle);

/**
 * Update the particle system
 * This regenerates meshes in dirty regions, limited to a fixed number per frame
 * @param maxUpdatesPerFrame Maximum number of cell updates to process
 * @return Number of cells updated
 */
int UpdateParticleSystem(int maxUpdatesPerFrame);

/**
 * Get the total number of particles in the system
 * @return Current particle count
 */
int GetParticleCount(void);

/**
 * Get the maximum number of particles the system can handle
 * @return Maximum particle capacity
 */
int GetParticleCapacity(void);

/**
 * Draw all particle meshes
 * @param material Material to use for rendering
 * @param wireframe Whether to render in wireframe mode
 */
void DrawParticleMeshes(Material material, bool wireframe);

/**
 * Draw debug visualization of particle system cells
 * @param showBounds Whether to show cell bounds
 */
void DrawParticleSystemDebug(bool showBounds);

/**
 * Draw particles as individual spheres (for debugging)
 * @param useInstancing Whether to use instanced rendering for better performance
 * @param maxInstancesToDraw Maximum number of instances to draw (for performance)
 */
void DrawParticles(bool useInstancing, int maxInstancesToDraw);

/**
 * Get performance statistics for the particle system
 * @param activeCellCount Pointer to store the number of active cells
 * @param dirtyRegionCount Pointer to store the number of dirty regions
 * @param meshVertexCount Pointer to store the total number of vertices
 */
void GetParticleSystemStats(int* activeCellCount, int* dirtyRegionCount, int* meshVertexCount);

#endif // OPEN_PARTICLE_SURFACE_H