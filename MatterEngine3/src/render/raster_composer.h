#ifndef VIEWER_RASTER_COMPOSER_H
#define VIEWER_RASTER_COMPOSER_H

#include "raylib.h"
#include "sector_resolver.h"
#include "part_store.h"
#include "raster_mesh.h"
#include "probe_texture.h"
#include "world_lights.h"

#include <string>
#include <vector>

// Forward declaration for GPU-driven path.
namespace viewer { class GpuCuller; }

namespace viewer {

// Batch key: (part_hash, cluster_index, lod_level).
// cluster_index == UINT32_MAX means whole-part path (compositional parts).
// Retained solely as the output type of GpuCuller::readback_batches, which
// gpu_cull_tests uses to verify per-bucket instance_count/transforms parity
// against the CPU cull reference. NOT used by the live draw path anymore —
// that goes straight from cull.comp SSBOs → glMultiDrawArraysIndirect.
struct RasterBatch {
    uint64_t part_hash     = 0;
    uint32_t cluster_index = 0;
    int      level         = 0;
    std::vector<Matrix> transforms;
};

class RasterComposer {
public:
    ~RasterComposer();

    // GL: LoadShader("shaders/raster.vs", "shaders/raster.fs"). Loads the base
    // raster shader/material used by init_gpu_driven() as the FS source.
    bool init(std::string& err);

    // GPU-driven path: load raster_gpu_driven.vs + patched raster.fs (#version 460).
    // Must be called after init(). Raster path REQUIRES this to succeed at startup
    // (GL 4.6 is a hard requirement; MATTER_RT=1 is the fallback).
    bool init_gpu_driven(std::string& err);

    // Set shader uniforms + issue glMultiDrawArraysIndirect via culler.
    // Returns drawn tris (sum from live cmd buckets).
    int draw_gpu_driven(GpuCuller& culler, PartStore& store, const Camera3D& cam);

    // Runtime LOD quality/speed dial; forwarded to GpuCuller::cull each frame.
    void set_pixel_budget(float b) { pixel_budget_ = b; }

    // Store WorldLights; draw_gpu_driven() uploads them on the next dirty frame only.
    void set_lights(const world_lights::WorldLights& l) { lights_ = l; uniforms_dirty_ = true; }

    // Store probe textures; draw_gpu_driven() binds them on the next dirty frame only.
    void set_probes(const ProbeTextures& t) { probes_ = t; uniforms_dirty_ = true; }

    // Accessor for testing (verifies set_lights stores values correctly).
    const world_lights::WorldLights& lights() const { return lights_; }

    // HUD counter (last draw_gpu_driven tri count; culler owns the rest).
    size_t drawn_tris() const { return stat_drawn_tris_; }

    // Wireframe toggle: when true, draw_gpu_driven() flips glPolygonMode to
    // GL_LINE around the indirect draw. Useful for visually inspecting mesh
    // topology (e.g., before/after retopo). Restored to GL_FILL afterwards
    // so subsequent raylib HUD draws remain filled.
    void set_wireframe(bool w) { wireframe_ = w; }
    bool wireframe() const { return wireframe_; }

    // Keep backface culling enabled during the indirect draw. Default off:
    // mesh-session winding is not guaranteed for all part kinds.
    void set_cull_backfaces(bool c) { cull_backfaces_ = c; }

private:
    // Upload sun/probe/material uniforms to a shader (used by draw_gpu_driven()).
    void setup_frame_uniforms(Shader& sh,
                              int loc_sun, int loc_sun_col, int loc_amb,
                              int loc_mat, int loc_cnt,
                              int loc_pa, int loc_pd, int loc_po,
                              int loc_pc, int loc_pdims, int loc_up);

    Shader   shader_{};
    Material material_{};
    bool ready_ = false;
    int  loc_sun_dir_ = -1, loc_sun_color_ = -1, loc_ambient_ = -1,
         loc_mat_table_ = -1, loc_mat_count_ = -1;
    // Probe-volume uniform locations.
    int  loc_probe_ambient_  = -1, loc_probe_dominant_ = -1;
    int  loc_probe_origin_   = -1, loc_probe_cell_     = -1;
    int  loc_probe_dims_     = -1, loc_use_probes_     = -1;

    // GPU-driven shader (raster_gpu_driven.vs + patched raster.fs).
    Shader shader_gpu_{};
    bool   gpu_ready_ = false;
    int    loc_gpu_mvp_         = -1;
    int    loc_gpu_sun_dir_     = -1, loc_gpu_sun_color_ = -1, loc_gpu_ambient_   = -1;
    int    loc_gpu_mat_table_   = -1, loc_gpu_mat_count_ = -1;
    int    loc_gpu_probe_amb_   = -1, loc_gpu_probe_dom_ = -1;
    int    loc_gpu_probe_orig_  = -1, loc_gpu_probe_cell_= -1;
    int    loc_gpu_probe_dims_  = -1, loc_gpu_use_probes_= -1;

    float pixel_budget_ = 1.0f;            // runtime LOD quality/speed dial
    world_lights::WorldLights lights_{};   // defaults reproduce Phase-1 hardcoded values
    ProbeTextures probes_{};               // default-constructed: valid() == false

    // Dirty flag: set on set_lights / set_probes / reconnect; cleared after
    // the first upload_frame_uniforms so the hot path skips per-frame uploads.
    bool uniforms_dirty_ = true;

    // HUD stat (written by draw_gpu_driven).
    size_t stat_drawn_tris_ = 0;

    // Toggle for wireframe rendering (see set_wireframe).
    bool wireframe_ = false;

    // Toggle for backface culling during the indirect draw (see set_cull_backfaces).
    bool cull_backfaces_ = false;
};

} // namespace viewer

#endif // VIEWER_RASTER_COMPOSER_H
