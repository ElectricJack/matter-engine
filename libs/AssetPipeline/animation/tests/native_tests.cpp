#include "fixtures.h"
#include "native.h"
#include "animation/animation_binding_bake.h"
#include "animation/animation_runtime_asset.h"
#include "animation/animation_validate.h"
#include "animation/ozz_adapter.h"

namespace n = matter::animation;
namespace bridge = matter::component_adapters::animation;
using namespace fixture;

int main() try {
  n::AnimationBuild source;
  n::JointDef root; root.name = "root";
  n::JointDef tip; tip.name = "tip"; tip.parent = "root"; tip.local.translation = {2,0,0};
  source.rig.joints = {root,tip}; // Native authoring requires parents first.
  n::SocketDef socket; socket.name = "tool"; socket.joint = "tip"; socket.local.translation = {0,3,0};
  source.rig.sockets = {socket};
  n::SkinBindingDef skin; skin.name = "body"; skin.joints = {"tip"}; skin.geometry = {{0,1,0,1}};
  source.skin_bindings = {skin};
  n::RigidBindingDef rigid; rigid.name = "decoration"; rigid.joint = "tip";
  rigid.decorative = true; rigid.local.translation = {0,2,0}; rigid.geometry = {{1,2,1,2}};
  source.rigid_bindings = {rigid};
  n::AttachmentDef attachment; attachment.name = "attached"; attachment.socket = "tool";
  attachment.child_hash = 123; attachment.local.translation = {0,0,4}; source.attachments = {attachment};
  n::ClipDefinition clip; clip.name = "walk"; clip.duration = 2; clip.rate = 30; clip.loop = true;
  n::ClipTrack track; track.joint = "tip";
  auto end = tip.local; end.translation.x = 4;
  track.keys = {{0,tip.local,{}}, {2,end,{}}}; clip.tracks = {track}; clip.markers = {{"step",0.5f,{}}};
  source.clips = {clip};
  n::GraphNode clip_node; clip_node.name = "walkNode"; clip_node.kind = n::GraphNodeKind::Clip;
  clip_node.clip = "walk";
  n::GraphNode output; output.name = "out"; output.kind = n::GraphNodeKind::Output;
  output.is_output = true; output.dependencies = {clip_node.name}; source.graph.nodes = {clip_node,output};
  n::CanonicalAnimationBuild canonical; n::Diagnostics diagnostics;
  const bool valid = n::validate_and_canonicalize_animation_build(source,canonical,diagnostics);
  if (!valid) for (const auto &d : diagnostics.items) std::cerr << d.code << ": " << d.message << '\n';
  check(valid, "native canonicalization");
  viewer::IndexedPartGeometry geometry;
  geometry.vertices = {0,0,0,1,0,0,0,1,0}; geometry.normals = {0,0,1,0,0,1,0,0,1};
  geometry.colors.assign(12,255); geometry.texcoords = {0,1,0,1,0,1};
  geometry.surface_uvs = {0,0,1,0,0,1}; geometry.material_ids = {7,7,7};
  geometry.baked_ao = {1,1,1}; geometry.indices = {0,1,2}; geometry.vertex_count = 3;
  n::BindingBake binding;
  check(n::build_skin_binding(canonical.rig,{1},{geometry},1,binding), "existing weight baker");
  n::RigidSegmentBake segment; segment.name = rigid.name; segment.joint = 1;
  segment.decorative = true; segment.bind_offset = rigid.local;
  segment.geometry = rigid.geometry; segment.lod_geometry = {{1,1}}; binding.rigid_segments = {segment};
  binding.attachments = {{attachment.name,attachment.socket,n::AttachmentTargetKind::Socket,
                          attachment.child_hash,attachment.local}};
  auto result = bridge::prepare_native(envelope(),source,binding,0xa401,{7,9}); good(result);
  check(source.ozz_skeleton_blob.empty(), "native input is unchanged");
  auto runtime = bridge::prepare_runtime(result); good(runtime);
  const auto &evaluation = *runtime.value()->definition.binding->evaluation;
  check(evaluation.clips.size() == 1 && evaluation.clips[0].markers.size() == 1,
        "ANIM-NATIVE: clip and marker survive");
  check(result.value()->authored.clips[0].rate == 30 && result.value()->authored.clips[0].loop,
        "sample rate and looping retained");
  n::OzzAnimation animation; n::OzzSampleContext context;
  const auto &blob = result.value()->authored.clips[0].ozz_blob;
  check(n::deserialize_animation(blob.data(),blob.size(),animation,diagnostics), "prepared clip deserializes");
  std::vector<matter::AnimationTransform> locals;
  check(n::sample(animation,0.5f,context,locals), "sample prepared native clip");
  // Ozz uses approximate SIMD interpolation; match the existing suite's 1e-3 tolerance.
  check(std::fabs(locals[1].translation.x-3) <= 1e-3f, "sampled native midpoint");
  n::OzzAnimation baseline; n::OzzSampleContext baseline_context;
  std::vector<matter::AnimationTransform> baseline_locals;
  check(n::build_clip(source.rig,source.clips[0],baseline,diagnostics) &&
        n::sample(baseline,0.5f,baseline_context,baseline_locals), "original native baker baseline");
  check(locals[1].translation.x == baseline_locals[1].translation.x,
        "extracted clip sampling exactly matches original baker");
  auto baked = result.value()->authored; baked.clips[0].tracks.clear();
  auto reused = bridge::prepare_native(envelope(),baked,binding,0xa401,{7,9}); good(reused);
  check(reused.value()->authored.clips[0].ozz_blob == blob, "existing clip archive remains authoritative");
  bridge::GraphicsMapping mapping{{"rig"},{{"triangle"}},{{"triangle"}},{{"triangle"}}};
  auto projected = bridge::prepare_graphics(result,fixture::geometry(),mapping); good(projected);
  const auto &weights = projected.value()->document.skins[0];
  for (size_t v=0; v<3; ++v) {
    const float converted[] = {weights.weights[v].x, weights.weights[v].y, weights.weights[v].z, weights.weights[v].w};
    uint32_t sum=0;
    for (size_t i=0; i<4; ++i) {
      check(uint16_t(std::lround(converted[i]*65535)) == binding.lods[0].influences[v].weights[i],
            "ANIM-WEIGHTS: all UNORM16 weights round-trip");
      sum += binding.lods[0].influences[v].weights[i];
    }
    check(sum==65535, "weights normalize exactly");
  }
  a::PoseFrame pose{envelope(),{"actor"},1,mm::translation({10,0,0}),{},
      {{{"rig"},{{},mm::translation({3,0,0})},{{},mm::translation({2,0,0})}}}};
  auto frame = a::graphics_frame(projected,pose,1); good(frame);
  check(near(frame.value()->rigid[0].world.m[3],13) && near(frame.value()->rigid[0].world.m[7],2),
        "native rigid pivot preserved");
  check(near(frame.value()->rigid[1].world.m[3],13) && near(frame.value()->rigid[1].world.m[7],3) &&
        near(frame.value()->rigid[1].world.m[11],4), "native socket and attachment offsets preserved");
  check(!a::supports(*projected.value(),a::Consumer::browser_pose).ok(), "native browser explicitly fails");
  check(!a::supports(*projected.value(),a::Consumer::gltf_export).ok(), "native glTF explicitly fails");
  mapping.attachment_prototypes.clear();
  check(!bridge::prepare_graphics(result,fixture::geometry(),mapping).ok(), "incomplete mapping fails");
  auto bad = binding; bad.rigid_segments[0].bind_offset.translation.x = 1;
  check(!bridge::prepare_native(envelope(),source,bad,0xa401,{7,9}).ok(), "authored pivot mismatch fails");
  auto nested = source; nested.attachments[0].child_has_committed_animation = true;
  check(!bridge::prepare_native(envelope(),nested,binding,0xa401,{7,9}).ok(), "native authored child limit retained");
  auto too_big = source; too_big.rig.joints.resize(n::kMaxJoints+1);
  check(!bridge::prepare_native(envelope(),too_big,binding,0xa401,{7,9}).ok(), "native 256 joint limit retained");
  auto forward_parent = source; std::swap(forward_parent.rig.joints[0],forward_parent.rig.joints[1]);
  check(!bridge::prepare_native(envelope(),forward_parent,binding,0xa401,{7,9}).ok(), "native parent-first limit retained");
  matter::services::CancellationSource cancellation; cancellation.cancel();
  matter::services::JobControl control; control.cancellation = cancellation.token();
  check(!bridge::prepare_native(envelope(),source,binding,0xa401,{7,9},control).ok(), "native cancel publishes no result");
  auto corrupt = *result.value(); corrupt.artifact.sections.clear();
  auto forged = bridge::NativeResult::success(envelope(),std::move(corrupt));
  check(!bridge::prepare_runtime(forged).ok(), "corrupt runtime fails");
  check(!bridge::prepare_graphics(forged,fixture::geometry(),mapping).ok(), "corrupt projection fails");
  // Existing rigid-only convention synthesizes a rest clip without adding a rig.
  auto rigid_only = source; rigid_only.skin_bindings.clear(); rigid_only.clips.clear(); rigid_only.graph.nodes.clear();
  auto rigid_binding = binding; rigid_binding.lods.clear(); rigid_binding.rigid_segments[0].lod_geometry[0].blas_slot=0;
  auto rest = bridge::prepare_native(envelope(),rigid_only,rigid_binding,0xa402,{8,9}); good(rest);
  check(rest.value()->authored.rig.joints.size()==2 && rest.value()->authored.clips[0].name=="__bind_rest",
        "ANIM-RIGID: explicit rig with static rest fallback");
  auto grouped = source; grouped.skin_bindings.clear();
  auto sibling = tip; sibling.name = "sibling"; sibling.local.translation = {0,2,0};
  grouped.rig.joints.push_back(sibling);
  auto second_piece = rigid; second_piece.joint = sibling.name; grouped.rigid_bindings.push_back(second_piece);
  check(n::validate_and_canonicalize_animation_build(grouped,canonical,diagnostics), "grouped rigid fixture");
  n::BindingBake grouped_binding;
  check(n::build_skin_binding(canonical.rig,{1,2},{},1,grouped_binding), "grouped bind matrices");
  segment.lod_geometry[0].blas_slot=0;
  grouped_binding.rigid_segments = {segment,segment};
  grouped_binding.rigid_segments[1].joint=2; grouped_binding.rigid_segments[1].lod_geometry[0].blas_slot=1;
  grouped_binding.attachments = binding.attachments;
  auto grouped_result = bridge::prepare_native(envelope(),grouped,grouped_binding,0xa403,{9,9}); good(grouped_result);
  mapping.skin_prototypes.clear(); mapping.rigid_prototypes = {{"triangle"},{"triangle"}};
  mapping.attachment_prototypes = {{"triangle"}};
  auto pieces = bridge::prepare_graphics(grouped_result,fixture::geometry(),mapping); good(pieces);
  check(pieces.value()->bindings.rigid[0].instance.value != pieces.value()->bindings.rigid[1].instance.value,
        "native named rigid group preserves independent segment identities");
  std::cout << "ANIM-NATIVE ANIM-WEIGHTS ANIM-RIGID PASS\n";
} catch (const std::exception &error) {
  std::cerr << "FAIL: " << error.what() << '\n'; return 1;
}
