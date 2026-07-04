#include "raster_composer.h"
#include "raster_cull.h"
#include "material_registry.h"
#include "gpu_culler.h"
// raylib must come before rlgl.h and glad to avoid double-definition of GL types.
#include "raylib.h"
#include "rlgl.h"
#include "external/glad.h"
#include <cmath>
#include <cstring>
#include <functional>
#include <sstream>
#include <tuple>

// NOTE: do NOT include raymath.h — it conflicts with the engine's float3 type
// (precomp.h defines float3 as a plain struct; raymath.h also defines Vector3
// operators that collide). All math here is hand-rolled.

// Multiply two raylib Matrix values (column-major layout: M[col][row] in memory).
// Matches the standard column-vector convention used by raylib's DrawMeshInstanced.
static Matrix mat_mul(Matrix a, Matrix b) {
    // a and b store column-major; raw pointer indexing col*4+row.
    const float* af = &a.m0;
    const float* bf = &b.m0;
    float r[16];
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += af[k*4 + row] * bf[col*4 + k];
            r[col*4 + row] = s;
        }
    Matrix out;
    float* of = &out.m0;
    for (int i = 0; i < 16; ++i) of[i] = r[i];
    return out;
}

namespace viewer {

// mul16 / make_lookat / make_perspective / extract_frustum_planes /
// camera_frustum_planes_raw now live in raster_cull.h (GL-free, shared with
// viewer_logic_tests and the upcoming GpuCuller compute path).

// Build the 6 frustum planes from a Camera3D and viewport aspect.
// Unpacks Camera3D fields and delegates to the GL-free raw version.
static void camera_frustum_planes(const Camera3D& cam, float aspect,
                                  float planes[6][4]) {
    float eye[3]    = { cam.position.x, cam.position.y, cam.position.z };
    float target[3] = { cam.target.x,   cam.target.y,   cam.target.z   };
    float up[3]     = { cam.up.x,       cam.up.y,       cam.up.z       };
    camera_frustum_planes_raw(eye, target, up, cam.fovy, aspect, planes);
}

// transform_point / aabb_culled / inst_scale / cluster_lod_select live in
// raster_cull.h (shared with viewer_logic_tests).

// ---------------------------------------------------------------------------
// FNV-1a fold helper (matches world_composer.cpp).
// ---------------------------------------------------------------------------
static void fnv_fold(uint64_t& fp, const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) fp = (fp ^ b[i]) * 1099511628211ull;
}

// ---------------------------------------------------------------------------
// build_batches — non-static (needs to write stat_ counters + fingerprint cache)
// ---------------------------------------------------------------------------
std::vector<RasterBatch> RasterComposer::build_batches(
        const std::vector<ResolvedInstance>& resolved,
        PartStore& store,
        const Camera3D& cam,
        uint64_t world_version) {

    // ---- Compute fingerprint: (camera, world_version, per-instance (part_hash, lod)) ----
    // Transform bytes are NOT hashed: transforms only change via world deltas, which
    // bump world_version (Stage 1 — drops ~3.3 MB/frame of FNV input).
    uint64_t fp = 1469598103934665603ull;
    // Fold camera (position, target, fovy) so any camera move invalidates cache.
    fnv_fold(fp, &cam.position, sizeof cam.position);
    fnv_fold(fp, &cam.target,   sizeof cam.target);
    fnv_fold(fp, &cam.fovy,     sizeof cam.fovy);
    fnv_fold(fp, &world_version, sizeof(world_version));
    fnv_fold(fp, &pixel_budget_, sizeof(pixel_budget_));
    for (const auto& r : resolved) {
        fnv_fold(fp, &r.part_hash,  sizeof r.part_hash);
        fnv_fold(fp, &r.lod_level,  sizeof r.lod_level);
    }

    if (last_valid_ && fp == last_fp_) {
        // Reuse cached result; update HUD stats from the cached batches.
        stat_cache_hit_       = true;
        stat_culled_clusters_ = 0;   // reset to 0: last frame's culled count would be misleading
        stat_batches_         = last_batches_.size();
        stat_drawn_tris_      = 0;
        // drawn_tris is GL-only; caller queries after draw(); leave at 0.
        return last_batches_;
    }
    stat_cache_hit_ = false;

    // ---- Build frustum planes ----
    float aspect = 16.0f / 9.0f;   // default; overridden below if GL is available
    {
        int sw = GetScreenWidth(), sh = GetScreenHeight();
        if (sw > 0 && sh > 0) aspect = (float)sw / (float)sh;
    }
    float planes[6][4];
    camera_frustum_planes(cam, aspect, planes);

    float cam_eye[3] = { cam.position.x, cam.position.y, cam.position.z };

    const int kMaxDepth = 8;
    const size_t kMaxInstances = 200000;

    // Batch key: (part_hash, cluster_index, level)
    using BatchKey = std::tuple<uint64_t, uint32_t, uint32_t>;
    std::map<BatchKey, RasterBatch> acc;
    size_t emitted = 0;
    size_t culled  = 0;

    std::function<void(uint64_t, const float*, int, int)> emit =
        [&](uint64_t hash, const float* world, int lod, int depth) {
            if (depth > kMaxDepth || emitted >= kMaxInstances) return;
            const LoadedPart* lp = store.get_or_load(hash);
            if (!lp) return;

            if (!lp->lod_mesh_data.empty()) {
                if (!lp->clusters.empty()) {
                    // ---- Clustered path (v3 or v2-synthetic clusters) ----
                    for (uint32_t ci = 0; ci < (uint32_t)lp->clusters.size(); ++ci) {
                        const LoadedCluster& cl = lp->clusters[ci];

                        // Frustum cull: transform cluster AABB corners by instance transform.
                        if (aabb_culled(cl.aabb_min, cl.aabb_max, world, planes)) {
                            ++culled;
                            continue;
                        }

                        // Per-cluster LOD selection.
                        int lv = cluster_lod_select(cl, world, cam_eye, pixel_budget_);
                        // lv is index into cl.thresholds / cl.lod_mesh; clamp defensively.
                        if (lv >= (int)cl.lod_mesh.size())
                            lv = (int)cl.lod_mesh.size() - 1;
                        int mesh_idx = cl.lod_mesh[lv];
                        if (mesh_idx < 0 || mesh_idx >= (int)lp->lod_mesh_data.size())
                            continue;

                        BatchKey key{ hash, ci, (uint32_t)lv };
                        auto& b = acc[key];
                        b.part_hash     = hash;
                        b.cluster_index = ci;
                        b.level         = lv;
                        b.transforms.push_back(row_major_to_matrix(world));
                        ++emitted;
                    }
                } else {
                    // ---- Whole-part path (compositional, no clusters) ----
                    int lv = lod < 0 ? 0 : lod;
                    if (lv >= (int)lp->lod_mesh_data.size())
                        lv = (int)lp->lod_mesh_data.size() - 1;
                    static constexpr uint32_t kWholePart = UINT32_MAX;
                    BatchKey key{ hash, kWholePart, (uint32_t)lv };
                    auto& b = acc[key];
                    b.part_hash     = hash;
                    b.cluster_index = kWholePart;
                    b.level         = lv;
                    b.transforms.push_back(row_major_to_matrix(world));
                    ++emitted;
                }
            }

            // Recurse into children (compositional parts).
            for (const auto& c : lp->children) {
                float cw[16]; mul16(world, c.transform, cw);
                emit(c.child_resolved_hash, cw, 0, depth + 1);
            }
        };

    for (const auto& r : resolved)
        emit(r.part_hash, r.transform, r.lod_level, 0);

    std::vector<RasterBatch> out;
    out.reserve(acc.size());
    for (auto& kv : acc) out.push_back(std::move(kv.second));

    // Update HUD stats.
    stat_batches_         = out.size();
    stat_culled_clusters_ = culled;
    stat_drawn_tris_      = 0;   // filled in by draw()
    stat_cache_hit_       = false;

    // Cache for next frame.
    last_fp_      = fp;
    last_valid_   = true;
    last_batches_ = out;

    return out;
}

bool RasterComposer::init(std::string& err) {
    shader_ = LoadShader("shaders/raster.vs", "shaders/raster.fs");
    if (shader_.id == 0) { err = "raster shader failed to load"; return false; }
    if (shader_.locs[SHADER_LOC_VERTEX_INSTANCE_TX] == -1)      // defensive; raylib auto-resolves
        shader_.locs[SHADER_LOC_VERTEX_INSTANCE_TX] = GetShaderLocationAttrib(shader_, "instanceTransform");
    loc_sun_dir_   = GetShaderLocation(shader_, "sunDir");
    loc_sun_color_ = GetShaderLocation(shader_, "sunColor");
    loc_ambient_   = GetShaderLocation(shader_, "ambientColor");
    loc_mat_table_ = GetShaderLocation(shader_, "materialTable");
    loc_mat_count_ = GetShaderLocation(shader_, "materialCount");
    // Probe-volume uniforms (Task 6).
    loc_probe_ambient_  = GetShaderLocation(shader_, "probeAmbient");
    loc_probe_dominant_ = GetShaderLocation(shader_, "probeDominant");
    loc_probe_origin_   = GetShaderLocation(shader_, "probeOrigin");
    loc_probe_cell_     = GetShaderLocation(shader_, "probeCell");
    loc_probe_dims_     = GetShaderLocation(shader_, "probeDims");
    loc_use_probes_     = GetShaderLocation(shader_, "useProbes");
    material_ = LoadMaterialDefault();
    material_.shader = shader_;
    ready_ = true;
    return true;
}

Mesh* RasterComposer::ensure_mesh(uint64_t hash, int cluster_index, int level, PartStore& store) {
    auto key = std::make_tuple(hash, (uint32_t)cluster_index, level);
    auto it = mesh_cache_.find(key);
    if (it != mesh_cache_.end()) return &it->second;

    const LoadedPart* lp = store.get_or_load(hash);
    if (!lp) return nullptr;

    // Determine the lod_mesh_data index.
    int mesh_idx = -1;
    if (cluster_index == (int)UINT32_MAX) {
        // Whole-part path: lod_mesh_data[level] is the whole-part LOD.
        mesh_idx = level;
    } else {
        // Clustered path: look up lod_mesh[level] from the cluster.
        if (cluster_index >= 0 && cluster_index < (int)lp->clusters.size()) {
            const LoadedCluster& cl = lp->clusters[cluster_index];
            if (level >= 0 && level < (int)cl.lod_mesh.size())
                mesh_idx = cl.lod_mesh[level];
        }
    }
    if (mesh_idx < 0 || mesh_idx >= (int)lp->lod_mesh_data.size()) return nullptr;

    const RasterMeshData& d = lp->lod_mesh_data[mesh_idx];
    if (d.vertex_count == 0) return nullptr;

    Mesh m{};
    m.vertexCount   = d.vertex_count;
    m.triangleCount = d.vertex_count / 3;
    m.vertices  = const_cast<float*>(d.vertices.data());
    m.normals   = const_cast<float*>(d.normals.data());
    m.colors    = const_cast<unsigned char*>(d.colors.data());
    m.texcoords = const_cast<float*>(d.texcoords.data());
    UploadMesh(&m, false);
    // Detach CPU pointers: PartStore owns them.
    m.vertices = nullptr; m.normals = nullptr; m.colors = nullptr; m.texcoords = nullptr;
    return &(mesh_cache_[key] = m);
}

int RasterComposer::draw(const std::vector<RasterBatch>& batches, PartStore& store,
                         const Camera3D& cam) {
    if (!ready_) return 0;

    // Upload WorldLights + probe + material uniforms using the shared helper.
    setup_frame_uniforms(shader_,
        loc_sun_dir_, loc_sun_color_, loc_ambient_,
        loc_mat_table_, loc_mat_count_,
        loc_probe_ambient_, loc_probe_dominant_,
        loc_probe_origin_, loc_probe_cell_, loc_probe_dims_, loc_use_probes_);

    int tris = 0;
    BeginMode3D(cam);
    rlDisableBackfaceCulling();   // mesh-session winding is not guaranteed consistent
    for (const auto& b : batches) {
        Mesh* m = ensure_mesh(b.part_hash, (int)b.cluster_index, b.level, store);
        if (!m || b.transforms.empty()) continue;
        DrawMeshInstanced(*m, material_, b.transforms.data(), (int)b.transforms.size());
        tris += m->triangleCount * (int)b.transforms.size();
    }
    rlEnableBackfaceCulling();
    EndMode3D();
    stat_drawn_tris_ = (size_t)tris;
    return tris;
}

// ---------------------------------------------------------------------------
// setup_frame_uniforms — upload sun/probe/material uniforms to any shader.
// Shares logic between draw() (using shader_) and draw_gpu_driven() (shader_gpu_).
// ---------------------------------------------------------------------------
void RasterComposer::setup_frame_uniforms(Shader& sh,
    int loc_sun, int loc_sun_col, int loc_amb,
    int loc_mat, int loc_cnt,
    int loc_pa, int loc_pd, int loc_po, int loc_pc, int loc_pdims, int loc_up)
{
    float table[64 * MATERIAL_FLOATS_PER_DEF] = {0};
    MaterialRegistryPackForGPU(table);
    int count = MaterialRegistryCount();
    SetShaderValueV(sh, loc_mat, table, SHADER_UNIFORM_FLOAT, count * MATERIAL_FLOATS_PER_DEF);
    SetShaderValue(sh, loc_cnt, &count, SHADER_UNIFORM_INT);

    float sdx = lights_.sun_dir[0], sdy = lights_.sun_dir[1], sdz = lights_.sun_dir[2];
    float sdlen = std::sqrt(sdx*sdx + sdy*sdy + sdz*sdz);
    if (sdlen < 1e-6f) sdlen = 1.0f;
    Vector3 sun_dir = (Vector3){ sdx/sdlen, sdy/sdlen, sdz/sdlen };
    Vector3 sun_col = (Vector3){ lights_.sun_color[0], lights_.sun_color[1], lights_.sun_color[2] };
    Vector3 ambient = (Vector3){ lights_.sky_color[0], lights_.sky_color[1], lights_.sky_color[2] };
    SetShaderValue(sh, loc_sun,     &sun_dir, SHADER_UNIFORM_VEC3);
    SetShaderValue(sh, loc_sun_col, &sun_col, SHADER_UNIFORM_VEC3);
    SetShaderValue(sh, loc_amb,     &ambient, SHADER_UNIFORM_VEC3);

    if (probes_.valid()) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_3D, probes_.tex_ambient);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_3D, probes_.tex_dominant);
        glActiveTexture(GL_TEXTURE0);
        int unit4 = 4, unit5 = 5;
        SetShaderValue(sh, loc_pa, &unit4, SHADER_UNIFORM_INT);
        SetShaderValue(sh, loc_pd, &unit5, SHADER_UNIFORM_INT);
        Vector3 origin = (Vector3){ probes_.grid.origin[0], probes_.grid.origin[1], probes_.grid.origin[2] };
        float cell = probes_.grid.cell;
        Vector3 dims = (Vector3){ (float)probes_.grid.nx, (float)probes_.grid.ny, (float)probes_.grid.nz };
        SetShaderValue(sh, loc_po,    &origin, SHADER_UNIFORM_VEC3);
        SetShaderValue(sh, loc_pc,    &cell,   SHADER_UNIFORM_FLOAT);
        SetShaderValue(sh, loc_pdims, &dims,   SHADER_UNIFORM_VEC3);
        int use = 1;
        SetShaderValue(sh, loc_up, &use, SHADER_UNIFORM_INT);
    } else {
        int use = 0;
        SetShaderValue(sh, loc_up, &use, SHADER_UNIFORM_INT);
    }
}

// ---------------------------------------------------------------------------
// init_gpu_driven — load raster_gpu_driven.vs + patched raster.fs (#version 460).
// ---------------------------------------------------------------------------
bool RasterComposer::init_gpu_driven(std::string& err) {
    // Load VS text from shaders_gpu/raster_gpu_driven.vs.
    char* vs_text = LoadFileText("shaders_gpu/raster_gpu_driven.vs");
    if (!vs_text) {
        err = "init_gpu_driven: cannot open shaders_gpu/raster_gpu_driven.vs";
        return false;
    }

    // Load FS text from shaders/raster.fs (same file used by draw()).
    char* fs_text_raw = LoadFileText("shaders/raster.fs");
    if (!fs_text_raw) {
        UnloadFileText(vs_text);
        err = "init_gpu_driven: cannot open shaders/raster.fs";
        return false;
    }

    // Patch FS: replace the leading "#version 330" with "#version 460".
    std::string fs_str(fs_text_raw);
    {
        const std::string old_ver = "#version 330";
        const std::string new_ver = "#version 460";
        auto pos = fs_str.find(old_ver);
        if (pos != std::string::npos)
            fs_str.replace(pos, old_ver.size(), new_ver);
    }
    UnloadFileText(fs_text_raw);

    shader_gpu_ = LoadShaderFromMemory(vs_text, fs_str.c_str());
    UnloadFileText(vs_text);

    if (shader_gpu_.id == 0) {
        err = "init_gpu_driven: LoadShaderFromMemory failed";
        return false;
    }

    loc_gpu_mvp_         = GetShaderLocation(shader_gpu_, "mvp");
    loc_gpu_sun_dir_     = GetShaderLocation(shader_gpu_, "sunDir");
    loc_gpu_sun_color_   = GetShaderLocation(shader_gpu_, "sunColor");
    loc_gpu_ambient_     = GetShaderLocation(shader_gpu_, "ambientColor");
    loc_gpu_mat_table_   = GetShaderLocation(shader_gpu_, "materialTable");
    loc_gpu_mat_count_   = GetShaderLocation(shader_gpu_, "materialCount");
    loc_gpu_probe_amb_   = GetShaderLocation(shader_gpu_, "probeAmbient");
    loc_gpu_probe_dom_   = GetShaderLocation(shader_gpu_, "probeDominant");
    loc_gpu_probe_orig_  = GetShaderLocation(shader_gpu_, "probeOrigin");
    loc_gpu_probe_cell_  = GetShaderLocation(shader_gpu_, "probeCell");
    loc_gpu_probe_dims_  = GetShaderLocation(shader_gpu_, "probeDims");
    loc_gpu_use_probes_  = GetShaderLocation(shader_gpu_, "useProbes");

    gpu_ready_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// draw_gpu_driven — Stage-2: set uniforms + issue glMultiDrawArraysIndirect.
// ---------------------------------------------------------------------------
int RasterComposer::draw_gpu_driven(GpuCuller& culler, PartStore& /*store*/,
                                     const Camera3D& cam) {
    if (!gpu_ready_) return 0;

    // Upload frame uniforms to shader_gpu_ (sun/probes/material table).
    setup_frame_uniforms(shader_gpu_,
        loc_gpu_sun_dir_, loc_gpu_sun_color_, loc_gpu_ambient_,
        loc_gpu_mat_table_, loc_gpu_mat_count_,
        loc_gpu_probe_amb_, loc_gpu_probe_dom_,
        loc_gpu_probe_orig_, loc_gpu_probe_cell_,
        loc_gpu_probe_dims_, loc_gpu_use_probes_);

    // BeginMode3D sets the GL viewport + loads the view/projection matrices into
    // rlgl's internal state, which we then read via rlGetMatrix*.
    BeginMode3D(cam);
    // Flush any pending raylib batch before switching shaders.
    rlDrawRenderBatchActive();

    // Compute MVP following DrawMeshInstanced's formula:
    //   matModelView = mat_mul(rlGetMatrixTransform(), rlGetMatrixModelview())
    //   mvp          = mat_mul(matModelView, rlGetMatrixProjection())
    // rlGetMatrixTransform() is identity unless push/pop transform is active.
    Matrix matModelView = mat_mul(rlGetMatrixTransform(), rlGetMatrixModelview());
    Matrix mvp          = mat_mul(matModelView, rlGetMatrixProjection());

    // Activate the GPU-driven shader and upload mvp.
    glUseProgram(shader_gpu_.id);
    rlSetUniformMatrix(loc_gpu_mvp_, mvp);

    // Disable backface culling (mesh-session winding not guaranteed).
    rlDisableBackfaceCulling();

    int tris = culler.draw_indirect();

    rlEnableBackfaceCulling();

    // Restore default shader so raylib's subsequent draws (HUD, gizmos) work.
    rlEnableShader(rlGetShaderIdDefault());

    EndMode3D();

    stat_drawn_tris_ = (size_t)tris;
    return tris;
}

RasterComposer::~RasterComposer() {
    for (auto& kv : mesh_cache_) UnloadMesh(kv.second);
    if (ready_) UnloadShader(shader_);   // material_ holds the same shader; don't double-free via UnloadMaterial
    if (gpu_ready_) UnloadShader(shader_gpu_);
}

} // namespace viewer
