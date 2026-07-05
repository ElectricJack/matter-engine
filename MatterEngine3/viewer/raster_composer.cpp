#include "raster_composer.h"
#include "material_registry.h"
#include "gpu_culler.h"
// raylib must come before rlgl.h and glad to avoid double-definition of GL types.
#include "raylib.h"
#include "rlgl.h"
#include "external/glad.h"
#include <cmath>
#include <string>

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

// Frustum/matrix helpers live in raster_cull.h (GL-free, shared with the
// GpuCuller compute path and viewer_logic_tests).

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
    // Probe-volume uniforms.
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

// ---------------------------------------------------------------------------
// setup_frame_uniforms — upload sun/probe/material uniforms to any shader.
// Called by draw_gpu_driven().
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

    // Load FS text from shaders/raster.fs (same file used by the base shader).
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
// draw_gpu_driven — set uniforms + issue glMultiDrawArraysIndirect.
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
    if (ready_) UnloadShader(shader_);   // material_ holds the same shader; don't double-free via UnloadMaterial
    if (gpu_ready_) UnloadShader(shader_gpu_);
}

} // namespace viewer
