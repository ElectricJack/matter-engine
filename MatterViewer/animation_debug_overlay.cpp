#include "animation_debug_overlay.h"

#include "animation/anim_asset.h"
#include "animation/animation_binding_bake.h"
#include "animation/animation_evaluator.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace viewer {
namespace {

struct Mat4 { float m[16] = {}; };

Mat4 look_at(const float eye[3], const float target[3], const float up[3]) {
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float fl = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (fl < 1e-6f) return {};
    for (float& c : f) c /= fl;
    float r[3] = {f[1]*up[2] - f[2]*up[1], f[2]*up[0] - f[0]*up[2], f[0]*up[1] - f[1]*up[0]};
    const float rl = std::sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    if (rl < 1e-6f) return {};
    for (float& c : r) c /= rl;
    const float u[3] = {r[1]*f[2] - r[2]*f[1], r[2]*f[0] - r[0]*f[2], r[0]*f[1] - r[1]*f[0]};
    Mat4 out{};
    out.m[0]=r[0]; out.m[4]=r[1]; out.m[8]=r[2]; out.m[12]=-(r[0]*eye[0]+r[1]*eye[1]+r[2]*eye[2]);
    out.m[1]=u[0]; out.m[5]=u[1]; out.m[9]=u[2]; out.m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    out.m[2]=-f[0]; out.m[6]=-f[1]; out.m[10]=-f[2]; out.m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    out.m[15]=1.0f;
    return out;
}

Mat4 perspective(float fov_y, float aspect, float near_p, float far_p) {
    Mat4 out{};
    const float t = std::tan(fov_y * 0.5f);
    if (t <= 0.0f || aspect <= 0.0f || far_p <= near_p) return out;
    out.m[0] = 1.0f / (aspect * t); out.m[5] = 1.0f / t;
    out.m[10] = -(far_p + near_p) / (far_p - near_p); out.m[11] = -1.0f;
    out.m[14] = -(2.0f * far_p * near_p) / (far_p - near_p);
    return out;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int col = 0; col != 4; ++col) for (int row = 0; row != 4; ++row)
        for (int k = 0; k != 4; ++k) out.m[row + col*4] += a.m[row + k*4] * b.m[k + col*4];
    return out;
}

bool project(const Mat4& vp, int width, int height, float ox, float oy,
             const matter::Float3& p, ImVec2& out) {
    float x = vp.m[0]*p.x + vp.m[4]*p.y + vp.m[8]*p.z + vp.m[12];
    float y = vp.m[1]*p.x + vp.m[5]*p.y + vp.m[9]*p.z + vp.m[13];
    const float w = vp.m[3]*p.x + vp.m[7]*p.y + vp.m[11]*p.z + vp.m[15];
    if (w <= 1e-4f || !std::isfinite(w)) return false;
    x /= w; y /= w;
    out = {(x * .5f + .5f) * width + ox, (1.0f - (y * .5f + .5f)) * height + oy};
    return std::isfinite(out.x) && std::isfinite(out.y);
}

matter::Float3 point(const matter::Mat4f& m, const matter::Float3& p) {
    return {m.m[0]*p.x + m.m[1]*p.y + m.m[2]*p.z + m.m[3],
            m.m[4]*p.x + m.m[5]*p.y + m.m[6]*p.z + m.m[7],
            m.m[8]*p.x + m.m[9]*p.y + m.m[10]*p.z + m.m[11]};
}

matter::Float3 origin(const matter::Mat4f& m) { return point(m, {}); }

matter::Mat4f local_matrix(const matter::AnimationTransform& t) {
    const float x=t.rotation.x, y=t.rotation.y, z=t.rotation.z, w=t.rotation.w;
    matter::Mat4f out{};
    out.m[0]=(1-2*y*y-2*z*z)*t.scale.x; out.m[1]=(2*x*y-2*z*w)*t.scale.y; out.m[2]=(2*x*z+2*y*w)*t.scale.z; out.m[3]=t.translation.x;
    out.m[4]=(2*x*y+2*z*w)*t.scale.x; out.m[5]=(1-2*x*x-2*z*z)*t.scale.y; out.m[6]=(2*y*z-2*x*w)*t.scale.z; out.m[7]=t.translation.y;
    out.m[8]=(2*x*z-2*y*w)*t.scale.x; out.m[9]=(2*y*z+2*x*w)*t.scale.y; out.m[10]=(1-2*x*x-2*y*y)*t.scale.z; out.m[11]=t.translation.z;
    out.m[15]=1.0f;
    return out;
}

matter::Mat4f multiply(const matter::Mat4f& a, const matter::Mat4f& b) {
    matter::Mat4f out{};
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) for (int k=0;k<4;++k)
        out.m[r*4+c] += a.m[r*4+k] * b.m[k*4+c];
    return out;
}

bool parse_u16(const std::string& text, uint16_t& value) {
    char* end = nullptr; const unsigned long v = std::strtoul(text.c_str(), &end, 10);
    if (!end || *end || v > UINT16_MAX) return false;
    value = static_cast<uint16_t>(v);
    return true;
}
bool parse_float(const std::string& text, float& value) {
    char* end = nullptr; value = std::strtof(text.c_str(), &end);
    return end && !*end && std::isfinite(value);
}
std::vector<std::string> split(const std::string& line, char separator) {
    std::vector<std::string> fields; std::stringstream stream(line); std::string field;
    while (std::getline(stream, field, separator)) fields.push_back(field);
    return fields;
}
bool parse_float3(const std::string& text, matter::Float3& result) {
    const auto fields = split(text, ',');
    return fields.size() == 3 && parse_float(fields[0], result.x) && parse_float(fields[1], result.y) && parse_float(fields[2], result.z);
}
bool parse_transform(const std::vector<std::string>& fields, size_t at, matter::AnimationTransform& out) {
    if (at + 2 >= fields.size() || !parse_float3(fields[at], out.translation) || !parse_float3(fields[at + 2], out.scale)) return false;
    const auto rotation = split(fields[at + 1], ',');
    return rotation.size() == 4 && parse_float(rotation[0], out.rotation.x) && parse_float(rotation[1], out.rotation.y) &&
           parse_float(rotation[2], out.rotation.z) && parse_float(rotation[3], out.rotation.w);
}

const matter::animation::AnimSection* find_section(const matter::animation::AnimAsset& asset,
                                                    matter::animation::AnimSectionKind kind) {
    const matter::animation::AnimSection* found = nullptr;
    for (const auto& section : asset.sections) {
        if (section.kind != kind) continue;
        if (found) return nullptr;
        found = &section;
    }
    return found;
}

void draw_line(ImDrawList* dl, const Mat4& vp, int w, int h, float ox, float oy,
               const matter::Float3& a, const matter::Float3& b, ImU32 color, float thickness = 1.5f) {
    ImVec2 pa, pb;
    if (project(vp, w, h, ox, oy, a, pa) && project(vp, w, h, ox, oy, b, pb)) dl->AddLine(pa, pb, color, thickness);
}

void draw_axes(ImDrawList* dl, const Mat4& vp, int w, int h, float ox, float oy,
               const matter::Mat4f& m, float scale) {
    const matter::Float3 p = origin(m);
    draw_line(dl, vp, w, h, ox, oy, p, point(m, {scale,0,0}), IM_COL32(255,80,80,230));
    draw_line(dl, vp, w, h, ox, oy, p, point(m, {0,scale,0}), IM_COL32(80,255,80,230));
    draw_line(dl, vp, w, h, ox, oy, p, point(m, {0,0,scale}), IM_COL32(80,150,255,230));
}

void draw_aabb(ImDrawList* dl, const Mat4& vp, int w, int h, float ox, float oy,
               const matter::Mat4f& transform, const AnimationDebugJointBound& aabb) {
    const matter::Float3 local[8] = {
        {aabb.minimum.x,aabb.minimum.y,aabb.minimum.z}, {aabb.maximum.x,aabb.minimum.y,aabb.minimum.z},
        {aabb.maximum.x,aabb.maximum.y,aabb.minimum.z}, {aabb.minimum.x,aabb.maximum.y,aabb.minimum.z},
        {aabb.minimum.x,aabb.minimum.y,aabb.maximum.z}, {aabb.maximum.x,aabb.minimum.y,aabb.maximum.z},
        {aabb.maximum.x,aabb.maximum.y,aabb.maximum.z}, {aabb.minimum.x,aabb.maximum.y,aabb.maximum.z}};
    matter::Float3 world[8]; for (int i=0;i<8;++i) world[i] = point(transform, local[i]);
    static constexpr int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (const auto& edge : edges) draw_line(dl, vp, w, h, ox, oy, world[edge[0]], world[edge[1]], IM_COL32(100,220,255,170), 1.0f);
}

} // namespace

bool make_animation_debug_asset(const matter::animation::AnimAsset& committed,
                                AnimationDebugAsset& out) {
    out = {};
    if (committed.target_abi_tag != matter::animation::kAnimationTargetAbiTag ||
        committed.ozz_tag_hash != matter::animation::kAnimationOzzTagHash) return false;
    const auto* rig = find_section(committed, matter::animation::AnimSectionKind::RigSchema);
    if (!rig || rig->bytes.empty()) return false;
    AnimationDebugAsset parsed{};
    parsed.resolved_hash = committed.resolved_hash;
    parsed.nonce_high = committed.nonce.high;
    parsed.nonce_low = committed.nonce.low;
    std::istringstream lines(std::string(rig->bytes.begin(), rig->bytes.end()));
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        const auto fields = split(line, '|');
        if (fields.empty()) return false;
        if (fields[0] == "graph") break;
        if (fields[0] == "socket") {
            if (fields.size() != 6) return false;
            AnimationDebugSocket socket{};
            if (!parse_u16(fields[2], socket.joint) || !parse_transform(fields, 3, socket.local)) return false;
            parsed.sockets.push_back(socket);
            continue;
        }
        // Joint rows have a subtree range at field 2. Target rows do not.
        if (fields.size() == 7 && fields[2].find(':') != std::string::npos) {
            const auto range = split(fields[2], ':');
            AnimationDebugJoint joint{};
            uint16_t subtree_begin = UINT16_MAX, subtree_end = UINT16_MAX;
            if (range.size() != 2 || !parse_u16(fields[1], joint.parent) || !parse_u16(range[0], subtree_begin) ||
                !parse_u16(range[1], subtree_end) || subtree_begin > subtree_end ||
                !parse_float(fields[6], joint.radius) || joint.radius <= 0.0f) return false;
            matter::AnimationTransform ignored{};
            if (!parse_transform(fields, 3, ignored)) return false;
            parsed.joints.push_back(joint);
            continue;
        }
        if (fields.size() < 16) return false;
        AnimationDebugTarget target{};
        if (!parse_float3(fields[5], target.pole)) return false;
        target.has_pole = fields[4] == "1";
        for (size_t i = 13; i < fields.size(); ++i) {
            uint16_t joint = UINT16_MAX;
            if (!parse_u16(fields[i], joint)) return false;
            target.chain.push_back(joint);
        }
        if (target.chain.size() != 3) return false;
        parsed.targets.push_back(std::move(target));
    }
    if (parsed.joints.empty() || parsed.joints.size() > matter::animation::kMaxJoints) return false;
    for (size_t i=0;i<parsed.joints.size();++i) {
        const uint16_t parent = parsed.joints[i].parent;
        if ((parent != UINT16_MAX && parent >= i) || !std::isfinite(parsed.joints[i].radius)) return false;
    }
    for (const auto& socket : parsed.sockets) if (socket.joint >= parsed.joints.size()) return false;
    for (const auto& target : parsed.targets) for (uint16_t joint : target.chain) if (joint >= parsed.joints.size()) return false;
    matter::animation::BindingBake binding{};
    if (matter::animation::get_anim_binding_bake(committed, binding) && !binding.lods.empty()) {
        parsed.lod0_influence_count = static_cast<uint32_t>(binding.lods.front().influences.size());
        for (const auto& cluster : binding.lods.front().clusters) for (const auto& joint : cluster.joints) {
            if (joint.joint >= parsed.joints.size()) return false;
            parsed.joint_bounds.push_back({joint.joint, joint.minimum, joint.maximum});
        }
    }
    out = std::move(parsed);
    return true;
}

void draw_animation_debug_overlay(const AnimationDebugAsset& asset,
                                  const matter::animation::AnimationPoseSnapshot& pose,
                                  const matter::CameraDesc& camera,
                                  int framebuffer_width, int framebuffer_height,
                                  float viewport_x, float viewport_y,
                                  const AnimationDebugOverlayOptions& options) {
    if (!options.enabled || !pose.instance.valid() || !pose.model_pose.data ||
        pose.model_pose.count != asset.joints.size() || framebuffer_width <= 0 || framebuffer_height <= 0) return;
    const float eye[3] = {camera.position.x, camera.position.y, camera.position.z};
    const float target[3] = {camera.target.x, camera.target.y, camera.target.z};
    const float up[3] = {camera.up.x, camera.up.y, camera.up.z};
    const Mat4 vp = multiply(perspective(camera.vertical_fov_radians,
                                         static_cast<float>(framebuffer_width) / framebuffer_height,
                                         camera.near_plane, camera.far_plane), look_at(eye, target, up));
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (uint32_t i=0;i<pose.model_pose.count;++i) {
        const auto& model = pose.model_pose[i];
        const matter::Float3 p = origin(model);
        const uint16_t parent = asset.joints[i].parent;
        const float radius = std::max(0.01f, asset.joints[i].radius);
        if (options.bones && parent != UINT16_MAX)
            draw_line(dl, vp, framebuffer_width, framebuffer_height, viewport_x, viewport_y,
                      origin(pose.model_pose[parent]), p, IM_COL32(255, 220, 90, 240), 2.0f);
        if (options.joint_axes) draw_axes(dl, vp, framebuffer_width, framebuffer_height, viewport_x, viewport_y, model, radius * .8f);
        if (options.radius_envelopes) {
            ImVec2 screen;
            if (project(vp, framebuffer_width, framebuffer_height, viewport_x, viewport_y, p, screen))
                dl->AddCircle(screen, std::max(2.0f, radius * 8.0f), IM_COL32(255, 190, 70, 150), 12, 1.0f);
        }
    }
    if (options.sockets) for (const auto& socket : asset.sockets) {
        const matter::Mat4f world = multiply(pose.model_pose[socket.joint], local_matrix(socket.local));
        draw_axes(dl, vp, framebuffer_width, framebuffer_height, viewport_x, viewport_y, world, .18f);
    }
    if (options.targets_and_ik) for (const auto& target : asset.targets) {
        const matter::Float3 start = origin(pose.model_pose[target.chain.front()]);
        const matter::Float3 end = origin(pose.model_pose[target.chain.back()]);
        draw_line(dl, vp, framebuffer_width, framebuffer_height, viewport_x, viewport_y, start, end, IM_COL32(210, 100, 255, 220), 1.0f);
        if (target.has_pole) draw_line(dl, vp, framebuffer_width, framebuffer_height, viewport_x, viewport_y, start, target.pole, IM_COL32(145, 100, 255, 180), 1.0f);
    }
    if (options.conservative_bounds) for (const auto& bound : asset.joint_bounds)
        draw_aabb(dl, vp, framebuffer_width, framebuffer_height, viewport_x, viewport_y,
                  pose.model_pose[bound.joint], bound);
    if (options.skin_weights && asset.lod0_influence_count) {
        ImGui::SetNextWindowBgAlpha(.35f);
        ImGui::SetNextWindowPos({viewport_x + 12.0f, viewport_y + 12.0f}, ImGuiCond_Always);
        ImGui::Begin("##animation-weight-summary", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs);
        ImGui::Text("Skin weights: LOD0 %u vertices", asset.lod0_influence_count);
        ImGui::End();
    }
}

void draw_animation_debug_overlay_controls(AnimationDebugOverlayOptions& options) {
    if (!ImGui::CollapsingHeader("Animation Overlay")) return;
    ImGui::Checkbox("Enabled##animation-overlay", &options.enabled);
    ImGui::BeginDisabled(!options.enabled);
    ImGui::Checkbox("Bones", &options.bones);
    ImGui::Checkbox("Joint axes", &options.joint_axes);
    ImGui::Checkbox("Radius envelopes", &options.radius_envelopes);
    ImGui::Checkbox("Sockets", &options.sockets);
    ImGui::Checkbox("Targets / IK", &options.targets_and_ik);
    ImGui::Checkbox("Conservative bounds", &options.conservative_bounds);
    ImGui::Checkbox("Skin weight summary", &options.skin_weights);
    ImGui::EndDisabled();
}

} // namespace viewer
