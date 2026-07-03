#include "raster_composer.h"
#include "material_registry.h"
#include "rlgl.h"
#include "external/glad.h"
#include <cmath>
#include <cstring>
#include <functional>
#include <tuple>

// NOTE: do NOT include raymath.h — it conflicts with the engine's float3 type
// (precomp.h defines float3 as a plain struct; raymath.h also defines Vector3
// operators that collide). All math here is hand-rolled.

namespace viewer {

// ---------------------------------------------------------------------------
// Row-major 4x4 multiply (same convention as world_composer.cpp:9).
// ---------------------------------------------------------------------------
static void mul16(const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i*4+k] * b[k*4+j];
            out[i*4+j] = s;
        }
}

// ---------------------------------------------------------------------------
// Frustum plane extraction (Gribb-Hartmann, row-major VP matrix).
//
// For a row-major matrix VP (row_i = row i of VP), the clip planes are:
//   left   = row3 + row0
//   right  = row3 - row0
//   bottom = row3 + row1
//   top    = row3 - row1
//   near   = row3 + row2
//   far    = row3 - row2
// Each plane is (a, b, c, d) where ax+by+cz+d >= 0 means inside.
//
// The VP is produced by:
//   view (row-major look-at) then proj (row-major perspective),
//   combined as VP = view * proj.
//
// Plane storage: planes[6][4], each = {a,b,c,d}.
// ---------------------------------------------------------------------------

// Build a row-major look-at view matrix for row-vector convention.
// v_cam = v_world * View, so View is the TRANSPOSE of the standard column-vector look-at.
// Column j of View = j-th camera axis expressed in world space:
//   col0 = right, col1 = true_up, col2 = -forward (GL looks -z), col3 = (0,0,0,1)
// with translation folded into row 3.
static void make_lookat(const float eye[3], const float target[3],
                        const float up[3], float out[16]) {
    // Forward = normalize(target - eye)
    float fx = target[0]-eye[0], fy = target[1]-eye[1], fz = target[2]-eye[2];
    float flen = std::sqrt(fx*fx + fy*fy + fz*fz);
    if (flen < 1e-9f) flen = 1.0f;
    fx /= flen; fy /= flen; fz /= flen;

    // Right = normalize(forward x up)
    float rx = fy*up[2] - fz*up[1];
    float ry = fz*up[0] - fx*up[2];
    float rz = fx*up[1] - fy*up[0];
    float rlen = std::sqrt(rx*rx + ry*ry + rz*rz);
    if (rlen < 1e-9f) rlen = 1.0f;
    rx /= rlen; ry /= rlen; rz /= rlen;

    // True up = right x forward
    float ux = ry*fz - rz*fy;
    float uy = rz*fx - rx*fz;
    float uz = rx*fy - ry*fx;

    // Row-vector convention: M[row=i, col=j] = vp[i*4+j].
    // col0 = (rx,ry,rz,0): M[0,0]=rx, M[1,0]=ry, M[2,0]=rz, M[3,0]=-dot(r,eye)
    // col1 = (ux,uy,uz,0): M[0,1]=ux, M[1,1]=uy, M[2,1]=uz, M[3,1]=-dot(u,eye)
    // col2 = (-fx,-fy,-fz,0): M[0,2]=-fx, M[1,2]=-fy, M[2,2]=-fz, M[3,2]=dot(f,eye)
    // col3 = (0,0,0,1):   M[0,3]=0, M[1,3]=0, M[2,3]=0, M[3,3]=1
    float dot_re = rx*eye[0]+ry*eye[1]+rz*eye[2];
    float dot_ue = ux*eye[0]+uy*eye[1]+uz*eye[2];
    float dot_fe = fx*eye[0]+fy*eye[1]+fz*eye[2];
    out[0]=rx;   out[1]=ux;   out[2]=-fx;  out[3]=0;
    out[4]=ry;   out[5]=uy;   out[6]=-fy;  out[7]=0;
    out[8]=rz;   out[9]=uz;   out[10]=-fz; out[11]=0;
    out[12]=-dot_re; out[13]=-dot_ue; out[14]=dot_fe; out[15]=1;
}

// Build a row-major perspective projection matrix for row-vector convention.
// This is the TRANSPOSE of the standard column-vector OpenGL projection.
// For row-vector convention: v_clip = v_view * Proj, so:
//   w_clip = v_view.z * M[2,3] + v_view.w * M[3,3] = v_view.z * (-1) + 0 = -v_view.z
//   z_clip = v_view.z * M[2,2] + v_view.w * M[3,2] = v_view.z * (-(f+n)/(f-n)) - 2fn/(f-n)
static void make_perspective(float fovy_deg, float aspect,
                              float near_z, float far_z, float out[16]) {
    const float pi = 3.14159265358979323846f;
    float fovy_rad = fovy_deg * pi / 180.0f;
    float f = 1.0f / std::tan(fovy_rad * 0.5f);
    float d = far_z - near_z;
    // Row-major, row-vector convention (transpose of standard GL column-vector proj):
    out[ 0]=f/aspect; out[ 1]=0; out[ 2]=0;                   out[ 3]=0;
    out[ 4]=0;        out[ 5]=f; out[ 6]=0;                   out[ 7]=0;
    out[ 8]=0;        out[ 9]=0; out[10]=-(far_z+near_z)/d;   out[11]=-1.0f;
    out[12]=0;        out[13]=0; out[14]=-(2.0f*far_z*near_z)/d; out[15]=0;
}

// Extract 6 frustum planes from a row-major VP matrix.
// planes[0..5][4] = {a,b,c,d}; point p is inside plane when dot(p,{a,b,c})+d >= 0.
//
// The engine uses row-vector convention: v_clip = v_world * VP, where vp[i*4+j] = VP[row=i,col=j].
// Clip planes in homogeneous: x_clip = v · col0, w_clip = v · col3, etc.
// col0 = {vp[0],vp[4],vp[8],vp[12]}, col3 = {vp[3],vp[7],vp[11],vp[15]}, etc.
// Left   (x+w >= 0): col0 + col3
// Right  (w-x >= 0): col3 - col0
// Bottom (y+w >= 0): col1 + col3
// Top    (w-y >= 0): col3 - col1
// Near   (z+w >= 0): col2 + col3
// Far    (w-z >= 0): col3 - col2
// where col_j[row] = vp[row*4 + j].
static void extract_frustum_planes(const float vp[16], float planes[6][4]) {
    // col0 = vp[0],vp[4],vp[8],vp[12]
    // col1 = vp[1],vp[5],vp[9],vp[13]
    // col2 = vp[2],vp[6],vp[10],vp[14]
    // col3 = vp[3],vp[7],vp[11],vp[15]
    // left  = col0 + col3
    planes[0][0]=vp[0]+vp[3];  planes[0][1]=vp[4]+vp[7];
    planes[0][2]=vp[8]+vp[11]; planes[0][3]=vp[12]+vp[15];
    // right = col3 - col0
    planes[1][0]=vp[3]-vp[0];  planes[1][1]=vp[7]-vp[4];
    planes[1][2]=vp[11]-vp[8]; planes[1][3]=vp[15]-vp[12];
    // bottom = col1 + col3
    planes[2][0]=vp[1]+vp[3];  planes[2][1]=vp[5]+vp[7];
    planes[2][2]=vp[9]+vp[11]; planes[2][3]=vp[13]+vp[15];
    // top = col3 - col1
    planes[3][0]=vp[3]-vp[1];  planes[3][1]=vp[7]-vp[5];
    planes[3][2]=vp[11]-vp[9]; planes[3][3]=vp[15]-vp[13];
    // near = col2 + col3
    planes[4][0]=vp[2]+vp[3];  planes[4][1]=vp[6]+vp[7];
    planes[4][2]=vp[10]+vp[11];planes[4][3]=vp[14]+vp[15];
    // far  = col3 - col2
    planes[5][0]=vp[3]-vp[2];  planes[5][1]=vp[7]-vp[6];
    planes[5][2]=vp[11]-vp[10];planes[5][3]=vp[15]-vp[14];
}

// Build the 6 frustum planes from a Camera3D and viewport aspect.
// fovy is in degrees (raylib convention).
static void camera_frustum_planes(const Camera3D& cam, float aspect,
                                  float planes[6][4]) {
    const float near_z = 0.05f, far_z = 4000.0f;
    float eye[3]    = { cam.position.x, cam.position.y, cam.position.z };
    float target[3] = { cam.target.x,   cam.target.y,   cam.target.z   };
    float up[3]     = { cam.up.x,       cam.up.y,       cam.up.z       };

    float view[16], proj[16], vp[16];
    make_lookat(eye, target, up, view);
    make_perspective(cam.fovy, aspect, near_z, far_z, proj);
    mul16(view, proj, vp);
    extract_frustum_planes(vp, planes);
}

// Transform a point by a row-major 4x4 matrix using row-vector convention:
// v_out = [x,y,z,1] * M  →  out[j] = x*M[0,j] + y*M[1,j] + z*M[2,j] + M[3,j]
// where M[i,j] = m[i*4+j].
static void transform_point(const float m[16], float x, float y, float z,
                             float& ox, float& oy, float& oz) {
    float w = x*m[3] + y*m[7] + z*m[11] + m[15];
    if (std::fabs(w) < 1e-12f) w = 1.0f;
    ox = (x*m[0] + y*m[4] + z*m[8]  + m[12]) / w;
    oy = (x*m[1] + y*m[5] + z*m[9]  + m[13]) / w;
    oz = (x*m[2] + y*m[6] + z*m[10] + m[14]) / w;
}

// Returns true if the AABB (in local space), transformed by inst_transform,
// is entirely outside any of the 6 frustum planes (culled).
// We transform all 8 corners to world space and test.
static bool aabb_culled(const float aabb_min[3], const float aabb_max[3],
                        const float inst[16], const float planes[6][4]) {
    // Build 8 corners in local space
    float cx[2] = { aabb_min[0], aabb_max[0] };
    float cy[2] = { aabb_min[1], aabb_max[1] };
    float cz[2] = { aabb_min[2], aabb_max[2] };

    // Transform 8 corners to world space
    float wx[8], wy[8], wz[8];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k) {
                int idx = i*4 + j*2 + k;
                transform_point(inst, cx[i], cy[j], cz[k], wx[idx], wy[idx], wz[idx]);
            }

    // For each plane: if ALL corners are outside (negative side), the AABB is culled.
    for (int p = 0; p < 6; ++p) {
        float a = planes[p][0], b = planes[p][1], c = planes[p][2], d = planes[p][3];
        bool all_outside = true;
        for (int ci = 0; ci < 8; ++ci) {
            if (a*wx[ci] + b*wy[ci] + c*wz[ci] + d >= 0.0f) {
                all_outside = false;
                break;
            }
        }
        if (all_outside) return true;   // culled
    }
    return false;   // not culled (visible)
}

// Extract the uniform scale from a row-major transform (for projected-size LOD).
static float inst_scale(const float m[16]) {
    float sx = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    float sy = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    float sz = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
    return (sx + sy + sz) * (1.0f / 3.0f);   // average; clusters use same metric
}

// Select per-cluster LOD level: same formula as lod_select.cpp.
// projected_size = cluster.radius * scale / max(dist, 0.01)
// Pick coarsest level whose threshold <= projected_size (thresholds fine->coarse).
static int cluster_lod_select(const LoadedCluster& cl,
                               const float* inst,
                               const float* cam_eye) {
    float scale = inst_scale(inst);
    // Cluster center = midpoint of AABB, transformed to world space.
    float lcx = (cl.aabb_min[0] + cl.aabb_max[0]) * 0.5f;
    float lcy = (cl.aabb_min[1] + cl.aabb_max[1]) * 0.5f;
    float lcz = (cl.aabb_min[2] + cl.aabb_max[2]) * 0.5f;
    float wcx, wcy, wcz;
    transform_point(inst, lcx, lcy, lcz, wcx, wcy, wcz);
    float dx = wcx - cam_eye[0], dy = wcy - cam_eye[1], dz = wcz - cam_eye[2];
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (dist < 0.01f) dist = 0.01f;
    float psize = cl.radius * scale / dist;

    // Same as lod_select::select_level: iterate fine->coarse, pick first >= threshold.
    const auto& thr = cl.thresholds;
    if (thr.empty()) return 0;
    for (size_t i = 0; i < thr.size(); ++i)
        if (psize >= thr[i]) return (int)i;
    return (int)thr.size() - 1;   // coarsest
}

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
        const Camera3D& cam) {

    // ---- Compute fingerprint for camera + all resolved instances ----
    uint64_t fp = 1469598103934665603ull;
    // Fold camera (position, target, fovy) so any camera move invalidates cache.
    fnv_fold(fp, &cam.position, sizeof cam.position);
    fnv_fold(fp, &cam.target,   sizeof cam.target);
    fnv_fold(fp, &cam.fovy,     sizeof cam.fovy);
    for (const auto& r : resolved) {
        fnv_fold(fp, &r.part_hash,  sizeof r.part_hash);
        fnv_fold(fp, &r.lod_level,  sizeof r.lod_level);
        fnv_fold(fp, r.transform,   sizeof r.transform);
    }

    if (fp == last_fp_ && !last_batches_.empty()) {
        // Reuse cached result; update HUD stats from the cached batches.
        stat_cache_hit_       = true;
        stat_culled_clusters_ = 0;   // can't know from cache; keep previous value
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
                        int lv = cluster_lod_select(cl, world, cam_eye);
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
    float table[64 * MATERIAL_FLOATS_PER_DEF] = {0};
    MaterialRegistryPackForGPU(table);
    int count = MaterialRegistryCount();
    SetShaderValueV(shader_, loc_mat_table_, table, SHADER_UNIFORM_FLOAT, count * MATERIAL_FLOATS_PER_DEF);
    SetShaderValue(shader_, loc_mat_count_, &count, SHADER_UNIFORM_INT);
    // Upload WorldLights (stored via set_lights(); defaults reproduce Phase-1 values).
    // Normalize sun_dir inline to avoid raymath.h / float3 conflicts.
    float sdx = lights_.sun_dir[0], sdy = lights_.sun_dir[1], sdz = lights_.sun_dir[2];
    float sdlen = std::sqrt(sdx*sdx + sdy*sdy + sdz*sdz);
    if (sdlen < 1e-6f) sdlen = 1.0f;
    Vector3 sun_dir = (Vector3){ sdx/sdlen, sdy/sdlen, sdz/sdlen };
    Vector3 sun_col = (Vector3){ lights_.sun_color[0], lights_.sun_color[1], lights_.sun_color[2] };
    Vector3 ambient = (Vector3){ lights_.sky_color[0], lights_.sky_color[1], lights_.sky_color[2] };
    SetShaderValue(shader_, loc_sun_dir_,   &sun_dir, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_sun_color_, &sun_col, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_ambient_,   &ambient, SHADER_UNIFORM_VEC3);

    // Bind probe textures to units 4 and 5 (every frame; raylib only manages unit 0).
    if (probes_.valid()) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_3D, probes_.tex_ambient);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_3D, probes_.tex_dominant);
        glActiveTexture(GL_TEXTURE0);   // restore default active unit

        int unit4 = 4, unit5 = 5;
        SetShaderValue(shader_, loc_probe_ambient_,  &unit4, SHADER_UNIFORM_INT);
        SetShaderValue(shader_, loc_probe_dominant_, &unit5, SHADER_UNIFORM_INT);

        Vector3 origin = (Vector3){ probes_.grid.origin[0],
                                    probes_.grid.origin[1],
                                    probes_.grid.origin[2] };
        float cell = probes_.grid.cell;
        Vector3 dims = (Vector3){ (float)probes_.grid.nx,
                                  (float)probes_.grid.ny,
                                  (float)probes_.grid.nz };
        SetShaderValue(shader_, loc_probe_origin_, &origin, SHADER_UNIFORM_VEC3);
        SetShaderValue(shader_, loc_probe_cell_,   &cell,   SHADER_UNIFORM_FLOAT);
        SetShaderValue(shader_, loc_probe_dims_,   &dims,   SHADER_UNIFORM_VEC3);
        int use = 1;
        SetShaderValue(shader_, loc_use_probes_, &use, SHADER_UNIFORM_INT);
    } else {
        int use = 0;
        SetShaderValue(shader_, loc_use_probes_, &use, SHADER_UNIFORM_INT);
    }

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

RasterComposer::~RasterComposer() {
    for (auto& kv : mesh_cache_) UnloadMesh(kv.second);
    if (ready_) UnloadShader(shader_);   // material_ holds the same shader; don't double-free via UnloadMaterial
}

} // namespace viewer
