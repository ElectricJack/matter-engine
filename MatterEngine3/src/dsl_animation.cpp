#include "dsl_state.h"

#include "animation/animation_validate.h"
#include "animation/ozz_adapter.h"

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
using matter::animation::ClipTrack;
using matter::animation::InputSchema;
using matter::animation::TargetSchema;
using matter::animation::ControllerDef;
using matter::animation::GraphNode;
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
Quaternion qmul(const Quaternion& a, const Quaternion& b) {
    return {a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}
Quaternion qaxis(float x, float y, float z, float radians) {
    const float h = radians * 0.5f; const float s = std::sin(h);
    return {x*s, y*s, z*s, std::cos(h)};
}
void normalize_q(Quaternion& q) { const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w); if(n>1e-12f){q.x/=n;q.y/=n;q.z/=n;q.w/=n;} }
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
RigDebugState DslState::rig_debug_state() const {
    RigDebugState result;
    if (!animation_) return result;
    result.joint_count = animation_->authored.rig.joints.size();
    result.socket_count = animation_->authored.rig.sockets.size();
    result.current_parent = animation_->current_parent;
    result.radius = animation_->radius;
    return result;
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
    AnimationTransform normalized = local; canonicalize(normalized.rotation);
    animation_->authored.rig.joints.push_back({name, "", normalized, animation_->radius, rig_source_}); animation_->current_parent = name;
}
void DslState::rig_bone(const std::string& name, const AnimationTransform& local) {
    if (!rig_open()) { set_rig_error("bone outside an open rig session"); return; }
    if (animation_->current_parent.empty() || find_joint(animation_->authored, animation_->current_parent) < 0) { set_rig_error("bone has no valid selected parent"); return; }
    if (name.empty() || find_joint(animation_->authored, name) >= 0 || has_socket(animation_->authored, name)) { set_rig_error("duplicate joint name"); return; }
    if (!valid_transform(local)) { set_rig_error("bone requires a finite positive transform"); return; }
    AnimationTransform normalized = local; canonicalize(normalized.rotation);
    animation_->authored.rig.joints.push_back({name, animation_->current_parent, normalized, animation_->radius, rig_source_}); animation_->current_parent = name;
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
    AnimationTransform normalized = local; canonicalize(normalized.rotation);
    animation_->authored.rig.sockets.push_back({name, animation_->current_parent, normalized, rig_source_});
}
void DslState::rig_mirror_branch(const std::string& from, const std::string& to, int axis, const std::string& rename_from, const std::string& rename_to, const std::map<std::string, std::string>& names) {
    if (!rig_open()) { set_rig_error("mirrorBranch outside an open rig session"); return; }
    if (axis < 0 || axis > 2 || find_joint(animation_->authored, from) < 0 || to.empty()) { set_rig_error("mirrorBranch has an invalid source, axis, or destination"); return; }
    if (find_joint(animation_->authored, to) >= 0 || has_socket(animation_->authored, to)) { set_rig_error("mirrorBranch destination name collision"); return; }
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
    if (!matter::animation::validate_and_canonicalize_animation_build(candidate,canonical,diagnostics)) { set_rig_error(diagnostics.items.empty()?"rig validation failed":diagnostics.items.front().message, "rig-validation"); return; }
    animation_->canonical=std::move(canonical); animation_->open=false; animation_->ended=true;
}

void DslState::begin_clip(const std::string& name, float duration, float rate, bool loop, bool additive) {
    if(animation_&&animation_->generating){set_rig_error("structural authoring is forbidden during generate");return;}
    if (animation_ && animation_->clip_open) { set_rig_error("clip session already open"); return; }
    if (!animation_ || !animation_->ended) { set_rig_error("beginClip requires a completed rig"); return; }
    if (name.empty()) { set_rig_error("clip name must be non-empty"); return; }
    animation_->current_clip=name; animation_->clip_open=true; animation_->clip_duration=duration;
    animation_->clip_rate=rate; animation_->clip_loop=loop; animation_->clip_additive=additive;
    animation_->authored.clips.push_back({});
    auto& clip=animation_->authored.clips.back(); clip.name=name; clip.duration=duration; clip.rate=rate; clip.loop=loop; clip.additive=additive; clip.source=rig_source_;
}
void DslState::clip_duration(float duration) { if(animation_&&animation_->generating){set_rig_error("duration is structural and forbidden during generate");return;} if(!animation_||!animation_->clip_open){set_rig_error("duration outside an open clip");return;} animation_->clip_duration=duration; animation_->authored.clips.back().duration=duration; }
void DslState::clip_rate(float rate) { if(animation_&&animation_->generating){set_rig_error("sampleRate is structural and forbidden during generate");return;} if(!animation_||!animation_->clip_open){set_rig_error("sampleRate outside an open clip");return;} animation_->clip_rate=rate; animation_->authored.clips.back().rate=rate; }
void DslState::clip_loop(bool loop) { if(animation_&&animation_->generating){set_rig_error("loop is structural and forbidden during generate");return;} if(!animation_||!animation_->clip_open){set_rig_error("loop outside an open clip");return;} animation_->clip_loop=loop; animation_->authored.clips.back().loop=loop; }
void DslState::clip_mode(bool additive) { if(animation_&&animation_->generating){set_rig_error("mode is structural and forbidden during generate");return;} if(!animation_||!animation_->clip_open){set_rig_error("mode outside an open clip");return;} animation_->clip_additive=additive; animation_->authored.clips.back().additive=additive; }
void DslState::clip_at(const std::string& joint) {
    if(!animation_||!animation_->clip_open){set_rig_error("at outside an open clip");return;}
    if(find_joint(animation_->authored,joint)<0){set_rig_error("clip at selects an unknown joint");return;}
    animation_->name=joint;
}
void DslState::clip_rotate(float x,float y,float z,float radians) {
    if(!animation_||!animation_->clip_open){set_rig_error("clip rotation outside an open clip");return;}
    const int j=find_joint(animation_->authored,animation_->name); if(j<0){set_rig_error("clip rotation has no selected joint");return;}
    if(!finite(radians)){set_rig_error("clip rotation must be finite");return;}
    auto& p=animation_->clip_pose[(size_t)j]; p.rotation=qmul(p.rotation,qaxis(x,y,z,radians)); normalize_q(p.rotation);
}
void DslState::clip_translate(float x,float y,float z) {
    if(!animation_||!animation_->clip_open){set_rig_error("clip translation outside an open clip");return;}
    const int j=find_joint(animation_->authored,animation_->name); if(j<0){set_rig_error("clip translation has no selected joint");return;}
    if(!finite(x)||!finite(y)||!finite(z)){set_rig_error("clip translation must be finite");return;}
    auto& p=animation_->clip_pose[(size_t)j]; p.translation.x+=x; p.translation.y+=y; p.translation.z+=z;
}
void DslState::clip_key(const std::string& joint,float time,const AnimationTransform& value) {
    if(animation_&&animation_->generating){set_rig_error("key is structural and forbidden during generate");return;}
    if(!animation_||!animation_->clip_open){set_rig_error("key outside an open clip");return;}
    if(find_joint(animation_->authored,joint)<0){set_rig_error("clip key references an unknown joint");return;}
    ClipTrack* track=nullptr; auto& tracks=animation_->authored.clips.back().tracks; for(auto& t:tracks)if(t.joint==joint)track=&t;
    if(!track){tracks.push_back({joint,{},rig_source_});track=&tracks.back();} track->keys.push_back({time,value,rig_source_});
}
void DslState::clip_marker(float normalized_time,const std::string& name) {
    if(animation_&&animation_->generating){set_rig_error("marker is structural and forbidden during generate");return;}
    if(!animation_||!animation_->clip_open){set_rig_error("marker outside an open clip");return;}
    animation_->authored.clips.back().markers.push_back({name,normalized_time,rig_source_});
}
bool DslState::begin_clip_sample() {
    if(!animation_||!animation_->clip_open){set_rig_error("generate outside an open clip");return false;}
    if(animation_->generating){set_rig_error("nested generate callback is forbidden");return false;}
    animation_->generating=true; animation_->clip_pose.clear(); animation_->clip_pose_joints.clear();
    for(const auto& j:animation_->authored.rig.joints){animation_->clip_pose.push_back(j.local);animation_->clip_pose_joints.push_back(j.name);}
    animation_->name.clear(); return true;
}
bool DslState::capture_clip_sample(float phase) {
    if(!animation_||!animation_->clip_open||animation_->clip_pose.size()!=animation_->authored.rig.joints.size()){if(animation_)animation_->generating=false;return false;}
    auto& clip=animation_->authored.clips.back();
    for(auto& track:clip.tracks) std::stable_sort(track.keys.begin(),track.keys.end(),[](const auto&a,const auto&b){return a.time<b.time;});
    std::stable_sort(clip.markers.begin(),clip.markers.end(),[](const auto&a,const auto&b){return a.time<b.time;});
    for(size_t i=0;i<animation_->clip_pose.size();++i){ ClipTrack* track=nullptr; for(auto& t:clip.tracks)if(t.joint==animation_->clip_pose_joints[i])track=&t; if(!track){clip.tracks.push_back({animation_->clip_pose_joints[i],{},rig_source_});track=&clip.tracks.back();} track->keys.push_back({phase*clip.duration,animation_->clip_pose[i],rig_source_}); }
    animation_->generating=false; return true;
}
uint32_t DslState::clip_sample_segments() const { if(!animation_||!animation_->clip_open||!finite(animation_->clip_duration)||!finite(animation_->clip_rate)||animation_->clip_duration<=0||animation_->clip_rate<=0)return 0; const double n=std::ceil((double)animation_->clip_duration*(double)animation_->clip_rate); return (uint32_t)std::max(1.0,std::min(n,(double)UINT32_MAX)); }
bool DslState::clip_is_loop() const { return animation_&&animation_->clip_open&&animation_->clip_loop; }
void DslState::end_clip() {
    if(animation_&&animation_->generating){set_rig_error("endClip is forbidden during generate");return;}
    if(!animation_||!animation_->clip_open){set_rig_error("endClip outside an open clip");return;}
    auto& clip=animation_->authored.clips.back();
    for(auto& track:clip.tracks) std::stable_sort(track.keys.begin(),track.keys.end(),[](const auto& left,const auto& right){return left.time<right.time;});
    std::stable_sort(clip.markers.begin(),clip.markers.end(),[](const auto& left,const auto& right){return left.time<right.time;});
    if(clip.loop){for(auto& t:clip.tracks)if(!t.keys.empty())t.keys.push_back({clip.duration,t.keys.front().value,rig_source_});}
    AnimationBuild validation = animation_->authored; validation.graph.nodes.push_back({"__clip_validation_output",{},true,matter::animation::EvaluationCadence::Fixed,rig_source_}); matter::animation::Diagnostics vd; if(!matter::animation::validate_animation_build(validation,vd)){set_rig_error(vd.items.empty()?"clip validation failed":vd.items.front().message,"clip-validation");return;}
    matter::animation::Diagnostics d; matter::animation::OzzSkeleton sk; matter::animation::OzzAnimation oa;
    if(!matter::animation::build_skeleton(animation_->authored.rig,sk,d)||!matter::animation::build_clip(animation_->authored.rig,clip,oa,d)){set_rig_error(d.items.empty()?"clip compilation failed":d.items.front().message,"clip-compile");return;}
    if(animation_->authored.ozz_skeleton_blob.empty()&&!matter::animation::serialize_skeleton(sk,animation_->authored.ozz_skeleton_blob)){set_rig_error("skeleton serialization failed","clip-compile");return;}
    if(!matter::animation::serialize_animation(oa,clip.ozz_blob)||clip.ozz_blob.empty()){set_rig_error("clip serialization failed","clip-compile");return;}
    animation_->clip_open=false; animation_->current_clip.clear(); animation_->name.clear();
}
void DslState::begin_motion(const std::string& name) { if(animation_&&animation_->generating){set_rig_error("structural authoring is forbidden during generate");return;} if(!animation_||!animation_->ended){set_rig_error("beginMotion requires a completed rig");return;} if(animation_->clip_open){set_rig_error("beginMotion cannot nest inside a clip");return;} if(animation_->motion_open){set_rig_error("motion session already open");return;} auto& nodes=animation_->authored.graph.nodes; nodes.erase(std::remove_if(nodes.begin(),nodes.end(),[](const GraphNode& n){return n.name=="__rig_only_output";}),nodes.end()); animation_->motion_open=true; animation_->current_motion=name; }
void DslState::motion_input(const InputSchema& input){if(!animation_||!animation_->motion_open){set_rig_error("input outside an open motion");return;} animation_->authored.inputs.push_back(input);}
void DslState::motion_target(const TargetSchema& target){if(!animation_||!animation_->motion_open){set_rig_error("target outside an open motion");return;} TargetSchema copy=target; copy.require_explicit_pole = copy.require_explicit_pole || copy.source.module=="<part>"; animation_->authored.targets.push_back(std::move(copy));}
void DslState::motion_controller(const ControllerDef& controller){if(!animation_||!animation_->motion_open){set_rig_error("controller outside an open motion");return;} animation_->authored.controllers.push_back(controller);}
void DslState::motion_node(const GraphNode& node){if(!animation_||!animation_->motion_open){set_rig_error("graph node outside an open motion");return;} animation_->authored.graph.nodes.push_back(node);}
void DslState::end_motion(){if(animation_&&animation_->generating){set_rig_error("endMotion is forbidden during generate");return;} if(!animation_||!animation_->motion_open){set_rig_error("endMotion outside an open motion");return;} matter::animation::Diagnostics d; matter::animation::CanonicalAnimationBuild c; if(!matter::animation::validate_and_canonicalize_animation_build(animation_->authored,c,d)){set_rig_error(d.items.empty()?"motion validation failed":d.items.front().message,"motion-validation");return;} animation_->canonical=std::move(c); animation_->motion_open=false; animation_->current_motion.clear();}
const std::optional<matter::animation::CanonicalAnimationBuild>& DslState::canonical_animation() const { return canonical_rig(); }
const matter::animation::AnimationBuild* DslState::authored_animation() const { return animation_ ? &animation_->authored : nullptr; }

} // namespace dsl
