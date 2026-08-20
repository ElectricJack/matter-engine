// ---------------------------------------------------------------------------
// MatterEngine3/src/dsl_animation.cpp
//
// The rig / clip / motion / binding half of `dsl::DslState`. Every animation
// authoring verb a part script can call lands here: `beginRig`/`root`/`bone`/
// `socket`/`push`/`pop`/`atJoint`/`radius`/`mirrorBranch`/`endRig`, the clip
// verbs (`beginClip` .. `generate` .. `endClip`), the motion-graph verbs
// (`beginMotion`/`input`/`target`/`controller`/nodes/`endMotion`) and the
// binding verbs (`skin`, `segments`, `attach`, `bind`).
//
// How it fits:
//   - The JS entry points are the `j_*` functions in `dsl_bindings.cpp`; they
//     coerce arguments and then call the `DslState::rig_*` / `clip_*` /
//     `motion_*` methods below. Nothing in the engine calls this directly.
//   - All authored state is held in `DslState::animation_`, an
//     `AnimationBuildBuffer` (`dsl_animation.h`) created by `begin_rig` and
//     destroyed with the `DslState`. There is at most ONE rig per bake.
//   - Validation and canonicalization live in `animation/animation_validate.h`;
//     the Ozz skeleton/clip compilers in `animation/ozz_adapter.h`. The IR
//     types (`AnimationBuild`, `JointDef`, `ClipTrack`, `GraphNode`, ...) come
//     from `animation/animation_ir.h`.
//
// Call order: `beginRig` -> `root` -> bones/sockets -> `endRig`, and only then
// clips, motions and bindings, each of which requires a COMPLETED rig.
//
// Error model: every verb is fail-soft. A rejected argument or a wrong session
// state calls `set_rig_error`, which is sticky and first-error-wins, and the
// verb returns. Nothing throws and the JS script keeps running; the bake
// harvests the error afterwards. Failed validation additionally records the
// full `Diagnostics` list via `record_animation_diagnostics`.
//
// Canonicalization: `buffer.canonical` is refreshed after each successful
// structural edit (`refresh_canonical_animation`). It is therefore absent when
// no rig was authored AND when the most recent edit failed validation — never
// stale-but-present.
//
// Geometry: bindings do not own triangles. `rig_skin` and the `bind(name, fn)`
// scope record RANGES — `[op_begin, op_end)` into `DslState::buffer_.ops` and
// `[triangle_begin, triangle_end)` into the triangle buffer — so the geometry
// itself stays in the shared build buffer and `cancel_binding_scope` can
// truncate both back.
//
// Units and conventions: metres for translations and joint radii, radians for
// `clip_rotate`, seconds for clip durations and key times, samples/second for
// the clip rate. Quaternions are stored xyzw and canonicalized (unit length,
// positive leading component) so the same authored rig always hashes the same.
//
// Threading: one `DslState` and one QuickJS context per bake worker. Nothing
// here is shared between threads and nothing here takes a lock.
// ---------------------------------------------------------------------------
#include "dsl_state.h"
#include "triangle_emit.hpp"

#include "animation/animation_validate.h"
#include "animation/animation_binding_bake.h"
#include "animation/ozz_adapter.h"
#include "animation/animation_math.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

// Validity gate shared by every verb that stores a transform. `valid_transform`
// is the whole contract: finite translation/rotation/scale, a rotation with
// non-degenerate length (so it can be normalized), and a strictly positive
// scale on all three axes. A transform that fails it is refused at the verb
// rather than being repaired.
bool finite(float value) { return std::isfinite(value); }
bool finite3(const Float3& value) { return finite(value.x) && finite(value.y) && finite(value.z); }
bool finiteq(const Quaternion& value) { return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w); }
bool valid_transform(const AnimationTransform& value) {
    const float length2 = value.rotation.x*value.rotation.x + value.rotation.y*value.rotation.y + value.rotation.z*value.rotation.z + value.rotation.w*value.rotation.w;
    return finite3(value.translation) && finiteq(value.rotation) && finite3(value.scale) && length2 > 1e-12f &&
           value.scale.x > 0.0f && value.scale.y > 0.0f && value.scale.z > 0.0f;
}
// Joint / socket lookup by name. Linear scans over the authored rig, called
// once or more per verb — fine at authoring scale (a rig is tens of joints),
// but it is O(joints) and `rig_mirror_branch` calls it inside a loop.
// Returns -1 for an unknown joint; joints and sockets share one name space.
int find_joint(const AnimationBuild& build, const std::string& name) {
    for (size_t i = 0; i < build.rig.joints.size(); ++i) if (build.rig.joints[i].name == name) return static_cast<int>(i);
    return -1;
}
bool has_socket(const AnimationBuild& build, const std::string& name) {
    return std::any_of(build.rig.sockets.begin(), build.rig.sockets.end(), [&](const SocketDef& s) { return s.name == name; });
}
// Normalize in place and pick a canonical sign: `q` and `-q` are the same
// rotation, so the first non-zero component of (w, x, y, z) is forced positive.
// That keeps the authored bytes — and therefore the part's resolved hash —
// stable across two spellings of the same pose. A zero quaternion is left
// alone (`valid_transform` rejects it upstream).
void canonicalize(Quaternion& q) {
    const float length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (length == 0.0f) return;
    q.x /= length; q.y /= length; q.z /= length; q.w /= length;
    const float sign = q.w != 0.0f ? q.w : (q.x != 0.0f ? q.x : (q.y != 0.0f ? q.y : q.z));
    if (sign < 0.0f) { q.x=-q.x; q.y=-q.y; q.z=-q.z; q.w=-q.w; }
}
// Mirror a rotation across the plane normal to `axis` (0 = x, 1 = y, 2 = z).
// Expands the quaternion to a rotation matrix, conjugates it by the reflection
// (negating the mirrored row AND column, which keeps the determinant +1 so the
// result is still a rotation), then reads a quaternion back out. Used only by
// `mirrorBranch`.
Quaternion reflect_rotation(Quaternion q, int axis) {
    canonicalize(q);
    const float x=q.x, y=q.y, z=q.z, w=q.w;
    float r[3][3] = {{1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)},
                     {2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)},
                     {2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)}};
    const float s[3] = {axis == 0 ? -1.0f : 1.0f, axis == 1 ? -1.0f : 1.0f, axis == 2 ? -1.0f : 1.0f};
    for (int row=0; row<3; ++row) for (int col=0; col<3; ++col) r[row][col] *= s[row]*s[col];

    Quaternion out{};
    const float trace = r[0][0] + r[1][1] + r[2][2];
    // All four Shepperd branches: trace == 1 + 2*cos(theta), so the trace>0 case
    // alone covers only rotations under 120 degrees.
    if (trace > 0) {
        float t = std::sqrt(trace + 1) * 2;
        out.w = .25f * t;
        out.x = (r[2][1] - r[1][2]) / t;
        out.y = (r[0][2] - r[2][0]) / t;
        out.z = (r[1][0] - r[0][1]) / t;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        float t = std::sqrt(1 + r[0][0] - r[1][1] - r[2][2]) * 2;
        out.w = (r[2][1] - r[1][2]) / t;
        out.x = .25f * t;
        out.y = (r[0][1] + r[1][0]) / t;
        out.z = (r[0][2] + r[2][0]) / t;
    } else if (r[1][1] > r[2][2]) {
        float t = std::sqrt(1 + r[1][1] - r[0][0] - r[2][2]) * 2;
        out.w = (r[0][2] - r[2][0]) / t;
        out.x = (r[0][1] + r[1][0]) / t;
        out.y = .25f * t;
        out.z = (r[1][2] + r[2][1]) / t;
    } else {
        float t = std::sqrt(1 + r[2][2] - r[0][0] - r[1][1]) * 2;
        out.w = (r[1][0] - r[0][1]) / t;
        out.x = (r[0][2] + r[2][0]) / t;
        out.y = (r[1][2] + r[2][1]) / t;
        out.z = .25f * t;
    }
    canonicalize(out);
    return out;
}
Quaternion qmul(const Quaternion& a, const Quaternion& b) {
    return matter::animation::quaternion_multiply(a, b);
}
// Rodrigues form: v + 2*(w*(u x v) + u x (u x v)), with u the vector part.
Float3 qrotate(const Quaternion& q, const Float3& value) {
    const Float3 u{q.x, q.y, q.z};
    const Float3 uv{u.y*value.z - u.z*value.y,
                    u.z*value.x - u.x*value.z,
                    u.x*value.y - u.y*value.x};
    const Float3 uuv{u.y*uv.z - u.z*uv.y,
                     u.z*uv.x - u.x*uv.z,
                     u.x*uv.y - u.y*uv.x};
    return {value.x + 2.0f * (q.w*uv.x + uuv.x),
            value.y + 2.0f * (q.w*uv.y + uuv.y),
            value.z + 2.0f * (q.w*uv.z + uuv.z)};
}

Quaternion qaxis(float x, float y, float z, float radians) {
    const float h = radians * 0.5f;
    const float s = std::sin(h);
    return {x*s, y*s, z*s, std::cos(h)};
}

void normalize_q(Quaternion& q) {
    const float n = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (n > 1e-12f) {
        q.x /= n;
        q.y /= n;
        q.z /= n;
        q.w /= n;
    }
}
// Mirror a joint-local transform across `axis`: negate that component of the
// translation, mirror the rotation, and take the absolute value of the scale.
// The mirror lives entirely in the translation and rotation — a negative scale
// would flip triangle winding on any geometry bound to the mirrored branch.
AnimationTransform reflected(AnimationTransform value, int axis) {
    if (axis == 0) value.translation.x = -value.translation.x;
    if (axis == 1) value.translation.y = -value.translation.y;
    if (axis == 2) value.translation.z = -value.translation.z;
    value.rotation = reflect_rotation(value.rotation, axis);
    value.scale.x = std::fabs(value.scale.x); value.scale.y = std::fabs(value.scale.y); value.scale.z = std::fabs(value.scale.z);
    return value;
}
// `mirrorBranch`'s implicit rename: replace `from` with `to` in `source`, but
// only when `from` occurs EXACTLY once. Zero occurrences, two or more, or an
// empty `from` all return false, so an ambiguous rename is reported as an
// authoring error instead of being guessed at.
bool token_name(const std::string& source, const std::string& from, const std::string& to, std::string& out) {
    const size_t pos = source.find(from);
    if (from.empty() || pos == std::string::npos || source.find(from, pos + from.size()) != std::string::npos) return false;
    out = source; out.replace(pos, from.size(), to); return true;
}

// Re-validate the authored build and replace `buffer.canonical`. Called after
// every structural edit that can change the IR (endRig, each binding verb).
//
// A build with no graph nodes gets a synthetic `__rig_only_output` node so a
// rig-only part still canonicalizes; `begin_motion` erases that node again when
// a real motion graph is declared.
//
// On failure `buffer.canonical` is left untouched and `diagnostics` carries the
// reasons — the caller records them and raises the rig error.
bool refresh_canonical_animation(AnimationBuildBuffer& buffer, const SourceSpan& source,
                                 matter::animation::Diagnostics& diagnostics) {
    AnimationBuild candidate = buffer.authored;
    if (candidate.graph.nodes.empty())
        candidate.graph.nodes.push_back({"__rig_only_output", {}, true,
                                         matter::animation::EvaluationCadence::Fixed, source});
    matter::animation::CanonicalAnimationBuild canonical;
    if (!matter::animation::validate_and_canonicalize_animation_build(candidate, canonical, diagnostics)) return false;
    buffer.canonical = std::move(canonical);
    return true;
}
} // namespace

// The last successfully validated canonical build. Empty when no rig was
// authored, and also empty when the most recent structural edit failed
// validation. The no-rig case returns a reference to a function-local static
// so the returned reference is always valid.
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
// Open the one rig a part may author. Returns the buffer's handle, or 0 after
// setting the rig error — a second `beginRig`, or one inside an open geometry
// session, is refused. The handle is a constant (there is exactly one rig per
// bake); scripts hold it only to spell `rig.<verb>()`.
uint64_t DslState::begin_rig(const std::string& name) {
    if (animation_) { set_rig_error("only one rig is permitted per bake"); return 0; }
    if (session_ != Session::None || region_open_ || polygon_open_ || contour_open_) { set_rig_error("beginRig inside an open authoring session"); return 0; }
    animation_ = std::make_unique<AnimationBuildBuffer>(); animation_->open = true; animation_->name = name;
    return animation_->handle;
}
// ---------------------------------------------------------------------------
// Rig cursor verbs.
//
// The rig is authored with a cursor, not with explicit parent arguments:
// `root` and `bone` both APPEND a joint and then select it as the parent for
// whatever is added next; `push`/`pop` save and restore the (parent, radius)
// pair; `atJoint` re-selects an existing joint without appending anything.
//
// `local` is parent-relative (metres for the translation, xyzw quaternion for
// the rotation) and is canonicalized before it is stored. The joint also
// captures the cursor's current `radius` (metres) — that is what generated skin
// geometry is sized from, so `radius()` must be set BEFORE the bones it should
// apply to.
// ---------------------------------------------------------------------------
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
// Clone the subtree rooted at `from`, mirrored across `axis` (0 = x, 1 = y,
// 2 = z), including every socket attached to a joint in that subtree.
//
// Destination names come from one of two spellings: an explicit `names` map,
// which must cover every descendant joint and socket exactly (the subtree root
// itself maps to `to`), or — when the map is empty — the single-occurrence
// token replacement `rename_from` -> `rename_to` (see `token_name`).
//
// All-or-nothing: every collision and every rename ambiguity, for joints AND
// sockets, is checked before the first joint is appended, so a rejected
// mirrorBranch leaves the rig exactly as it was.
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
// Close the rig and canonicalize it. Requires a balanced push/pop stack.
//
// On validation failure the rig stays OPEN (`open` true, `ended` false) and the
// rig error is set, so every later clip/motion/binding verb also fails with
// "requires a completed rig" rather than silently authoring against a rig that
// never validated.
void DslState::end_rig() {
    if (!rig_open()) { set_rig_error("endRig outside an open rig session"); return; }
    if (!animation_->stack.empty()) { set_rig_error("rig stack left unbalanced at endRig"); return; }
    matter::animation::Diagnostics diagnostics;
    if (!refresh_canonical_animation(*animation_, rig_source_, diagnostics)) { record_animation_diagnostics(diagnostics); set_rig_error(diagnostics.items.empty()?"rig validation failed":diagnostics.items.front().message, "rig-validation"); return; }
    animation_->open=false; animation_->ended=true;
}

// ---------------------------------------------------------------------------
// Clip authoring.
//
// `duration` is seconds; `rate` is samples per second and is only consumed by
// `generate()` (see `clip_sample_segments`). The ClipTrack container is pushed
// immediately, and every `clip_*` verb below writes into `clips.back()` — which
// is why only one clip may be open at a time.
//
// Structural clip authoring is refused inside a `bind` scope and inside a
// `generate()` callback; the pose verbs (`clip_at`/`clip_rotate`/
// `clip_translate`) are the only ones legal during generate.
// ---------------------------------------------------------------------------
void DslState::begin_clip(const std::string& name, float duration, float rate, bool loop, bool additive) {
    if (binding_scope_) { set_rig_error("structural animation authoring is forbidden inside a bind scope"); return; }
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
// Select the joint the pose verbs write to.
//
// TWO THINGS WORTH KNOWING. The selection is stored in `animation_->name` —
// the same field `beginRig` put the rig's name in; while a clip is open that
// field means "selected joint". And `DslState::translate`/`rotateX`/`rotateY`/
// `rotateZ` (dsl_state.cpp) REROUTE into `clip_translate`/`clip_rotate`
// whenever a clip is open and this selection is non-empty. That reroute is how
// a `generate()` callback poses the rig with the ordinary transform verbs.
void DslState::clip_at(const std::string& joint) {
    if(!animation_||!animation_->clip_open){set_rig_error("at outside an open clip");return;}
    if(find_joint(animation_->authored,joint)<0){set_rig_error("clip at selects an unknown joint");return;}
    animation_->name=joint;
}
// Post-multiply the selected joint's working pose by a rotation of `radians`
// about the axis (x, y, z); the axis is expected already normalized (the
// rotateX/Y/Z reroute passes a unit axis). Accumulates into `clip_pose`, which
// `begin_clip_sample` seeded from the rig's bind pose — `capture_clip_sample`
// is what turns the accumulated pose into keys.
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
// Append a key at `time` (seconds) on `joint`'s track, creating the track on
// first use. Keys are appended UNSORTED — `capture_clip_sample` and `end_clip`
// stable-sort each track by time before anything reads it. The track lookup is
// a linear scan of the clip's tracks.
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
// One half of the `generate(cb)` protocol, driven by `j_generate` in
// dsl_bindings.cpp: begin a sample by resetting `clip_pose` to the rig's bind
// pose, clearing the joint selection, and setting `generating` — which blocks
// all structural animation authoring AND all geometry authoring for the
// duration of the callback. The JS callback then poses joints, and
// `capture_clip_sample` closes the sample.
//
// Returns false (with the rig error set) when no clip is open or a generate is
// already in flight; the caller stops sampling.
bool DslState::begin_clip_sample() {
    if(!animation_||!animation_->clip_open){set_rig_error("generate outside an open clip");return false;}
    if(animation_->generating){set_rig_error("nested generate callback is forbidden");return false;}
    animation_->generating=true; animation_->clip_pose.clear(); animation_->clip_pose_joints.clear();
    for(const auto& j:animation_->authored.rig.joints){animation_->clip_pose.push_back(j.local);animation_->clip_pose_joints.push_back(j.name);}
    animation_->name.clear(); return true;
}
// Close one `generate()` sample: emit a key for EVERY rig joint at
// `phase * duration`, whether the callback moved it or not (generated clips are
// densely sampled), then clear `generating`. `phase` is normalized 0-1.
//
// Returns false — after clearing `generating` — when the pose array no longer
// matches the rig, which the caller treats as "stop sampling".
bool DslState::capture_clip_sample(float phase) {
    if(!animation_||!animation_->clip_open||animation_->clip_pose.size()!=animation_->authored.rig.joints.size()){if(animation_)animation_->generating=false;return false;}
    auto& clip=animation_->authored.clips.back();
    for(auto& track:clip.tracks) std::stable_sort(track.keys.begin(),track.keys.end(),[](const auto&a,const auto&b){return a.time<b.time;});
    std::stable_sort(clip.markers.begin(),clip.markers.end(),[](const auto&a,const auto&b){return a.time<b.time;});
    for(size_t i=0;i<animation_->clip_pose.size();++i){ ClipTrack* track=nullptr; for(auto& t:clip.tracks)if(t.joint==animation_->clip_pose_joints[i])track=&t; if(!track){clip.tracks.push_back({animation_->clip_pose_joints[i],{},rig_source_});track=&clip.tracks.back();} track->keys.push_back({phase*clip.duration,animation_->clip_pose[i],rig_source_}); }
    animation_->generating=false; return true;
}
// Number of sample segments `generate()` should walk: ceil(duration * rate),
// clamped to [1, UINT32_MAX]. 0 means "not samplable" — no open clip, or a
// non-finite / non-positive duration or rate — and the caller reports that as
// an authoring error. A looping clip generates `segments` samples (the wrap
// key closes the cycle); a non-looping clip generates `segments + 1`.
uint32_t DslState::clip_sample_segments() const { if(!animation_||!animation_->clip_open||!finite(animation_->clip_duration)||!finite(animation_->clip_rate)||animation_->clip_duration<=0||animation_->clip_rate<=0)return 0; const double n=std::ceil((double)animation_->clip_duration*(double)animation_->clip_rate); return (uint32_t)std::max(1.0,std::min(n,(double)UINT32_MAX)); }
bool DslState::clip_is_loop() const { return animation_&&animation_->clip_open&&animation_->clip_loop; }
// Close the clip and COMPILE it. This is the expensive verb in this file: it
// sorts tracks and markers, applies the loop closure documented below,
// re-validates the whole build, then builds and serializes the Ozz skeleton
// (once per rig, into `authored.ozz_skeleton_blob`) and the Ozz animation
// (into `clip.ozz_blob`).
//
// On any failure the clip stays OPEN and the rig error is set.
void DslState::end_clip() {
    if(animation_&&animation_->generating){set_rig_error("endClip is forbidden during generate");return;}
    if(!animation_||!animation_->clip_open){set_rig_error("endClip outside an open clip");return;}
    auto& clip=animation_->authored.clips.back();
    for(auto& track:clip.tracks) std::stable_sort(track.keys.begin(),track.keys.end(),[](const auto& left,const auto& right){return left.time<right.time;});
    std::stable_sort(clip.markers.begin(),clip.markers.end(),[](const auto& left,const auto& right){return left.time<right.time;});
    // Loop closure: a cyclic track has to arrive back at its starting value at
    // the end of the cycle, so the wrap is seamless.  That is right for a
    // swinging hip and wrong for a travelling root: a track the author keyed
    // explicitly AT clip.duration is spelling a wrap DISCONTINUITY on purpose.
    // Root-motion extraction is built to consume exactly that -- it samples the
    // loop boundary as `duration` rather than wrapping to 0 (see
    // forward_clip_root_delta) and turns the end-of-cycle displacement into
    // entity travel.  Appending the folded-back value on top silently deleted
    // the authored travel and left the entity walking forward then snapping
    // back each cycle, which is why the gallery walk had to be re-authored in
    // place.  An explicit end key now wins; everything else closes as before.
    if (clip.loop) {
        for (auto& t : clip.tracks) {
            if (t.keys.empty()) continue;
            // Keys are sorted above, so back() is the latest authored time.
            if (std::fabs(t.keys.back().time - clip.duration) <= 1e-6f) continue;
            t.keys.push_back({clip.duration, t.keys.front().value, rig_source_});
        }
    }
    AnimationBuild validation = animation_->authored; validation.graph.nodes.push_back({"__clip_validation_output",{},true,matter::animation::EvaluationCadence::Fixed,rig_source_}); matter::animation::Diagnostics vd; if(!matter::animation::validate_animation_build(validation,vd)){if(!vd.items.empty())rig_source_=vd.items.front().source;record_animation_diagnostics(vd); set_rig_error(vd.items.empty()?"clip validation failed":vd.items.front().message,"clip-validation");return;}
    // Clip compilation runs wherever a part is baked. There is no separate
    // shipped-runtime binary to protect from the Ozz offline builders: a game
    // ships the whole engine and editor, so the process that authors content is
    // the process that runs it. (This block used to fail closed under
    // MATTER_RUNTIME_ANIMATION_ONLY, which drew a bake-host/runtime boundary
    // this product does not have -- it made animated worlds unbakeable in the
    // only binary anyone actually runs.)
    matter::animation::Diagnostics d; matter::animation::OzzSkeleton sk; matter::animation::OzzAnimation oa;
    if(!matter::animation::build_skeleton(animation_->authored.rig,sk,d)||!matter::animation::build_clip(animation_->authored.rig,clip,oa,d)){record_animation_diagnostics(d); set_rig_error(d.items.empty()?"clip compilation failed":d.items.front().message,"clip-compile");return;}
    if(animation_->authored.ozz_skeleton_blob.empty()&&!matter::animation::serialize_skeleton(sk,animation_->authored.ozz_skeleton_blob)){set_rig_error("skeleton serialization failed","clip-compile");return;}
    if(!matter::animation::serialize_animation(oa,clip.ozz_blob)||clip.ozz_blob.empty()){set_rig_error("clip serialization failed","clip-compile");return;}
    animation_->clip_open=false; animation_->current_clip.clear(); animation_->name.clear();
}
// ---------------------------------------------------------------------------
// Motion graph authoring.
//
// Between `beginMotion` and `endMotion`, `input`/`target`/`controller`/node
// declarations are appended to `authored.inputs` / `.targets` / `.controllers`
// / `.graph.nodes`; `end_motion` validates and canonicalizes the whole build in
// one pass. `begin_motion` erases the synthetic `__rig_only_output` node that
// `refresh_canonical_animation` inserts, because a real motion graph declares
// its own output node.
//
// Requires a completed rig, cannot nest inside a clip, and only one motion
// session may be open.
// ---------------------------------------------------------------------------
void DslState::begin_motion(const std::string& name) { if(binding_scope_){set_rig_error("structural animation authoring is forbidden inside a bind scope");return;} if(animation_&&animation_->generating){set_rig_error("structural authoring is forbidden during generate");return;} if(!animation_||!animation_->ended){set_rig_error("beginMotion requires a completed rig");return;} if(animation_->clip_open){set_rig_error("beginMotion cannot nest inside a clip");return;} if(animation_->motion_open){set_rig_error("motion session already open");return;} auto& nodes=animation_->authored.graph.nodes; nodes.erase(std::remove_if(nodes.begin(),nodes.end(),[](const GraphNode& n){return n.name=="__rig_only_output";}),nodes.end()); animation_->motion_open=true; animation_->current_motion=name; }
void DslState::motion_input(const InputSchema& input){if(!animation_||!animation_->motion_open){set_rig_error("input outside an open motion");return;} animation_->authored.inputs.push_back(input);}
// An externally driven target starts INACTIVE. The spec's runtime contract is
// set_transform(handle, world) followed by enable(handle) -- the enable call
// would be redundant if a declaration were already live. It is also the only
// safe default: a fresh instance's desired transform is the identity, so an
// enabled external target solves its chain toward the model origin from the
// first tick, tearing the mesh apart before gameplay has written anything.
// A controller-driven target is written by its controller every tick and so
// stays enabled. `enabled` has no authored spelling in the DSL, so this sets
// the default rather than overriding an author's choice.
void DslState::motion_target(const TargetSchema& target){if(!animation_||!animation_->motion_open){set_rig_error("target outside an open motion");return;} TargetSchema copy=target; copy.require_explicit_pole = copy.require_explicit_pole || copy.source.module=="<part>"; copy.enabled = copy.driver != matter::animation::TargetDriverKind::External; animation_->authored.targets.push_back(std::move(copy));}
void DslState::motion_controller(const ControllerDef& controller){if(!animation_||!animation_->motion_open){set_rig_error("controller outside an open motion");return;} animation_->authored.controllers.push_back(controller);}
void DslState::motion_node(const GraphNode& node){if(!animation_||!animation_->motion_open){set_rig_error("graph node outside an open motion");return;} animation_->authored.graph.nodes.push_back(node);}
void DslState::end_motion(){if(animation_&&animation_->generating){set_rig_error("endMotion is forbidden during generate");return;} if(!animation_||!animation_->motion_open){set_rig_error("endMotion outside an open motion");return;} matter::animation::Diagnostics d; matter::animation::CanonicalAnimationBuild c; if(!matter::animation::validate_and_canonicalize_animation_build(animation_->authored,c,d)){if(!d.items.empty())rig_source_=d.items.front().source;record_animation_diagnostics(d); set_rig_error(d.items.empty()?"motion validation failed":d.items.front().message,"motion-validation");return;} animation_->canonical=std::move(c); animation_->motion_open=false; animation_->current_motion.clear();}

namespace {
// Resolve a binding's joint selection to joint INDICES. An empty `requested`
// means "every non-root joint".
//
// A selection names the CHILD joint of a parent-child segment — a segment is
// the bone between a joint and its parent — so the root joint is rejected, as
// are duplicates and unknown names. `error` receives the message the caller
// hands to `set_rig_error`.
bool binding_segments(const AnimationBuildBuffer& animation, const std::vector<std::string>& requested,
                      std::vector<size_t>& selected, std::string& error) {
    const auto& joints = animation.authored.rig.joints;
    if (joints.empty()) { error = "binding requires a completed rig"; return false; }
    std::set<std::string> seen;
    const auto add = [&](const std::string& name) {
        if (!seen.insert(name).second) { error = "binding selection contains a duplicate joint"; return false; }
        const int index = find_joint(animation.authored, name);
        if (index < 0) { error = "binding selection references an unknown joint"; return false; }
        if (animation.authored.rig.joints[static_cast<size_t>(index)].parent.empty()) { error = "binding selection cannot use the root joint"; return false; }
        selected.push_back(static_cast<size_t>(index)); return true;
    };
    if (requested.empty()) {
        for (size_t index = 0; index < joints.size(); ++index)
            if (!joints[index].parent.empty() && !add(joints[index].name)) return false;
    } else for (const std::string& name : requested) if (!add(name)) return false;
    if (selected.empty()) { error = "binding selection has no parent-child segments"; return false; }
    return true;
}
// Skin bindings, rigid bindings and attachments share ONE name space: a name is
// unique only if it appears in none of the three lists. An empty name is never
// unique.
bool unique_binding_name(const AnimationBuild& build, const std::string& name) {
    if (name.empty()) return false;
    for (const auto& binding : build.skin_bindings) if (binding.name == name) return false;
    for (const auto& binding : build.rigid_bindings) if (binding.name == name) return false;
    for (const auto& attachment : build.attachments) if (attachment.name == name) return false;
    return true;
}
}

// Declare a skin binding over the selected segments and, when `generate` is
// set, AUTHOR ITS GEOMETRY as a side effect.
//
// The generated path walks the rig to accumulate world-space joint positions
// (the rig stores only parent-local transforms), opens its own voxel session,
// unions a cylinder per selected segment plus a sphere at every endpoint at
// `joint.radius * radius_scale`, closes the session, and records the resulting
// `[op, triangle)` ranges as the binding's geometry. So this verb mutates the
// shared build buffer, not just the animation IR — which is why it refuses to
// run with any geometry session already open.
//
// `radius_scale` and `falloff` are unitless positive multipliers; `spacing` is
// the generated voxel size in metres. Each selected segment may be claimed by
// only one primary (non-decorative) binding — see `primary_segment_claims`.
void DslState::rig_skin(const std::string& name, const std::vector<std::string>& requested,
                        float radius_scale, float falloff, bool generate, float spacing) {
    if (binding_scope_) { set_rig_error("structural animation authoring is forbidden inside a bind scope"); return; }
    if (!animation_ || !animation_->ended || animation_->clip_open || animation_->motion_open) { set_rig_error("skin requires a completed rig outside clip or motion authoring"); return; }
    if (!unique_binding_name(animation_->authored, name)) { set_rig_error("binding name must be non-empty and unique"); return; }
    if (!finite(radius_scale) || radius_scale <= 0.0f) { set_rig_error("skin radiusScale must be finite and positive"); return; }
    if (!finite(falloff) || falloff <= 0.0f) { set_rig_error("skin falloffScale must be finite and positive"); return; }
    if (generate && (!finite(spacing) || spacing <= 0.0f)) { set_rig_error("generated skin spacing must be finite and positive"); return; }
    if (session_ != Session::None || polygon_open_ || contour_open_ || region_open_) { set_rig_error("skin generation requires no open geometry session"); return; }
    std::vector<size_t> selected; std::string error;
    if (!binding_segments(*animation_, requested, selected, error)) { set_rig_error(error); return; }
    if (animation_->primary_segment_claims.empty()) animation_->primary_segment_claims.assign(animation_->authored.rig.joints.size(), false);
    for (size_t index : selected) if (animation_->primary_segment_claims[index]) { set_rig_error("binding segment is already claimed by a primary binding"); return; }
    matter::animation::SkinBindingDef binding; binding.name=name; binding.radius_scale=radius_scale; binding.falloff=falloff; binding.voxel_size=spacing; binding.generated=generate; binding.source=rig_source_;
    for (size_t index : selected) binding.joints.push_back(animation_->authored.rig.joints[index].name);
    const size_t generated_op_begin=buffer_.ops.size(), generated_triangle_begin=direct_triangle_count();
    if (generate) {
        const auto& joints = animation_->authored.rig.joints;
        std::vector<Float3> positions(joints.size()); std::vector<Quaternion> rotations(joints.size());
        for (size_t index = 0; index < joints.size(); ++index) {
            const int parent = joints[index].parent.empty() ? -1 : find_joint(animation_->authored, joints[index].parent);
            if (parent < 0) { positions[index]=joints[index].local.translation; rotations[index]=joints[index].local.rotation; }
            else { const Float3 rotated=qrotate(rotations[static_cast<size_t>(parent)], joints[index].local.translation); positions[index]={positions[static_cast<size_t>(parent)].x+rotated.x,positions[static_cast<size_t>(parent)].y+rotated.y,positions[static_cast<size_t>(parent)].z+rotated.z}; rotations[index]=qmul(rotations[static_cast<size_t>(parent)], joints[index].local.rotation); normalize_q(rotations[index]); }
        }
        beginVoxels(spacing);
        if (has_error_) return;
        std::set<size_t> endpoints;
        for (size_t child : selected) {
            const size_t parent=static_cast<size_t>(find_joint(animation_->authored, joints[child].parent));
            emit_voxel_segment(BrushKind::Cylinder, {positions[parent].x,positions[parent].y,positions[parent].z}, {positions[child].x,positions[child].y,positions[child].z}, joints[parent].radius*radius_scale, joints[child].radius*radius_scale, CsgOp::Union);
            endpoints.insert(parent); endpoints.insert(child);
        }
        for (size_t joint : endpoints) emit_voxel_sphere({positions[joint].x,positions[joint].y,positions[joint].z}, joints[joint].radius*radius_scale, CsgOp::Union);
        endVoxels(); if (has_error_) return;
    }
    if (generate) {
        const size_t generated_op_end=buffer_.ops.size(), generated_triangle_end=direct_triangle_count();
        if (generated_op_end > std::numeric_limits<uint32_t>::max() ||
            generated_triangle_end > std::numeric_limits<uint32_t>::max()) {
            set_rig_error("generated skin geometry exceeds v1 range limits"); return;
        }
        binding.geometry.push_back({static_cast<uint32_t>(generated_op_begin), static_cast<uint32_t>(generated_op_end),
                                    static_cast<uint32_t>(generated_triangle_begin), static_cast<uint32_t>(generated_triangle_end)});
    }
    for (size_t index : selected) animation_->primary_segment_claims[index]=true;
    animation_->authored.skin_bindings.push_back(std::move(binding));
    matter::animation::Diagnostics diagnostics;
    if (!refresh_canonical_animation(*animation_, rig_source_, diagnostics)) {
        if (!diagnostics.items.empty()) rig_source_=diagnostics.items.front().source;
        record_animation_diagnostics(diagnostics); set_rig_error(diagnostics.items.empty()?"skin binding validation failed":diagnostics.items.front().message, "binding-validation");
    }
}

// Open a `bind(name, fn)` scope: remember where the build buffer's op list and
// triangle buffer stand so everything the callback authors can be attributed to
// the named binding.
//
// `name` is resolved against skin bindings first, then rigid bindings — a name
// matches only one kind — and ALL bindings sharing that name receive the range.
// Returns false with the rig error set when the name is unknown, a scope is
// already open, or a geometry session is open.
//
// Must be paired with `end_binding_scope` or `cancel_binding_scope`;
// `j_bind_geometry` in dsl_bindings.cpp owns that pairing.
bool DslState::begin_binding_scope(const std::string& name) {
    if (!animation_ || !animation_->ended || animation_->clip_open || animation_->motion_open || animation_->generating) {
        set_rig_error("bind requires a completed rig outside clip or motion authoring"); return false;
    }
    if (binding_scope_) { set_rig_error("bind scopes cannot nest"); return false; }
    if (session_ != Session::None || polygon_open_ || contour_open_ || region_open_) {
        set_rig_error("bind scope requires no open geometry session"); return false;
    }
    BindingScope scope;
    for (size_t index=0; index<animation_->authored.skin_bindings.size(); ++index)
        if (animation_->authored.skin_bindings[index].name == name) scope.indices.push_back(index);
    if (scope.indices.empty()) {
        scope.kind=BindingScope::Kind::Rigid;
        for (size_t index=0; index<animation_->authored.rigid_bindings.size(); ++index)
            if (animation_->authored.rigid_bindings[index].name == name) scope.indices.push_back(index);
    }
    if (scope.indices.empty()) { set_rig_error("bind references an unknown skin or rigid binding"); return false; }
    scope.op_begin=buffer_.ops.size(); scope.triangle_begin=direct_triangle_count();
    binding_scope_ = std::move(scope);
    return true;
}

// Close the scope and record `[op_begin, op_end) x [triangle_begin,
// triangle_end)` onto every binding the scope named. An empty scope is an
// error — `bind` must author something.
//
// If re-validation fails, the just-pushed ranges are popped back off so the IR
// is left as it was and false is returned. NOTE that the failure path does NOT
// reset `binding_scope_`: the caller is expected to follow a false return with
// `cancel_binding_scope`, which also discards the authored geometry.
bool DslState::end_binding_scope() {
    if (!binding_scope_) { set_rig_error("bind scope is not open"); return false; }
    const BindingScope scope=*binding_scope_;
    if (session_ != Session::None || polygon_open_ || contour_open_ || region_open_ ||
        animation_->clip_open || animation_->motion_open || animation_->generating) {
        set_rig_error("bind scope requires no open geometry session"); return false;
    }
    // Retained polygons are lazy direct geometry. Flush before recording the
    // end so they cannot leak into a later, unrelated binding scope.
    flush_retained_profile();
    if (has_error_) return false;
    const size_t op_end=buffer_.ops.size(), triangle_end=direct_triangle_count();
    if (op_end > std::numeric_limits<uint32_t>::max() || triangle_end > std::numeric_limits<uint32_t>::max()) {
        set_rig_error("binding geometry exceeds v1 range limits"); return false;
    }
    if (scope.op_begin == op_end && scope.triangle_begin == triangle_end) {
        set_rig_error("bind scope must author geometry"); return false;
    }
    const matter::animation::BindingGeometryRange range{
        static_cast<uint32_t>(scope.op_begin), static_cast<uint32_t>(op_end),
        static_cast<uint32_t>(scope.triangle_begin), static_cast<uint32_t>(triangle_end)};
    for (size_t index : scope.indices) {
        if (scope.kind == BindingScope::Kind::Skin) animation_->authored.skin_bindings[index].geometry.push_back(range);
        else animation_->authored.rigid_bindings[index].geometry.push_back(range);
    }
    matter::animation::Diagnostics diagnostics;
    if (!refresh_canonical_animation(*animation_, rig_source_, diagnostics)) {
        for (size_t index : scope.indices) {
            if (scope.kind == BindingScope::Kind::Skin) animation_->authored.skin_bindings[index].geometry.pop_back();
            else animation_->authored.rigid_bindings[index].geometry.pop_back();
        }
        record_animation_diagnostics(diagnostics); set_rig_error(diagnostics.items.empty()?"binding geometry validation failed":diagnostics.items.front().message,
                      "binding-validation");
        return false;
    }
    binding_scope_.reset();
    return true;
}

// Abandon the open scope and TRUNCATE the build buffer's ops and the triangle
// buffer back to where the scope started, discarding whatever the callback
// authored. A no-op when no scope is open, so it is safe on every error path.
void DslState::cancel_binding_scope() {
    if (!binding_scope_) return;
    const BindingScope scope=*binding_scope_;
    if (buffer_.ops.size() >= scope.op_begin) buffer_.ops.resize(scope.op_begin);
    if (tris_buf_ && tris_buf_->triangles().size() >= scope.triangle_begin)
        tris_buf_->truncate(scope.triangle_begin);
    binding_scope_.reset();
}

// Declare a rigid binding: one `RigidBindingDef` per selected joint, all
// sharing `name`, each carrying the same `bind_offset` as the joint-local
// placement for whatever geometry a later `bind(name)` scope authors.
//
// `decorative` bindings do NOT claim their segments, so a decorative rigid can
// ride along on a segment a skin binding already owns; a non-decorative one
// competes for the single primary claim per segment.
void DslState::rig_segments(const std::string& name, const std::vector<std::string>& requested, bool decorative,
                            const AnimationTransform& bind_offset) {
    if (binding_scope_) { set_rig_error("structural animation authoring is forbidden inside a bind scope"); return; }
    if (!animation_ || !animation_->ended || animation_->clip_open || animation_->motion_open) { set_rig_error("segments requires a completed rig outside clip or motion authoring"); return; }
    if (!unique_binding_name(animation_->authored, name)) { set_rig_error("binding name must be non-empty and unique"); return; }
    if (!valid_transform(bind_offset)) { set_rig_error("segments bind offset must be a finite positive transform"); return; }
    std::vector<size_t> selected; std::string error;
    if (!binding_segments(*animation_, requested, selected, error)) { set_rig_error(error); return; }
    if (animation_->primary_segment_claims.empty()) animation_->primary_segment_claims.assign(animation_->authored.rig.joints.size(), false);
    if (!decorative) for (size_t index : selected) if (animation_->primary_segment_claims[index]) { set_rig_error("binding segment is already claimed by a primary binding"); return; }
    for (size_t index : selected) {
        matter::animation::RigidBindingDef binding; binding.name=name; binding.joint=animation_->authored.rig.joints[index].name; binding.local=bind_offset; binding.decorative=decorative; binding.source=rig_source_;
        animation_->authored.rigid_bindings.push_back(std::move(binding));
        if (!decorative) animation_->primary_segment_claims[index]=true;
    }
    matter::animation::Diagnostics diagnostics;
    if (!refresh_canonical_animation(*animation_, rig_source_, diagnostics)) {
        if (!diagnostics.items.empty()) rig_source_=diagnostics.items.front().source;
        record_animation_diagnostics(diagnostics); set_rig_error(diagnostics.items.empty()?"rigid binding validation failed":diagnostics.items.front().message, "binding-validation");
    }
}

// Attach an already-declared child part at a socket or joint. The `socket`
// argument is looked up as a socket first and then as a joint, so either kind
// of anchor is spelled the same way.
//
// The child must be in the part's static requires: its resolved hash is looked
// up here, and BOTH an unresolvable child and one whose committed artifact is
// known-invalid are errors. v1 additionally forbids attaching a child that
// itself carries committed animation (no nested rigs).
void DslState::rig_attach(const std::string& name, const std::string& socket,
                          const std::string& child_module, const AnimationTransform& local) {
    if (binding_scope_) { set_rig_error("structural animation authoring is forbidden inside a bind scope"); return; }
    if (!animation_ || !animation_->ended || animation_->clip_open || animation_->motion_open) { set_rig_error("attach requires a completed rig outside clip or motion authoring"); return; }
    if (!unique_binding_name(animation_->authored, name)) { set_rig_error("attachment name must be non-empty and unique"); return; }
    const bool socket_target=has_socket(animation_->authored, socket);
    const bool joint_target=find_joint(animation_->authored, socket)>=0;
    if (!socket_target && !joint_target) { set_rig_error("attachment references an unknown joint or socket"); return; }
    if (!valid_transform(local)) { set_rig_error("attachment requires a finite positive transform"); return; }
    uint64_t child_hash=0;
    if (child_module.empty() || !lookup_child_hash(child_module, nullptr, 0, child_hash) || child_hash == 0) { set_rig_error("attachment references an unresolved child"); return; }
    if (child_animation_artifact_is_invalid(child_hash)) {
        set_rig_error("attachment references an invalid committed part artifact"); return;
    }
    const bool child_has_animation = child_has_committed_animation(child_hash);
    if (!matter::animation::validate_attachment(true, child_has_animation)) {
        set_rig_error("v1 attachment cannot target a child with nested committed animation"); return;
    }
    matter::animation::AttachmentDef attachment; attachment.name=name; attachment.child_hash=child_hash; attachment.child_has_committed_animation=child_has_animation; attachment.local=local; attachment.source=rig_source_;
    if (socket_target) attachment.socket=socket; else attachment.joint=socket;
    animation_->authored.attachments.push_back(std::move(attachment));
    matter::animation::Diagnostics diagnostics;
    if (!refresh_canonical_animation(*animation_, rig_source_, diagnostics)) {
        if (!diagnostics.items.empty()) rig_source_=diagnostics.items.front().source;
        record_animation_diagnostics(diagnostics); set_rig_error(diagnostics.items.empty()?"attachment validation failed":diagnostics.items.front().message, "binding-validation");
    }
}

// Bake-side readers, called after `build()` returns. `canonical_animation` is
// an alias of `canonical_rig` (the validated IR the runtime consumes);
// `authored_animation` hands back the pre-canonicalization build, or null when
// the part authored no rig at all.
const std::optional<matter::animation::CanonicalAnimationBuild>& DslState::canonical_animation() const { return canonical_rig(); }
const matter::animation::AnimationBuild* DslState::authored_animation() const { return animation_ ? &animation_->authored : nullptr; }

} // namespace dsl
