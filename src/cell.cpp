#include "../include/cell.h"
#include "../include/cluster.h"
#include <cmath>
#include <cstdio>
#include <algorithm>

// Forward declarations we need for surface mesh generation
extern "C" {
    // Surface library function
    typedef struct {
        Vector3 center;
        Vector3 size;
        int     divisionPow;
    } Bounds;
    
    typedef struct {
        Vector3 position;
        int materialId;
    } Particle;
    
    Mesh GenerateMesh(Particle* particles, float particleRadius, int particleCount, Bounds volume);
    
    // Raymath and raylib functions we need
    Vector3 Vector3Add(Vector3 v1, Vector3 v2);
    Vector3 Vector3Subtract(Vector3 v1, Vector3 v2);
    Vector3 Vector3Scale(Vector3 v, float scalar);
    float Vector3Length(Vector3 v);
    float Vector3DotProduct(Vector3 v1, Vector3 v2);
    Material LoadMaterialDefault(void);
    void DrawMesh(Mesh mesh, Material material, Matrix transform);
    Matrix MatrixIdentity(void);
    void DrawCubeWires(Vector3 position, float width, float height, float length, Color color);
    void DrawSphere(Vector3 centerPos, float radius, Color color);
    void RL_FREE(void *ptr);
}

Cell::Cell(const Vector3& coords, int size_pow, float smallest_cell_size)
    : coordinates(coords),
      size_power(size_pow),
      actual_size(smallest_cell_size * (1 << size_pow)),
      has_meshes(false),
      is_dirty(true),
      mesh_version(0) {
    
    calculate_bounds(smallest_cell_size);
}

Cell::~Cell() {
    clear_meshes();
}

void Cell::calculate_bounds(float smallest_cell_size) {
    // Calculate center and bounds based on coordinates and size
    center = Vector3{
        (coordinates.x + 0.5f) * actual_size,
        (coordinates.y + 0.5f) * actual_size,
        (coordinates.z + 0.5f) * actual_size
    };
    
    Vector3 half_size = Vector3{actual_size * 0.5f, actual_size * 0.5f, actual_size * 0.5f};
    min_bound = Vector3Subtract(center, half_size);
    max_bound = Vector3Add(center, half_size);
}

bool Cell::contains_point(const Vector3& local_point) const {
    return (local_point.x >= min_bound.x && local_point.x <= max_bound.x &&
            local_point.y >= min_bound.y && local_point.y <= max_bound.y &&
            local_point.z >= min_bound.z && local_point.z <= max_bound.z);
}

bool Cell::intersects_sphere(const Vector3& sphere_center, float radius) const {
    // Find closest point on the cell's bounding box to the sphere center
    Vector3 closest = {
        fmaxf(min_bound.x, fminf(sphere_center.x, max_bound.x)),
        fmaxf(min_bound.y, fminf(sphere_center.y, max_bound.y)),
        fmaxf(min_bound.z, fminf(sphere_center.z, max_bound.z))
    };
    
    // Calculate distance from sphere center to closest point
    Vector3 diff = Vector3Subtract(sphere_center, closest);
    float distance_squared = Vector3DotProduct(diff, diff);
    
    return distance_squared <= (radius * radius);
}

void Cell::add_particle_index(uint32_t particle_index, uint32_t material_id) {
    auto& material_particles = material_particle_indices[material_id];
    // Check if already exists
    if (std::find(material_particles.begin(), material_particles.end(), particle_index) == material_particles.end()) {
        material_particles.push_back(particle_index);
        is_dirty = true;
    }
}

void Cell::remove_particle_index(uint32_t particle_index, uint32_t material_id) {
    auto material_it = material_particle_indices.find(material_id);
    if (material_it != material_particle_indices.end()) {
        auto& material_particles = material_it->second;
        auto it = std::find(material_particles.begin(), material_particles.end(), particle_index);
        if (it != material_particles.end()) {
            material_particles.erase(it);
            is_dirty = true;
            
            // Clean up empty material entries
            if (material_particles.empty()) {
                material_particle_indices.erase(material_it);
            }
        }
    }
}

void Cell::clear_particle_indices() {
    material_particle_indices.clear();
    is_dirty = true;
}

void Cell::rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager) {
    clear_meshes();
    
    if (material_particle_indices.empty()) {
        return;
    }
    
    // Generate a mesh for each material
    for (const auto& material_entry : material_particle_indices) {
        uint32_t material_id = material_entry.first;
        generate_mesh_for_material(material_id, cluster_particles, blas_manager);
    }
    
    has_meshes = !material_meshes.empty();
}

void Cell::generate_mesh_for_material(uint32_t material_id, const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager) {
    auto material_it = material_particle_indices.find(material_id);
    if (material_it == material_particle_indices.end() || material_it->second.empty()) {
        return;
    }
    
    const auto& particle_indices = material_it->second;
    
    // Convert cluster particles to SurfaceLib format (only for this material)
    std::vector<Particle> surface_particles;
    surface_particles.reserve(particle_indices.size());
    
    for (uint32_t idx : particle_indices) {
        if (idx < cluster_particles.size()) {
            const StaticParticle& static_particle = cluster_particles[idx];
            
            Particle surface_particle;
            surface_particle.position = static_particle.position;
            surface_particle.materialId = static_particle.materialId;
            
            surface_particles.push_back(surface_particle);
        }
    }
    
    if (surface_particles.empty()) {
        return;
    }
    
    // Calculate particle radius (use first particle's radius)
    float particle_radius = cluster_particles[particle_indices[0]].radius;
    
    // Create bounds for mesh generation
    Bounds bounds;
    bounds.center = center;
    bounds.size = Vector3{actual_size, actual_size, actual_size};
    
    // Set division power based on cell size for appropriate detail
    if (actual_size <= 2.0f) {
        bounds.divisionPow = 4; // 16x16x16 resolution
    } else if (actual_size <= 8.0f) {
        bounds.divisionPow = 3; // 8x8x8 resolution
    } else {
        bounds.divisionPow = 2; // 4x4x4 resolution
    }
    
    // Generate mesh using SurfaceLib
    Mesh mesh = GenerateMesh(surface_particles.data(), particle_radius, 
                            static_cast<int>(surface_particles.size()), bounds);
    
    if (mesh.vertexCount > 0) {
        // Store the mesh
        material_meshes[material_id] = mesh;
        
        // Create BLAS for this mesh for ray tracing
        // Note: We'll need to include the BLASManager header and implement this properly
        // For now, this is a placeholder
        material_blas[material_id] = 0; // TODO: Register mesh with BLAS manager
        
        printf("Generated mesh for cell (%.0f,%.0f,%.0f) material %u size %.1f: %d vertices, %d triangles\n",
               coordinates.x, coordinates.y, coordinates.z, material_id, actual_size,
               mesh.vertexCount, mesh.triangleCount);
    }
}

void Cell::clear_meshes() {
    // Free all mesh data
    for (auto& mesh_entry : material_meshes) {
        Mesh& mesh = mesh_entry.second;
        if (mesh.vertices) RL_FREE(mesh.vertices);
        if (mesh.normals) RL_FREE(mesh.normals);
        if (mesh.indices) RL_FREE(mesh.indices);
        if (mesh.colors) RL_FREE(mesh.colors);
    }
    
    material_meshes.clear();
    material_blas.clear();
    has_meshes = false;
}

void Cell::render(bool wireframe) const {
    if (!has_meshes || material_meshes.empty()) {
        return;
    }
    
    // Create a simple material for rendering
    static Material default_material = {0};
    static bool material_initialized = false;
    
    if (!material_initialized) {
        default_material = LoadMaterialDefault();
        material_initialized = true;
    }
    
    // TODO: Set wireframe mode on material if wireframe == true
    
    // Draw all material meshes
    for (const auto& mesh_entry : material_meshes) {
        const Mesh& mesh = mesh_entry.second;
        if (mesh.vertexCount > 0) {
            DrawMesh(mesh, default_material, MatrixIdentity());
        }
    }
}

void Cell::render_debug_bounds() const {
    // Draw wireframe cube for cell bounds
    Vector3 size = Vector3Subtract(max_bound, min_bound);
    Color color = is_dirty ? RED : (has_meshes ? GREEN : GRAY);
    
    DrawCubeWires(center, size.x, size.y, size.z, color);
    
    // Draw a small sphere at the center
    DrawSphere(center, 0.1f, color);
}

float Cell::get_diagonal_length() const {
    Vector3 size = Vector3Subtract(max_bound, min_bound);
    return Vector3Length(size);
}