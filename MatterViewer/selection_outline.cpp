#include "selection_outline.h"

#include "imgui.h"
#include "matter/ecs.h"
#include "matter/query.h"
#include "matter/scene.h"
#include "matter/world_session.h"

#include <cmath>
#include <cstdint>

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

// Project world point to screen pixel. Returns false if behind camera.
bool project(const Mat4& vp, int fb_w, int fb_h, const float p[3], ImVec2& screen) {
    float x = vp.m[0]*p[0] + vp.m[4]*p[1] + vp.m[8]*p[2] + vp.m[12];
    float y = vp.m[1]*p[0] + vp.m[5]*p[1] + vp.m[9]*p[2] + vp.m[13];
    float w = vp.m[3]*p[0] + vp.m[7]*p[1] + vp.m[11]*p[2] + vp.m[15];
    if (w <= 0.001f) return false;
    x /= w; y /= w;
    screen.x = (x * 0.5f + 0.5f) * fb_w;
    screen.y = (1.0f - (y * 0.5f + 0.5f)) * fb_h;
    return true;
}

void draw_aabb_wireframe(ImDrawList* dl, const Mat4& vp, int fb_w, int fb_h,
                         const float mn[3], const float mx[3], ImU32 color) {
    float corners[8][3] = {
        {mn[0],mn[1],mn[2]}, {mx[0],mn[1],mn[2]},
        {mx[0],mx[1],mn[2]}, {mn[0],mx[1],mn[2]},
        {mn[0],mn[1],mx[2]}, {mx[0],mn[1],mx[2]},
        {mx[0],mx[1],mx[2]}, {mn[0],mx[1],mx[2]},
    };
    ImVec2 screen[8];
    bool visible[8];
    for (int i = 0; i < 8; ++i)
        visible[i] = project(vp, fb_w, fb_h, corners[i], screen[i]);
    static constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
    };
    for (const auto& e : edges) {
        if (visible[e[0]] && visible[e[1]])
            dl->AddLine(screen[e[0]], screen[e[1]], color, 2.0f);
    }
}

bool aabb_for_object(const SelectedObject& obj, matter::WorldSession& session,
                     float aabb_min[3], float aabb_max[3]) {
    if (obj.kind == SelectedObject::BakedRoot) {
        const uint32_t count = session.instance_count();
        for (uint32_t i = 0; i < count; ++i) {
            matter::InstanceInfo info;
            if (!session.instance_info(i, info)) continue;
            if (info.part_hash != obj.id) continue;
            // Mat4f uses row-major storage with column-vector algebra; the
            // translation lives in the last column of each row.
            const float translation[3] = {
                info.transform[3], info.transform[7], info.transform[11]
            };
            for (int a = 0; a < 3; ++a) {
                aabb_min[a] = translation[a] - 2.0f;
                aabb_max[a] = translation[a] + 2.0f;
            }
            return true;
        }
        return false;
    }

    // SelectedObject::Entity ids live in SceneEntityId space (the stable
    // authored-id hash), matching the pick, tree, and validation code paths.
    bool found = false;
    session.ecs().each(
        [&](flecs::entity, const matter::scene::SceneEntityId& sid,
            const matter::ecs::LocalTransform& lt) {
            if (found || sid.value != obj.id) return;
            found = true;
            aabb_min[0] = lt.translation.x - 0.5f;
            aabb_min[1] = lt.translation.y - 0.5f;
            aabb_min[2] = lt.translation.z - 0.5f;
            aabb_max[0] = lt.translation.x + 0.5f;
            aabb_max[1] = lt.translation.y + 0.5f;
            aabb_max[2] = lt.translation.z + 0.5f;
        });
    return found;
}

} // namespace

void draw_selection_outlines(const SelectionSet& selection,
                             const matter::CameraDesc& camera,
                             int fb_width, int fb_height,
                             matter::WorldSession& session) {
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

        float aabb_min[3], aabb_max[3];
        if (aabb_for_object(obj, session, aabb_min, aabb_max))
            draw_aabb_wireframe(dl, vp, fb_width, fb_height, aabb_min, aabb_max, color);
    }
}

} // namespace viewer
