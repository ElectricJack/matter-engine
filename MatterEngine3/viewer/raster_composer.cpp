#include "raster_composer.h"
#include "material_registry.h"
#include "rlgl.h"
#include "external/glad.h"
#include <cmath>
#include <cstring>
#include <functional>

namespace viewer {

static void mul16(const float* a, const float* b, float* out) {   // row-major, as world_composer.cpp:9
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i*4+k] * b[k*4+j];
            out[i*4+j] = s;
        }
}

std::vector<RasterBatch> RasterComposer::build_batches(
        const std::vector<ResolvedInstance>& resolved, PartStore& store) {
    const int kMaxDepth = 8; const size_t kMaxInstances = 200000;
    std::map<uint64_t, RasterBatch> acc;      // key = (hash<<4)|level
    size_t emitted = 0;

    std::function<void(uint64_t, const float*, int, int)> emit =
        [&](uint64_t hash, const float* world, int lod, int depth) {
            if (depth > kMaxDepth || emitted >= kMaxInstances) return;
            const LoadedPart* lp = store.get_or_load(hash);
            if (!lp) return;
            if (!lp->lod_mesh_data.empty()) {
                int lv = lod < 0 ? 0 : lod;
                if (lv >= (int)lp->lod_mesh_data.size()) lv = (int)lp->lod_mesh_data.size() - 1;
                uint64_t key = (hash << 4) | (uint64_t)lv;
                auto& b = acc[key];
                b.part_hash = hash; b.level = lv;
                b.transforms.push_back(row_major_to_matrix(world));
                ++emitted;
            }
            for (const auto& c : lp->children) {
                float cw[16]; mul16(world, c.transform, cw);
                emit(c.child_resolved_hash, cw, 0, depth + 1);
            }
        };
    for (const auto& r : resolved) emit(r.part_hash, r.transform, r.lod_level, 0);

    std::vector<RasterBatch> out;
    out.reserve(acc.size());
    for (auto& kv : acc) out.push_back(std::move(kv.second));
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

Mesh* RasterComposer::ensure_mesh(uint64_t hash, int level, PartStore& store) {
    uint64_t key = (hash << 4) | (uint64_t)level;
    auto it = mesh_cache_.find(key);
    if (it != mesh_cache_.end()) return &it->second;
    const LoadedPart* lp = store.get_or_load(hash);
    if (!lp || level >= (int)lp->lod_mesh_data.size()) return nullptr;
    const RasterMeshData& d = lp->lod_mesh_data[level];
    if (d.vertex_count == 0) return nullptr;

    Mesh m{};                                  // non-indexed; raylib copies nothing on UploadMesh
    m.vertexCount   = d.vertex_count;
    m.triangleCount = d.vertex_count / 3;
    m.vertices  = const_cast<float*>(d.vertices.data());
    m.normals   = const_cast<float*>(d.normals.data());
    m.colors    = const_cast<unsigned char*>(d.colors.data());
    m.texcoords = const_cast<float*>(d.texcoords.data());
    UploadMesh(&m, false);
    // detach CPU pointers: PartStore owns them; UnloadMesh must not free them
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
        Mesh* m = ensure_mesh(b.part_hash, b.level, store);
        if (!m || b.transforms.empty()) continue;
        DrawMeshInstanced(*m, material_, b.transforms.data(), (int)b.transforms.size());
        tris += m->triangleCount * (int)b.transforms.size();
    }
    rlEnableBackfaceCulling();
    EndMode3D();
    return tris;
}

RasterComposer::~RasterComposer() {
    for (auto& kv : mesh_cache_) UnloadMesh(kv.second);
    if (ready_) UnloadShader(shader_);   // material_ holds the same shader; don't double-free via UnloadMaterial
}

} // namespace viewer
