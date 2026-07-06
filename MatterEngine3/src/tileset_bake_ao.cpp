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
    // Material SSBO: minimal stub (all zeros) so getMaterialProperties in
    // bvh_tlas_common.glsl doesn't read out-of-bounds. The AO pass uses
    // shadowQuery for AO rays and intersectScene only for the primary hit
    // normal; material colour is not consumed by the caller.
    // -----------------------------------------------------------------------
    const int kMinMats = 64;
    std::vector<float> packed(kMinMats * 12, 0.0f);
    GLuint ssboMat = 0;
    glGenBuffers(1, &ssboMat);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboMat);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 (GLsizeiptr)(packed.size() * sizeof(float)),
                 packed.data(), GL_STATIC_DRAW);
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
    // Set materialCount = 0 so getMaterialProperties always returns the default
    // material (flatShading = true). This makes intersectScene always use the
    // face normal (cross(e1,e2)) rather than the stored per-vertex normals, which
    // may be zero for procedurally generated base-field triangles.
    set_i("materialCount",    0);

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

} // namespace tileset
