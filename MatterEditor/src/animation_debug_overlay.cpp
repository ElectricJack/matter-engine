// MatterEditor/src/animation_debug_overlay.cpp
//
// Implementation of the viewport animation-diagnostics overlay declared in
// animation_debug_overlay.h. Everything is drawn as 2D primitives on ImGui's
// foreground draw list: this file projects world-space points itself and hands
// ImGui screen-space coordinates, so it needs no Vulkan pipeline, no shader and
// no engine render state.
//
// TWO MATRIX CONVENTIONS LIVE IN THIS FILE. Do not confuse them:
//
//   - the file-local `Mat4` (anonymous namespace) is COLUMN-major, the classic
//     GL view/projection layout: element (row, column) is `m[row + column * 4]`.
//     `look_at`, `perspective`, the `Mat4` overload of `multiply` and `project`
//     all use it, and it is used ONLY for the camera's view-projection.
//   - `matter::Mat4f` (MatterEngine3/include/matter/math_types.h) is ROW-major:
//     element (row, column) is `m[row * 4 + column]`, with translation in m[3],
//     m[7], m[11]. The `matter::Mat4f` overload of `multiply`, plus `point`,
//     `origin` and `local_matrix`, use it -- and it is the format every
//     transform in the snapshot arrives in.
//
// Coordinate flow for one point: joint model-space transform -> composed with
// the instance's world transform (`multiply`, row-major) -> `point` to get a
// world position -> `project` through the column-major view-projection into
// viewport pixels, offset by (viewport_x, viewport_y).
//
// `project` returns false for anything at or behind the eye plane and for any
// non-finite result, and every draw helper silently skips the primitive in that
// case. An off-screen or degenerate rig therefore draws nothing rather than
// smearing garbage across the window.
//
// Skin weights arrive as uint16 and are normalized by dividing by 65535. They
// are never renormalized, so an under-weighted vertex is shown shrunk toward
// the origin exactly as the GPU would render it.
//
// The three point-cloud layers subsample `asset.lod0_influences` with a stride
// chosen so at most ~4096 points (~2048 for skin weights) are ever projected.
// Cost is bounded no matter how dense the mesh is, but the cloud is a SAMPLE --
// a single bad vertex may simply not be in it.
//
// Render thread only (ImGui), between NewFrame and Render.

#include "animation_debug_overlay.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace viewer {
namespace {

// Column-major 4x4 (element (row, column) == m[row + column * 4]), used only
// for the camera's view and projection matrices. Distinct from matter::Mat4f,
// which is row-major -- see the file header above.
struct Mat4 { float m[16] = {}; };

// Right-handed look-at looking down -Z. Returns an ALL-ZERO matrix, not
// identity, when the forward or right vector degenerates (target coincident
// with eye, or `up` parallel to forward). `project` then rejects every point,
// so the overlay draws nothing rather than something wrong.
Mat4 look_at(const float eye[3], const float target[3], const float up[3]) {
    float f[3] = {target[0] - eye[0], target[1] - eye[1],
                  target[2] - eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-6f) return {};
    for (float& c : f) c /= fl;
    float r[3] = {f[1] * up[2] - f[2] * up[1],
                  f[2] * up[0] - f[0] * up[2],
                  f[0] * up[1] - f[1] * up[0]};
    const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6f) return {};
    for (float& c : r) c /= rl;
    const float u[3] = {r[1] * f[2] - r[2] * f[1],
                        r[2] * f[0] - r[0] * f[2],
                        r[0] * f[1] - r[1] * f[0]};
    Mat4 out{};
    out.m[0] = r[0]; out.m[4] = r[1]; out.m[8] = r[2];
    out.m[12] = -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]);
    out.m[1] = u[0]; out.m[5] = u[1]; out.m[9] = u[2];
    out.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    out.m[2] = -f[0]; out.m[6] = -f[1]; out.m[10] = -f[2];
    out.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    out.m[15] = 1.0f;
    return out;
}

// GL-style perspective. `fov_y` is the VERTICAL field of view in RADIANS (the
// caller passes camera.vertical_fov_radians). The depth row it writes is never
// read -- `project` consumes only x, y and w -- so this matrix's depth
// convention is unrelated to whatever the engine's Vulkan pipeline uses.
// Returns an all-zero matrix on degenerate parameters.
Mat4 perspective(float fov_y, float aspect, float near_p, float far_p) {
    Mat4 out{};
    const float t = std::tan(fov_y * 0.5f);
    if (t <= 0.0f || aspect <= 0.0f || far_p <= near_p) return out;
    out.m[0] = 1.0f / (aspect * t);
    out.m[5] = 1.0f / t;
    out.m[10] = -(far_p + near_p) / (far_p - near_p);
    out.m[11] = -1.0f;
    out.m[14] = -(2.0f * far_p * near_p) / (far_p - near_p);
    return out;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int column = 0; column != 4; ++column)
        for (int row = 0; row != 4; ++row)
            for (int k = 0; k != 4; ++k)
                out.m[row + column * 4] +=
                    a.m[row + k * 4] * b.m[k + column * 4];
    return out;
}

// World-space point -> viewport pixel. `width`/`height` are the 3D viewport's
// pixel size and `ox`/`oy` its top-left offset in the window; the y axis is
// flipped so the result is in ImGui's top-left-origin screen space.
//
// Returns false for anything at or behind the eye plane (w <= 1e-4) and for any
// non-finite result; `out` is meaningless in that case. Callers must treat
// false as "skip this primitive", not as an error.
bool project(const Mat4& vp, int width, int height, float ox, float oy,
             const matter::Float3& p, ImVec2& out) {
    float x = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12];
    float y = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13];
    const float w =
        vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
    if (w <= 1e-4f || !std::isfinite(w)) return false;
    x /= w;
    y /= w;
    out = {(x * 0.5f + 0.5f) * width + ox,
           (1.0f - (y * 0.5f + 0.5f)) * height + oy};
    return std::isfinite(out.x) && std::isfinite(out.y);
}

// Affine transform of a point by a ROW-major matter::Mat4f (translation read
// from m[3], m[7], m[11]). The bottom row is assumed to be (0, 0, 0, 1) and is
// not applied: there is no perspective divide here.
matter::Float3 point(const matter::Mat4f& matrix,
                     const matter::Float3& p) {
    return {
        matrix.m[0] * p.x + matrix.m[1] * p.y + matrix.m[2] * p.z + matrix.m[3],
        matrix.m[4] * p.x + matrix.m[5] * p.y + matrix.m[6] * p.z + matrix.m[7],
        matrix.m[8] * p.x + matrix.m[9] * p.y + matrix.m[10] * p.z + matrix.m[11]};
}

matter::Float3 origin(const matter::Mat4f& matrix) {
    return point(matrix, {});
}

// Builds a row-major matter::Mat4f from a translation/rotation/scale triple.
// The quaternion is used as-is and assumed unit length (matter::Quaternion
// documents that nothing normalizes it). Scale is applied to the rotation
// basis' COLUMNS -- scale.x scales m[0], m[4], m[8] -- i.e. it is a local,
// pre-rotation scale.
matter::Mat4f local_matrix(const matter::AnimationTransform& transform) {
    const float x = transform.rotation.x, y = transform.rotation.y;
    const float z = transform.rotation.z, w = transform.rotation.w;
    matter::Mat4f out{};
    out.m[0] = (1 - 2 * y * y - 2 * z * z) * transform.scale.x;
    out.m[1] = (2 * x * y - 2 * z * w) * transform.scale.y;
    out.m[2] = (2 * x * z + 2 * y * w) * transform.scale.z;
    out.m[3] = transform.translation.x;
    out.m[4] = (2 * x * y + 2 * z * w) * transform.scale.x;
    out.m[5] = (1 - 2 * x * x - 2 * z * z) * transform.scale.y;
    out.m[6] = (2 * y * z - 2 * x * w) * transform.scale.z;
    out.m[7] = transform.translation.y;
    out.m[8] = (2 * x * z - 2 * y * w) * transform.scale.x;
    out.m[9] = (2 * y * z + 2 * x * w) * transform.scale.y;
    out.m[10] = (1 - 2 * x * x - 2 * y * y) * transform.scale.z;
    out.m[11] = transform.translation.z;
    out.m[15] = 1.0f;
    return out;
}

// Row-major product a*b -- parent * child when composing a joint chain. This is
// a different function from the column-major `multiply` above, for a different
// matrix type; the two are not interchangeable.
matter::Mat4f multiply(const matter::Mat4f& a, const matter::Mat4f& b) {
    matter::Mat4f out{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            for (int k = 0; k < 4; ++k)
                out.m[row * 4 + column] +=
                    a.m[row * 4 + k] * b.m[k * 4 + column];
    return out;
}

void draw_line(ImDrawList* draw_list, const Mat4& vp,
               int width, int height, float ox, float oy,
               const matter::Float3& a, const matter::Float3& b,
               ImU32 color, float thickness = 1.5f) {
    ImVec2 pa, pb;
    if (project(vp, width, height, ox, oy, a, pa) &&
        project(vp, width, height, ox, oy, b, pb))
        draw_list->AddLine(pa, pb, color, thickness);
}

// Three lines from the transform's origin along its local X (red), Y (green)
// and Z (blue) axes, `scale` long in world units. Used for joint frames, socket
// frames and evaluated IK target frames alike, so colour is the only cue for
// which axis is which -- and the only cue for what kind of frame it is is its
// size, chosen by the caller.
void draw_axes(ImDrawList* draw_list, const Mat4& vp,
               int width, int height, float ox, float oy,
               const matter::Mat4f& transform, float scale) {
    const matter::Float3 p = origin(transform);
    draw_line(draw_list, vp, width, height, ox, oy, p,
              point(transform, {scale, 0, 0}), IM_COL32(255, 80, 80, 230));
    draw_line(draw_list, vp, width, height, ox, oy, p,
              point(transform, {0, scale, 0}), IM_COL32(80, 255, 80, 230));
    draw_line(draw_list, vp, width, height, ox, oy, p,
              point(transform, {0, 0, scale}), IM_COL32(80, 150, 255, 230));
}

// Wireframe box for one conservative joint bound. The AABB's min/max are in the
// JOINT's space, so they are transformed by that joint's world matrix; the box
// drawn on screen is therefore generally not axis-aligned.
void draw_aabb(ImDrawList* draw_list, const Mat4& vp,
               int width, int height, float ox, float oy,
               const matter::Mat4f& transform,
               const matter::AnimationDebugJointBound& aabb) {
    const matter::Float3 local[8] = {
        {aabb.minimum.x, aabb.minimum.y, aabb.minimum.z},
        {aabb.maximum.x, aabb.minimum.y, aabb.minimum.z},
        {aabb.maximum.x, aabb.maximum.y, aabb.minimum.z},
        {aabb.minimum.x, aabb.maximum.y, aabb.minimum.z},
        {aabb.minimum.x, aabb.minimum.y, aabb.maximum.z},
        {aabb.maximum.x, aabb.minimum.y, aabb.maximum.z},
        {aabb.maximum.x, aabb.maximum.y, aabb.maximum.z},
        {aabb.minimum.x, aabb.maximum.y, aabb.maximum.z}};
    matter::Float3 world[8];
    for (int i = 0; i < 8; ++i) world[i] = point(transform, local[i]);
    static constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},
        {6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (const auto& edge : edges)
        draw_line(draw_list, vp, width, height, ox, oy,
                  world[edge[0]], world[edge[1]],
                  IM_COL32(100, 220, 255, 170), 1.0f);
}

// The CPU's answer for where a skinned vertex lands: a linear blend of up to
// four palette matrices applied to the bind position, then the instance's world
// transform. Weights are uint16 normalized by 65535 and are NOT renormalized.
// Lanes with a zero weight or a joint index past the end of the palette are
// skipped, which is the only bounds check performed on the palette.
matter::Float3 skinned_position(
    const matter::AnimationDebugVertexInfluence& vertex,
    const std::vector<matter::Mat4f>& palette,
    const matter::Mat4f& world_transform) {
    matter::Float3 result{};
    for (size_t i = 0; i < 4; ++i) {
        if (!vertex.weights[i] || vertex.joints[i] >= palette.size()) continue;
        const float weight = vertex.weights[i] / 65535.0f;
        const matter::Float3 transformed =
            point(palette[vertex.joints[i]], vertex.bind_position);
        result.x += transformed.x * weight;
        result.y += transformed.y * weight;
        result.z += transformed.z * weight;
    }
    return point(world_transform, result);
}

// Normalized (0-1) weight this vertex gives `selected_joint`, or 0 when that
// joint is not one of its four influences. Zero is a normal result, not an
// error -- most vertices are unaffected by most joints.
float selected_weight(const matter::AnimationDebugVertexInfluence& vertex,
                      uint16_t selected_joint) {
    for (size_t i = 0; i < 4; ++i)
        if (vertex.joints[i] == selected_joint)
            return vertex.weights[i] / 65535.0f;
    return 0.0f;
}

} // namespace

void draw_animation_debug_overlay(
    const matter::AnimationDebugInstanceSnapshot& snapshot,
    const matter::CameraDesc& camera,
    int framebuffer_width, int framebuffer_height,
    float viewport_x, float viewport_y,
    const AnimationDebugOverlayOptions& options) {
    const auto& asset = snapshot.asset;
    const auto& pose = snapshot.pose;
    if (!options.enabled ||
        !matter::valid_animation_debug_snapshot(snapshot) ||
        framebuffer_width <= 0 || framebuffer_height <= 0) return;
    const float eye[3] =
        {camera.position.x, camera.position.y, camera.position.z};
    const float target[3] =
        {camera.target.x, camera.target.y, camera.target.z};
    const float up[3] = {camera.up.x, camera.up.y, camera.up.z};
    const Mat4 vp = multiply(
        perspective(camera.vertical_fov_radians,
                    static_cast<float>(framebuffer_width) / framebuffer_height,
                    camera.near_plane, camera.far_plane),
        look_at(eye, target, up));
    // Foreground draw list: over the composited frame, outside any ImGui window
    // (so panels do not clip it) and with no depth test.
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    // One world matrix per joint, built once and reused by every layer below.
    // Indexed in lockstep with `asset.joints`; valid_animation_debug_snapshot is
    // what guarantees the pose and the asset joint arrays are parallel.
    std::vector<matter::Mat4f> world_models;
    world_models.reserve(pose.model_pose.size());
    for (const auto& model : pose.model_pose)
        world_models.push_back(multiply(snapshot.world_transform, model));

    for (size_t i = 0; i < world_models.size(); ++i) {
        const auto& model = world_models[i];
        const matter::Float3 p = origin(model);
        const uint16_t parent = asset.joints[i].parent;
        const float radius = std::max(0.01f, asset.joints[i].radius);
        if (options.bones && parent != UINT16_MAX &&
            parent < world_models.size())
            draw_line(draw_list, vp, framebuffer_width, framebuffer_height,
                      viewport_x, viewport_y,
                      origin(world_models[parent]), p,
                      IM_COL32(255, 220, 90, 240), 2.0f);
        if (options.joint_axes)
            draw_axes(draw_list, vp, framebuffer_width, framebuffer_height,
                      viewport_x, viewport_y, model, radius * 0.8f);
        if (options.radius_envelopes) {
            ImVec2 screen;
            if (project(vp, framebuffer_width, framebuffer_height,
                        viewport_x, viewport_y, p, screen))
                draw_list->AddCircle(
                    screen, std::max(2.0f, radius * 8.0f),
                    IM_COL32(255, 190, 70, 150), 12, 1.0f);
        }
    }

    if (options.sockets)
        for (const auto& socket : asset.sockets)
            if (socket.joint < world_models.size())
                draw_axes(draw_list, vp, framebuffer_width, framebuffer_height,
                          viewport_x, viewport_y,
                          multiply(world_models[socket.joint],
                                   local_matrix(socket.local)),
                          0.18f);

    if (options.targets_and_ik) {
        const size_t count = std::min(asset.targets.size(), pose.targets.size());
        for (size_t i = 0; i < count; ++i) {
            const auto& definition = asset.targets[i];
            const auto& state = pose.targets[i];
            // Only three-joint (two-bone) chains are visualized; anything else
            // is skipped outright rather than drawn partially.
            if (definition.chain.size() != 3) continue;
            for (size_t joint = 1; joint < definition.chain.size(); ++joint)
                draw_line(draw_list, vp, framebuffer_width, framebuffer_height,
                          viewport_x, viewport_y,
                          origin(world_models[definition.chain[joint - 1]]),
                          origin(world_models[definition.chain[joint]]),
                          IM_COL32(210, 100, 255, 220), 2.0f);
            const matter::Float3 start =
                origin(world_models[definition.chain.front()]);
            if (definition.has_pole) {
                matter::Float3 pole_direction{};
                if (matter::animation_debug_world_pole_direction(
                        snapshot.world_transform,
                        pose.model_pose[definition.chain.front()],
                        definition.pole, pole_direction)) {
                    const matter::Float3 pole_end = {
                        start.x + pole_direction.x * 0.6f,
                        start.y + pole_direction.y * 0.6f,
                        start.z + pole_direction.z * 0.6f};
                    draw_line(draw_list, vp, framebuffer_width,
                              framebuffer_height, viewport_x, viewport_y,
                              start, pole_end,
                              IM_COL32(145, 100, 255, 180), 1.0f);
                }
            }
            if (state.available && state.enabled && state.weight > 0.0f) {
                const matter::Mat4f live =
                    multiply(snapshot.world_transform,
                             local_matrix(state.evaluated));
                draw_axes(draw_list, vp, framebuffer_width, framebuffer_height,
                          viewport_x, viewport_y, live, 0.25f);
                ImVec2 label;
                if (project(vp, framebuffer_width, framebuffer_height,
                            viewport_x, viewport_y, origin(live), label)) {
                    char text[32];
                    std::snprintf(text, sizeof(text), "IK %.2f", state.weight);
                    draw_list->AddText(label, IM_COL32(235, 170, 255, 240), text);
                }
            }
        }
    }

    if (options.conservative_bounds)
        for (const auto& bound : asset.joint_bounds)
            if (bound.joint < world_models.size())
                draw_aabb(draw_list, vp, framebuffer_width, framebuffer_height,
                          viewport_x, viewport_y,
                          world_models[bound.joint], bound);

    // CPU reference cloud: the position the CPU derives from the SAME immutable
    // pose the GPU received. If these points trace a clean surface while the
    // rendered mesh is torn, the fault is downstream in the GPU skinning path.
    if (options.cpu_reference && !asset.lod0_influences.empty() &&
        pose.skin_palette.size() == asset.joints.size()) {
        const size_t stride =
            std::max<size_t>(1, (asset.lod0_influences.size() + 4095) / 4096);
        for (size_t i = 0; i < asset.lod0_influences.size(); i += stride) {
            ImVec2 screen;
            if (!project(vp, framebuffer_width, framebuffer_height,
                         viewport_x, viewport_y,
                         skinned_position(asset.lod0_influences[i],
                                          pose.skin_palette,
                                          snapshot.world_transform),
                         screen))
                continue;
            draw_list->AddCircleFilled(screen, 1.0f, IM_COL32(80, 255, 120, 190));
        }
    }

    // Dominant-joint colouring: one hue per joint, so the weight partition is
    // legible as regions. A vertex bound to the wrong limb reads as a speck of
    // foreign colour instead of needing a per-joint sweep to find.
    if (options.dominant_joint && !asset.lod0_influences.empty() &&
        pose.skin_palette.size() == asset.joints.size()) {
        const size_t stride =
            std::max<size_t>(1, (asset.lod0_influences.size() + 4095) / 4096);
        for (size_t i = 0; i < asset.lod0_influences.size(); i += stride) {
            const auto& vertex = asset.lod0_influences[i];
            uint16_t best_joint = 0;
            uint16_t best_weight = 0;
            for (size_t lane = 0; lane != 4; ++lane) {
                if (vertex.weights[lane] > best_weight) {
                    best_weight = vertex.weights[lane];
                    best_joint = vertex.joints[lane];
                }
            }
            if (best_weight == 0) continue;
            ImVec2 screen;
            if (!project(vp, framebuffer_width, framebuffer_height,
                         viewport_x, viewport_y,
                         skinned_position(vertex, pose.skin_palette,
                                          snapshot.world_transform),
                         screen))
                continue;
            // Golden-ratio hue spread keeps adjacent joint indices visually
            // distinct rather than a near-identical gradient.
            const float hue = std::fmod(static_cast<float>(best_joint) * 0.61803f, 1.0f);
            float r = 0.0f, g = 0.0f, b = 0.0f;
            ImGui::ColorConvertHSVtoRGB(hue, 0.85f, 1.0f, r, g, b);
            draw_list->AddCircleFilled(
                screen, 2.0f,
                IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255),
                         static_cast<int>(b * 255), 210));
        }
    }

    if (options.skin_weights && !asset.lod0_influences.empty() &&
        pose.skin_palette.size() == asset.joints.size()) {
        const uint16_t selected = static_cast<uint16_t>(std::clamp(
            options.weight_joint, 0,
            static_cast<int>(asset.joints.size() - 1)));
        const size_t stride =
            std::max<size_t>(1, (asset.lod0_influences.size() + 2047) / 2048);
        for (size_t i = 0; i < asset.lod0_influences.size(); i += stride) {
            const auto& vertex = asset.lod0_influences[i];
            const float weight = selected_weight(vertex, selected);
            if (weight <= 0.0f) continue;
            ImVec2 screen;
            if (!project(vp, framebuffer_width, framebuffer_height,
                         viewport_x, viewport_y,
                         skinned_position(vertex, pose.skin_palette,
                                          snapshot.world_transform),
                         screen))
                continue;
            const int red = static_cast<int>(255.0f * weight);
            const int blue = static_cast<int>(255.0f * (1.0f - weight));
            draw_list->AddCircleFilled(
                screen, 1.5f + weight * 2.5f,
                IM_COL32(red, 60, blue, 210));
        }
    }
}

void draw_animation_debug_overlay_controls(
    AnimationDebugOverlayOptions& options,
    const std::vector<std::string>* joint_names) {
    if (!ImGui::CollapsingHeader("Animation Overlay")) return;
    ImGui::Checkbox("Enabled##animation-overlay", &options.enabled);
    ImGui::BeginDisabled(!options.enabled);
    ImGui::Checkbox("Bones", &options.bones);
    ImGui::Checkbox("Joint axes", &options.joint_axes);
    ImGui::Checkbox("Radius envelopes", &options.radius_envelopes);
    ImGui::Checkbox("Sockets", &options.sockets);
    ImGui::Checkbox("Targets / IK", &options.targets_and_ik);
    ImGui::Checkbox("Conservative bounds", &options.conservative_bounds);
    ImGui::Checkbox("Dominant joint", &options.dominant_joint);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Colour each vertex by its highest-weight joint.");
    ImGui::Checkbox("CPU reference points", &options.cpu_reference);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Where the CPU says each vertex should be, from the same pose the GPU got. Divergence from the drawn surface means the GPU skinning path is at fault.");
    ImGui::Checkbox("Skin weights", &options.skin_weights);
    ImGui::BeginDisabled(!options.skin_weights);
    if (joint_names != nullptr && !joint_names->empty()) {
        options.weight_joint = std::clamp(
            options.weight_joint, 0, static_cast<int>(joint_names->size()) - 1);
        if (ImGui::BeginCombo("Weight joint",
                              (*joint_names)[options.weight_joint].c_str())) {
            for (int i = 0; i < static_cast<int>(joint_names->size()); ++i) {
                const bool selected = i == options.weight_joint;
                if (ImGui::Selectable((*joint_names)[i].c_str(), selected))
                    options.weight_joint = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::InputInt("Weight joint", &options.weight_joint);
        options.weight_joint = std::max(options.weight_joint, 0);
    }
    ImGui::EndDisabled();  // closes BeginDisabled(!options.skin_weights)
    ImGui::EndDisabled();  // closes BeginDisabled(!options.enabled)
}

} // namespace viewer
