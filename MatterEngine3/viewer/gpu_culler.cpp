// GpuCuller implementation — Task 5 of the GPU instancing/culling feature.
// Requires GL 4.6 (compute shaders, SSBOs, indirect dispatch).
// Wired into main.cpp frame loop by Task 7; runtime-validated by Task 6.
//
// Matrix conventions (important — read before editing):
//   Engine float[16] is row-major storage of column-vector matrices.
//   Translation lives at t[3], t[7], t[11] (row 0,1,2 col 3).
//   transpose_to_gl(t, out) writes out[c*4+r] = t[r*4+c], producing GL column-major.
//   The GLSL shader then has M[col][row] memory order, which matches standard gl_Position math.

#include "gpu_culler.h"
#include "raster_cull.h"    // mul16, inst_scale
#include "raster_mesh.h"    // row_major_to_matrix

// Raylib must come before glad to avoid double-definition of GL types.
#include "raylib.h"
#include "external/glad.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace viewer {

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
GpuCuller::~GpuCuller() {
    for (auto& pg : parts_) {
        if (pg.vao) glDeleteVertexArrays(1, &pg.vao);
        if (pg.vbo) glDeleteBuffers(1, &pg.vbo);
    }
    if (ssbo_clusters_)  glDeleteBuffers(1, &ssbo_clusters_);
    if (ssbo_instances_) glDeleteBuffers(1, &ssbo_instances_);
    if (ssbo_cmds_)      glDeleteBuffers(1, &ssbo_cmds_);
    if (ssbo_xforms_)    glDeleteBuffers(1, &ssbo_xforms_);
    if (ssbo_stats_)     glDeleteBuffers(1, &ssbo_stats_);
    if (program_cull_)   glDeleteProgram(program_cull_);
}

// ---------------------------------------------------------------------------
// compile_compute — load GLSL source from file, compile as GL_COMPUTE_SHADER,
// link into a program.  Returns program name or 0.
// ---------------------------------------------------------------------------
unsigned GpuCuller::compile_compute(const char* path, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = std::string("cull.comp: cannot open ") + path;
        return 0;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();
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
    program_cull_ = compile_compute("shaders_gpu/cull.comp", err);
    if (!program_cull_) return false;

    // Cache uniform locations.
    uloc_planes_                = glGetUniformLocation(program_cull_, "planes");
    uloc_cam_eye_               = glGetUniformLocation(program_cull_, "cam_eye");
    uloc_pixel_budget_          = glGetUniformLocation(program_cull_, "pixel_budget");
    uloc_instance_count_        = glGetUniformLocation(program_cull_, "instance_count");
    uloc_max_clusters_per_inst_ = glGetUniformLocation(program_cull_, "max_clusters_per_instance");

    // Stats SSBO: 8 bytes (two uint32s: culled, emitted). Fixed size.
    glGenBuffers(1, &ssbo_stats_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_stats_);
    uint32_t zeros[2] = {0, 0};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zeros), zeros, GL_DYNAMIC_READ);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

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
// base_instance fields, then reallocate ssbo_xforms_.
//
// Called whenever any part's region_cap changes (overflow growth ×1.5) or a
// new part is registered.  O(total parts * max_lod * max_clusters) but this
// only fires on a resize event, not every frame.
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

    // Reallocate ssbo_xforms_ to hold total_xform_slots_ mat4s.
    size_t need = (size_t)total_xform_slots_ * 16 * sizeof(float);
    if (need == 0) need = 1;   // keep valid GL object

    if (ssbo_xforms_) glDeleteBuffers(1, &ssbo_xforms_);
    glGenBuffers(1, &ssbo_xforms_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_xforms_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)need, nullptr, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    xforms_cap_bytes_ = need;
}

// ---------------------------------------------------------------------------
// upload_cmd_template — upload the CPU cmd_template_ to ssbo_cmds_,
// grow-reallocating ssbo_cmds_ if needed.
// ---------------------------------------------------------------------------
void GpuCuller::upload_cmd_template() {
    size_t need = cmd_template_.size() * sizeof(DrawArraysCmd);
    if (need == 0) return;

    if (need > cmds_cap_bytes_) {
        size_t new_cap = cmds_cap_bytes_ == 0 ? need
                       : cmds_cap_bytes_ + cmds_cap_bytes_ / 2;
        if (new_cap < need) new_cap = need;

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cmds_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)new_cap, nullptr, GL_DYNAMIC_DRAW);
        cmds_cap_bytes_ = new_cap;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cmds_);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)need, cmd_template_.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
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
    pg.region_cap     = 4096;

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
    } else {
        // Whole-part synthetic cluster.
        cluster_staging_.push_back(pack_whole_part(*lp, part_slot_u));
        pg.cluster_count = 1;
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
// cull — the frame main entry point.
// ---------------------------------------------------------------------------
bool GpuCuller::cull(const std::vector<ResolvedInstance>& resolved,
                     PartStore& store,
                     const float cam_eye[3],
                     const float planes[6][4],
                     float pixel_budget)
{
    if (resolved.empty()) return false;

    // ------------------------------------------------------------------
    // Pass 1: expand resolved instances via ExpandedNode tables.
    // Count instances per part first so we can check/grow region_caps before
    // dispatch (structural overflow prevention: region_cap >= N_p).
    // ------------------------------------------------------------------
    struct ExpandedInst {
        int      part_slot;
        float    transform[16];   // GL column-major (post-transpose_to_gl)
    };

    // First pass: build the full expanded list and record max per-part counts.
    std::vector<ExpandedInst> expanded;
    expanded.reserve(resolved.size() * 4);

    // Per-slot instance counts.
    std::vector<uint32_t> per_slot_count;

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
                std::memcpy(ei.transform, gl_xform, 64);
                expanded.push_back(ei);

                // Track per-slot count.
                if ((int)per_slot_count.size() <= node_slot)
                    per_slot_count.resize(node_slot + 1, 0);
                per_slot_count[node_slot]++;
            }
        } else {
            // No expansion: the root part is drawable directly.
            float gl_xform[16];
            transpose_to_gl(ri.transform, gl_xform);

            ExpandedInst ei;
            ei.part_slot = root_slot;
            std::memcpy(ei.transform, gl_xform, 64);
            expanded.push_back(ei);

            if ((int)per_slot_count.size() <= root_slot)
                per_slot_count.resize(root_slot + 1, 0);
            per_slot_count[root_slot]++;
        }
    }

    if (expanded.empty()) return false;

    // ------------------------------------------------------------------
    // Overflow check: if any part's resolved count > region_cap, grow that
    // cap ×1.5 and recompute ALL regions.
    // ------------------------------------------------------------------
    bool regions_dirty = false;
    for (int s = 0; s < (int)per_slot_count.size(); ++s) {
        if (s >= (int)parts_.size()) break;
        uint32_t n = per_slot_count[s];
        if (n > parts_[s].region_cap) {
            uint32_t new_cap = parts_[s].region_cap + parts_[s].region_cap / 2;
            if (new_cap < n) new_cap = n;
            parts_[s].region_cap = new_cap;
            regions_dirty = true;
        }
    }
    if (regions_dirty) recompute_regions();

    // ------------------------------------------------------------------
    // Build GpuInstanceRec staging vector.
    // ------------------------------------------------------------------
    std::vector<GpuInstanceRec> inst_recs;
    inst_recs.reserve(expanded.size());

    for (const auto& ei : expanded) {
        if (ei.part_slot < 0 || ei.part_slot >= (int)parts_.size()) continue;
        const PartGpu& pg = parts_[ei.part_slot];

        GpuInstanceRec rec{};
        std::memcpy(rec.transform, ei.transform, 64);
        rec.part_slot      = (uint32_t)ei.part_slot;
        rec.base_lod       = 0;   // debug only; cluster-level selection is authoritative
        rec.cluster_start  = pg.cluster_start;
        rec.cluster_count  = pg.cluster_count;
        inst_recs.push_back(rec);
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

    // ------------------------------------------------------------------
    // Seed cmds SSBO from template (zeros instance_count in every bucket).
    // ------------------------------------------------------------------
    upload_cmd_template();

    // ------------------------------------------------------------------
    // Zero stats SSBO.
    // ------------------------------------------------------------------
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_stats_);
    uint32_t zeros[2] = {0, 0};
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
    glUniform1ui(uloc_instance_count_, n_records);
    glUniform1ui(uloc_max_clusters_per_inst_, max_cpi);

    uint32_t total_threads = n_records * max_cpi;
    uint32_t groups = (total_threads + 63) / 64;
    glDispatchCompute(groups, 1, 1);

    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(0);

    return true;
}

// ---------------------------------------------------------------------------
// readback_batches — read back cmds + xforms; rebuild RasterBatch list.
//
// Matrix conversion note:
//   GL ssbo_xforms_ stores mat4 column-major (column c at floats [c*4..c*4+3]).
//   raylib Matrix memory is ROW-major (declaration order m0,m4,m8,m12 = first row),
//   and engine float[16] is also row-major, so row_major_to_matrix is a straight copy.
//   A direct memcpy of GL data into Matrix would therefore yield the TRANSPOSE;
//   we first transpose back to engine layout (transpose_to_gl is self-inverse),
//   then convert via row_major_to_matrix.
// ---------------------------------------------------------------------------
std::vector<RasterBatch> GpuCuller::readback_batches(PartStore& store) {
    std::vector<RasterBatch> out;

    if (cmd_template_.empty() || !ssbo_cmds_ || !ssbo_xforms_) return out;

    // Read back the executed command buffer.
    size_t cmds_bytes = cmd_template_.size() * sizeof(DrawArraysCmd);
    std::vector<DrawArraysCmd> cmds_gpu(cmd_template_.size());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cmds_);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)cmds_bytes, cmds_gpu.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Read back all xform slots that might be populated.
    size_t xforms_bytes = xforms_cap_bytes_;
    std::vector<float> xforms_gpu;
    if (xforms_bytes > 0 && total_xform_slots_ > 0) {
        xforms_gpu.resize(total_xform_slots_ * 16);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_xforms_);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                           (GLsizeiptr)(total_xform_slots_ * 16 * sizeof(float)),
                           xforms_gpu.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Read back stats.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_stats_);
    uint32_t stats[2] = {0, 0};
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(stats), stats);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    stat_culled_  = stats[0];
    stat_emitted_ = stats[1];

    // Walk every bucket; emit a RasterBatch for each with instance_count > 0.
    for (int ps = 0; ps < (int)parts_.size(); ++ps) {
        const PartGpu& pg = parts_[ps];
        for (uint32_t ci = 0; ci < pg.cluster_count; ++ci) {
            uint32_t global_ci = pg.cluster_start + ci;
            for (int lv = 0; lv < kMaxLod; ++lv) {
                uint32_t bucket = global_ci * (uint32_t)kMaxLod + (uint32_t)lv;
                if (bucket >= (uint32_t)cmds_gpu.size()) continue;
                const DrawArraysCmd& cmd = cmds_gpu[bucket];
                if (cmd.instance_count == 0) continue;

                RasterBatch b;
                b.part_hash     = pg.part_hash;
                // cluster_index: UINT32_MAX for synthetic whole-part, else LOCAL cluster
                // index within the part (ci). ensure_mesh uses this to index into
                // lp->clusters[ci].lod_mesh[level], so it must be per-part-local, NOT global.
                b.cluster_index = (pg.cluster_count == 1 &&
                                   cluster_staging_[global_ci].cluster_index == 0xFFFFFFFFu)
                                  ? UINT32_MAX : ci;
                b.level = lv;

                uint32_t base = cmd.base_instance;
                uint32_t n    = cmd.instance_count;
                b.transforms.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    uint32_t xf_slot = base + i;
                    if (xf_slot >= total_xform_slots_) break;
                    // GL column-major → engine row-major (transpose_to_gl is its
                    // own inverse as a memory op) → raylib Matrix.
                    float engine_t[16];
                    transpose_to_gl(xforms_gpu.data() + xf_slot * 16, engine_t);
                    b.transforms.push_back(row_major_to_matrix(engine_t));
                }
                if (!b.transforms.empty())
                    out.push_back(std::move(b));
            }
        }
    }

    return out;
}

} // namespace viewer
