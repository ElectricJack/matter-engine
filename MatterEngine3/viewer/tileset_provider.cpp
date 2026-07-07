// tileset_provider.cpp — see header.

#include "tileset_provider.h"
#include "tileset_gtex.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace viewer {
namespace tileset_provider {

namespace {

TilesetSlot g_slots[kMaxSlots];
const TilesetSlot k_empty{};

GLuint upload_tex_2d(const void* data, int w, int h, GLenum internal, GLenum format, GLenum type) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // NORMAL_RG8 requires an alignment fallback; force 1 to be safe for all pixel sizes.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    // Check for per-texture GL errors; a driver rejection here must not leak a partial texture.
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        glDeleteTextures(1, &id);
        return 0;
    }
    return id;
}

void set_sampler(GLuint program, const char* name, int unit) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1i(loc, unit);
}

void set_float_uniform(GLuint program, const char* name, float v) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1f(loc, v);
}

void set_int_uniform(GLuint program, const char* name, int v) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1i(loc, v);
}

} // anon

int max_slots() { return kMaxSlots; }

const TilesetSlot& get_slot(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return k_empty;
    return g_slots[slot];
}

void unload_slot(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return;
    TilesetSlot& s = g_slots[slot];
    if (s.tex_albedo) glDeleteTextures(1, &s.tex_albedo);
    if (s.tex_normal) glDeleteTextures(1, &s.tex_normal);
    if (s.tex_orm)    glDeleteTextures(1, &s.tex_orm);
    if (s.tex_height) glDeleteTextures(1, &s.tex_height);
    s = TilesetSlot{};
}

void unload_all() {
    for (int i = 0; i < kMaxSlots; ++i) unload_slot(i);
}

bool load_slot(int slot, const std::string& gtex_path, std::string& err) {
    if (slot < 0 || slot >= kMaxSlots) {
        err = "tileset_provider::load_slot: slot " + std::to_string(slot) +
              " out of range [0," + std::to_string(kMaxSlots) + ")";
        return false;
    }
    // Fail-closed on missing GL context: any glGetError seed here is meaningless
    // without one, so we require the caller to have InitWindow'd. The .gtex reader
    // is CPU-only and gates I/O errors itself.
    tileset::GTexHeader hdr;
    std::vector<uint8_t>  a, n2, o;
    std::vector<uint16_t> h;
    if (!tileset::load_gtex(gtex_path, hdr, a, n2, o, h, err)) {
        // load_gtex sets a structured err; augment with the WSLg hint (safe even if
        // the failure is not GL-related — a stray hint costs nothing).
        err += " (if you see a GL error, set GALLIUM_DRIVER=d3d12 on WSLg)";
        return false;
    }
    // tile_size_m is float in the header but v1 spec uses integer meter sizes; the cast is deliberate.
    const int W = hdr.atlas_tiles_x * (int)hdr.tile_size_m * hdr.texels_per_meter;
    const int H = hdr.atlas_tiles_y * (int)hdr.tile_size_m * hdr.texels_per_meter;
    if (W <= 0 || H <= 0) {
        err = "tileset_provider::load_slot: bad atlas dims (" + std::to_string(W) +
              "x" + std::to_string(H) + ") from " + gtex_path;
        return false;
    }
    if ((int)a.size()  != W*H*3 || (int)n2.size() != W*H*2 ||
        (int)o.size()  != W*H*3 || (int)h.size()  != W*H) {
        err = "tileset_provider::load_slot: channel-size mismatch for " + gtex_path;
        return false;
    }

    unload_slot(slot);  // replace any existing contents

    TilesetSlot& s = g_slots[slot];
    s.tex_albedo = upload_tex_2d(a.data(),  W, H, GL_RGB8,  GL_RGB,          GL_UNSIGNED_BYTE);
    if (!s.tex_albedo) {
        err = "tileset_provider::load_slot: GL upload failed for albedo in " + gtex_path +
              " (set GALLIUM_DRIVER=d3d12 on WSLg)";
        return false;
    }

    s.tex_normal = upload_tex_2d(n2.data(), W, H, GL_RG8,   GL_RG,           GL_UNSIGNED_BYTE);
    if (!s.tex_normal) {
        glDeleteTextures(1, &s.tex_albedo);
        err = "tileset_provider::load_slot: GL upload failed for normal in " + gtex_path +
              " (set GALLIUM_DRIVER=d3d12 on WSLg)";
        return false;
    }

    s.tex_orm = upload_tex_2d(o.data(),  W, H, GL_RGB8,  GL_RGB,          GL_UNSIGNED_BYTE);
    if (!s.tex_orm) {
        glDeleteTextures(1, &s.tex_albedo);
        glDeleteTextures(1, &s.tex_normal);
        err = "tileset_provider::load_slot: GL upload failed for orm in " + gtex_path +
              " (set GALLIUM_DRIVER=d3d12 on WSLg)";
        return false;
    }

    s.tex_height = upload_tex_2d(h.data(),  W, H, GL_R16,   GL_RED,          GL_UNSIGNED_SHORT);
    if (!s.tex_height) {
        glDeleteTextures(1, &s.tex_albedo);
        glDeleteTextures(1, &s.tex_normal);
        glDeleteTextures(1, &s.tex_orm);
        err = "tileset_provider::load_slot: GL upload failed for height in " + gtex_path +
              " (set GALLIUM_DRIVER=d3d12 on WSLg)";
        return false;
    }

    s.tile_size_m      = hdr.tile_size_m;
    s.texels_per_meter = hdr.texels_per_meter;
    s.atlas_tiles_x    = hdr.atlas_tiles_x;
    s.atlas_tiles_y    = hdr.atlas_tiles_y;
    s.height_min       = hdr.height_min;
    s.height_max       = hdr.height_max;
    s.valid            = true;
    return true;
}

void bind_all_to_shader(GLuint program) {
    // Base unit = 10, four samplers per slot; 4 slots => units 10..25.
    for (int i = 0; i < kMaxSlots; ++i) {
        const TilesetSlot& s = g_slots[i];
        if (!s.valid) continue;
        const int base = 10 + i * 4;
        char name[64];
        std::snprintf(name, sizeof name, "groundAlbedo%d", i);
        glActiveTexture(GL_TEXTURE0 + base + 0);
        glBindTexture(GL_TEXTURE_2D, s.tex_albedo);
        set_sampler(program, name, base + 0);

        std::snprintf(name, sizeof name, "groundNormal%d", i);
        glActiveTexture(GL_TEXTURE0 + base + 1);
        glBindTexture(GL_TEXTURE_2D, s.tex_normal);
        set_sampler(program, name, base + 1);

        std::snprintf(name, sizeof name, "groundORM%d", i);
        glActiveTexture(GL_TEXTURE0 + base + 2);
        glBindTexture(GL_TEXTURE_2D, s.tex_orm);
        set_sampler(program, name, base + 2);

        std::snprintf(name, sizeof name, "groundHeight%d", i);
        glActiveTexture(GL_TEXTURE0 + base + 3);
        glBindTexture(GL_TEXTURE_2D, s.tex_height);
        set_sampler(program, name, base + 3);

        std::snprintf(name, sizeof name, "tilesetSlot%d_tileSize_m", i);
        set_float_uniform(program, name, s.tile_size_m);
        std::snprintf(name, sizeof name, "tilesetSlot%d_texelsPerMeter", i);
        set_int_uniform(program, name, s.texels_per_meter);
    }
    // Restore TEXTURE0 as the "active" slot so downstream code doesn't inherit
    // an unrelated unit selection.
    glActiveTexture(GL_TEXTURE0);
}

} // namespace tileset_provider
} // namespace viewer
