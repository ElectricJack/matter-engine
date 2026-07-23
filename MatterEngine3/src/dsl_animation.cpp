#include "dsl_state.h"

#include "animation/animation_validate.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace dsl {
namespace {
using matter::AnimationTransform;
using matter::Float3;
using matter::Quaternion;
using matter::animation::AnimationBuild;
using matter::animation::JointDef;
using matter::animation::SocketDef;
using matter::animation::SourceSpan;

bool finite(float value) { return std::isfinite(value); }
bool finite3(const Float3& value) { return finite(value.x) && finite(value.y) && finite(value.z); }
bool finiteq(const Quaternion& value) { return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w); }
bool valid_transform(const AnimationTransform& value) {
    const float length2 = value.rotation.x*value.rotation.x + value.rotation.y*value.rotation.y + value.rotation.z*value.rotation.z + value.rotation.w*value.rotation.w;
    return finite3(value.translation) && finiteq(value.rotation) && finite3(value.scale) && length2 > 1e-12f &&
           value.scale.x > 0.0f && value.scale.y > 0.0f && value.scale.z > 0.0f;
}
int find_joint(const AnimationBuild& build, const std::string& name) {
    for (size_t i = 0; i < build.rig.joints.size(); ++i) if (build.rig.joints[i].name == name) return static_cast<int>(i);
    return -1;
}
bool has_socket(const AnimationBuild& build, const std::string& name) {
    return std::any_of(build.rig.sockets.begin(), build.rig.sockets.end(), [&](const SocketDef& s) { return s.name == name; });
}
void canonicalize(Quaternion& q) {
    const float length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (length == 0.0f) return;
    q.x /= length; q.y /= length; q.z /= length; q.w /= length;
    const float sign = q.w != 0.0f ? q.w : (q.x != 0.0f ? q.x : (q.y != 0.0f ? q.y : q.z));
    if (sign < 0.0f) { q.x=-q.x; q.y=-q.y; q.z=-q.z; q.w=-q.w; }
}
Quaternion reflect_rotation(Quaternion q, int axis) {
    canonicalize(q);
    const float x=q.x, y=q.y, z=q.z, w=q.w;
    float r[3][3] = {{1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)}, {2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)}, {2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)}};
    const float s[3] = {axis == 0 ? -1.0f : 1.0f, axis == 1 ? -1.0f : 1.0f, axis == 2 ? -1.0f : 1.0f};
    for (int row=0; row<3; ++row) for (int col=0; col<3; ++col) r[row][col] *= s[row]*s[col];
    Quaternion out{}; const float trace=r[0][0]+r[1][1]+r[2][2];
    if (trace > 0) { float t=std::sqrt(trace+1)*2; out.w=.25f*t; out.x=(r[2][1]-r[1][2])/t; out.y=(r[0][2]-r[2][0])/t; out.z=(r[1][0]-r[0][1])/t; }
    else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) { float t=std::sqrt(1+r[0][0]-r[1][1]-r[2][2])*2; out.w=(r[2][1]-r[1][2])/t; out.x=.25f*t; out.y=(r[0][1]+r[1][0])/t; out.z=(r[0][2]+r[2][0])/t; }
    else if (r[1][1] > r[2][2]) { float t=std::sqrt(1+r[1][1]-r[0][0]-r[2][2])*2; out.w=(r[0][2]-r[2][0])/t; out.x=(r[0][1]+r[1][0])/t; out.y=.25f*t; out.z=(r[1][2]+r[2][1])/t; }
    else { float t=std::sqrt(1+r[2][2]-r[0][0]-r[1][1])*2; out.w=(r[1][0]-r[0][1])/t; out.x=(r[0][2]+r[2][0])/t; out.y=(r[1][2]+r[2][1])/t; out.z=.25f*t; }
    canonicalize(out); return out;
}
AnimationTransform reflected(AnimationTransform value, int axis) {
    if (axis == 0) value.translation.x = -value.translation.x;
    if (axis == 1) value.translation.y = -value.translation.y;
    if (axis == 2) value.translation.z = -value.translation.z;
    value.rotation = reflect_rotation(value.rotation, axis);
    value.scale.x = std::fabs(value.scale.x); value.scale.y = std::fabs(value.scale.y); value.scale.z = std::fabs(value.scale.z);
    return value;
}
bool token_name(const std::string& source, const std::string& from, const std::string& to, std::string& out) {
    const size_t pos = source.find(from);
    if (from.empty() || pos == std::string::npos || source.find(from, pos + from.size()) != std::string::npos) return false;
    out = source; out.replace(pos, from.size(), to); return true;
}
} // namespace

const std::optional<matter::animation::CanonicalAnimationBuild>& DslState::canonical_rig() const {
    static const std::optional<matter::animation::CanonicalAnimationBuild> none;
    return animation_ ? animation_->canonical : none;
}
uint64_t DslState::begin_rig(const std::string& name) {
    if (animation_) { set_rig_error("only one rig is permitted per bake"); return 0; }
    if (session_ != Session::None || region_open_ || polygon_open_ || contour_open_) { set_rig_error("beginRig inside an open authoring session"); return 0; }
    animation_ = std::make_unique<AnimationBuildBuffer>(); animation_->open = true; animation_->name = name;
    return animation_->handle;
}
void DslState::rig_root(const std::string& name, const AnimationTransform& local) {
    if (!rig_open()) { set_rig_error("root outside an open rig session"); return; }
    if (!animation_->authored.rig.joints.empty()) { set_rig_error("multiple roots in rig session"); return; }
    if (name.empty() || !valid_transform(local)) { set_rig_error("root requires a finite positive transform"); return; }
    animation_->authored.rig.joints.push_back({name, "", local, animation_->radius, rig_source_}); animation_->current_parent = name;
}
void DslState::rig_bone(const std::string& name, const AnimationTransform& local) {
    if (!rig_open()) { set_rig_error("bone outside an open rig session"); return; }
    if (animation_->current_parent.empty() || find_joint(animation_->authored, animation_->current_parent) < 0) { set_rig_error("bone has no valid selected parent"); return; }
    if (name.empty() || find_joint(animation_->authored, name) >= 0 || has_socket(animation_->authored, name)) { set_rig_error("duplicate joint name"); return; }
    if (!valid_transform(local)) { set_rig_error("bone requires a finite positive transform"); return; }
    animation_->authored.rig.joints.push_back({name, animation_->current_parent, local, animation_->radius, rig_source_}); animation_->current_parent = name;
}
void DslState::rig_push() {
    if (!rig_open()) { set_rig_error("push outside an open rig session"); return; }
    if (animation_->current_parent.empty()) { set_rig_error("push has no selected joint"); return; }
    animation_->stack.push_back({animation_->current_parent, animation_->radius});
}
void DslState::rig_pop() {
    if (!rig_open()) { set_rig_error("pop outside an open rig session"); return; }
    if (animation_->stack.empty()) { set_rig_error("pop without matching push"); return; }
    const RigCursor cursor = animation_->stack.back(); animation_->stack.pop_back(); animation_->current_parent=cursor.parent; animation_->radius=cursor.radius;
}
void DslState::rig_at_joint(const std::string& name) {
    if (!rig_open()) { set_rig_error("atJoint outside an open rig session"); return; }
    if (find_joint(animation_->authored, name) < 0) { set_rig_error("atJoint selects an unknown joint"); return; }
    animation_->current_parent = name;
}
void DslState::rig_radius(float value) {
    if (!rig_open()) { set_rig_error("radius outside an open rig session"); return; }
    if (!finite(value) || value <= 0.0f) { set_rig_error("radius must be finite and positive"); return; }
    animation_->radius = value;
}
void DslState::rig_socket(const std::string& name, const AnimationTransform& local) {
    if (!rig_open()) { set_rig_error("socket outside an open rig session"); return; }
    if (animation_->current_parent.empty() || find_joint(animation_->authored, animation_->current_parent) < 0) { set_rig_error("socket has no valid selected parent"); return; }
    if (name.empty() || has_socket(animation_->authored, name) || find_joint(animation_->authored, name) >= 0) { set_rig_error("duplicate socket name"); return; }
    if (!valid_transform(local)) { set_rig_error("socket requires a finite positive transform"); return; }
    animation_->authored.rig.sockets.push_back({name, animation_->current_parent, local, rig_source_});
}
void DslState::rig_mirror_branch(const std::string& from, const std::string& to, int axis, const std::string& rename_from, const std::string& rename_to, const std::map<std::string, std::string>& names) {
    if (!rig_open()) { set_rig_error("mirrorBranch outside an open rig session"); return; }
    if (axis < 0 || axis > 2 || find_joint(animation_->authored, from) < 0 || to.empty() || find_joint(animation_->authored, to) >= 0 || has_socket(animation_->authored, to)) { set_rig_error("mirrorBranch has an invalid source, axis, or destination"); return; }
    std::vector<JointDef> joints; std::vector<std::string> queue{from};
    for (size_t i=0; i<queue.size(); ++i) for (const JointDef& joint : animation_->authored.rig.joints) if (joint.parent == queue[i]) queue.push_back(joint.name);
    for (const std::string& name : queue) joints.push_back(animation_->authored.rig.joints[find_joint(animation_->authored, name)]);
    std::vector<SocketDef> sockets; for (const SocketDef& socket : animation_->authored.rig.sockets) if (std::find(queue.begin(), queue.end(), socket.joint) != queue.end()) sockets.push_back(socket);
    if (!names.empty()) {
        std::set<std::string> required(queue.begin() + 1, queue.end()); for (const SocketDef& socket : sockets) required.insert(socket.name);
        if (names.size() != required.size()) { set_rig_error("mirrorBranch explicit name map is incomplete"); return; }
        for (const auto& entry : names) if (!required.count(entry.first) || entry.second.empty()) { set_rig_error("mirrorBranch explicit name map is incomplete"); return; }
    }
    std::map<std::string, std::string> remap; remap[from]=to;
    for (size_t i=1; i<queue.size(); ++i) {
        std::string dest;
        if (!names.empty()) { auto it=names.find(queue[i]); if (it == names.end()) { set_rig_error("mirrorBranch explicit name map is incomplete"); return; } dest=it->second; }
        else if (!token_name(queue[i], rename_from, rename_to, dest)) { set_rig_error("mirrorBranch rename token must occur exactly once"); return; }
        if (dest.empty() || find_joint(animation_->authored,dest) >= 0 || has_socket(animation_->authored,dest) || std::any_of(remap.begin(), remap.end(), [&](const auto& p){ return p.second == dest; })) { set_rig_error("mirrorBranch name collision"); return; }
        remap[queue[i]]=dest;
    }
    // Preflight every socket before appending a single joint. A late socket
    // failure must not leave a half-mirrored rig behind.
    std::vector<std::pair<SocketDef, std::string>> mirrored_sockets;
    std::set<std::string> occupied;
    for (const JointDef& joint : animation_->authored.rig.joints) occupied.insert(joint.name);
    for (const SocketDef& socket : animation_->authored.rig.sockets) occupied.insert(socket.name);
    for (const auto& entry : remap) occupied.insert(entry.second);
    for (const SocketDef& source : sockets) {
        std::string name;
        if (!names.empty()) name=names.at(source.name);
        else if (!token_name(source.name, rename_from, rename_to, name)) { set_rig_error("mirrorBranch socket rename token must occur exactly once"); return; }
        if (name.empty() || !occupied.insert(name).second) { set_rig_error("mirrorBranch socket name collision"); return; }
        mirrored_sockets.push_back({source, name});
    }
    for (const JointDef& source : joints) { AnimationTransform local=reflected(source.local,axis); const std::string parent = source.name == from ? source.parent : remap[source.parent]; animation_->authored.rig.joints.push_back({remap[source.name], parent, local, source.radius, rig_source_}); }
    for (const auto& cloned : mirrored_sockets) animation_->authored.rig.sockets.push_back({cloned.second,remap[cloned.first.joint],reflected(cloned.first.local,axis),rig_source_});
}
void DslState::end_rig() {
    if (!rig_open()) { set_rig_error("endRig outside an open rig session"); return; }
    if (!animation_->stack.empty()) { set_rig_error("rig stack left unbalanced at endRig"); return; }
    AnimationBuild candidate=animation_->authored; candidate.graph.nodes.push_back({"__rig_only_output",{},true,matter::animation::EvaluationCadence::Fixed,rig_source_});
    matter::animation::Diagnostics diagnostics; matter::animation::CanonicalAnimationBuild canonical;
    if (!matter::animation::validate_and_canonicalize_animation_build(candidate,canonical,diagnostics)) { set_rig_error(diagnostics.items.empty()?"rig validation failed":diagnostics.items.front().message); return; }
    animation_->canonical=std::move(canonical); animation_->open=false; animation_->ended=true;
}

} // namespace dsl
