// probe_texture.cpp — Upload ProbeVolume float data as pair of RGBA8 GL_TEXTURE_3D.
// Uses raylib's bundled glad loader; function pointers are live after InitWindow.
// Do NOT include raylib.h before glad.h in this TU — raylib.h pulls in rlgl.h which
// includes glad.h under an RLGL_IMPLEMENTATION guard, causing redefinition warnings.
// We only need the raw GL declarations here.
#include "async_bake.h"
#include "probe_texture.h"

// Pull in glad declarations (header-only pointer declarations; the definitions were
// loaded by raylib's InitWindow via gladLoadGL).
#include "external/glad.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace viewer {

static inline uint8_t to_u8(float x) {
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    return (uint8_t)(x * 255.0f + 0.5f);
}

ProbeTextures upload_probe_textures(const probe_volume::ProbeVolume& v) {
    matter_async::assert_gl_thread("upload_probe_textures");
    ProbeTextures out;
    if (!v.valid()) return out;

    const int nx = v.grid.nx;
    const int ny = v.grid.ny;
    const int nz = v.grid.nz;
    const size_t cells = (size_t)nx * ny * nz;

    // Texture A: ambient (RGBA8)
    //   R,G,B = clamp(ambient_rgb / kProbeAmbientScale, 0, 1) * 255
    //   A     = sun_vis * 255
    std::vector<uint8_t> texA(cells * 4);
    for (size_t i = 0; i < cells; ++i) {
        const float* a = v.ambient.data() + i * 4;
        texA[i*4+0] = to_u8(a[0] / kProbeAmbientScale);
        texA[i*4+1] = to_u8(a[1] / kProbeAmbientScale);
        texA[i*4+2] = to_u8(a[2] / kProbeAmbientScale);
        texA[i*4+3] = to_u8(a[3]);   // sun_vis already in [0,1]
    }

    // Texture B: dominant (RGBA8)
    //   R,G,B = dir * 0.5 + 0.5  (maps [-1,1] -> [0,1])
    //   A     = clamp(intensity / kProbeAmbientScale, 0, 1) * 255
    std::vector<uint8_t> texB(cells * 4);
    for (size_t i = 0; i < cells; ++i) {
        const float* d = v.dominant.data() + i * 4;
        texB[i*4+0] = to_u8(d[0] * 0.5f + 0.5f);
        texB[i*4+1] = to_u8(d[1] * 0.5f + 0.5f);
        texB[i*4+2] = to_u8(d[2] * 0.5f + 0.5f);
        texB[i*4+3] = to_u8(d[3] / kProbeAmbientScale);   // intensity
    }

    unsigned int ids[2] = {0, 0};
    glGenTextures(2, ids);

    auto upload3D = [&](unsigned int id, const uint8_t* data) {
        glBindTexture(GL_TEXTURE_3D, id);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8,
                     nx, ny, nz,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glBindTexture(GL_TEXTURE_3D, 0);
    };

    upload3D(ids[0], texA.data());
    upload3D(ids[1], texB.data());

    out.tex_ambient  = ids[0];
    out.tex_dominant = ids[1];
    out.grid = v.grid;
    return out;
}

void release_probe_textures(ProbeTextures& t) {
    if (t.tex_ambient || t.tex_dominant) {
        unsigned int ids[2] = { t.tex_ambient, t.tex_dominant };
        glDeleteTextures(2, ids);
    }
    t.tex_ambient  = 0;
    t.tex_dominant = 0;
    t.grid = probe_volume::ProbeGrid{};
}

} // namespace viewer
