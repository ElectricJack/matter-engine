#ifndef VIEWER_RASTER_COMPOSER_H
#define VIEWER_RASTER_COMPOSER_H

#include "raylib.h"
#include "sector_resolver.h"
#include "part_store.h"
#include "raster_mesh.h"
#include "probe_texture.h"
#include "world_lights.h"

#include <map>
#include <string>
#include <tuple>
#include <vector>

// Forward declaration for Stage-2 GPU-driven path.
namespace viewer { class GpuCuller; }

namespace viewer {

// Batch key: (part_hash, cluster_index, lod_level).
// cluster_index == UINT32_MAX means whole-part path (compositional parts).
struct RasterBatch {
    uint64_t part_hash     = 0;
    uint32_t cluster_index = 0;
    int      level         = 0;
    std::vector<Matrix> transforms;
};

class RasterComposer {
public:
    ~RasterComposer();

    // GL: LoadShader("shaders/raster.vs", "shaders/raster.fs")
    bool init(std::string& err);

    // Stage-2 GPU-driven path: load raster_gpu_driven.vs + patched raster.fs (#version 460).
    // Called only when MATTER_GPU_CULL=1.  Must be called after init().
    bool init_gpu_driven(std::string& err);

    // Stage-2: set shader uniforms + issue glMultiDrawArraysIndirect via culler.
    // Returns drawn tris.
    int draw_gpu_driven(GpuCuller& culler, PartStore& store, const Camera3D& cam);

    // GL-free: recursive child expansion (depth<=8, 200k cap), per-cluster
    // frustum cull + LOD selection when clusters are present (v3/v2 flat parts),
    // whole-part path for compositional parts. Camera drives the frustum and the
    // per-cluster projected-size LOD metric. Batches are fingerprinted over
    // (camera, world_version, per-instance (part_hash, lod)) and reused across
    // frames when nothing changed. Transform bytes are NOT hashed: transforms only
    // change via world deltas, which bump world_version (Stage 1 — drops ~3.3 MB/frame
    // of FNV input).
    //
    // Invariant: PartStore contents referenced by `resolved` are stable across
    // calls with the same fingerprint. In practice, any new part appearing in
    // the resolved set arrives via a world delta (which bumps world_version and
    // invalidates the cache); PartStore is otherwise read-only during a frame.
    // If a future change introduces a code path that mutates a loaded part
    // (e.g. re-load its BLAS handles) without a world_version bump, this cache
    // will serve stale batches — extend the fingerprint accordingly.
    std::vector<RasterBatch> build_batches(
        const std::vector<ResolvedInstance>& resolved,
        PartStore& store,
        const Camera3D& cam,
        uint64_t world_version);

    // GL: lazy-upload meshes, BeginMode3D, one DrawMeshInstanced per batch. Returns drawn tris.
    int draw(const std::vector<RasterBatch>& batches, PartStore& store, const Camera3D& cam);

    // Runtime LOD quality/speed dial (Stage 2); folded into the batch fingerprint.
    void set_pixel_budget(float b) { pixel_budget_ = b; }

    // Store WorldLights; draw() uploads them each frame instead of hardcoded values.
    void set_lights(const world_lights::WorldLights& l) { lights_ = l; }

    // Store probe textures; draw() binds them to units 4/5 and sets useProbes=1.
    void set_probes(const ProbeTextures& t) { probes_ = t; }

    // Accessor for testing (verifies set_lights stores values correctly).
    const world_lights::WorldLights& lights() const { return lights_; }

    // HUD counters (accumulated in build_batches, reset each call).
    size_t batches()         const { return stat_batches_; }
    size_t drawn_tris()      const { return stat_drawn_tris_; }
    size_t culled_clusters() const { return stat_culled_clusters_; }
    bool   cache_hit()       const { return stat_cache_hit_; }

private:
    Mesh* ensure_mesh(uint64_t hash, int cluster_index, int level, PartStore& store);

    // Upload sun/probe/material uniforms to a shader (shared by draw() and draw_gpu_driven()).
    void setup_frame_uniforms(Shader& sh,
                              int loc_sun, int loc_sun_col, int loc_amb,
                              int loc_mat, int loc_cnt,
                              int loc_pa, int loc_pd, int loc_po,
                              int loc_pc, int loc_pdims, int loc_up);

    Shader   shader_{};
    Material material_{};
    // Mesh cache key: (part_hash, cluster_index, level). cluster_index==UINT32_MAX->whole-part.
    std::map<std::tuple<uint64_t,uint32_t,int>, Mesh> mesh_cache_;
    bool ready_ = false;
    int  loc_sun_dir_ = -1, loc_sun_color_ = -1, loc_ambient_ = -1,
         loc_mat_table_ = -1, loc_mat_count_ = -1;
    // Probe-volume uniform locations (Task 6).
    int  loc_probe_ambient_  = -1, loc_probe_dominant_ = -1;
    int  loc_probe_origin_   = -1, loc_probe_cell_     = -1;
    int  loc_probe_dims_     = -1, loc_use_probes_     = -1;

    // Stage-2 GPU-driven shader (raster_gpu_driven.vs + patched raster.fs).
    Shader shader_gpu_{};
    bool   gpu_ready_ = false;
    int    loc_gpu_mvp_         = -1;
    int    loc_gpu_sun_dir_     = -1, loc_gpu_sun_color_ = -1, loc_gpu_ambient_   = -1;
    int    loc_gpu_mat_table_   = -1, loc_gpu_mat_count_ = -1;
    int    loc_gpu_probe_amb_   = -1, loc_gpu_probe_dom_ = -1;
    int    loc_gpu_probe_orig_  = -1, loc_gpu_probe_cell_= -1;
    int    loc_gpu_probe_dims_  = -1, loc_gpu_use_probes_= -1;

    float pixel_budget_ = 1.0f;            // runtime LOD quality/speed dial (Stage 2)
    world_lights::WorldLights lights_{};   // defaults reproduce Phase-1 hardcoded values
    ProbeTextures probes_{};               // default-constructed: valid() == false

    // Fingerprint-based batch reuse: cache built batches when camera+instances
    // are identical across consecutive frames.
    uint64_t              last_fp_       = 0;
    bool                  last_valid_    = false;   // true after the first build
    std::vector<RasterBatch> last_batches_;

    // HUD stats (written by build_batches, read by accessors above).
    size_t stat_batches_         = 0;
    size_t stat_drawn_tris_      = 0;
    size_t stat_culled_clusters_ = 0;
    bool   stat_cache_hit_       = false;
};

} // namespace viewer

#endif // VIEWER_RASTER_COMPOSER_H
