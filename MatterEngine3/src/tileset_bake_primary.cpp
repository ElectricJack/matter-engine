// tileset_bake_primary.cpp — see header.

#include "tileset_bake_primary.h"
#include "tileset_layout.h"  // kTorusN

#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"

#include "raylib.h"
#include "external/glad.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tileset {

// Fabricate a raylib Shader wrapper around a raw compute program id so we can
// use BLASManager::bind_to_shader / TLASManager::bind_to_shader (which expect
// a Shader by value). raylib::Shader is a POD { id, locs* }. We don't own any
// location array; bind_to_shader queries locs by name via GetShaderLocation on
// its own path.
static Shader wrap_program(GLuint program) {
    Shader sh{};
    sh.id = program;
    sh.locs = nullptr;
    return sh;
}

bool bake_primary(GLuint program,
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
    std::vector<float> packed(mats.size() * 12);
    for (size_t i = 0; i < mats.size(); ++i) {
        const MaterialDef& m = mats[i];
        packed[i*12 + 0]  = m.albedo[0];
        packed[i*12 + 1]  = m.albedo[1];
        packed[i*12 + 2]  = m.albedo[2];
        packed[i*12 + 3]  = m.roughness;
        packed[i*12 + 4]  = m.metallic;
        packed[i*12 + 5]  = m.emission;
        packed[i*12 + 6]  = m.translucency;
        packed[i*12 + 7]  = m.ior;
        packed[i*12 + 8]  = (float)m.flatShading;
        packed[i*12 + 9]  = (float)m.mergeGroup;
        packed[i*12 + 10] = (float)m.meshingAlgorithm;
        packed[i*12 + 11] = 0.0f;
    }
    GLuint ssboMat = 0;
    glGenBuffers(1, &ssboMat);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboMat);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(packed.size() * 4), packed.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, ssboMat);

    // -----------------------------------------------------------------------
    // BLAS/TLAS bindings + scalar uniforms.
    //
    // bind_to_shader uploads integer uniforms (counts) correctly for compute
    // shaders. However, SetShaderValueTexture is a no-op under GL 4.3+ (the
    // viewer compile flag), so we manually bind each sampler2D texture to an
    // explicit texture unit and set the corresponding uniform ourselves.
    // -----------------------------------------------------------------------
    Shader sh = wrap_program(program);
    blas.bind_to_shader(sh);
    tlas.bind_to_shader(sh, blas);

    // Manually bind BLAS + TLAS textures to known units.
    // bvh_tlas_common.glsl expects sampler2D uniforms:
    //   trianglesTexture, blasNodesTexture, tlasNodesTexture, instancesTexture
    // We also need imposterColorVolume + imposterNormalVolume slots (3D samplers
    // used in intersectScene); bind dummy IDs of 0 (safe — never sampled when
    // no imposter instance is hit).
    struct TexBind { const char* name; GLuint id; GLenum target; };
    const TexBind bindings[] = {
        { "trianglesTexture",    blas.triangles_texture_id(),   GL_TEXTURE_2D },
        { "blasNodesTexture",    blas.blas_nodes_texture_id(),  GL_TEXTURE_2D },
        { "tlasNodesTexture",    tlas.tlas_nodes_texture_id(),  GL_TEXTURE_2D },
        { "instancesTexture",    tlas.instances_texture_id(),   GL_TEXTURE_2D },
    };
    for (int unit = 0; unit < 4; ++unit) {
        GLint loc = glGetUniformLocation(program, bindings[unit].name);
        if (loc < 0) continue;
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(bindings[unit].target, bindings[unit].id);
        glUniform1i(loc, unit);
    }
    // Imposter volumes unused in this pass; bind texture-unit 4 to unit 4 and 5
    // but leave them as default (0) — intersectScene only reads them when
    // inst.isImposter == true, which doesn't happen in our base+pebble fixture.
    {
        GLint cLoc = glGetUniformLocation(program, "imposterColorVolume");
        GLint nLoc = glGetUniformLocation(program, "imposterNormalVolume");
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_3D, 0);
        if (cLoc >= 0) glUniform1i(cLoc, 4);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_3D, 0);
        if (nLoc >= 0) glUniform1i(nLoc, 5);
        glActiveTexture(GL_TEXTURE0);
    }

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
