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
//   culler.cull(resolved, store, eye, planes, vp, budget);  // upload + dispatch
//   culler.draw_indirect();                      // issue glMultiDrawArraysIndirect

#include "gpu_cull_types.h"    // GpuClusterMeta, GpuInstanceRec, DrawArraysCmd, kMaxLod
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
    // planes[6][4] from raster_cull.h extract_frustum_planes (or the
    // camera_frustum_planes wrappers).  cam_eye[3] world-space eye position.
    // view_proj[16] is the engine ROW-MAJOR VP from mul16(view, proj) — the
    // SAME matrix extract_frustum_planes consumes; cull() uploads it so the
    // shader receives the column-vector (shader-convention) VP for HiZ.
    // pixel_budget is the runtime LOD quality dial (default 1.0).
    // Returns false if nothing to draw (empty resolved list or no registered parts).
    bool cull(const std::vector<ResolvedInstance>& resolved,
              PartStore& store,
              const float cam_eye[3],
              const float planes[6][4],
              const float view_proj[16],
              float pixel_budget);

    // Stage-2: issue glMultiDrawArraysIndirect for all registered parts using the
    // GPU command + xform SSBOs written by the most recent cull() call.
    // shader_id must already be active (glUseProgram).
    // Caller must call BeginMode3D + rlDrawRenderBatchActive before this.
    // Returns the number of triangles submitted (sum of count/3 * instance_count per
    // live bucket, from a small cmd readback).
    int draw_indirect();

    // Reset all per-part GPU state (called on world switch when gpu_cull active).
    // Releases per-part VAO/VBO objects, clears bookkeeping, re-initializes fixed
    // buffers in-place.  After reset(), ensure_part() must be called again for all
    // parts in the new world.
    // NOTE: HiZ textures/FBO are screen-sized (not world-sized) and are intentionally
    // kept across reset() to avoid recreating them on every world switch.
    void reset();

    // -----------------------------------------------------------------------
    // HiZ depth max-pyramid.
    // build_hiz(): blit the default framebuffer depth into an R32F mip chain,
    // then downsample a max-pyramid with one compute dispatch per mip level.
    // Called at end-of-frame (after 3D draw, before UI).  No-op when hiz_enabled_
    // is false (default).  Recreates textures/FBO on resize.
    // -----------------------------------------------------------------------
    void build_hiz(int screen_w, int screen_h);

    // Expose the mip-0-copy-then-downsample chain without the depth blit, so
    // tests can upload a synthetic pattern to hiz_tex_ mip 0 and verify the
    // reduce math directly.  build_hiz() calls this internally after the blit.
    void downsample_pyramid();

    // Disabling invalidates the pyramid: build_hiz() stops running, so any
    // existing pyramid content goes stale — hiz_valid_ must drop with it and
    // only come back after the next successful build_hiz().
    void set_hiz_enabled(bool v) { hiz_enabled_ = v; if (!v) hiz_valid_ = false; }
    bool hiz_enabled() const     { return hiz_enabled_; }
    // True once build_hiz() has completed under the current enable state.
    // The cull shader gets hiz_enabled=0 until this is true (first frame after
    // enable has no pyramid yet).
    bool hiz_valid() const       { return hiz_valid_; }

    // Test hook: returns the hiz_tex_ GL name so tests can upload synthetic
    // patterns into mip 0 and verify the reduce math without a depth blit.
    unsigned hiz_tex_for_test() const { return hiz_tex_; }

    // Enable/disable the per-frame GPU stats readback (glGetBufferSubData on the
    // stats SSBO in draw_indirect()).  When disabled, stat_culled_ / stat_emitted_
    // retain the values from the last readback (one-frame-late).  Default: off.
    // Turn on only when the HUD stats panel is visible.
    void set_stats_readback(bool v) { stats_readback_ = v; }
    bool stats_readback() const     { return stats_readback_; }

    // HUD counters (valid after draw_indirect() with stats_readback_ enabled).
    size_t culled_clusters() const { return stat_culled_; }        // frustum
    size_t culled_hiz()      const { return stat_culled_hiz_; }    // occlusion
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

    // TEST-ONLY accessors: expose the GL buffer names and CPU-mirror data
    // needed by gpu_cull_tests.cpp to implement its own readback helpers.
    // Not for use in production viewer code.
    unsigned test_ssbo_cmds()   const { return ssbo_cmds_; }
    unsigned test_ssbo_xforms() const { return ssbo_xforms_; }
    unsigned test_ssbo_stats()  const { return ssbo_stats_; }
    uint32_t test_total_xform_slots() const { return total_xform_slots_; }
    const std::vector<DrawArraysCmd>&  test_cmd_template()      const { return cmd_template_; }
    const std::vector<GpuClusterMeta>& test_cluster_staging()   const { return cluster_staging_; }

    // TEST-ONLY: read the stats SSBO and update the private stat counters
    // (culled_clusters / culled_hiz / emitted).  Equivalent to the stats
    // portion of the old readback_batches() without the xform/cmd readback.
    // Call after cull() + a glMemoryBarrier to get current-frame values.
    void test_readback_stats();

private:
    // --- GL object names ---
    unsigned ssbo_clusters_       = 0;   // binding 0: ClusterMeta array
    unsigned ssbo_instances_      = 0;   // binding 1: GpuInstanceRec array
    unsigned ssbo_cmds_           = 0;   // binding 2: DrawArraysCmd array (live, written by cull.comp)
    unsigned ssbo_cmds_template_  = 0;   // pristine zero-instance_count seed; GPU-blitted to ssbo_cmds_ each frame
    unsigned ssbo_xforms_         = 0;   // binding 3: mat4 output transforms
    unsigned ssbo_stats_          = 0;   // binding 4: {stat_culled_frustum, stat_culled_hiz, stat_emitted}
    unsigned program_cull_        = 0;
    bool     cmds_template_dirty_ = false;  // set when base_instance fields change without a size change

    // Uniform locations cached after program link.
    int uloc_planes_               = -1;
    int uloc_cam_eye_              = -1;
    int uloc_pixel_budget_         = -1;
    int uloc_instance_count_       = -1;
    int uloc_max_clusters_per_inst_= -1;
    int uloc_hiz_enabled_          = -1;
    int uloc_hiz_tex_              = -1;
    int uloc_view_proj_            = -1;
    int uloc_hiz_size_             = -1;

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

    // Per-frame active part slots: set in cull(), consumed in draw_indirect().
    // active_slots_[i] == 1 iff part slot i received >=1 GpuInstanceRec this frame.
    std::vector<uint8_t> active_slots_;

    // HUD stats populated by draw_indirect().
    size_t stat_culled_      = 0;   // frustum
    size_t stat_culled_hiz_  = 0;   // occlusion
    size_t stat_emitted_     = 0;
    int    stat_last_tris_   = 0;   // tri count from the last cmd readback (one-frame-late when gate is off)

    // --- GPU readback gate ---
    // When false, draw_indirect() skips glGetBufferSubData on ssbo_stats_
    // so the frame-loop GPU sync is avoided when the HUD is not reading stats.
    bool stats_readback_ = false;

    // --- Instance upload dirty-check ---
    // FNV-1a fingerprint over (part_hash, transform) of every ResolvedInstance
    // from the previous cull() call, plus the count.  When both match, skip
    // the expand + SSBO re-upload phase (static world fast-path).
    uint64_t last_resolved_fp_    = 0;
    int      last_resolved_count_ = -1;

    // Hoisted per-frame expansion state (reused across frames; avoids repeated
    // large-vector alloc/free for static worlds on the dirty-check fast-path).
    struct ExpandedInst {
        int   part_slot;
        float transform[16];   // GL column-major (post-transpose_to_gl)
    };
    std::vector<ExpandedInst> expanded_;
    std::vector<uint32_t>     per_slot_count_;

    // -----------------------------------------------------------------------
    // HiZ pyramid state (screen-sized; kept across reset()).
    // -----------------------------------------------------------------------
    bool     hiz_enabled_     = false;
    bool     hiz_valid_       = false;   // pyramid built under current enable state
    bool     hiz_msaa_warned_ = false;   // print MSAA warning at most once
    unsigned depth_copy_tex_  = 0;       // GL_DEPTH_COMPONENT32F, no mips
    unsigned depth_fbo_       = 0;       // FBO wrapping depth_copy_tex_
    unsigned hiz_tex_         = 0;       // R32F, full mip chain (glTexStorage2D)
    unsigned program_hiz_     = 0;       // hiz_downsample.comp program
    int      hiz_w_           = 0;       // cached width  (for resize detection)
    int      hiz_h_           = 0;       // cached height (for resize detection)
    int      hiz_mip_levels_  = 0;       // floor(log2(max(w,h)))+1
    // Uniform locations on program_hiz_.
    int      uloc_hiz_src_        = -1;   // "src" sampler (depth copy + downsample)
    int      uloc_hiz_src_mip_    = -1;
    int      uloc_hiz_dst_size_   = -1;
    int      uloc_hiz_copy_mode_  = -1;

    // Release HiZ GL objects (textures + FBO + program).
    // Called from destructor and when reallocating on resize.
    void release_hiz_objects();

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
