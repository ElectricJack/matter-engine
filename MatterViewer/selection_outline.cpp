#include "selection_outline.h"
#include "selection_bounds.h"

#include "imgui.h"

#include <cmath>

namespace viewer {
namespace {

struct Mat4 { float m[16]; };

Mat4 look_at(const float eye[3], const float target[3], const float up[3]) {
    float f[3] = { target[0]-eye[0], target[1]-eye[1], target[2]-eye[2] };
    float fl = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    f[0]/=fl; f[1]/=fl; f[2]/=fl;
    float r[3] = { f[1]*up[2] - f[2]*up[1], f[2]*up[0] - f[0]*up[2], f[0]*up[1] - f[1]*up[0] };
    float rl = std::sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    r[0]/=rl; r[1]/=rl; r[2]/=rl;
    float u[3] = { r[1]*f[2] - r[2]*f[1], r[2]*f[0] - r[0]*f[2], r[0]*f[1] - r[1]*f[0] };
    Mat4 out{};
    out.m[0]=r[0]; out.m[4]=r[1]; out.m[8]=r[2];  out.m[12]=-(r[0]*eye[0]+r[1]*eye[1]+r[2]*eye[2]);
    out.m[1]=u[0]; out.m[5]=u[1]; out.m[9]=u[2];  out.m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    out.m[2]=-f[0]; out.m[6]=-f[1]; out.m[10]=-f[2]; out.m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    out.m[3]=0; out.m[7]=0; out.m[11]=0; out.m[15]=1;
    return out;
}

Mat4 perspective(float fov_y, float aspect, float near_p, float far_p) {
    float t = std::tan(fov_y * 0.5f);
    Mat4 out{};
    out.m[0] = 1.0f / (aspect * t);
    out.m[5] = 1.0f / t;
    out.m[10] = -(far_p + near_p) / (far_p - near_p);
    out.m[11] = -1.0f;
    out.m[14] = -(2.0f * far_p * near_p) / (far_p - near_p);
    return out;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += a.m[row + k*4] * b.m[k + col*4];
            out.m[row + col*4] = sum;
        }
    return out;
}

bool project(const Mat4& vp, int fb_w, int fb_h,
             float off_x, float off_y,
             const float p[3], ImVec2& screen) {
    float x = vp.m[0]*p[0] + vp.m[4]*p[1] + vp.m[8]*p[2] + vp.m[12];
    float y = vp.m[1]*p[0] + vp.m[5]*p[1] + vp.m[9]*p[2] + vp.m[13];
    float w = vp.m[3]*p[0] + vp.m[7]*p[1] + vp.m[11]*p[2] + vp.m[15];
    if (w <= 0.001f) return false;
    x /= w; y /= w;
    screen.x = (x * 0.5f + 0.5f) * fb_w + off_x;
    screen.y = (1.0f - (y * 0.5f + 0.5f)) * fb_h + off_y;
    return true;
}

void transform_point(const float mat[16], const float in[3], float out[3]) {
    out[0] = mat[0]*in[0] + mat[1]*in[1] + mat[2]*in[2]  + mat[3];
    out[1] = mat[4]*in[0] + mat[5]*in[1] + mat[6]*in[2]  + mat[7];
    out[2] = mat[8]*in[0] + mat[9]*in[1] + mat[10]*in[2] + mat[11];
}

void draw_obb_wireframe(ImDrawList* dl, const Mat4& vp, int fb_w, int fb_h,
                        float off_x, float off_y,
                        const float world_corners[8][3], ImU32 color) {
    ImVec2 screen[8];
    bool visible[8];
    for (int i = 0; i < 8; ++i)
        visible[i] = project(vp, fb_w, fb_h, off_x, off_y, world_corners[i], screen[i]);
    static constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
    };
    for (const auto& e : edges) {
        if (visible[e[0]] && visible[e[1]])
            dl->AddLine(screen[e[0]], screen[e[1]], color, 2.0f);
    }
}

void make_obb_corners(const float mn[3], const float mx[3],
                      const float mat[16], float out[8][3]) {
    float local[8][3] = {
        {mn[0],mn[1],mn[2]}, {mx[0],mn[1],mn[2]},
        {mx[0],mx[1],mn[2]}, {mn[0],mx[1],mn[2]},
        {mn[0],mn[1],mx[2]}, {mx[0],mn[1],mx[2]},
        {mx[0],mx[1],mx[2]}, {mn[0],mx[1],mx[2]},
    };
    for (int i = 0; i < 8; ++i)
        transform_point(mat, local[i], out[i]);
}

} // namespace

void draw_selection_outlines(const SelectionSet& selection,
                             const matter::CameraDesc& camera,
                             int fb_width, int fb_height,
                             matter::WorldSession& session,
                             float offset_x, float offset_y) {
    if (selection.empty() || fb_width <= 0 || fb_height <= 0) return;

    float eye[3] = {camera.position.x, camera.position.y, camera.position.z};
    float tgt[3] = {camera.target.x, camera.target.y, camera.target.z};
    float up[3] = {camera.up.x, camera.up.y, camera.up.z};

    float dx = tgt[0]-eye[0], dy = tgt[1]-eye[1], dz = tgt[2]-eye[2];
    if (dx*dx + dy*dy + dz*dz < 1e-12f) return;

    float aspect = static_cast<float>(fb_width) / static_cast<float>(fb_height);

    Mat4 view = look_at(eye, tgt, up);
    Mat4 proj = perspective(camera.vertical_fov_radians, aspect,
                           camera.near_plane, camera.far_plane);
    Mat4 vp = multiply(proj, view);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const SelectedObject* primary = selection.primary();

    for (const auto& obj : selection.items()) {
        const bool is_primary = primary && *primary == obj;
        const ImU32 color = is_primary ? IM_COL32(255, 200, 0, 255)
                                       : IM_COL32(100, 180, 255, 200);

        SelectionBounds sb;
        if (bounds_for_object(obj, session, sb)) {
            float corners[8][3];
            make_obb_corners(sb.local_min, sb.local_max, sb.world_matrix, corners);
            draw_obb_wireframe(dl, vp, fb_width, fb_height, offset_x, offset_y, corners, color);
        }
    }
}

} // namespace viewer
