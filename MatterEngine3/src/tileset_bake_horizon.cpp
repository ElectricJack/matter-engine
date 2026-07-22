// tileset_bake_horizon.cpp — see header.

#include "tileset_bake_horizon.h"

#include "raylib.h"
#include "external/glad.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Confirm the header's uint32_t aliasing is safe on this platform.
static_assert(sizeof(GLuint) == sizeof(uint32_t),
              "GLuint != uint32_t: tileset_bake_horizon.h API is mismatched");

namespace tileset {

bool bake_horizon(uint32_t program,
                  const std::vector<uint16_t>& height_r16,
                  int full_w, int full_h,
                  int texels_per_meter,
                  float height_min, float height_max,
                  std::vector<uint8_t>& horizon_a_out,
                  std::vector<uint8_t>& horizon_b_out,
                  int& horizon_w_out, int& horizon_h_out,
                  std::string& err)
{
    if (program == 0) { err = "bake_horizon: null program"; return false; }
    if (full_w <= 0 || full_h <= 0 || texels_per_meter <= 0) {
        err = "bake_horizon: invalid dimensions"; return false;
    }
    if (height_r16.size() != (size_t)full_w * (size_t)full_h) {
        err = "bake_horizon: height buffer size mismatch"; return false;
    }

    // Quarter-resolution output dims. Integer division: callers should ensure
    // full_w/full_h are multiples of 4 (true for all standard tile configs —
    // texels_per_meter is conventionally a multiple of 4); a non-exact
    // division silently truncates up to 3 texels of coverage at the far edge.
    const int qw = full_w / 4;
    const int qh = full_h / 4;
    if (qw <= 0 || qh <= 0) { err = "bake_horizon: quarter-res dims collapse to 0"; return false; }
    horizon_w_out = qw;
    horizon_h_out = qh;

    // -----------------------------------------------------------------------
    // Upload full-res height as a plain R16 readonly image (re-uploaded from
    // the CPU array bake_primary already returned — no BVH needed here).
    // -----------------------------------------------------------------------
    GLuint texHeight = 0;
    glGenTextures(1, &texHeight);
    glBindTexture(GL_TEXTURE_2D, texHeight);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R16, full_w, full_h);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, full_w, full_h,
                    GL_RED, GL_UNSIGNED_SHORT, height_r16.data());

    // -----------------------------------------------------------------------
    // Create quarter-res RGBA8 output images.
    // -----------------------------------------------------------------------
    GLuint texA = 0, texB = 0;
    glGenTextures(1, &texA);
    glBindTexture(GL_TEXTURE_2D, texA);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, qw, qh);
    glGenTextures(1, &texB);
    glBindTexture(GL_TEXTURE_2D, texB);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, qw, qh);

    glUseProgram(program);
    glBindImageTexture(5, texA,      0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(6, texB,      0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(7, texHeight, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R16);

    auto set_i = [&](const char* n, int v) {
        GLint l = glGetUniformLocation(program, n);
        if (l >= 0) glUniform1i(l, v);
    };
    auto set_f = [&](const char* n, float v) {
        GLint l = glGetUniformLocation(program, n);
        if (l >= 0) glUniform1f(l, v);
    };
    set_i("atlasW",         full_w);
    set_i("atlasH",         full_h);
    set_i("horizonW",       qw);
    set_i("horizonH",       qh);
    set_i("texelsPerMeter", texels_per_meter);
    set_f("heightMin",      height_min);
    set_f("heightMax",      height_max);

    // -----------------------------------------------------------------------
    // Dispatch: one thread per quarter-res texel.
    // -----------------------------------------------------------------------
    glDispatchCompute((qw + 7) / 8, (qh + 7) / 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    // -----------------------------------------------------------------------
    // Readback.
    // -----------------------------------------------------------------------
    horizon_a_out.assign((size_t)qw * qh * 4, 0);
    horizon_b_out.assign((size_t)qw * qh * 4, 0);
    glBindTexture(GL_TEXTURE_2D, texA);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, horizon_a_out.data());
    glBindTexture(GL_TEXTURE_2D, texB);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, horizon_b_out.data());

    {
        GLenum gl_err = glGetError();
        if (gl_err != GL_NO_ERROR) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "0x%04X", (unsigned)gl_err);
            err = std::string("bake_horizon: glGetTexImage returned GL error ") + buf;
            glDeleteTextures(1, &texA);
            glDeleteTextures(1, &texB);
            glDeleteTextures(1, &texHeight);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(0);
            return false;
        }
    }

    glDeleteTextures(1, &texA);
    glDeleteTextures(1, &texB);
    glDeleteTextures(1, &texHeight);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    return true;
}

} // namespace tileset
