#include "../include/cell.h"
#include "../include/cluster.h"
#include "../include/blas_manager.hpp"
#include "../include/bvh_analyzer.h"
#include "../include/cell_visitor.h"
#include "mesh_simplifier.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
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
    void ComputeSurfaceNormals(Mesh* mesh, Particle* particles, float particleRadius, int particleCount);
    
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
    // Cell coordinates use the cluster's CORNER convention: a point at local
    // position p belongs to cell floor(p / size), so cell C spans
    // [C*size, (C+1)*size] and is centered at (C+0.5)*size. This must match
    // Cluster::get_cell_coordinates and the cell spatial-hash key; otherwise
    // the mesh-generation box is shifted half a cell and spheres render as
    // partial blobs.
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

void Cell::rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio) {
    clear_meshes(&blas_manager);

    if (material_particle_indices.empty()) {
        return;
    }

    // Generate a mesh for each material
    for (const auto& material_entry : material_particle_indices) {
        uint32_t material_id = material_entry.first;
        generate_mesh_for_material(material_id, cluster_particles, blas_manager, simplification_ratio);
    }

    has_meshes = !material_meshes.empty();
}

// Helper function to convert Raylib Mesh to triangles for BLAS registration
std::vector<Tri> convert_mesh_to_triangles(const Mesh& mesh, std::vector<TriEx>* out_triex) {
    std::vector<Tri> triangles;

    if (mesh.vertexCount == 0 || mesh.triangleCount == 0 || !mesh.vertices) {
        return triangles;
    }

    triangles.reserve(mesh.triangleCount);
    if (out_triex) {
        out_triex->clear();
        out_triex->reserve(mesh.triangleCount);
    }
    // Per-vertex smooth normals are only available when the mesh carries them.
    const bool have_normals = (out_triex != nullptr) && (mesh.normals != nullptr);

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

        if (out_triex) {
            TriEx ex{};
            if (have_normals) {
                ex.N0 = make_float3(mesh.normals[idx0 * 3 + 0], mesh.normals[idx0 * 3 + 1], mesh.normals[idx0 * 3 + 2]);
                ex.N1 = make_float3(mesh.normals[idx1 * 3 + 0], mesh.normals[idx1 * 3 + 1], mesh.normals[idx1 * 3 + 2]);
                ex.N2 = make_float3(mesh.normals[idx2 * 3 + 0], mesh.normals[idx2 * 3 + 1], mesh.normals[idx2 * 3 + 2]);
            } else {
                // Fall back to the face normal so all three vertices share it (flat shading).
                float3 fn = normalize(cross(tri.vertex1 - tri.vertex0, tri.vertex2 - tri.vertex0));
                ex.N0 = ex.N1 = ex.N2 = fn;
            }
            out_triex->push_back(ex);
        }
    }

    return triangles;
}

// Build one pre-upload mesh for a set of particles that all share the same
// radius. SurfaceLib's GenerateMesh bakes a single radius into its SDF, so each
// radius must be meshed separately. Decimation (when requested) and the SDF
// gradient normal pass both run at this group's own radius.
static Mesh build_radius_group_mesh(std::vector<Particle>& group, float radius,
                                    const Vector3& center, float actual_size,
                                    const Vector3& min_bound, const Vector3& max_bound,
                                    float simplification_ratio) {
    Bounds bounds;
    bounds.center = center;
    bounds.size = Vector3{actual_size, actual_size, actual_size};
    bounds.divisionPow = 4; // Always 16x16x16 resolution

    Mesh mesh = GenerateMesh(group.data(), radius, static_cast<int>(group.size()), bounds);

    // Decimate to a low-poly proxy when the cluster requests it. Boundary
    // vertices on this cell's face planes are locked so seams with same-level
    // neighbor cells stay watertight.
    if (simplification_ratio < 1.0f && mesh.vertexCount > 0 && mesh.triangleCount > 0) {
        CellBounds cb;
        cb.min_bound = min_bound;
        cb.max_bound = max_bound;
        SimplifyOptions so;
        so.target_ratio = simplification_ratio;
        so.lock_boundary = true;
        Mesh simplified = simplify_mesh(mesh, so, &cb);
        if (simplified.vertexCount > 0 && simplified.triangleCount > 0) {
            // simplify_mesh rebuilds normals from face geometry, reintroducing
            // the per-cell shading seams; reapply the cross-cell-continuous SDF
            // gradient so the decimated proxy shades identically to the dense mesh.
            ComputeSurfaceNormals(&simplified, group.data(), radius,
                                  static_cast<int>(group.size()));
            UnloadMesh(mesh);   // free the pre-upload CPU arrays of the dense mesh
            mesh = simplified;
        } else {
            UnloadMesh(simplified); // simplification produced nothing usable; keep dense mesh
        }
    }
    return mesh;
}

// Concatenate several pre-upload meshes into one indexed mesh allocated with
// raylib's allocator (safe for UploadMesh/UnloadMesh). Consumes (frees) the
// parts. Vertex colors are kept only when every part carries them -- the
// decimated path drops colors, matching the prior single-mesh behavior.
static Mesh concat_meshes(std::vector<Mesh>& parts) {
    int totalV = 0, totalT = 0;
    bool all_colors = !parts.empty();
    for (const Mesh& m : parts) {
        totalV += m.vertexCount;
        totalT += m.triangleCount;
        if (!m.colors) all_colors = false;
    }

    Mesh out = {0};
    // 16-bit indices cap a merged cell at 65535 verts; at divisionPow 4 a single
    // cell never approaches this, so bail rather than emit wrapped indices.
    if (totalV == 0 || totalT == 0 || totalV > 65535) {
        for (Mesh& m : parts) UnloadMesh(m);
        return out;
    }

    out.vertexCount   = totalV;
    out.triangleCount = totalT;
    out.vertices = (float*)MemAlloc(totalV * 3 * sizeof(float));
    out.normals  = (float*)MemAlloc(totalV * 3 * sizeof(float));
    out.indices  = (unsigned short*)MemAlloc(totalT * 3 * sizeof(unsigned short));
    out.colors   = all_colors ? (unsigned char*)MemAlloc(totalV * 4 * sizeof(unsigned char)) : nullptr;

    int vOff = 0, tOff = 0;
    for (Mesh& m : parts) {
        memcpy(out.vertices + vOff * 3, m.vertices, m.vertexCount * 3 * sizeof(float));
        if (m.normals) memcpy(out.normals + vOff * 3, m.normals, m.vertexCount * 3 * sizeof(float));
        else           memset(out.normals + vOff * 3, 0, m.vertexCount * 3 * sizeof(float));
        if (all_colors) memcpy(out.colors + vOff * 4, m.colors, m.vertexCount * 4 * sizeof(unsigned char));
        for (int t = 0; t < m.triangleCount * 3; ++t) {
            out.indices[tOff * 3 + t] = (unsigned short)(m.indices[t] + vOff);
        }
        vOff += m.vertexCount;
        tOff += m.triangleCount;
        UnloadMesh(m);
    }
    return out;
}

void Cell::generate_mesh_for_material(uint32_t material_id, const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio) {
    auto material_it = material_particle_indices.find(material_id);
    if (material_it == material_particle_indices.end() || material_it->second.empty()) {
        return;
    }

    const auto& particle_indices = material_it->second;

    // Group this material's particles by radius. Mixing radii in one GenerateMesh
    // call would render every particle at the first one's radius; a small added
    // particle inflated to a large radius fills the whole cell and erases the
    // isosurface, punching holes in the surface (most visible at small cell
    // sizes, i.e. low LOD). One sub-mesh per radius avoids that; merge below.
    struct RadiusGroup { float radius; std::vector<Particle> particles; };
    std::vector<RadiusGroup> groups;
    for (uint32_t idx : particle_indices) {
        if (idx >= cluster_particles.size()) continue;
        const StaticParticle& sp = cluster_particles[idx];

        RadiusGroup* group = nullptr;
        for (auto& candidate : groups) {
            if (candidate.radius == sp.radius) { group = &candidate; break; }
        }
        if (!group) {
            groups.push_back({sp.radius, {}});
            group = &groups.back();
        }

        Particle surface_particle;
        surface_particle.position = sp.position;
        surface_particle.materialId = sp.materialId;
        group->particles.push_back(surface_particle);
    }

    if (groups.empty()) {
        return;
    }

    std::vector<Mesh> parts;
    parts.reserve(groups.size());
    for (auto& group : groups) {
        Mesh part = build_radius_group_mesh(group.particles, group.radius, center, actual_size,
                                            min_bound, max_bound, simplification_ratio);
        if (part.vertexCount > 0 && part.triangleCount > 0) {
            parts.push_back(part);
        } else {
            UnloadMesh(part);
        }
    }

    Mesh mesh = concat_meshes(parts);

    if (mesh.vertexCount > 0) {
        // Store the mesh
        material_meshes[material_id] = mesh;

        UploadMesh(&material_meshes[material_id], false);

        // Register mesh with BLAS manager for ray tracing
        try {
            std::vector<TriEx> triangle_normals;
            std::vector<Tri> triangles = convert_mesh_to_triangles(mesh, &triangle_normals);
            printf("Converting mesh to %zu triangles for BLAS registration\\n", triangles.size());

            if (!triangles.empty() && triangles.size() > 0) {
                printf("Registering %zu triangles with BLAS manager...\\n", triangles.size());
                material_blas[material_id] = blas_manager.register_triangles(triangles, triangle_normals);
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

void Cell::clear_meshes(BLASManager* blas_manager) {
    // Release this cell's BLAS references so the manager can reclaim entries
    // that no live cell still points at (prevents unbounded GPU accumulation).
    if (blas_manager) {
        for (const auto& blas_entry : material_blas) {
            blas_manager->release_blas(blas_entry.second);
        }
    }

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