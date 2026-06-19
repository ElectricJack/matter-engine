#include "marching_cubes_algorithm.h"
#include "mesh_build_utils.h"
#include "mesh_simplifier.hpp"   // simplify_mesh, SimplifyOptions
#include <cstdio>
#include <utility>               // std::move
extern "C" {
#include "spatial_hash.h"        // sh_query_radius_nearest
}

GroupMeshResult MarchingCubesAlgorithm::generate(const MeshContext& ctx) const {
    GroupMeshResult result;
    result.group_id = ctx.group_id;

    if (ctx.particles.empty()) return result;

    Particle* particles = const_cast<Particle*>(ctx.particles.data());
    int particleCount = (int)ctx.particles.size();
    Particle* clip = const_cast<Particle*>(ctx.clip);
    Particle* carve = const_cast<Particle*>(ctx.carve);

    Mesh mesh = GenerateMeshWithScratch(ctx.scratch, particles, ctx.max_radius, particleCount,
                             ctx.bounds, ctx.blend_width, clip, ctx.clip_count,
                             carve, ctx.carve_count, ctx.carve_blend);

    if (ctx.simplification_ratio < 1.0f && mesh.vertexCount > 0 && mesh.triangleCount > 0) {
        SimplifyOptions so;
        so.target_ratio = ctx.simplification_ratio;
        so.lock_boundary = true;
        CellBounds cb = ctx.cell_bounds;
        Mesh simplified = simplify_mesh(mesh, so, &cb);
        if (simplified.vertexCount > 0 && simplified.triangleCount > 0) {
            ComputeSurfaceNormalsWithScratch(ctx.scratch, &simplified, particles, ctx.max_radius,
                                  particleCount, ctx.blend_width, clip, ctx.clip_count,
                                  carve, ctx.carve_count, ctx.carve_blend);
            unload_cpu_mesh(mesh);
            mesh = simplified;
        } else {
            unload_cpu_mesh(simplified);
        }
    }

    if (mesh.vertexCount <= 0) return result;

    std::vector<TriEx> triangle_normals;
    std::vector<Tri> triangles = convert_mesh_to_triangles(mesh, &triangle_normals);

    SpatialHash* tri_hash = SurfaceScratchHash(ctx.scratch);
    float tri_search = ctx.max_radius * 2.5f + ctx.blend_width * 4.0f;
    for (size_t t = 0; t < triangle_normals.size() && t < triangles.size(); ++t) {
        const float3& c = triangles[t].centroid;
        int bestIdx = 0;
        Particle* nearest = NULL;
        int nfound = tri_hash
            ? sh_query_radius_nearest(tri_hash, c.x, c.y, c.z, tri_search, (void**)&nearest, 1)
            : 0;
        if (nfound > 0 && nearest) {
            bestIdx = (int)(nearest - particles);
        }
        triangle_normals[t].materialId = particles[bestIdx].materialId;
        triangle_normals[t].tint = ctx.particle_tints[bestIdx];
    }

    result.mesh = mesh;
    result.triangles = std::move(triangles);
    result.triangle_normals = std::move(triangle_normals);
    return result;
}
