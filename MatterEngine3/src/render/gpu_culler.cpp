// GpuCuller implementation — Task 5 of the GPU instancing/culling feature.
// Requires GL 4.6 (compute shaders, SSBOs, indirect dispatch).
// Wired into main.cpp frame loop by Task 7; runtime-validated by Task 6.
//
// Matrix conventions (important — read before editing):
//   Engine float[16] is row-major storage of column-vector matrices.
//   Translation lives at t[3], t[7], t[11] (row 0,1,2 col 3).
//   transpose_to_gl(t, out) writes out[c*4+r] = t[r*4+c], producing GL column-major.
//   The GLSL shader then has M[col][row] memory order, which matches standard gl_Position math.

#include "async_bake.h"
#include "gpu_culler.h"
#include "raster_cull.h"    // mul16, inst_scale

// Raylib must come before glad to avoid double-definition of GL types.
#include "raylib.h"
#include "external/glad.h"
#include "shader_source.h"   // matter::shader_text

#include <cassert>
#include <cstdio>
#include <cstring>

namespace viewer {

// Per-bucket instance-transform slot count assigned to each newly registered
// part.  Starts small because worlds stream many unique single-instance terrain
// tiles; the overflow path in cull() grows region_cap to the real per-part
// count on the part's first resolved frame, so no OOB window exists.
static constexpr uint32_t kInitialRegionCap = 16;

// ---------------------------------------------------------------------------
// release_hiz_objects — free HiZ GL objects (textures, FBO, program).
// Called from destructor and when recreating on resize.
// ---------------------------------------------------------------------------
void GpuCuller::release_hiz_objects() {
    if (depth_copy_tex_) { glDeleteTextures(1, &depth_copy_tex_); depth_copy_tex_ = 0; }
    if (depth_fbo_)      { glDeleteFramebuffers(1, &depth_fbo_);  depth_fbo_      = 0; }
    if (hiz_tex_)        { glDeleteTextures(1, &hiz_tex_);        hiz_tex_        = 0; }
    // Note: program_hiz_ is compiled once and NOT released on resize — only on destroy.
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
GpuCuller::~GpuCuller() {
    for (auto& pg : parts_) {
        if (pg.vao) glDeleteVertexArrays(1, &pg.vao);
        if (pg.vbo) glDeleteBuffers(1, &pg.vbo);
    }
    if (ssbo_clusters_)       glDeleteBuffers(1, &ssbo_clusters_);
    if (ssbo_instances_)      glDeleteBuffers(1, &ssbo_instances_);
    if (ssbo_cmds_)           glDeleteBuffers(1, &ssbo_cmds_);
    if (ssbo_cmds_template_)  glDeleteBuffers(1, &ssbo_cmds_template_);
    if (ssbo_xforms_)         glDeleteBuffers(1, &ssbo_xforms_);
    if (ssbo_stats_)          glDeleteBuffers(1, &ssbo_stats_);
    if (program_cull_)   glDeleteProgram(program_cull_);
    // HiZ objects (program_hiz_ + textures/FBO).
    release_hiz_objects();
    if (program_hiz_)    glDeleteProgram(program_hiz_);
}

// ---------------------------------------------------------------------------
// compile_compute — load GLSL source via matter::shader_text, compile as
// GL_COMPUTE_SHADER, link into a program.  Returns program name or 0.
// ---------------------------------------------------------------------------
unsigned GpuCuller::compile_compute(const char* path, std::string& err) {
    std::string src, serr;
    if (!matter::shader_text(path, src, serr)) {
        err = std::string("compile_compute: cannot load ") + path + ": " + serr;
        return 0;
    }
    const char* csrc = src.c_str();

    unsigned shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &csrc, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(log), &len, log);
        err = std::string("cull.comp compile error: ") + log;
        glDeleteShader(shader);
        return 0;
    }

    unsigned prog = glCreateProgram();
    glAttachShader(prog, shader);
    glLinkProgram(prog);
    glDeleteShader(shader);   // flagged for deletion; freed when detached at prog delete

    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei len = 0;
        glGetProgramInfoLog(prog, sizeof(log), &len, log);
        err = std::string("cull.comp link error: ") + log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ---------------------------------------------------------------------------
// init — compile shader, allocate fixed-size buffers.
// ---------------------------------------------------------------------------
bool GpuCuller::init(std::string& err) {
    matter_async::assert_gl_thread("GpuCuller::init");
    program_cull_ = compile_compute("shaders_gpu/cull.comp", err);
    if (!program_cull_) return false;

    // Cache uniform locations.
    uloc_planes_                = glGetUniformLocation(program_cull_, "planes");
    uloc_cam_eye_               = glGetUniformLocation(program_cull_, "cam_eye");
    uloc_pixel_budget_          = glGetUniformLocation(program_cull_, "pixel_budget");
    uloc_min_projected_size_    = glGetUniformLocation(program_cull_, "min_projected_size");
    uloc_instance_count_        = glGetUniformLocation(program_cull_, "instance_count");
    uloc_max_clusters_per_inst_ = glGetUniformLocation(program_cull_, "max_clusters_per_instance");
    uloc_hiz_enabled_           = glGetUniformLocation(program_cull_, "hiz_enabled");
    uloc_hiz_tex_               = glGetUniformLocation(program_cull_, "hiz_tex");
    uloc_view_proj_             = glGetUniformLocation(program_cull_, "view_proj");
    uloc_hiz_size_              = glGetUniformLocation(program_cull_, "hiz_size");

    // Stats SSBO: 12 bytes (three uint32s: culled_frustum, culled_hiz, emitted).
    glGenBuffers(1, &ssbo_stats_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_stats_);
    uint32_t zeros[3] = {0, 0, 0};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zeros), zeros, GL_DYNAMIC_READ);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Query the GL implementation's max SSBO size and convert to xform slots
    // (each slot = one mat4 = 16 floats = 64 bytes).  Mesa d3d12 reports 128 MB.
    GLint64 max_ssbo_bytes = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_ssbo_bytes);
    max_ssbo_slots_ = (uint32_t)(max_ssbo_bytes / (16 * sizeof(float)));

    // Create zero-size placeholder buffers for the growable SSBOs; actual
    // allocations happen lazily in ensure_part / cull.
    glGenBuffers(1, &ssbo_clusters_);
    glGenBuffers(1, &ssbo_instances_);
    glGenBuffers(1, &ssbo_cmds_);
    glGenBuffers(1, &ssbo_xforms_);

    return true;
}

// ---------------------------------------------------------------------------
// grow_clusters_ssbo — grow the cluster SSBO preserving existing content.
// ---------------------------------------------------------------------------
void GpuCuller::grow_clusters_ssbo(size_t need_bytes) {
    if (need_bytes <= clusters_cap_bytes_) return;

    // Compute new capacity: at least need_bytes, at least ×1.5 current.
    size_t new_cap = clusters_cap_bytes_ == 0 ? need_bytes
                   : clusters_cap_bytes_ + clusters_cap_bytes_ / 2;
    if (new_cap < need_bytes) new_cap = need_bytes;

    // Create a new buffer.
    unsigned new_ssbo = 0;
    glGenBuffers(1, &new_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, new_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)new_cap, nullptr, GL_DYNAMIC_DRAW);

    // Copy existing content if any.
    if (clusters_cap_bytes_ > 0 && ssbo_clusters_) {
        glBindBuffer(GL_COPY_READ_BUFFER, ssbo_clusters_);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_SHADER_STORAGE_BUFFER,
                            0, 0, (GLsizeiptr)clusters_cap_bytes_);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (ssbo_clusters_) glDeleteBuffers(1, &ssbo_clusters_);
    ssbo_clusters_     = new_ssbo;
    clusters_cap_bytes_ = new_cap;
}

// ---------------------------------------------------------------------------
// recompute_regions — recompute ALL part region_base / cmd_template_
// base_instance fields, then grow ssbo_xforms_ if necessary.
//
// Called whenever any part's region_cap changes (overflow growth ×1.5) or a
// new part is registered.  O(total parts * max_lod * max_clusters) but this
// only fires on a resize event, not every frame.
//
// Capacity-based growth: ssbo_xforms_ is only reallocated when
// total_xform_slots_ exceeds xforms_cap_slots_.  The new capacity is
//   max(total_xform_slots_, xforms_cap_slots_ * 3/2)
// clamped to GL_MAX_SHADER_STORAGE_BLOCK_SIZE (128 MB on Mesa d3d12).
// ×1.5 instead of ×2 prevents the doubling from breaching the GL limit
// for large forests (~86k instances, Task #40).
//
// ssbo_xforms_ is output-only from the cull compute shader; its contents
// are written entirely by the shader every frame, so the existing GPU
// buffer content does not need to be preserved on reallocation.
// ---------------------------------------------------------------------------
void GpuCuller::recompute_regions() {
    total_xform_slots_ = 0;
    for (auto& pg : parts_) {
        pg.region_base = total_xform_slots_;
        // Each bucket (ci_local, lv) gets region_cap slots.
        // bucket index = ci_local * kMaxLod + lv
        // base_instance = region_base + bucket * region_cap
        uint32_t buckets = pg.cluster_count * (uint32_t)kMaxLod;
        total_xform_slots_ += buckets * pg.region_cap;

        // Update cmd_template_ base_instance for each bucket of this part.
        for (uint32_t ci = 0; ci < pg.cluster_count; ++ci) {
            for (int lv = 0; lv < kMaxLod; ++lv) {
                uint32_t global_ci = pg.cluster_start + ci;
                uint32_t bucket    = global_ci * (uint32_t)kMaxLod + (uint32_t)lv;
                if (bucket < (uint32_t)cmd_template_.size()) {
                    uint32_t ci_local = ci;
                    cmd_template_[bucket].base_instance =
                        pg.region_base + (ci_local * (uint32_t)kMaxLod + (uint32_t)lv) * pg.region_cap;
                }
            }
        }
    }

    // base_instance fields in cmd_template_ were updated above; mark dirty so
    // upload_cmd_template() refreshes the pristine GPU buffer.
    cmds_template_dirty_ = true;

    // Capacity-based growth for ssbo_xforms_: only reallocate when the needed
    // slot count exceeds the current GPU buffer capacity.  On realloc, grow by
    // ×1.5 (not ×2) and clamp to the GL implementation's max SSBO size.
    // ssbo_xforms_ is written entirely by the cull compute shader each frame;
    // no CPU content needs to be preserved — glBufferData(nullptr) suffices.
    if (total_xform_slots_ > xforms_cap_slots_ || !ssbo_xforms_) {
        uint32_t new_cap = (xforms_cap_slots_ == 0)
                         ? total_xform_slots_
                         : xforms_cap_slots_ + xforms_cap_slots_ / 2;
        if (new_cap < total_xform_slots_) new_cap = total_xform_slots_;
        if (max_ssbo_slots_ > 0 && new_cap > max_ssbo_slots_) {
            new_cap = max_ssbo_slots_;
            if (new_cap < total_xform_slots_)
                printf("GpuCuller: WARNING: xforms SSBO clamped to %u slots "
                       "(need %u); rendering may be corrupt\n",
                       new_cap, total_xform_slots_);
        }
        if (new_cap == 0) new_cap = 1;   // keep valid GL object

        size_t new_bytes = (size_t)new_cap * 16 * sizeof(float);

        // P2-decision telemetry: log realloc events (not every registration).
        printf("GpuCuller: xforms SSBO grow %u -> %u slots (%.1f MB, %zu parts)\n",
               xforms_cap_slots_, new_cap,
               (double)new_bytes / (1024.0 * 1024.0), parts_.size());

        if (ssbo_xforms_) glDeleteBuffers(1, &ssbo_xforms_);
        glGenBuffers(1, &ssbo_xforms_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_xforms_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)new_bytes, nullptr, GL_DYNAMIC_COPY);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        xforms_cap_slots_ = new_cap;
        xforms_cap_bytes_ = new_bytes;
    }
    // else: existing buffer is large enough; base_instance offsets are already
    // updated in cmd_template_ above; the shader will write into the correct
    // slots within the oversized buffer — no GL work needed.
}

// ---------------------------------------------------------------------------
// upload_cmd_template — upload the CPU cmd_template_ to ssbo_cmds_.
//
// Strategy: maintain a "pristine" template buffer (ssbo_cmds_template_) that
// holds the zero-instance_count seed exactly once after any structural change.
// Each frame we copy it to the live ssbo_cmds_ via glCopyBufferSubData, which
// is a pure GPU-side blit (no CPU→GPU DMA) and avoids the per-frame CPU
// glBufferSubData call.  The pristine buffer is reallocated only when
// cmd_template_.size() grows (new part registered).
// ---------------------------------------------------------------------------
void GpuCuller::upload_cmd_template() {
    size_t need = cmd_template_.size() * sizeof(DrawArraysCmd);
    if (need == 0) return;

    // (Re)allocate both buffers if the pristine buffer is too small.
    if (need > cmds_cap_bytes_) {
        size_t new_cap = cmds_cap_bytes_ == 0 ? need
                       : cmds_cap_bytes_ + cmds_cap_bytes_ / 2;
        if (new_cap < need) new_cap = need;

        // Allocate pristine buffer; upload the CPU template as the seed.
        if (!ssbo_cmds_template_) glGenBuffers(1, &ssbo_cmds_template_);
        glBindBuffer(GL_COPY_READ_BUFFER, ssbo_cmds_template_);
        glBufferData(GL_COPY_READ_BUFFER, (GLsizeiptr)new_cap, nullptr, GL_STATIC_DRAW);
        glBufferSubData(GL_COPY_READ_BUFFER, 0, (GLsizeiptr)need, cmd_template_.data());
        glBindBuffer(GL_COPY_READ_BUFFER, 0);

        // Allocate live cmds buffer (will be overwritten every frame by the copy).
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cmds_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)new_cap, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        cmds_cap_bytes_ = new_cap;
    } else if (cmds_template_dirty_) {
        // Template size unchanged but content changed (base_instance update after
        // region growth): refresh the pristine buffer from the CPU mirror.
        glBindBuffer(GL_COPY_READ_BUFFER, ssbo_cmds_template_);
        glBufferSubData(GL_COPY_READ_BUFFER, 0, (GLsizeiptr)need, cmd_template_.data());
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
    }
    cmds_template_dirty_ = false;

    // GPU-side blit: pristine → live (no CPU stall).
    glBindBuffer(GL_COPY_READ_BUFFER,  ssbo_cmds_template_);
    glBindBuffer(GL_COPY_WRITE_BUFFER, ssbo_cmds_);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, (GLsizeiptr)need);
    glBindBuffer(GL_COPY_READ_BUFFER,  0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

// ---------------------------------------------------------------------------
// ensure_part — lazy per-part GPU registration.
// ---------------------------------------------------------------------------
int GpuCuller::ensure_part(uint64_t part_hash, PartStore& store) {
    auto it = slot_of_.find(part_hash);
    if (it != slot_of_.end()) return it->second;

    const LoadedPart* lp = store.get_or_load(part_hash);
    if (!lp) return -1;

    // Assign a new slot.
    int slot = (int)parts_.size();
    slot_of_[part_hash] = slot;
    parts_.emplace_back();
    PartGpu& pg = parts_.back();
    pg.part_hash      = part_hash;
    pg.vao            = 0;
    pg.vbo            = 0;
    pg.cluster_start  = (uint32_t)cluster_staging_.size();
    pg.cluster_count  = 0;
    pg.region_base    = 0;   // recomputed below
    pg.region_cap     = kInitialRegionCap;

    // -----------------------------------------------------------------
    // Build interleaved VBO (stride 36 B per vertex):
    //   loc 0: position   3 × float   (bytes  0-11)
    //   loc 1: texcoord   2 × float   (bytes 12-19)
    //   loc 2: normal     3 × float   (bytes 20-31)
    //   loc 3: color      4 × uint8   (bytes 32-35)
    //
    // Concatenate all lod_mesh_data entries into one buffer; record MeshRange.
    // -----------------------------------------------------------------
    const int kStride = 36;
    std::vector<unsigned char> vbo_data;

    for (const auto& md : lp->lod_mesh_data) {
        uint32_t first = (uint32_t)(vbo_data.size() / kStride);
        uint32_t n     = (uint32_t)md.vertex_count;
        pg.ranges.push_back(MeshRange{ first, n });

        // Reserve space for n vertices * 36 bytes.
        size_t base = vbo_data.size();
        vbo_data.resize(base + n * kStride, 0);

        for (uint32_t vi = 0; vi < n; ++vi) {
            unsigned char* dst = vbo_data.data() + base + vi * kStride;

            // Position (3 floats, bytes 0-11).
            float px = 0, py = 0, pz = 0;
            if (vi * 3 + 2 < md.vertices.size()) {
                px = md.vertices[vi * 3 + 0];
                py = md.vertices[vi * 3 + 1];
                pz = md.vertices[vi * 3 + 2];
            }
            std::memcpy(dst + 0,  &px, 4);
            std::memcpy(dst + 4,  &py, 4);
            std::memcpy(dst + 8,  &pz, 4);

            // Texcoord (2 floats, bytes 12-19).
            float tu = 0, tv = 0;
            if (vi * 2 + 1 < md.texcoords.size()) {
                tu = md.texcoords[vi * 2 + 0];
                tv = md.texcoords[vi * 2 + 1];
            }
            std::memcpy(dst + 12, &tu, 4);
            std::memcpy(dst + 16, &tv, 4);

            // Normal (3 floats, bytes 20-31).
            float nx = 0, ny = 1, nz = 0;
            if (vi * 3 + 2 < md.normals.size()) {
                nx = md.normals[vi * 3 + 0];
                ny = md.normals[vi * 3 + 1];
                nz = md.normals[vi * 3 + 2];
            }
            std::memcpy(dst + 20, &nx, 4);
            std::memcpy(dst + 24, &ny, 4);
            std::memcpy(dst + 28, &nz, 4);

            // Color (4 × uint8, bytes 32-35).
            unsigned char cr = 255, cg = 255, cb = 255, ca = 255;
            if (vi * 4 + 3 < md.colors.size()) {
                cr = md.colors[vi * 4 + 0];
                cg = md.colors[vi * 4 + 1];
                cb = md.colors[vi * 4 + 2];
                ca = md.colors[vi * 4 + 3];
            }
            dst[32] = cr; dst[33] = cg; dst[34] = cb; dst[35] = ca;
        }
    }

    // Upload VBO + set up VAO.
    glGenVertexArrays(1, &pg.vao);
    glGenBuffers(1, &pg.vbo);

    glBindVertexArray(pg.vao);
    glBindBuffer(GL_ARRAY_BUFFER, pg.vbo);
    if (!vbo_data.empty()) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vbo_data.size(),
                     vbo_data.data(), GL_STATIC_DRAW);
    } else {
        // Empty part: upload 1-byte placeholder so the buffer object is valid.
        unsigned char dummy = 0;
        glBufferData(GL_ARRAY_BUFFER, 1, &dummy, GL_STATIC_DRAW);
    }

    // loc 0: position (3f, offset 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride, (void*)0);
    // loc 1: texcoord (2f, offset 12)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride, (void*)12);
    // loc 2: normal (3f, offset 20)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, kStride, (void*)20);
    // loc 3: color (4 × u8 normalized, offset 32)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, kStride, (void*)32);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // -----------------------------------------------------------------
    // Build ClusterMeta records for this part and append to cluster_staging_.
    // -----------------------------------------------------------------
    uint32_t part_slot_u = (uint32_t)slot;

    if (!lp->clusters.empty()) {
        for (uint32_t ci = 0; ci < (uint32_t)lp->clusters.size(); ++ci) {
            cluster_staging_.push_back(
                pack_cluster(lp->clusters[ci], part_slot_u,
                             pg.cluster_start + ci));
        }
        pg.cluster_count = (uint32_t)lp->clusters.size();
        pg.fine_cluster_count = lp->fine_cluster_count;
    } else {
        // Whole-part synthetic cluster.
        cluster_staging_.push_back(pack_whole_part(*lp, part_slot_u));
        pg.cluster_count = 1;
        pg.fine_cluster_count = 1;
    }

    // -----------------------------------------------------------------
    // Extend cmd_template_ with cluster_count * kMaxLod entries.
    // base_instance is computed by recompute_regions() below; fill
    // count/first from the MeshRange table.
    // -----------------------------------------------------------------
    for (uint32_t ci = 0; ci < pg.cluster_count; ++ci) {
        const GpuClusterMeta& cm = cluster_staging_[pg.cluster_start + ci];
        for (int lv = 0; lv < kMaxLod; ++lv) {
            DrawArraysCmd cmd{};
            cmd.instance_count = 0;
            cmd.base_instance  = 0;   // filled by recompute_regions()
            uint32_t mesh_idx = cm.lod_mesh_idx[lv < (int)cm.lod_count ? lv : cm.lod_count - 1];
            if (mesh_idx < pg.ranges.size()) {
                cmd.first  = pg.ranges[mesh_idx].first_vertex;
                cmd.count  = pg.ranges[mesh_idx].vertex_count;
            }
            cmd_template_.push_back(cmd);
        }
    }

    // -----------------------------------------------------------------
    // Upload expanded cluster staging to ssbo_clusters_.
    // -----------------------------------------------------------------
    size_t need_bytes = cluster_staging_.size() * sizeof(GpuClusterMeta);
    grow_clusters_ssbo(need_bytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_clusters_);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)need_bytes,
                    cluster_staging_.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // P2-decision telemetry: per-part region footprint at registration.
    printf("GpuCuller: part %016llx slot %d clusters %u region_cap %u (%zu region bytes)\n",
           (unsigned long long)part_hash, slot, pg.cluster_count, pg.region_cap,
           (size_t)pg.cluster_count * (size_t)kMaxLod * (size_t)pg.region_cap * 64);

    // Recompute P1 regions now that we have a new part.
    recompute_regions();

    return slot;
}

// ---------------------------------------------------------------------------
// part_slot_of
// ---------------------------------------------------------------------------
int GpuCuller::part_slot_of(uint64_t hash) const {
    auto it = slot_of_.find(hash);
    return it != slot_of_.end() ? it->second : -1;
}

// ---------------------------------------------------------------------------
// release_part — evict a single part's GPU resources.
//
// The dead slot (vao == 0) remains in parts_ as a hole; no compaction.
// draw_indirect() already skips entries with pg.vao == 0.
// The cull shader skips clusters with lod_count == 0 (zeroed here and
// patched into ssbo_clusters_ via glBufferSubData on the exact range).
//
// After this call:
//   - slot_of_[hash] no longer exists  → ensure_part() will assign a new slot
//   - parts_[old_slot].vao == 0        → dead; draw_indirect skips it
//   - cluster_staging_[cl_start..+cl_count]: lod_count zeroed in CPU mirror
//   - ssbo_clusters_ patched on that byte range (glBufferSubData)
//   - cmd_template_ entries for this part's buckets left in place but
//     region_base/base_instance values are now orphaned; next ensure_part
//     re-appends fresh entries at the end. The orphaned cmd entries still
//     occupy space but have instance_count = 0 every frame (cull shader
//     can't emit instances for dead clusters), so they produce no draw calls.
//
// Bounded waste: region_base slots and cmd_template_ entries for the dead
// slot remain allocated.  The waste is bounded by the number of released
// parts and is reclaimed on the next world reset().
//
// Safe no-op if part_hash is not registered.
// ---------------------------------------------------------------------------
void GpuCuller::release_part(uint64_t part_hash) {
    auto it = slot_of_.find(part_hash);
    if (it == slot_of_.end()) return;   // not registered — no-op

    int slot = it->second;
    slot_of_.erase(it);

    if (slot < 0 || slot >= (int)parts_.size()) return;
    PartGpu& pg = parts_[slot];

    // Delete GL objects.
    if (pg.vao) { glDeleteVertexArrays(1, &pg.vao); pg.vao = 0; }
    if (pg.vbo) { glDeleteBuffers(1, &pg.vbo);      pg.vbo = 0; }

    // Zero lod_count for all cluster entries in the CPU mirror so the cull
    // shader will produce no instances for this part's clusters.
    uint32_t cl_start = pg.cluster_start;
    uint32_t cl_count = pg.cluster_count;
    for (uint32_t ci = 0; ci < cl_count; ++ci) {
        uint32_t idx = cl_start + ci;
        if (idx < (uint32_t)cluster_staging_.size()) {
            cluster_staging_[idx].lod_count = 0;
        }
    }

    // Patch ssbo_clusters_ on exactly the affected range (avoids a full upload).
    if (cl_count > 0 && cl_start < (uint32_t)cluster_staging_.size() && ssbo_clusters_) {
        uint32_t actual_count = std::min(cl_count,
            (uint32_t)cluster_staging_.size() - cl_start);
        if (actual_count > 0) {
            size_t byte_offset = (size_t)cl_start * sizeof(GpuClusterMeta);
            size_t byte_len    = (size_t)actual_count * sizeof(GpuClusterMeta);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_clusters_);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                            (GLintptr)byte_offset, (GLsizeiptr)byte_len,
                            cluster_staging_.data() + cl_start);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
    }

    // Mark the slot dead (vao already zeroed above; also clear hash for clarity).
    pg.part_hash    = 0;
    pg.cluster_count = 0;
}

// ---------------------------------------------------------------------------
// cull — the frame main entry point.
// ---------------------------------------------------------------------------
bool GpuCuller::cull(const std::vector<ResolvedInstance>& resolved,
                     PartStore& store,
                     const float cam_eye[3],
                     const float planes[6][4],
                     const float view_proj[16],
                     float pixel_budget)
{
    if (resolved.empty()) return false;

    // ------------------------------------------------------------------
    // Dirty-check: compute FNV-1a fingerprint over (part_hash, transform)
    // of every ResolvedInstance.  If the resolved set is identical to the
    // previous frame (count + content), skip the expand + SSBO re-upload
    // (static world fast-path).  Mirrors world_composer.cpp:64-76.
    // ------------------------------------------------------------------
    bool instances_dirty = false;
    {
        uint64_t fp = 1469598103934665603ull;
        auto fold = [&fp](const void* p, size_t n) {
            const unsigned char* b = static_cast<const unsigned char*>(p);
            for (size_t i = 0; i < n; ++i) fp = (fp ^ b[i]) * 1099511628211ull;
        };
        for (const auto& ri : resolved) {
            fold(&ri.part_hash, sizeof ri.part_hash);
            fold(ri.transform,  sizeof ri.transform);
            fold(&ri.segment,   sizeof ri.segment);
        }
        if (last_resolved_count_ != (int)resolved.size() || last_resolved_fp_ != fp) {
            instances_dirty       = true;
            last_resolved_count_  = (int)resolved.size();
            last_resolved_fp_     = fp;
        }
    }

    if (instances_dirty) {
        // ------------------------------------------------------------------
        // Pass 1: expand resolved instances via ExpandedNode tables.
        // Count instances per part first so we can check/grow region_caps before
        // dispatch (structural overflow prevention: region_cap >= N_p).
        // ------------------------------------------------------------------
        expanded_.clear();
        expanded_.reserve(resolved.size() * 4);
        per_slot_count_.clear();

        for (const auto& ri : resolved) {
            // Ensure the root part is registered.
            int root_slot = ensure_part(ri.part_hash, store);
            if (root_slot < 0) continue;

            const LoadedPart* lp = store.get_or_load(ri.part_hash);
            if (!lp) continue;

            // Walk expansion table (built by build_expansion in part_store.cpp).
            if (!lp->expansion.empty()) {
                for (const auto& en : lp->expansion) {
                    int node_slot = ensure_part(en.part_hash, store);
                    if (node_slot < 0) continue;

                    // Combined world transform: resolved.transform × node.rel_transform.
                    float combined[16];
                    mul16(ri.transform, en.rel_transform, combined);

                    // Transpose to GL column-major.
                    float gl_xform[16];
                    transpose_to_gl(combined, gl_xform);

                    ExpandedInst ei;
                    ei.part_slot = node_slot;
                    ei.segment   = ri.segment;
                    std::memcpy(ei.transform, gl_xform, 64);
                    expanded_.push_back(ei);

                    // Track per-slot count.
                    if ((int)per_slot_count_.size() <= node_slot)
                        per_slot_count_.resize(node_slot + 1, 0);
                    per_slot_count_[node_slot]++;
                }
            } else {
                // No expansion: the root part is drawable directly.
                float gl_xform[16];
                transpose_to_gl(ri.transform, gl_xform);

                ExpandedInst ei;
                ei.part_slot = root_slot;
                ei.segment   = ri.segment;
                std::memcpy(ei.transform, gl_xform, 64);
                expanded_.push_back(ei);

                if ((int)per_slot_count_.size() <= root_slot)
                    per_slot_count_.resize(root_slot + 1, 0);
                per_slot_count_[root_slot]++;
            }
        }

        if (expanded_.empty()) {
            active_slots_.assign(parts_.size(), 0);
            return false;
        }

        // ------------------------------------------------------------------
        // Overflow check: if any part's resolved count > region_cap, grow that
        // cap ×1.5 and recompute ALL regions.
        // ------------------------------------------------------------------
        bool regions_dirty = false;
        for (int s = 0; s < (int)per_slot_count_.size(); ++s) {
            if (s >= (int)parts_.size()) break;
            uint32_t n = per_slot_count_[s];
            if (n > parts_[s].region_cap) {
                uint32_t new_cap = parts_[s].region_cap + parts_[s].region_cap / 2;
                if (new_cap < n) new_cap = n;
                parts_[s].region_cap = new_cap;
                regions_dirty = true;
                // P2-decision telemetry: region growth event.
                printf("GpuCuller: slot %d region_cap %u -> grew for %u instances "
                       "(clusters %u, %zu region bytes)\n",
                       s, new_cap, n, parts_[s].cluster_count,
                       (size_t)parts_[s].cluster_count * (size_t)kMaxLod * (size_t)new_cap * 64);
            }
        }
        if (regions_dirty) recompute_regions();

        // ------------------------------------------------------------------
        // Build GpuInstanceRec staging vector and record active part slots.
        // ------------------------------------------------------------------
        // Clear active_slots_ for this frame; mark slots that receive >= 1 record.
        active_slots_.assign(parts_.size(), 0);

        std::vector<GpuInstanceRec> inst_recs;
        inst_recs.reserve(expanded_.size());

        for (const auto& ei : expanded_) {
            if (ei.part_slot < 0 || ei.part_slot >= (int)parts_.size()) continue;
            const PartGpu& pg = parts_[ei.part_slot];

            GpuInstanceRec rec{};
            std::memcpy(rec.transform, ei.transform, 64);
            rec.part_slot      = (uint32_t)ei.part_slot;
            rec.base_lod       = 0;
            const uint32_t fine_n = pg.fine_cluster_count;
            if (fine_n < pg.cluster_count) {
                if (ei.segment == 0) {
                    rec.cluster_start = pg.cluster_start;
                    rec.cluster_count = fine_n;
                } else {
                    rec.cluster_start = pg.cluster_start + fine_n;
                    rec.cluster_count = pg.cluster_count - fine_n;
                }
            } else {
                rec.cluster_start = pg.cluster_start;
                rec.cluster_count = pg.cluster_count;
            }
            inst_recs.push_back(rec);

            active_slots_[(size_t)ei.part_slot] = 1;
        }

        uint32_t n_records = (uint32_t)inst_recs.size();
        if (n_records == 0) return false;

        // ------------------------------------------------------------------
        // Upload instance data (orphan + sub).
        // ------------------------------------------------------------------
        size_t inst_need = n_records * sizeof(GpuInstanceRec);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_instances_);
        if (inst_need > instances_cap_bytes_) {
            size_t new_cap = instances_cap_bytes_ == 0 ? inst_need
                           : instances_cap_bytes_ + instances_cap_bytes_ / 2;
            if (new_cap < inst_need) new_cap = inst_need;
            glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)new_cap, nullptr, GL_DYNAMIC_DRAW);
            instances_cap_bytes_ = new_cap;
        }
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)inst_need, inst_recs.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    } else {
        // Static world fast-path: active_slots_ already set; no SSBO re-upload needed.
        // Ensure active_slots_ is sized correctly (may have grown with a new part).
        if (active_slots_.size() < parts_.size())
            active_slots_.resize(parts_.size(), 0);
    }

    // Recover n_records for the dispatch (always needed, even on fast-path).
    uint32_t n_records = 0;
    {
        // Count from the expanded_ member (valid on both dirty and fast-path).
        for (const auto& ei : expanded_) {
            if (ei.part_slot >= 0 && ei.part_slot < (int)parts_.size())
                ++n_records;
        }
    }
    if (n_records == 0) return false;

    // ------------------------------------------------------------------
    // Seed cmds SSBO from template (zeros instance_count in every bucket).
    // ------------------------------------------------------------------
    upload_cmd_template();

    // ------------------------------------------------------------------
    // Zero stats SSBO.
    // ------------------------------------------------------------------
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_stats_);
    uint32_t zeros[3] = {0, 0, 0};
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros), zeros);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // ------------------------------------------------------------------
    // Compute max_clusters_per_instance (max over registered parts, min 1).
    // ------------------------------------------------------------------
    uint32_t max_cpi = 1;
    for (const auto& pg : parts_)
        if (pg.cluster_count > max_cpi) max_cpi = pg.cluster_count;

    // ------------------------------------------------------------------
    // Bind SSBOs and dispatch.
    // ------------------------------------------------------------------
    glUseProgram(program_cull_);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_clusters_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo_instances_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo_cmds_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, ssbo_xforms_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_stats_);

    // Flatten planes[6][4] into a contiguous float[24] for glUniform4fv.
    float flat_planes[24];
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 4; ++j)
            flat_planes[i * 4 + j] = planes[i][j];

    glUniform4fv(uloc_planes_, 6, flat_planes);
    glUniform3f(uloc_cam_eye_, cam_eye[0], cam_eye[1], cam_eye[2]);
    glUniform1f(uloc_pixel_budget_, pixel_budget);
    glUniform1f(uloc_min_projected_size_, min_projected_size_);
    glUniform1ui(uloc_instance_count_, n_records);
    glUniform1ui(uloc_max_clusters_per_inst_, max_cpi);

    // ------------------------------------------------------------------
    // HiZ occlusion uniforms.  hiz_enabled goes to the shader as 0 unless the
    // runtime flag is on AND a pyramid was actually built (hiz_valid_) — the
    // first frame after enable (and any MSAA-guard trip) has no pyramid.
    //
    // view_proj: engine ROW-MAJOR vp uploaded with transpose=GL_FALSE.  GL
    // reads the memory column-major, which is exactly the shader-convention
    // VP (= transpose of the C++ storage) — the same implicit transpose that
    // row_major_to_matrix + raylib's matrix upload perform for the draw path,
    // and the same convention extract_frustum_planes documents.
    // ------------------------------------------------------------------
    const int hiz_on = (hiz_enabled_ && hiz_valid_ && hiz_tex_) ? 1 : 0;
    glUniform1i(uloc_hiz_enabled_, hiz_on);
    glUniformMatrix4fv(uloc_view_proj_, 1, GL_FALSE, view_proj);
    if (hiz_on) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hiz_tex_);
        glUniform1i(uloc_hiz_tex_, 0);
        glUniform2f(uloc_hiz_size_, (float)hiz_w_, (float)hiz_h_);
    }

    uint32_t total_threads = n_records * max_cpi;
    uint32_t groups = (total_threads + 63) / 64;
    glDispatchCompute(groups, 1, 1);

    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    if (hiz_on) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUseProgram(0);

    return true;
}

// ---------------------------------------------------------------------------
// draw_indirect — Stage-2: issue glMultiDrawArraysIndirect for every registered
// part, reading commands + transforms directly from the GPU SSBOs written by the
// most recent cull() call.  Caller must have:
//   1. Called BeginMode3D (sets up GL viewport/projection state)
//   2. Called rlDrawRenderBatchActive() to flush any pending raylib batch
//   3. Activated the GPU-driven shader (glUseProgram(shader_gpu.id))
//   4. Set the mvp uniform on that shader
//   5. Called rlDisableBackfaceCulling() if desired
//
// Returns drawn tris (one-frame-late from the previous frame's cmd readback
// when stats_readback_ is false, or the current frame's count when true).
// Stats (stat_culled_ / stat_emitted_) are updated only when stats_readback_
// is true, avoiding a per-frame GPU sync on the hot path.
// ---------------------------------------------------------------------------
int GpuCuller::draw_indirect() {
    if (parts_.empty() || cmd_template_.empty() || !ssbo_cmds_ || !ssbo_xforms_)
        return 0;

    // Bind the xforms SSBO to binding 3 (DrawXforms in raster_gpu_driven.vs).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, ssbo_xforms_);

    // Draw each registered part.
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ssbo_cmds_);

    for (int ps = 0; ps < (int)parts_.size(); ++ps) {
        const PartGpu& pg = parts_[ps];
        if (pg.vao == 0) continue;
        // Skip parts that received no instances this frame (zero indirect cmds).
        if (ps >= (int)active_slots_.size() || active_slots_[(size_t)ps] == 0) continue;
        glBindVertexArray(pg.vao);

        // Part's command range starts at cluster_start * kMaxLod in the cmd array.
        size_t cmd_offset = (size_t)pg.cluster_start * (size_t)kMaxLod * sizeof(DrawArraysCmd);
        GLsizei cmd_count = (GLsizei)(pg.cluster_count * (uint32_t)kMaxLod);

        glMultiDrawArraysIndirect(GL_TRIANGLES,
            (const void*)(uintptr_t)cmd_offset,
            cmd_count,
            0);   // stride 0 = tightly packed
    }

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindVertexArray(0);

    // Gated readbacks: only when stats panel is active.  Both the cmd readback
    // (for tri count) and the stats readback stall the GPU pipeline.  When the
    // gate is off we return the cached tri count from the previous frame.
    if (stats_readback_) {
        // Read back the command buffer to compute the current-frame tri count.
        size_t cmds_bytes = cmd_template_.size() * sizeof(DrawArraysCmd);
        std::vector<DrawArraysCmd> cmds_gpu(cmd_template_.size());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cmds_);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)cmds_bytes, cmds_gpu.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        // Read back stats SSBO.
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_stats_);
        uint32_t stats[3] = {0, 0, 0};
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(stats), stats);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        stat_culled_     = stats[0];
        stat_culled_hiz_ = stats[1];
        stat_emitted_    = stats[2];

        // Recompute tri count from the current-frame cmd data.
        int total_tris = 0;
        for (int ps = 0; ps < (int)parts_.size(); ++ps) {
            const PartGpu& pg = parts_[ps];
            uint32_t base_bucket = pg.cluster_start * (uint32_t)kMaxLod;
            for (uint32_t b = 0; b < (uint32_t)(pg.cluster_count * (uint32_t)kMaxLod); ++b) {
                uint32_t bucket = base_bucket + b;
                if (bucket < (uint32_t)cmds_gpu.size()) {
                    const DrawArraysCmd& cmd = cmds_gpu[bucket];
                    if (cmd.instance_count > 0 && cmd.count >= 3)
                        total_tris += (int)(cmd.count / 3) * (int)cmd.instance_count;
                }
            }
        }
        stat_last_tris_ = total_tris;
    }

    return stat_last_tris_;
}

// ---------------------------------------------------------------------------
// reset — release all per-part GPU state and reinitialize fixed buffers.
// Called on world switch when gpu_cull is active, to prevent stale part slots
// from a previous world being used with a new PartStore.
// ---------------------------------------------------------------------------
void GpuCuller::reset() {
    // Release per-part GL objects.
    for (auto& pg : parts_) {
        if (pg.vao) { glDeleteVertexArrays(1, &pg.vao); pg.vao = 0; }
        if (pg.vbo) { glDeleteBuffers(1, &pg.vbo);      pg.vbo = 0; }
    }
    parts_.clear();
    slot_of_.clear();
    cluster_staging_.clear();
    cmd_template_.clear();

    clusters_cap_bytes_   = 0;
    instances_cap_bytes_  = 0;
    cmds_cap_bytes_       = 0;
    xforms_cap_bytes_     = 0;
    total_xform_slots_    = 0;
    xforms_cap_slots_     = 0;
    stat_culled_          = 0;
    stat_culled_hiz_      = 0;
    stat_emitted_         = 0;
    last_resolved_fp_     = 0;
    last_resolved_count_  = -1;
    cmds_template_dirty_  = false;
    expanded_.clear();
    per_slot_count_.clear();

    // Re-initialize fixed-size buffers (keep same GL names to avoid re-init overhead).
    // ssbo_stats_: reset to zeros.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_stats_);
    uint32_t zeros[3] = {0, 0, 0};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zeros), zeros, GL_DYNAMIC_READ);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Growable SSBOs: orphan them (zero-size placeholder keeps names valid for lazy alloc).
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_clusters_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 1, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_instances_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 1, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cmds_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 1, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (ssbo_cmds_template_) {
        glBindBuffer(GL_COPY_READ_BUFFER, ssbo_cmds_template_);
        glBufferData(GL_COPY_READ_BUFFER, 1, nullptr, GL_STATIC_DRAW);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_xforms_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 1, nullptr, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// ---------------------------------------------------------------------------
// ensure_depth_blit — Task 5 RT: ensure depth_copy_tex_ has a current copy of
// the default framebuffer's depth. When HiZ is already enabled and sized, this
// just blits depth; otherwise it forces a build_hiz pass to allocate textures.
// ---------------------------------------------------------------------------
void GpuCuller::ensure_depth_blit(int screen_w, int screen_h) {
    if (screen_w <= 0 || screen_h <= 0) return;
    if (!depth_copy_tex_ || hiz_w_ != screen_w || hiz_h_ != screen_h) {
        // Need to allocate / reallocate depth textures via build_hiz().
        bool was_enabled = hiz_enabled_;
        hiz_enabled_ = true;
        build_hiz(screen_w, screen_h);
        hiz_enabled_ = was_enabled;
        if (!was_enabled) hiz_valid_ = false;
        return;
    }
    // Textures exist and are the right size; just blit.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, depth_fbo_);
    glBlitFramebuffer(0, 0, screen_w, screen_h,
                      0, 0, screen_w, screen_h,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
// build_hiz — blit default-framebuffer depth into depth_copy_tex_, then copy
// into hiz_tex_ mip 0 (R32F), then downsample_pyramid() for all remaining mips.
//
// Recreates textures/FBO when screen size changes.
// No-op when hiz_enabled_ is false.
// MSAA guard: if the default framebuffer has samples > 0, prints once and keeps
// hiz_enabled_ false.
// ---------------------------------------------------------------------------
void GpuCuller::build_hiz(int screen_w, int screen_h) {
    if (!hiz_enabled_) return;
    if (screen_w <= 0 || screen_h <= 0) return;

    // MSAA guard: if default FB is multisampled, HiZ blit is not valid.
    {
        GLint samples = 0;
        glGetIntegerv(GL_SAMPLES, &samples);
        if (samples > 0) {
            if (!hiz_msaa_warned_) {
                printf("HiZ disabled: MSAA framebuffer\n");
                hiz_msaa_warned_ = true;
            }
            hiz_enabled_ = false;
            hiz_valid_   = false;
            return;
        }
    }

    // Compile hiz_downsample.comp once.
    if (!program_hiz_) {
        std::string err;
        program_hiz_ = compile_compute("shaders_gpu/hiz_downsample.comp", err);
        if (!program_hiz_) {
            printf("HiZ disabled: shader compile failed: %s\n", err.c_str());
            hiz_enabled_ = false;
            hiz_valid_   = false;
            return;
        }
        uloc_hiz_src_       = glGetUniformLocation(program_hiz_, "src");
        uloc_hiz_src_mip_   = glGetUniformLocation(program_hiz_, "src_mip");
        uloc_hiz_dst_size_  = glGetUniformLocation(program_hiz_, "dst_size");
        uloc_hiz_copy_mode_ = glGetUniformLocation(program_hiz_, "copy_mode");
    }

    // Recreate textures/FBO if size changed.
    if (screen_w != hiz_w_ || screen_h != hiz_h_) {
        release_hiz_objects();   // releases depth_copy_tex_, depth_fbo_, hiz_tex_

        hiz_w_ = screen_w;
        hiz_h_ = screen_h;

        // Compute mip chain length.
        int maxdim = screen_w > screen_h ? screen_w : screen_h;
        hiz_mip_levels_ = 1;
        {
            int d = maxdim;
            while (d > 1) { d >>= 1; ++hiz_mip_levels_; }
        }

        // depth_copy_tex_: GL_DEPTH_COMPONENT24, no mips, used as blit destination.
        // Must match the default framebuffer's depth format exactly for glBlitFramebuffer.
        // The viewer window uses no explicit depth format and GLFW defaults to 24-bit depth.
        glGenTextures(1, &depth_copy_tex_);
        glBindTexture(GL_TEXTURE_2D, depth_copy_tex_);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, screen_w, screen_h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);

        // depth_fbo_: wrap depth_copy_tex_ so we can blit into it.
        // A depth-only FBO requires GL_NONE draw/read buffers to be complete.
        glGenFramebuffers(1, &depth_fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, depth_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, depth_copy_tex_, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (fbo_status != GL_FRAMEBUFFER_COMPLETE)
            printf("HiZ: depth_fbo_ incomplete: 0x%X\n", fbo_status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // hiz_tex_: R32F with full mip chain.
        glGenTextures(1, &hiz_tex_);
        glBindTexture(GL_TEXTURE_2D, hiz_tex_);
        glTexStorage2D(GL_TEXTURE_2D, hiz_mip_levels_, GL_R32F, screen_w, screen_h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Blit default-framebuffer depth -> depth_copy_tex_ via depth_fbo_.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, depth_fbo_);
    glBlitFramebuffer(0, 0, screen_w, screen_h,
                      0, 0, screen_w, screen_h,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // Copy pass: depth_copy_tex_ (depth, mip 0) -> hiz_tex_ mip 0 (R32F).
    // Uses copy_mode=1: 1:1 texelFetch of the depth texture, no 2x2 reduce.
    glUseProgram(program_hiz_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depth_copy_tex_);
    glUniform1i(uloc_hiz_src_, 0);   // cached location — avoids per-call glGetUniformLocation
    glBindImageTexture(1, hiz_tex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glUniform1i(uloc_hiz_src_mip_,   0);
    glUniform1i(uloc_hiz_copy_mode_, 1);   // 1:1 copy
    glUniform2i(uloc_hiz_dst_size_,  screen_w, screen_h);

    uint32_t gx = ((uint32_t)screen_w + 7) / 8;
    uint32_t gy = ((uint32_t)screen_h + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Downsample the remaining mips.
    downsample_pyramid();

    // MATTER_HIZ_DEBUG=1: print min/max of a mid mip once per second-ish so a
    // live run can confirm the depth blit produced a real pyramid (all-1.0 =
    // blit failed / nothing drawn; min < 1 = scene depth landed).
    static const bool hiz_debug = [] {
        const char* e = getenv("MATTER_HIZ_DEBUG");
        return e && e[0] == '1';
    }();
    if (hiz_debug) {
        static int frame_ctr = 0;
        if ((frame_ctr++ % 60) == 0) {
            int mip = hiz_mip_levels_ > 5 ? 5 : hiz_mip_levels_ - 1;
            int mw = (hiz_w_ >> mip) > 0 ? (hiz_w_ >> mip) : 1;
            int mh = (hiz_h_ >> mip) > 0 ? (hiz_h_ >> mip) : 1;
            std::vector<float> buf((size_t)mw * mh);
            glBindTexture(GL_TEXTURE_2D, hiz_tex_);
            glGetTexImage(GL_TEXTURE_2D, mip, GL_RED, GL_FLOAT, buf.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            float mn = 1e9f, mx = -1e9f;
            for (float v : buf) { if (v < mn) mn = v; if (v > mx) mx = v; }
            printf("HIZ_DEBUG mip%d %dx%d min=%.6f max=%.6f\n", mip, mw, mh, mn, mx);
            fflush(stdout);
        }
    }

    // Pyramid is now current for this frame: the cull shader may consume it
    // next frame (previous-frame occlusion, conservative).
    hiz_valid_ = true;
}

// ---------------------------------------------------------------------------
// downsample_pyramid — run max-reduce from mip 0 through mip (levels-1).
// Can be called directly by tests after manually filling mip 0 via glTexSubImage2D.
// Requires hiz_tex_ and program_hiz_ to be valid.
// ---------------------------------------------------------------------------
void GpuCuller::downsample_pyramid() {
    if (!hiz_tex_ || !program_hiz_) return;
    if (hiz_mip_levels_ <= 1) return;

    glUseProgram(program_hiz_);

    // Bind hiz_tex_ as sampler at unit 0.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hiz_tex_);
    glUniform1i(uloc_hiz_src_, 0);   // cached location — avoids per-call glGetUniformLocation
    glUniform1i(uloc_hiz_copy_mode_, 0);   // 2x2 max-reduce

    // Keep BASE_LEVEL=0, MAX_LEVEL=full so texelFetch can access any level.
    // Simultaneous read of mip i-1 and write to mip i is defined when levels differ
    // and glMemoryBarrier separates dispatches (GL 4.6 spec).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  hiz_mip_levels_ - 1);

    for (int i = 1; i < hiz_mip_levels_; ++i) {
        // Destination mip i dimensions.
        int dst_w = (hiz_w_ >> i) > 0 ? (hiz_w_ >> i) : 1;
        int dst_h = (hiz_h_ >> i) > 0 ? (hiz_h_ >> i) : 1;

        glBindImageTexture(1, hiz_tex_, i, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);

        glUniform1i(uloc_hiz_src_mip_,  i - 1);
        glUniform2i(uloc_hiz_dst_size_, dst_w, dst_h);

        uint32_t gx = ((uint32_t)dst_w + 7) / 8;
        uint32_t gy = ((uint32_t)dst_h + 7) / 8;
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

// ---------------------------------------------------------------------------
// test_readback_stats — TEST-ONLY: read the stats SSBO and update the private
// stat counters (culled / culled_hiz / emitted).  Call after cull() to get
// current-frame values without issuing a full draw_indirect().
// ---------------------------------------------------------------------------
void GpuCuller::test_readback_stats() {
    if (!ssbo_stats_) return;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_stats_);
    uint32_t stats[3] = {0, 0, 0};
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(stats), stats);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    stat_culled_     = stats[0];
    stat_culled_hiz_ = stats[1];
    stat_emitted_    = stats[2];
}

} // namespace viewer
