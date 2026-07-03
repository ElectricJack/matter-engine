#ifndef VIEWER_RASTER_COMPOSER_H
#define VIEWER_RASTER_COMPOSER_H

#include "raylib.h"
#include "sector_resolver.h"
#include "part_store.h"
#include "raster_mesh.h"

#include <map>
#include <string>
#include <vector>

namespace viewer {

struct RasterBatch {
    uint64_t part_hash = 0;
    int      level     = 0;
    std::vector<Matrix> transforms;
};

class RasterComposer {
public:
    ~RasterComposer();

    // GL: LoadShader("shaders/raster.vs", "shaders/raster.fs")
    bool init(std::string& err);

    // GL-free, static: recursive child expansion (depth<=8, 200k cap), level clamped
    // to lod_mesh_data.size()-1, geometry-less assemblies recurse without emitting.
    static std::vector<RasterBatch> build_batches(
        const std::vector<ResolvedInstance>& resolved, PartStore& store);

    // GL: lazy-upload meshes, BeginMode3D, one DrawMeshInstanced per batch. Returns drawn tris.
    int draw(const std::vector<RasterBatch>& batches, PartStore& store, const Camera3D& cam);

private:
    Mesh* ensure_mesh(uint64_t hash, int level, PartStore& store);  // upload-once cache

    Shader   shader_{};
    Material material_{};
    std::map<uint64_t, Mesh> mesh_cache_;   // key = (hash<<4)|level  (levels < 16)
    bool ready_ = false;
    int  loc_sun_dir_ = -1, loc_sun_color_ = -1, loc_ambient_ = -1,
         loc_mat_table_ = -1, loc_mat_count_ = -1;
};

} // namespace viewer

#endif // VIEWER_RASTER_COMPOSER_H
