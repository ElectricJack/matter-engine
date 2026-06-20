#include "oriented_cube_algorithm.h"
#include "material_registry.h"
#include "raylib.h"     // Mesh, RL_MALLOC, Vector3
#include <cmath>
#include <cstdlib>      // getenv, atof
#include <cstdint>

namespace {

uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

// Quantize position to a stable integer grid and hash into a nonzero seed.
uint32_t seed_from_pos(Vector3 p, float voxel) {
    float q = (voxel > 0.0f) ? voxel : 1.0f;
    int xi = (int)floorf(p.x / q);
    int yi = (int)floorf(p.y / q);
    int zi = (int)floorf(p.z / q);
    uint32_t h = hash_u32((uint32_t)xi * 73856093u ^ (uint32_t)yi * 19349663u ^ (uint32_t)zi * 83492791u);
    return h ? h : 1u;
}

float next_unit(uint32_t& s) {  // xorshift32 -> [0,1)
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (s & 0xffffffu) / (float)0x1000000;
}

// Uniform random unit quaternion (Shoemake), blended toward identity by jitter.
float4 seeded_quat(uint32_t seed, float jitter) {
    uint32_t s = seed;
    float u1 = next_unit(s), u2 = next_unit(s), u3 = next_unit(s);
    float s1 = sqrtf(1.0f - u1), s2 = sqrtf(u1);
    const float TWO_PI = 6.28318531f;
    float4 q;
    q.x = s1 * sinf(TWO_PI * u2);
    q.y = s1 * cosf(TWO_PI * u2);
    q.z = s2 * sinf(TWO_PI * u3);
    q.w = s2 * cosf(TWO_PI * u3);
    float j = jitter < 0.0f ? 0.0f : (jitter > 1.0f ? 1.0f : jitter);
    q.x *= j; q.y *= j; q.z *= j; q.w = q.w * j + (1.0f - j); // nlerp toward (0,0,0,1)
    float inv = 1.0f / sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    q.x *= inv; q.y *= inv; q.z *= inv; q.w *= inv;
    return q;
}

Vector3 rotate(float4 q, Vector3 v) {
    Vector3 u = { q.x, q.y, q.z };
    Vector3 t = { u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x };
    t.x += q.w*v.x; t.y += q.w*v.y; t.z += q.w*v.z;
    Vector3 c = { u.y*t.z - u.z*t.y, u.z*t.x - u.x*t.z, u.x*t.y - u.y*t.x };
    return Vector3{ v.x + 2.0f*c.x, v.y + 2.0f*c.y, v.z + 2.0f*c.z };
}

// Local unit-cube faces at half-extent 1: outward normal + 4 CCW corners each.
const float FN[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
const float FV[6][4][3] = {
    {{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1}},     // +X
    {{-1,-1,1},{-1,1,1},{-1,1,-1},{-1,-1,-1}}, // -X
    {{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1}},     // +Y
    {{-1,-1,1},{-1,-1,-1},{1,-1,-1},{1,-1,1}}, // -Y
    {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}},     // +Z
    {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}}, // -Z
};

} // namespace

GroupMeshResult OrientedCubeAlgorithm::generate(const MeshContext& ctx) const {
    GroupMeshResult result;
    result.group_id = ctx.group_id;

    const int n = (int)ctx.particles.size();
    if (n == 0) return result;

    float sizeScale = 0.6f;
    if (const char* e = getenv("MSL_CUBE_SIZE_SCALE")) { float v = (float)atof(e); if (v > 0.0f) sizeScale = v; }
    float jitter = 1.0f;
    if (const char* e = getenv("MSL_CUBE_ROT_JITTER")) { float v = (float)atof(e); if (v >= 0.0f) jitter = v; }

    // 24 verts / 12 tris per cube. mesh.indices is raylib's unsigned short, so the
    // GL preview mesh wraps past n>2730 (24*2730 > 65535) — same limit every raylib
    // Mesh in the engine carries. The raytraced BLAS path uses result.triangles
    // (full float3), so it is unaffected; per-cell group counts stay well under this.
    const int VPC = 24, TPC = 12;
    int vertexCount = n * VPC;
    int triangleCount = n * TPC;

    Mesh mesh = {};
    mesh.vertexCount = vertexCount;
    mesh.triangleCount = triangleCount;
    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    mesh.normals  = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    mesh.colors   = (unsigned char*)RL_MALLOC(vertexCount * 4 * sizeof(unsigned char));
    mesh.indices  = (unsigned short*)RL_MALLOC(triangleCount * 3 * sizeof(unsigned short));

    result.triangles.reserve(triangleCount);
    result.triangle_normals.reserve(triangleCount);

    for (int i = 0; i < n; ++i) {
        const Particle& p = ctx.particles[i];
        float h = p.radius * sizeScale;  // half-edge (edge = 2*radius*sizeScale)
        float4 q = seeded_quat(seed_from_pos(p.position, ctx.voxel), jitter);

        const MaterialDef* md = MaterialRegistryGet(p.materialId);
        float4 tnt = ctx.particle_tints[i];
        float a = tnt.w;
        // Tints aren't guaranteed normalized; clamp before the byte cast so an
        // out-of-range channel can't wrap to garbage in the GL preview color.
        auto to_byte = [](float v) -> unsigned char {
            return (unsigned char)(255.0f * fminf(1.0f, fmaxf(0.0f, v)));
        };
        unsigned char cr = to_byte(md->albedo[0]*(1.0f-a) + tnt.x*a);
        unsigned char cg = to_byte(md->albedo[1]*(1.0f-a) + tnt.y*a);
        unsigned char cb = to_byte(md->albedo[2]*(1.0f-a) + tnt.z*a);

        int vbase = i * VPC;
        for (int f = 0; f < 6; ++f) {
            Vector3 ln = { FN[f][0], FN[f][1], FN[f][2] };
            Vector3 wn = rotate(q, ln); // unit-length in, unit-length out
            float3 fn3 = make_float3(wn.x, wn.y, wn.z);

            Vector3 cw[4];
            for (int k = 0; k < 4; ++k) {
                Vector3 lc = { FV[f][k][0]*h, FV[f][k][1]*h, FV[f][k][2]*h };
                Vector3 r = rotate(q, lc);
                cw[k] = Vector3{ p.position.x + r.x, p.position.y + r.y, p.position.z + r.z };
                int vi = vbase + f*4 + k;
                mesh.vertices[vi*3+0] = cw[k].x; mesh.vertices[vi*3+1] = cw[k].y; mesh.vertices[vi*3+2] = cw[k].z;
                mesh.normals[vi*3+0] = wn.x; mesh.normals[vi*3+1] = wn.y; mesh.normals[vi*3+2] = wn.z;
                mesh.colors[vi*4+0] = cr; mesh.colors[vi*4+1] = cg; mesh.colors[vi*4+2] = cb; mesh.colors[vi*4+3] = 255;
            }

            int tribase = (i*TPC + f*2);
            unsigned short i0 = (unsigned short)(vbase + f*4);
            unsigned short i1 = i0+1, i2 = i0+2, i3 = i0+3;
            mesh.indices[tribase*3+0]=i0; mesh.indices[tribase*3+1]=i1; mesh.indices[tribase*3+2]=i2;
            mesh.indices[tribase*3+3]=i0; mesh.indices[tribase*3+4]=i2; mesh.indices[tribase*3+5]=i3;

            const int tri_idx[2][3] = {{0,1,2},{0,2,3}};
            for (int t = 0; t < 2; ++t) {
                Vector3 va = cw[tri_idx[t][0]], vb = cw[tri_idx[t][1]], vc = cw[tri_idx[t][2]];
                Tri tri;
                tri.vertex0 = make_float3(va.x, va.y, va.z);
                tri.vertex1 = make_float3(vb.x, vb.y, vb.z);
                tri.vertex2 = make_float3(vc.x, vc.y, vc.z);
                tri.centroid = make_float3((va.x+vb.x+vc.x)/3.0f, (va.y+vb.y+vc.y)/3.0f, (va.z+vb.z+vc.z)/3.0f);
                result.triangles.push_back(tri);

                TriEx ex{};
                ex.N0 = ex.N1 = ex.N2 = fn3;
                ex.materialId = p.materialId;
                ex.tint = tnt;
                result.triangle_normals.push_back(ex);
            }
        }
    }

    result.mesh = mesh;
    return result;
}
