// MatterEditor/src/selection_outline.cpp
//
// The editor's two selection/debug wireframe overlays. They take opposite
// routes to the screen and it matters which is which:
//
//  - `submit_selection_overlay_lines` builds world-space line vertices for
//    every selected object's oriented box and hands them to
//    `WorldSession::submit_overlay_lines`. The ENGINE draws them, depth-tested
//    against the scene, so a box behind terrain is correctly occluded. It must
//    be called before `WorldSession::render()`, and it must be called EVERY
//    frame — the session clears its overlay buffer after each render.
//  - `draw_frozen_cull_frustum` projects on the CPU and paints onto ImGui's
//    foreground draw list, so it always sits on top of everything. Call it
//    between ImGui::NewFrame and ImGui::Render.
//
// Vertex format for the submitted lines: interleaved {x, y, z, r, g, b, a},
// two vertices per segment, drawn as a LINE_LIST. Positions are world metres,
// colours normalized 0-1. An empty selection submits a null pointer, which is
// how the engine is told to clear the overlay rather than keep the last frame's
// lines. Primary selection draws orange, secondary draws blue-violet.
//
// Matrix conventions — there are two in play and they are NOT the same:
//  - the private `Mat4` below (look_at / perspective / multiply / project) is
//    COLUMN-major, indexed `m[row + col*4]`;
//  - `SelectionBounds::world_matrix`, consumed by `transform_point`, is
//    ROW-major with translation in elements 3, 7 and 11.
// `transform_point` is the only place an object matrix is applied and it is
// written for the row-major layout; nothing converts between the two.
//
// The private projection helpers are a plain OpenGL-style pinhole model. They
// do not reproduce the renderer's reversed-Z projection or its temporal jitter
// — see the note on `draw_frozen_cull_frustum` for why that is deliberate for
// a debug line.

#include "selection_outline.h"
#include "selection_bounds.h"

#include "imgui.h"
#include "matter/world_session.h"

#include <cmath>
#include <memory>
#include <vector>

namespace viewer {
namespace {

// COLUMN-major 4x4, indexed `m[row + col*4]` — the convention `look_at`,
// `perspective`, `multiply` and `project` below all share. Distinct from the
// row-major `SelectionBounds::world_matrix` these functions never see.
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

// World point -> framebuffer pixel via `vp`. Returns false for a point at or
// behind the eye (clip w <= 0.001), in which case `screen` is left untouched —
// callers use this to drop any line with an unprojectable endpoint rather than
// drawing a wild segment across the viewport. `off_x`/`off_y` shift the result
// into the viewport's position within the window. Y is flipped for ImGui's
// top-left origin.
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

// Apply a ROW-major 4x4 (translation in elements 3, 7, 11) to a point,
// assuming the bottom row is [0 0 0 1]. This is the layout
// `SelectionBounds::world_matrix` uses — do NOT pass a `Mat4` here.
void transform_point(const float mat[16], const float in[3], float out[3]) {
    out[0] = mat[0]*in[0] + mat[1]*in[1] + mat[2]*in[2]  + mat[3];
    out[1] = mat[4]*in[0] + mat[5]*in[1] + mat[6]*in[2]  + mat[7];
    out[2] = mat[8]*in[0] + mat[9]*in[1] + mat[10]*in[2] + mat[11];
}

// Append the box's 12 edges as 24 line-list vertices to `out`, in the engine's
// interleaved {x, y, z, r, g, b, a} overlay format (7 floats per vertex, so
// `out.size() / 7` is the vertex count). Corner order must match
// `make_obb_corners`: 0-3 are the min-z face counter-clockwise, 4-7 the max-z
// face. Colour components are normalized 0-1 and are applied uniformly to
// every vertex.
void emit_obb_edges(std::vector<float>& out,
                    const float world_corners[8][3],
                    float r, float g, float b, float a) {
    static constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
    };
    for (const auto& e : edges) {
        const float* p0 = world_corners[e[0]];
        const float* p1 = world_corners[e[1]];
        out.insert(out.end(), {p0[0], p0[1], p0[2], r, g, b, a});
        out.insert(out.end(), {p1[0], p1[1], p1[2], r, g, b, a});
    }
}

// Expand a local-space AABB into its 8 world-space corners through a ROW-major
// matrix. Corner order is the contract `emit_obb_edges` depends on: 0-3 walk
// the min-z face, 4-7 the matching max-z face.
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

// Rebuilds the whole vertex buffer from scratch every call — there is no
// incremental path, and none is needed: the session clears its overlay after
// each render, so the editor has to resubmit regardless. An empty selection,
// or a selection whose every object failed to resolve, submits nothing, which
// clears the overlay.
//
// The primary object is drawn orange and the rest blue-violet, so a multi-
// selection still shows which one the gizmo and the range-extend anchor on.
// Objects `bounds_for_object` cannot resolve are silently skipped — a stale
// entry survives here for one frame until SelectionSet::validate prunes it.
void submit_selection_overlay_lines(const SelectionSet& selection,
                                    matter::WorldSession& session) {
    if (selection.empty()) {
        session.submit_overlay_lines(nullptr, 0);
        return;
    }

    std::vector<float> vertices;
    const SelectedObject* primary = selection.primary();

    // ONE ECS scan for the whole selection — bounds_for_object would spend one
    // per item, every frame (see selection_bounds.h).
    const std::vector<SelectedObject>& items = selection.items();
    std::vector<SelectionBounds> bounds(items.size());
    // Not std::vector<bool>: that specialization has no contiguous bool*.
    std::unique_ptr<bool[]> resolved(new bool[items.size()]);
    bounds_for_objects(items.data(), items.size(), session, bounds.data(),
                       resolved.get());

    for (size_t i = 0; i < items.size(); ++i) {
        if (!resolved[i]) continue;
        const bool is_primary = primary && *primary == items[i];
        const float r = is_primary ? 1.0f : 0.392f;
        const float g = is_primary ? 0.784f : 0.706f;
        const float b = is_primary ? 0.0f : 1.0f;
        const float a = is_primary ? 1.0f : 0.784f;

        float corners[8][3];
        make_obb_corners(bounds[i].local_min, bounds[i].local_max,
                         bounds[i].world_matrix, corners);
        emit_obb_edges(vertices, corners, r, g, b, a);
    }

    const uint32_t vertex_count =
        static_cast<uint32_t>(vertices.size() / 7);
    session.submit_overlay_lines(
        vertices.empty() ? nullptr : vertices.data(), vertex_count);
}

void draw_selection_outlines(const SelectionSet& selection,
                             const matter::CameraDesc& camera,
                             int fb_width, int fb_height,
                             matter::WorldSession& session,
                             float offset_x, float offset_y) {
    // Intentionally empty. Selection boxes moved to the depth-tested engine
    // overlay (submit_selection_overlay_lines above); the frozen-cull frustum
    // has its own entry point below. The signature is kept as the hook for any
    // future 2D-only overlay, so every argument is voided rather than removed.
    (void)selection; (void)camera; (void)fb_width; (void)fb_height;
    (void)session; (void)offset_x; (void)offset_y;
}

// The frozen cull frustum (M4 inspection aid). Same projection helpers as the
// selection outlines above, and the same caveat: this is a SKETCH of where the
// cull camera was, rebuilt from the pose the editor captured, not a readback of
// the planes the shader is testing. It ignores the temporal jitter and any
// reversed-Z detail in the renderer's own projection, which move the outline by
// well under a pixel at any distance you would be inspecting from. A debug line
// that needed those to be worth drawing would be a worse debug line.
//
// Drawn to a depth of `depth_limit` rather than the camera's far plane: a 5 km
// far plane projects to a shape whose far face is off-screen and whose sides
// are two nearly-parallel lines, which reads as nothing at all. The near face
// and a truncated far face read as a frustum.
void draw_frozen_cull_frustum(const matter::CameraDesc& frozen,
                              const matter::CameraDesc& live,
                              int fb_width, int fb_height,
                              float depth_limit,
                              float offset_x, float offset_y) {
    if (fb_width <= 0 || fb_height <= 0) return;

    float eye[3] = {live.position.x, live.position.y, live.position.z};
    float tgt[3] = {live.target.x, live.target.y, live.target.z};
    float up[3] = {live.up.x, live.up.y, live.up.z};
    float dx = tgt[0]-eye[0], dy = tgt[1]-eye[1], dz = tgt[2]-eye[2];
    if (dx*dx + dy*dy + dz*dz < 1e-12f) return;

    const float aspect = static_cast<float>(fb_width) /
                         static_cast<float>(fb_height);
    const Mat4 vp = multiply(
        perspective(live.vertical_fov_radians, aspect, live.near_plane,
                    live.far_plane),
        look_at(eye, tgt, up));

    // The frozen camera's basis, and the half-extents of its near and far
    // faces. Straight from the pinhole model: half_height = tan(fov/2) * depth.
    float f[3] = {frozen.target.x - frozen.position.x,
                  frozen.target.y - frozen.position.y,
                  frozen.target.z - frozen.position.z};
    const float fl = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (fl < 1e-6f) return;
    f[0]/=fl; f[1]/=fl; f[2]/=fl;
    float fu[3] = {frozen.up.x, frozen.up.y, frozen.up.z};
    float r[3] = {f[1]*fu[2] - f[2]*fu[1], f[2]*fu[0] - f[0]*fu[2],
                  f[0]*fu[1] - f[1]*fu[0]};
    const float rl = std::sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    if (rl < 1e-6f) return;
    r[0]/=rl; r[1]/=rl; r[2]/=rl;
    float u[3] = {r[1]*f[2] - r[2]*f[1], r[2]*f[0] - r[0]*f[2],
                  r[0]*f[1] - r[1]*f[0]};

    const float tan_half = std::tan(frozen.vertical_fov_radians * 0.5f);
    const float depths[2] = {frozen.near_plane, depth_limit};
    float corners[8][3];
    for (int face = 0; face < 2; ++face) {
        const float d = depths[face];
        const float hh = tan_half * d;
        const float hw = hh * aspect;
        for (int c = 0; c < 4; ++c) {
            const float sx = (c == 1 || c == 2) ? hw : -hw;
            const float sy = (c >= 2) ? hh : -hh;
            float* out = corners[face * 4 + c];
            out[0] = frozen.position.x + f[0]*d + r[0]*sx + u[0]*sy;
            out[1] = frozen.position.y + f[1]*d + r[1]*sx + u[1]*sy;
            out[2] = frozen.position.z + f[2]*d + r[2]*sx + u[2]*sy;
        }
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 screen[8];
    bool visible[8];
    for (int i = 0; i < 8; ++i)
        visible[i] = project(vp, fb_width, fb_height, offset_x, offset_y,
                             corners[i], screen[i]);
    static constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},          // near face
        {4,5},{5,6},{6,7},{7,4},          // truncated far face
        {0,4},{1,5},{2,6},{3,7},          // the four rays
    };
    const ImU32 color = IM_COL32(255, 96, 32, 220);
    for (const auto& e : edges)
        if (visible[e[0]] && visible[e[1]])
            dl->AddLine(screen[e[0]], screen[e[1]], color, 2.0f);
    // A dot at the frozen eye, so a frustum seen end-on still says where it is.
    ImVec2 eye_screen;
    float frozen_eye[3] = {frozen.position.x, frozen.position.y,
                           frozen.position.z};
    if (project(vp, fb_width, fb_height, offset_x, offset_y, frozen_eye,
                eye_screen))
        dl->AddCircleFilled(eye_screen, 5.0f, color);
}

} // namespace viewer
