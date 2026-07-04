#ifndef VIEWER_GPU_CULLER_H
#define VIEWER_GPU_CULLER_H

// GpuCuller: compute-shader frustum cull + LOD selection with indirect draw command output.
// Requires GL 4.6 (compute shaders, SSBOs, glMultiDrawArraysIndirect, gl_BaseInstance).
// Task 5 of the GPU instancing/culling feature. Wired into the frame loop by Task 7.
//
// Usage:
//   GpuCuller culler;
//   culler.init(err);                            // compile cull.comp, allocate fixed buffers
//   culler.ensure_part(hash, store);             // register per-part GPU state lazily
//   culler.cull(resolved, store, eye, planes, budget);  // upload + dispatch
//   auto batches = culler.readback_batches(store);       // bridge to RasterBatch list

#include "gpu_cull_types.h"    // GpuClusterMeta, GpuInstanceRec, DrawArraysCmd, kMaxLod
#include "raster_composer.h"   // RasterBatch
#include "sector_resolver.h"   // ResolvedInstance
#include "part_store.h"        // PartStore, LoadedPart, ExpandedNode

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace viewer {

// Vertex range within the part's monolithic VBO.
struct MeshRange {
    uint32_t first_vertex;
    uint32_t vertex_count;
};

class GpuCuller {
public:
    GpuCuller()  = default;
    ~GpuCuller();

    // Compile shaders_gpu/cull.comp, create fixed buffers (ssbo_stats_).
    // Must be called once after a GL context is current.
    // Returns false and sets err on any GL error.
    bool init(std::string& err);

    // Lazy per-part GPU registration.
    // Returns dense part_slot (0-based index into parts_) or -1 on failure.
    int ensure_part(uint64_t part_hash, PartStore& store);

    // Frame: upload instances (expansion applied), seed cmds, dispatch cull.
    // planes[6][4] from raster_cull.h camera_frustum_planes_raw or
    // camera_frustum_planes.  cam_eye[3] world-space eye position.
    // pixel_budget is the runtime LOD quality dial (default 1.0).
    // Returns false if nothing to draw (empty resolved list or no registered parts).
    bool cull(const std::vector<ResolvedInstance>& resolved,
              PartStore& store,
              const float cam_eye[3],
              const float planes[6][4],
              float pixel_budget);

    // Stage-1 bridge: GPU-sync (glMemoryBarrier already issued in cull()), read back
    // cmds + transforms, rebuild RasterBatch list for the existing draw path.
    // Populates stat_culled_ / stat_emitted_ from ssbo_stats_ readback.
    std::vector<RasterBatch> readback_batches(PartStore& store);

    // HUD counters (valid after readback_batches()).
    size_t culled_clusters() const { return stat_culled_; }
    size_t emitted()         const { return stat_emitted_; }

    // Per-part GPU bookkeeping — exposed for Task 8's direct draw loop.
    struct PartGpu {
        uint64_t part_hash;
        unsigned vao, vbo;
        std::vector<MeshRange> ranges;   // parallel to LoadedPart::lod_mesh_data
        uint32_t cluster_start;          // global ClusterMeta index of first cluster
        uint32_t cluster_count;
        uint32_t region_base;            // DrawInstance region start in ssbo_xforms_ (P1)
        uint32_t region_cap;             // per-instance cap (grows ×1.5 on overflow)
    };

    const std::vector<PartGpu>& parts() const { return parts_; }

    // -1 if not yet registered.
    int part_slot_of(uint64_t hash) const;

private:
    // --- GL object names ---
    unsigned ssbo_clusters_  = 0;   // binding 0: ClusterMeta array
    unsigned ssbo_instances_ = 0;   // binding 1: GpuInstanceRec array
    unsigned ssbo_cmds_      = 0;   // binding 2: DrawArraysCmd array
    unsigned ssbo_xforms_    = 0;   // binding 3: mat4 output transforms
    unsigned ssbo_stats_     = 0;   // binding 4: {stat_culled, stat_emitted}
    unsigned program_cull_   = 0;

    // Uniform locations cached after program link.
    int uloc_planes_               = -1;
    int uloc_cam_eye_              = -1;
    int uloc_pixel_budget_         = -1;
    int uloc_instance_count_       = -1;
    int uloc_max_clusters_per_inst_= -1;

    // CPU mirrors / bookkeeping.
    std::vector<PartGpu>            parts_;
    std::map<uint64_t, int>         slot_of_;
    std::vector<GpuClusterMeta>     cluster_staging_;  // CPU mirror of ssbo_clusters_
    std::vector<DrawArraysCmd>      cmd_template_;     // CPU template uploaded each frame

    // Current GPU buffer capacities (in bytes) — tracked to know when to realloc.
    size_t clusters_cap_bytes_  = 0;
    size_t instances_cap_bytes_ = 0;
    size_t cmds_cap_bytes_      = 0;
    size_t xforms_cap_bytes_    = 0;

    // Running total of xform slots allocated across all parts (P1 regions).
    uint32_t total_xform_slots_ = 0;

    // HUD stats populated by readback_batches().
    size_t stat_culled_  = 0;
    size_t stat_emitted_ = 0;

    // --- Internal helpers ---

    // Compile a compute shader from a GLSL source file.
    // Returns shader name or 0 on failure (err set).
    unsigned compile_compute(const char* path, std::string& err);

    // Grow ssbo_clusters_ if needed to hold at least `need_bytes` total,
    // preserving existing content via glCopyBufferSubData.
    void grow_clusters_ssbo(size_t need_bytes);

    // Recompute ALL part region_base / cmd_template_ base_instance values after
    // any region_cap change.  Also reallocates ssbo_xforms_.
    void recompute_regions();

    // Upload cmd_template_ to ssbo_cmds_ (grow-reallocate if needed).
    void upload_cmd_template();
};

} // namespace viewer

#endif // VIEWER_GPU_CULLER_H
