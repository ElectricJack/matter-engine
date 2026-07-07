#pragma once
// tileset_provider.h — viewer-side owner of GPU tileset slots.
//
// A slot holds four mipmapped 2D textures (albedo RGB8, normal RG8, ORM RGB8,
// height R16) uploaded from a .gtex atlas produced by run_tileset_phase(opts).
// Up to 4 slots (per spec §"Viewer Consumption").
//
// Life-cycle: LocalProvider::connect() calls load_slot(i, path, err) once per
// tileset root; the renderer calls bind_all_to_shader(program) each frame after
// glUseProgram(); unload_all() runs at shutdown.

#include <string>
#include "gl46.h"

namespace viewer {

struct TilesetSlot {
    GLuint tex_albedo = 0;
    GLuint tex_normal = 0;
    GLuint tex_orm    = 0;
    GLuint tex_height = 0;
    float  tile_size_m       = 0.0f;
    int    texels_per_meter  = 0;
    int    atlas_tiles_x     = 0;
    int    atlas_tiles_y     = 0;
    float  height_min        = 0.0f;
    float  height_max        = 0.0f;
    bool   valid             = false;
};

namespace tileset_provider {

// Max concurrent slots (bound to samplers groundAlbedo[0..3] etc.).
inline constexpr int kMaxSlots = 4;
int max_slots();

// Load a .gtex into slot [0..kMaxSlots). Overwrites (unloading) any existing
// contents. Fails closed on missing file, decode failure, or GL error; the
// error message names the path and reason. Requires an active GL 4.6 context;
// if unavailable, err is set with the GALLIUM_DRIVER=d3d12 hint.
bool load_slot(int slot, const std::string& gtex_path, std::string& err);

// Delete GL textures for slot; sets valid=false.
void unload_slot(int slot);
void unload_all();

// Read accessor; returns a slot with valid=false for out-of-range indices.
const TilesetSlot& get_slot(int slot);

// For each valid slot i, bind:
//   groundAlbedo<i>  at texture unit (10 + i*4 + 0)
//   groundNormal<i>  at (10 + i*4 + 1)
//   groundORM<i>     at (10 + i*4 + 2)
//   groundHeight<i>  at (10 + i*4 + 3)
// Also sets integer uniform tilesetSlot<i>_tileSize_m and tilesetSlot<i>_texelsPerMeter
// so the shader can compute cell coords without another CPU trip.
// Silently skips uniforms the shader did not declare (glGetUniformLocation < 0).
void bind_all_to_shader(GLuint program);

} // namespace tileset_provider
} // namespace viewer
