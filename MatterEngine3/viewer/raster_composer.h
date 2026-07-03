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

    // GL-free: recursive child expansion (depth<=8, 200k cap), per-cluster
    // frustum cull + LOD selection when clusters are present (v3/v2 flat parts),
    // whole-part path for compositional parts. Camera drives the frustum and the
    // per-cluster projected-size LOD metric. Batches are fingerprinted and reused
    // across frames when the camera + instance set is unchanged.
    std::vector<RasterBatch> build_batches(
        const std::vector<ResolvedInstance>& resolved,
        PartStore& store,
        const Camera3D& cam);

    // GL: lazy-upload meshes, BeginMode3D, one DrawMeshInstanced per batch. Returns drawn tris.
    int draw(const std::vector<RasterBatch>& batches, PartStore& store, const Camera3D& cam);

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

    world_lights::WorldLights lights_{};   // defaults reproduce Phase-1 hardcoded values
    ProbeTextures probes_{};               // default-constructed: valid() == false

    // Fingerprint-based batch reuse: cache built batches when camera+instances
    // are identical across consecutive frames.
    uint64_t              last_fp_       = 0;
    std::vector<RasterBatch> last_batches_;

    // HUD stats (written by build_batches, read by accessors above).
    size_t stat_batches_         = 0;
    size_t stat_drawn_tris_      = 0;
    size_t stat_culled_clusters_ = 0;
    bool   stat_cache_hit_       = false;
};

} // namespace viewer

#endif // VIEWER_RASTER_COMPOSER_H
