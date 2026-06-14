#include "../include/cell.h"
#include "../include/cluster.h"
#include "../include/blas_manager.hpp"
#include "../include/bvh_analyzer.h"
#include "../include/cell_visitor.h"
#include "material_registry.h"
#include "mesh_simplifier.hpp"
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
        float radius;
        int materialId;
    } Particle;
    
    Mesh GenerateMesh(Particle* particles, float particleRadius, int particleCount, Bounds volume, float blendWidth);
    void ComputeSurfaceNormals(Mesh* mesh, Particle* particles, float particleRadius, int particleCount, float blendWidth);
    
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
    // Bucket by merge group, not raw material id, so shades that share a group
    // feed one SDF field and blend into a single mesh.
    uint32_t group = (uint32_t)MaterialMergeGroup((int)material_id);
    auto& material_particles = material_particle_indices[group];
    // Check if already exists
    if (std::find(material_particles.begin(), material_particles.end(), particle_index) == material_particles.end()) {
        material_particles.push_back(particle_index);
        is_dirty = true;
    }
}

void Cell::remove_particle_index(uint32_t particle_index, uint32_t material_id) {
    uint32_t group = (uint32_t)MaterialMergeGroup((int)material_id);
    auto material_it = material_particle_indices.find(group);
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

    // Generate one mesh per merge group (the bucket key). Particles of different
    // materials that share a group blend together; their true per-particle
    // materialId is still tagged per-triangle inside generate_mesh_for_group.
    for (const auto& group_entry : material_particle_indices) {
        uint32_t group_id = group_entry.first;
        generate_mesh_for_group(group_id, cluster_particles, blas_manager, simplification_ratio);
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

// --- LOD feature-degradation tunables, expressed in voxels of the current cell ---
// Marching cubes only emits a surface where a grid sample lands inside an SDF
// well, so a feature must span ~1 voxel to be sampled at all. As LOD rises the
// voxel grows and a fixed-size particle's size-in-voxels (rv = radius/voxel)
// shrinks, walking it through three regimes:
//   rv >= kFeatureVisVoxels    : kept at its true radius (fully resolvable).
//   rv in [kCull, kVis)        : lifted to kFeatureVisVoxels voxels so it stays
//                                samplable -- a slight enlargement -- and the
//                                smooth-min union blends it into larger neighbors.
//   rv <  kFeatureCullVoxels   : dropped entirely. If it sat next to a bigger
//                                particle the blend already absorbed it, so no
//                                hole appears; isolated tiny features just vanish.
// Net effect across ascending LODs: slightly larger -> metaball blend -> gone.
static constexpr float kFeatureVisVoxels  = 1.0f;
static constexpr float kFeatureCullVoxels = 0.6f;

// Smooth-min fillet width as a fraction of voxel size. Voxel grows with LOD, so
// the metaball blend strengthens at coarser LODs and is near-sharp at LOD 0.
static constexpr float kBlendVoxels = 0.5f;

void Cell::generate_mesh_for_group(uint32_t group_id, const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio) {
    auto material_it = material_particle_indices.find(group_id);
    if (material_it == material_particle_indices.end() || material_it->second.empty()) {
        return;
    }

    const auto& particle_indices = material_it->second;

    Bounds bounds;
    bounds.center = center;
    bounds.size = Vector3{actual_size, actual_size, actual_size};
    bounds.divisionPow = 4; // Always 16x16x16 resolution
    int gridSize = 1 << bounds.divisionPow;
    float voxel = actual_size / (float)(gridSize - 1);
    float blend_width = kBlendVoxels * voxel;
    float cull_radius = kFeatureCullVoxels * voxel;
    float vis_radius  = kFeatureVisVoxels  * voxel;

    // All particles of this material feed one shared SDF field so differently-
    // sized particles merge naturally (metaballs) under the smooth-min union.
    // Each particle keeps its own radius, tapered for this LOD: sub-grid features
    // are lifted just enough to stay samplable, and features below the cull
    // threshold are dropped. This both fixes the old single-radius hole bug and
    // gives graceful "slightly larger -> blend -> gone" LOD degradation.
    std::vector<Particle> particles;
    particles.reserve(particle_indices.size());
    float max_radius = 0.0f;
    for (uint32_t idx : particle_indices) {
        if (idx >= cluster_particles.size()) continue;
        const StaticParticle& sp = cluster_particles[idx];
        if (sp.radius < cull_radius) continue; // too small to represent at this LOD
        float r_eff = (sp.radius < vis_radius) ? vis_radius : sp.radius;

        Particle surface_particle;
        surface_particle.position = sp.position;
        surface_particle.radius = r_eff;
        surface_particle.materialId = static_cast<int>(sp.materialId);
        particles.push_back(surface_particle);
        if (r_eff > max_radius) max_radius = r_eff;
    }

    if (particles.empty()) {
        return;
    }

    // max_radius is the reference radius for the SDF's spatial-hash search reach.
    Mesh mesh = GenerateMesh(particles.data(), max_radius, static_cast<int>(particles.size()),
                             bounds, blend_width);

    // Decimate to a low-poly proxy when requested. Boundary vertices on this
    // cell's face planes are locked so seams with same-level neighbors stay
    // watertight.
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
            // per-cell shading seams; reapply the cross-cell-continuous SDF
            // gradient (same blend width) so the proxy shades like the dense mesh.
            ComputeSurfaceNormals(&simplified, particles.data(), max_radius,
                                  static_cast<int>(particles.size()), blend_width);
            UnloadMesh(mesh);
            mesh = simplified;
        } else {
            UnloadMesh(simplified); // simplification produced nothing usable; keep dense mesh
        }
    }

    if (mesh.vertexCount > 0) {
        // Store the mesh
        material_meshes[group_id] = mesh;

        UploadMesh(&material_meshes[group_id], false);

        // Register mesh with BLAS manager for ray tracing
        try {
            std::vector<TriEx> triangle_normals;
            std::vector<Tri> triangles = convert_mesh_to_triangles(mesh, &triangle_normals);
            printf("Converting mesh to %zu triangles for BLAS registration\\n", triangles.size());

            // Tag each triangle with the material of the nearest particle to its centroid.
            // One mesh may carry multiple materials once meshing is regrouped, so resolve
            // per-triangle rather than assuming a single material for the whole mesh.
            for (size_t t = 0; t < triangle_normals.size() && t < triangles.size(); ++t) {
                const float3& c = triangles[t].centroid;
                // Seed the default from a REAL particle materialId (never the
                // group id, which is the bucket key). particles is non-empty
                // here, so the nearest-particle loop below always assigns a real
                // materialId anyway; this seed is just a belt-and-braces guard
                // so the group id can never leak onto a triangle's material.
                int best = particles[0].materialId;
                float bestD = 3.4e38f;
                for (const Particle& p : particles) {
                    float dx = c.x - p.position.x, dy = c.y - p.position.y, dz = c.z - p.position.z;
                    float d = dx*dx + dy*dy + dz*dz;
                    if (d < bestD) { bestD = d; best = p.materialId; }
                }
                triangle_normals[t].materialId = best;
            }

            if (!triangles.empty() && triangles.size() > 0) {
                printf("Registering %zu triangles with BLAS manager...\\n", triangles.size());
                material_blas[group_id] = blas_manager.register_triangles(triangles, triangle_normals);
                printf("Successfully registered mesh with BLAS manager, handle %u\\n", material_blas[group_id]);

                // Also register with BVH analyzer for analysis
                BVH* bvh = blas_manager.get_bvh(material_blas[group_id]);
                BvhMesh* mesh_ptr = blas_manager.get_mesh(material_blas[group_id]);
                if (bvh && mesh_ptr) {
                    std::string analysis_name = "Cell(" + std::to_string((int)coordinates.x) + "," + 
                                               std::to_string((int)coordinates.y) + "," + 
                                               std::to_string((int)coordinates.z) + ")_Mat" + 
                                               std::to_string(group_id) + "_" +
                                               std::to_string(triangles.size()) + "tris";
                    
                    BVHReportManager::RegisterBVH(analysis_name, bvh, mesh_ptr);
                    // Immediately update analysis for this BLAS
                    BVHReportManager::UpdateAnalysis(analysis_name);
                }
            } else {
                material_blas[group_id] = 0;
                printf("No valid triangles to register with BLAS manager\\n");
            }
        } catch (const std::exception& e) {
            printf("Error registering mesh with BLAS manager: %s\\n", e.what());
            material_blas[group_id] = 0;
        } catch (...) {
            printf("Unknown error registering mesh with BLAS manager\\n");
            material_blas[group_id] = 0;
        }
        
        printf("Generated mesh for cell (%.0f,%.0f,%.0f) group %u size %.1f: %d vertices, %d triangles, BLAS handle %u\n",
               coordinates.x, coordinates.y, coordinates.z, group_id, actual_size,
               mesh.vertexCount, mesh.triangleCount, material_blas[group_id]);
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