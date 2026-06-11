#include "../include/cell.h"
#include "../include/cluster.h"
#include "../include/blas_manager.hpp"
#include "../include/bvh_analyzer.h"
#include "../include/cell_visitor.h"
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
    Vector3 Vector3Transform(Vector3 v, Matrix mat);
    float Vector3Length(Vector3 v);
    float Vector3DotProduct(Vector3 v1, Vector3 v2);
    Material LoadMaterialDefault(void);
    void DrawMesh(Mesh mesh, Material material, Matrix transform);
    void DrawLine3D(Vector3 startPos, Vector3 endPos, Color color);
    Matrix MatrixIdentity(void);
    Matrix MatrixTranslate(float x, float y, float z);
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
    // Cell coordinates represent the center position, so no offset needed
    center = Vector3{
        coordinates.x * actual_size,
        coordinates.y * actual_size,
        coordinates.z * actual_size
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

// Helper function to convert Raylib Mesh to triangles for BLAS registration
std::vector<Tri> convert_mesh_to_triangles(const Mesh& mesh) {
    std::vector<Tri> triangles;
    
    if (mesh.vertexCount == 0 || mesh.triangleCount == 0 || !mesh.vertices) {
        return triangles;
    }
    
    triangles.reserve(mesh.triangleCount);
    
    for (int i = 0; i < mesh.triangleCount; i++) {
        Tri tri;
        
        // Get vertex indices (either from indices array or sequential)
        int idx0, idx1, idx2;
        if (mesh.indices) {
            idx0 = mesh.indices[i * 3 + 0];
            idx1 = mesh.indices[i * 3 + 1]; 
            idx2 = mesh.indices[i * 3 + 2];
        } else {
            idx0 = i * 3 + 0;
            idx1 = i * 3 + 1;
            idx2 = i * 3 + 2;
        }
        
        // Bounds check to prevent segmentation fault
        if (idx0 >= mesh.vertexCount || idx1 >= mesh.vertexCount || idx2 >= mesh.vertexCount) {
            printf("Warning: Vertex index out of bounds in mesh conversion (triangle %d, vertices %d %d %d, max %d)\\n", 
                   i, idx0, idx1, idx2, mesh.vertexCount);
            continue;
        }
        
        // Extract vertex positions
        float v0x = mesh.vertices[idx0 * 3 + 0];
        float v0y = mesh.vertices[idx0 * 3 + 1];
        float v0z = mesh.vertices[idx0 * 3 + 2];
        float v1x = mesh.vertices[idx1 * 3 + 0];
        float v1y = mesh.vertices[idx1 * 3 + 1];
        float v1z = mesh.vertices[idx1 * 3 + 2];
        float v2x = mesh.vertices[idx2 * 3 + 0];
        float v2y = mesh.vertices[idx2 * 3 + 1];
        float v2z = mesh.vertices[idx2 * 3 + 2];
        
        // Check for invalid floating point values (NaN or infinity)
        if (!isfinite(v0x) || !isfinite(v0y) || !isfinite(v0z) ||
            !isfinite(v1x) || !isfinite(v1y) || !isfinite(v1z) ||
            !isfinite(v2x) || !isfinite(v2y) || !isfinite(v2z)) {
            printf("Warning: Triangle %d has invalid vertex coordinates, skipping\\n", i);
            continue;
        }
        
        tri.vertex0 = make_float3(v0x, v0y, v0z);
        tri.vertex1 = make_float3(v1x, v1y, v1z);
        tri.vertex2 = make_float3(v2x, v2y, v2z);
        
        // Calculate centroid
        float cx = (tri.vertex0.x + tri.vertex1.x + tri.vertex2.x) / 3.0f;
        float cy = (tri.vertex0.y + tri.vertex1.y + tri.vertex2.y) / 3.0f;
        float cz = (tri.vertex0.z + tri.vertex1.z + tri.vertex2.z) / 3.0f;
        
        // Validate centroid
        if (!isfinite(cx) || !isfinite(cy) || !isfinite(cz)) {
            printf("Warning: Triangle %d has invalid centroid, skipping\\n", i);
            continue;
        }
        
        tri.centroid = make_float3(cx, cy, cz);
        
        triangles.push_back(tri);
    }
    
    return triangles;
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
    
    // Use fixed mesh resolution for all cells regardless of size
    // This ensures consistent mesh quality and memory usage per cell
    // Smaller cells get higher voxel density (more detail per world unit)
    bounds.divisionPow = 4; // Always 16x16x16 resolution
    
    // Generate mesh using SurfaceLib
    printf("    Generating mesh: %zu particles, bounds=(%.1f,%.1f,%.1f) size=(%.1f,%.1f,%.1f) divPow=%d\n",
           surface_particles.size(), bounds.center.x, bounds.center.y, bounds.center.z,
           bounds.size.x, bounds.size.y, bounds.size.z, bounds.divisionPow);
    
    Mesh mesh = GenerateMesh(surface_particles.data(), particle_radius, 
                            static_cast<int>(surface_particles.size()), bounds);
    
    printf("    Generated mesh: %d vertices, %d triangles\n", mesh.vertexCount, mesh.triangleCount);
    
    if (mesh.vertexCount > 0) {
        // Store the mesh
        material_meshes[material_id] = mesh;

        UploadMesh(&material_meshes[material_id], false);
        
        // Register mesh with BLAS manager for ray tracing
        try {
            std::vector<Tri> triangles = convert_mesh_to_triangles(mesh);
            printf("Converting mesh to %zu triangles for BLAS registration\\n", triangles.size());
            
            if (!triangles.empty() && triangles.size() > 0) {
                printf("Registering %zu triangles with BLAS manager...\\n", triangles.size());
                material_blas[material_id] = blas_manager.register_triangles(triangles);
                printf("Successfully registered mesh with BLAS manager, handle %u\\n", material_blas[material_id]);
                
                // Also register with BVH analyzer for analysis
                BVH* bvh = blas_manager.get_bvh(material_blas[material_id]);
                BvhMesh* mesh_ptr = blas_manager.get_mesh(material_blas[material_id]);
                if (bvh && mesh_ptr) {
                    std::string analysis_name = "Cell(" + std::to_string((int)coordinates.x) + "," + 
                                               std::to_string((int)coordinates.y) + "," + 
                                               std::to_string((int)coordinates.z) + ")_Mat" + 
                                               std::to_string(material_id) + "_" + 
                                               std::to_string(triangles.size()) + "tris";
                    
                    BVHReportManager::RegisterBVH(analysis_name, bvh, mesh_ptr);
                    // Immediately update analysis for this BLAS
                    BVHReportManager::UpdateAnalysis(analysis_name);
                }
            } else {
                material_blas[material_id] = 0;
                printf("No valid triangles to register with BLAS manager\\n");
            }
        } catch (const std::exception& e) {
            printf("Error registering mesh with BLAS manager: %s\\n", e.what());
            material_blas[material_id] = 0;
        } catch (...) {
            printf("Unknown error registering mesh with BLAS manager\\n");
            material_blas[material_id] = 0;
        }
        
        printf("Generated mesh for cell (%.0f,%.0f,%.0f) material %u size %.1f: %d vertices, %d triangles, BLAS handle %u\n",
               coordinates.x, coordinates.y, coordinates.z, material_id, actual_size,
               mesh.vertexCount, mesh.triangleCount, material_blas[material_id]);
    }
}

void Cell::clear_meshes() {
    // Properly free both GPU and CPU mesh resources
    for (auto& mesh_entry : material_meshes) {
        Mesh& mesh = mesh_entry.second;
        
        // First, unload GPU resources (VAO, VBOs, etc.)
        // This is critical - without this, old mesh data stays on GPU!
        UnloadMesh(mesh);
        
        // Note: UnloadMesh() already frees the CPU memory (vertices, normals, etc.)
        // so we don't need to manually call RL_FREE() anymore
    }
    
    material_meshes.clear();
    material_blas.clear();
    has_meshes = false;
}

void Cell::accept(CellVisitor& visitor) const {
    visitor.visit_cell(*this);
}

void Cell::accept_transformed(CellRenderVisitor& visitor, const Matrix& transform) const {
    visitor.visit_cell_transformed(*this, transform);
}

float Cell::get_diagonal_length() const {
    Vector3 size = Vector3Subtract(max_bound, min_bound);
    return Vector3Length(size);
}