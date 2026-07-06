// tileset_bake_ao.cpp — see header.

#include "tileset_bake_ao.h"
#include "tileset_layout.h"  // kTorusN
#include "tileset_gl_ctx.h"  // bind_bvh_samplers

#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include "raylib.h"
#include "external/glad.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tileset {

// ---------------------------------------------------------------------------
// Internal implementation — accepts a pre-packed material float buffer and
// an explicit materialCount. All public overloads delegate here.
// ---------------------------------------------------------------------------
static bool bake_ao_impl(GLuint program,
                         BLASManager& blas,
                         TLASManager& tlas,
                         const std::vector<float>& packed_mats,
                         int material_count,
                         const TileConfig& cfg,
                         float ray_y,
                         float height_min,
                         float height_max,
                         uint32_t seed,
                         std::vector<uint8_t>& ao_out,
                         std::string& err)
{
    if (program == 0) { err = "bake_ao: null program"; return false; }
    if (cfg.texels_per_meter <= 0 || cfg.size <= 0.0f) {
        err = "bake_ao: invalid tile config"; return false;
    }

    const int W = kTorusN * (int)cfg.size * cfg.texels_per_meter;
    const int H = W;

    // -----------------------------------------------------------------------
    // Create single-channel R8 output image texture.
    // -----------------------------------------------------------------------
    GLuint texAo = 0;
    glGenTextures(1, &texAo);
    glBindTexture(GL_TEXTURE_2D, texAo);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, W, H);

    glUseProgram(program);
    glBindImageTexture(4, texAo, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);

    // -----------------------------------------------------------------------
    // Material SSBO: upload packed_mats (may be a zero-stub or real table).
    // -----------------------------------------------------------------------
    GLuint ssboMat = 0;
    glGenBuffers(1, &ssboMat);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboMat);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 (GLsizeiptr)(packed_mats.size() * sizeof(float)),
                 packed_mats.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, ssboMat);

    // -----------------------------------------------------------------------
    // BLAS/TLAS bindings (calls ensure_gpu_textures_ready internally).
    // -----------------------------------------------------------------------
    tileset::bind_bvh_samplers(program, blas, tlas);

    // -----------------------------------------------------------------------
    // Scalar uniforms.
    // -----------------------------------------------------------------------
    auto set_i = [&](const char* n, int v) {
        GLint l = glGetUniformLocation(program, n);
        if (l >= 0) glUniform1i(l, v);
    };
    auto set_u = [&](const char* n, uint32_t v) {
        GLint l = glGetUniformLocation(program, n);
        if (l >= 0) glUniform1ui(l, v);
    };
    auto set_f = [&](const char* n, float v) {
        GLint l = glGetUniformLocation(program, n);
        if (l >= 0) glUniform1f(l, v);
    };

    set_i("atlasW",           W);
    set_i("atlasH",           H);
    set_i("texelsPerMeter",   cfg.texels_per_meter);
    set_f("rayY",             ray_y);
    set_f("heightMin",        height_min);
    set_f("heightMax",        height_max);
    set_f("maxRayDist",       cfg.edge_strip_width);
    set_i("aoSamples",        64);
    set_u("seed",             seed);
    // Force TLAS traversal.
    set_i("intersectionMode", 1);
    // Disable baked-AO fetch in intersectScene (primary hit only needs normal).
    set_i("aoEnabled",        0);
    set_i("debugTriangleTests", 0);
    set_i("materialCount",    material_count);

    // -----------------------------------------------------------------------
    // Dispatch.
    // -----------------------------------------------------------------------
    glDispatchCompute((W + 7) / 8, (H + 7) / 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    // -----------------------------------------------------------------------
    // Readback: R8 → one byte per texel.
    // -----------------------------------------------------------------------
    ao_out.assign((size_t)W * H, 0);
    glBindTexture(GL_TEXTURE_2D, texAo);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, ao_out.data());

    // GL error sweep: surface driver errors as structured bake errors.
    {
        GLenum gl_err = glGetError();
        if (gl_err != GL_NO_ERROR) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "0x%04X", (unsigned)gl_err);
            err = std::string("bake_ao: glGetTexImage returned GL error ") + buf;
            glDeleteTextures(1, &texAo);
            glDeleteBuffers(1, &ssboMat);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(0);
            return false;
        }
    }

    glDeleteTextures(1, &texAo);
    glDeleteBuffers(1, &ssboMat);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    return true;
}

// ---------------------------------------------------------------------------
// Overload 1: no material table — forces materialCount=0 (safe for base-only
// TLAS where vertex normals are zero; face normals are used throughout).
// ---------------------------------------------------------------------------
bool bake_ao(GLuint program,
             BLASManager& blas,
             TLASManager& tlas,
             const TileConfig& cfg,
             float ray_y,
             float height_min,
             float height_max,
             uint32_t seed,
             std::vector<uint8_t>& ao_out,
             std::string& err)
{
    // Zero-stub: 64 entries * 12 floats each, all zero.
    // getMaterialProperties in the shader returns the hardcoded default
    // (flatShading=true) when materialCount=0, so this is safe.
    const int kMinMats = 64;
    std::vector<float> stub(kMinMats * 12, 0.0f);
    return bake_ao_impl(program, blas, tlas, stub, /*material_count=*/0,
                        cfg, ray_y, height_min, height_max, seed, ao_out, err);
}

// ---------------------------------------------------------------------------
// Overload 2: real material table — the caller must ensure all materials
// whose triangles carry zero vertex normals have flatShading=true.
// The orchestrator (bake_tileset_gpu) forces the base material to
// flatShading=true before calling this overload.
// ---------------------------------------------------------------------------
bool bake_ao(GLuint program,
             BLASManager& blas,
             TLASManager& tlas,
             const std::vector<MaterialDef>& mats,
             const TileConfig& cfg,
             float ray_y,
             float height_min,
             float height_max,
             uint32_t seed,
             std::vector<uint8_t>& ao_out,
             std::string& err)
{
    // Pack mats[] into the same float layout as bake_primary / MaterialRegistryPackForGPU:
    // 12 floats per entry: [albedo.r, albedo.g, albedo.b, roughness,
    //                       metallic, emission, translucency, pad,
    //                       ior, flatShading, mergeGroup, pad]
    const int n = (int)mats.size();
    std::vector<float> packed((size_t)n * 12, 0.0f);
    for (int i = 0; i < n; ++i) {
        float* r = packed.data() + (size_t)i * 12;
        const MaterialDef& m = mats[i];
        r[0]  = m.albedo[0]; r[1]  = m.albedo[1]; r[2]  = m.albedo[2];
        r[3]  = m.roughness;
        r[4]  = m.metallic;  r[5]  = m.emission;
        r[6]  = m.translucency; r[7] = 0.0f;
        r[8]  = m.ior;
        r[9]  = (float)m.flatShading;
        r[10] = (float)m.mergeGroup;
        r[11] = 0.0f;
    }
    return bake_ao_impl(program, blas, tlas, packed, n,
                        cfg, ray_y, height_min, height_max, seed, ao_out, err);
}

} // namespace tileset
