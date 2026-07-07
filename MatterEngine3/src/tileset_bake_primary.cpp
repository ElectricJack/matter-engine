// tileset_bake_primary.cpp — see header.

#include "tileset_bake_primary.h"
#include "tileset_layout.h"  // kTorusN
#include "tileset_gl_ctx.h"  // bind_bvh_samplers

#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"

#include "raylib.h"
#include "external/glad.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Confirm the header's uint32_t aliasing is safe on this platform.
static_assert(sizeof(GLuint) == sizeof(uint32_t),
              "GLuint != uint32_t: tileset_bake_primary.h API is mismatched");

namespace tileset {

bool bake_primary(uint32_t program,
                  BLASManager& blas,
                  TLASManager& tlas,
                  const std::vector<MaterialDef>& mats,
                  const TileConfig& cfg,
                  float ray_y,
                  float height_min,
                  float height_max,
                  std::vector<uint8_t>&  albedo_out,
                  std::vector<uint8_t>&  normal_out,
                  std::vector<uint8_t>&  orm_out,
                  std::vector<uint16_t>& height_out,
                  std::string& err)
{
    if (program == 0) { err = "bake_primary: null program"; return false; }
    if (cfg.texels_per_meter <= 0 || cfg.size <= 0.0f) {
        err = "bake_primary: invalid tile config"; return false;
    }
    if (mats.empty()) { err = "bake_primary: empty material table"; return false; }

    const int W = kTorusN * (int)cfg.size * cfg.texels_per_meter;
    const int H = W;

    // -----------------------------------------------------------------------
    // Create output image textures + bind as compute images.
    // -----------------------------------------------------------------------
    auto make_tex = [](GLenum internal_fmt, int w, int h) -> GLuint {
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexStorage2D(GL_TEXTURE_2D, 1, internal_fmt, w, h);
        return id;
    };
    GLuint texA = make_tex(GL_RGBA8, W, H);
    GLuint texN = make_tex(GL_RG8,   W, H);
    GLuint texO = make_tex(GL_RGBA8, W, H);
    GLuint texH = make_tex(GL_R16,   W, H);

    glUseProgram(program);
    glBindImageTexture(0, texA, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, texN, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG8);
    glBindImageTexture(2, texO, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(3, texH, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16);

    // -----------------------------------------------------------------------
    // Material SSBO.
    // -----------------------------------------------------------------------
    // Pack using the canonical MaterialRegistryPackForGPU layout (12 floats):
    //   [0..2] albedo.rgb, [3] roughness, [4] metallic, [5] emission, [6] pad,
    //   [7] translucency, [8] ior, [9] flatShading, [10] mergeGroup, [11] groundTilesetSlot
    std::vector<float> packed(mats.size() * 12);
    for (size_t i = 0; i < mats.size(); ++i) {
        const MaterialDef& m = mats[i];
        float* r = packed.data() + i * 12;
        r[0]  = m.albedo[0]; r[1]  = m.albedo[1]; r[2]  = m.albedo[2];
        r[3]  = m.roughness;
        r[4]  = m.metallic;  r[5]  = m.emission;
        r[6]  = 0.0f; /* pad */
        r[7]  = m.translucency;
        r[8]  = m.ior;
        r[9]  = (float)m.flatShading;
        r[10] = (float)m.mergeGroup;
        r[11] = (float)m.groundTilesetSlot;   // Phase 4: -1 = untextured
    }
    GLuint ssboMat = 0;
    glGenBuffers(1, &ssboMat);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboMat);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(packed.size() * 4), packed.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, ssboMat);

    // Upload materialTable uniform so getMaterialProperties() in materials.glsl
    // reads real values (not zero-initialized defaults).
    {
        GLint loc = glGetUniformLocation(program, "materialTable");
        if (loc >= 0) glUniform1fv(loc, (GLsizei)packed.size(), packed.data());
    }

    // -----------------------------------------------------------------------
    // BLAS/TLAS bindings + scalar uniforms.
    //
    // bind_to_shader is a no-op under GL 4.3+ for samplers; bind_bvh_samplers
    // does the real work (including ensure_gpu_textures_ready) and also sets
    // the count uniforms directly.
    // -----------------------------------------------------------------------
    tileset::bind_bvh_samplers(program, blas, tlas);

    auto set_i = [&](const char* n, int v) {
        GLint l = glGetUniformLocation(program, n);
        if (l >= 0) glUniform1i(l, v);
    };
    auto set_f = [&](const char* n, float v) {
        GLint l = glGetUniformLocation(program, n);
        if (l >= 0) glUniform1f(l, v);
    };
    set_i("atlasW",         W);
    set_i("atlasH",         H);
    set_f("tileSize",       cfg.size);
    set_i("texelsPerMeter", cfg.texels_per_meter);
    set_f("rayY",           ray_y);
    set_f("heightMin",      height_min);
    set_f("heightMax",      height_max);
    // aoEnabled: disable AO computation in the BVH common shader.
    set_i("aoEnabled",       0);
    // materialCount: used by getMaterialProperties in materials.glsl.
    set_i("materialCount",  (int)mats.size());
    // Force TLAS traversal (mode 1) in intersectScene.
    set_i("intersectionMode", 1);
    set_i("debugTriangleTests", 0);

    // -----------------------------------------------------------------------
    // Dispatch + readback.
    // -----------------------------------------------------------------------
    glDispatchCompute((W + 7) / 8, (H + 7) / 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    albedo_out.assign((size_t)W * H * 3, 0);
    normal_out.assign((size_t)W * H * 2, 0);
    orm_out.assign((size_t)W * H * 3, 0);
    height_out.assign((size_t)W * H, 0);

    glBindTexture(GL_TEXTURE_2D, texA);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB,  GL_UNSIGNED_BYTE,  albedo_out.data());
    glBindTexture(GL_TEXTURE_2D, texN);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RG,   GL_UNSIGNED_BYTE,  normal_out.data());
    glBindTexture(GL_TEXTURE_2D, texO);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB,  GL_UNSIGNED_BYTE,  orm_out.data());
    glBindTexture(GL_TEXTURE_2D, texH);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED,  GL_UNSIGNED_SHORT, height_out.data());

    // GL error sweep: surface any driver error from the readbacks as a
    // structured bake error rather than silently returning zeroed output.
    {
        GLenum gl_err = glGetError();
        if (gl_err != GL_NO_ERROR) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "0x%04X", (unsigned)gl_err);
            err = std::string("bake_primary: glGetTexImage returned GL error ") + buf;
            glDeleteTextures(1, &texA);
            glDeleteTextures(1, &texN);
            glDeleteTextures(1, &texO);
            glDeleteTextures(1, &texH);
            glDeleteBuffers(1, &ssboMat);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(0);
            return false;
        }
    }

    glDeleteTextures(1, &texA);
    glDeleteTextures(1, &texN);
    glDeleteTextures(1, &texO);
    glDeleteTextures(1, &texH);
    glDeleteBuffers(1, &ssboMat);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    return true;
}

} // namespace tileset
