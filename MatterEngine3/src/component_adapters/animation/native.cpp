#include "native.h"
#include "animation/animation_binding_bake.h"
#include "animation/animation_runtime_asset.h"
#include "animation/animation_validate.h"
#include "animation/ozz_adapter.h"
#include <algorithm>

namespace matter::component_adapters::animation {
namespace {
using E = contracts::ErrorCode;
std::vector<contracts::Diagnostic> diagnostics(const native::Diagnostics &input) {
  std::vector<contracts::Diagnostic> out;
  for (const auto &d : input.items)
    out.push_back({contracts::Severity::error, E::invalid_input, d.code + ": " + d.message,
                   d.source.object, contracts::SourceLocation{d.source.module, d.source.line, d.source.column}, {}});
  return out;
}
bool cancelled(const services::JobControl &control) {
  return control.cancellation && control.cancellation->load(std::memory_order_relaxed);
}
contracts::Mat4 matrix(const Mat4f &m) {
  contracts::Mat4 result;
  std::copy(std::begin(m.m), std::end(m.m), std::begin(result.m));
  return result;
}
contracts::Mat4 matrix(const AnimationTransform &t) {
  return mm::from_trs({t.translation.x,t.translation.y,t.translation.z},
                 {t.rotation.x,t.rotation.y,t.rotation.z,t.rotation.w},
                 {t.scale.x,t.scale.y,t.scale.z});
}
bool same_transform(const AnimationTransform &a, const AnimationTransform &b) {
  return a.translation.x == b.translation.x && a.translation.y == b.translation.y && a.translation.z == b.translation.z &&
         a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z && a.rotation.w == b.rotation.w &&
         a.scale.x == b.scale.x && a.scale.y == b.scale.y && a.scale.z == b.scale.z;
}
} // namespace

NativeResult prepare_native(const services::JobEnvelope &envelope, const native::AnimationBuild &input,
                            const native::BindingBake &binding, uint64_t resolved_hash,
                            native::BuildNonce nonce, const services::JobControl &control) {
  auto fail = [&](E code, const char *message) { return NativeResult::failure(envelope, code, message); };
  if (cancelled(control)) return fail(E::cancelled, "native animation preparation cancelled");
  native::Diagnostics errors;
  NativeArtifact out{input, {}, {}};
  auto &authored = out.authored;
  if (authored.clips.empty()) {
    native::ClipDefinition rest;
    rest.name = "__bind_rest"; rest.duration = 1; rest.rate = 1; rest.loop = true;
    rest.source = authored.rig.source;
    authored.clips.push_back(std::move(rest));
  }
  if (authored.graph.nodes.empty()) {
    native::GraphNode clip;
    clip.name = "__bind_rest_clip"; clip.kind = native::GraphNodeKind::Clip;
    clip.clip = authored.clips.front().name;
    native::GraphNode output;
    output.name = "__bind_rest_output"; output.kind = native::GraphNodeKind::Output;
    output.is_output = true; output.dependencies = {clip.name};
    authored.graph.nodes = {clip, output};
  }
  auto &canonical = out.canonical;
  if (!native::validate_and_canonicalize_animation_build(authored, canonical, errors) ||
      !native::validate_final_animation_bindings(input, errors))
    return NativeResult::failure(envelope, diagnostics(errors));
  if (!resolved_hash || nonce == native::BuildNonce{})
    return fail(E::invalid_input, "native animation requires a resolved identity and build nonce");
  if (binding.inverse_bind_matrices.size() != canonical.rig.joints.size() ||
      binding.rigid_segments.size() != input.rigid_bindings.size() ||
      binding.attachments.size() != input.attachments.size() ||
      (binding.lods.empty() != input.skin_bindings.empty()))
    return fail(E::invalid_input, "final bindings do not cover authored animation");
  // Finalized ranges/owner slots are supplied by geometry integration, but
  // names, joints and pivots must still agree with the authored declarations.
  for (size_t i = 0; i < binding.rigid_segments.size(); ++i) {
    const auto &b = binding.rigid_segments[i];
    const auto &a = input.rigid_bindings[i];
    if (b.joint >= canonical.rig.joints.size() || canonical.rig.joints[b.joint].name != a.joint ||
        b.name != a.name || b.decorative != a.decorative ||
        !same_transform(b.bind_offset, a.local))
      return fail(E::invalid_input, "final rigid binding disagrees with authored pivot or joint");
  }
  for (size_t i = 0; i < binding.attachments.size(); ++i) {
    const auto &b = binding.attachments[i];
    const auto &a = input.attachments[i];
    const auto kind = a.socket.empty() ? native::AttachmentTargetKind::Joint : native::AttachmentTargetKind::Socket;
    if (b.name != a.name || b.target_kind != kind || b.target != (a.socket.empty() ? a.joint : a.socket) ||
        b.child_hash != a.child_hash || !same_transform(b.local, a.local))
      return fail(E::invalid_input, "final attachment disagrees with authored target");
  }
  // Existing serialized clips are authoritative (the IR explicitly permits
  // dropping tracks after baking). Missing blobs use the one native baker.
  if (authored.ozz_skeleton_blob.empty()) {
    native::OzzSkeleton skeleton;
    if (!native::build_skeleton(authored.rig, skeleton, errors))
      return NativeResult::failure(envelope, diagnostics(errors));
    if (!native::serialize_skeleton(skeleton, authored.ozz_skeleton_blob))
      return fail(E::backend_failure, "native skeleton serialization failed");
  }
  for (auto &clip : authored.clips) {
    if (cancelled(control)) return fail(E::cancelled, "native animation preparation cancelled");
    if (!clip.ozz_blob.empty()) continue;
    native::OzzAnimation animation;
    if (!native::build_clip(authored.rig, clip, animation, errors))
      return NativeResult::failure(envelope, diagnostics(errors));
    if (!native::serialize_animation(animation, clip.ozz_blob))
      return fail(E::backend_failure, "native clip serialization failed");
  }
  out.artifact.resolved_hash = resolved_hash;
  out.artifact.nonce = nonce;
  out.artifact.target_abi_tag = native::kAnimationTargetAbiTag;
  out.artifact.ozz_tag_hash = native::kAnimationOzzTagHash;
  if (!native::encode_animation_runtime_sections(authored, out.canonical, out.artifact, errors))
    return NativeResult::failure(envelope, diagnostics(errors));
  if (!native::set_anim_binding_bake(out.artifact, binding))
    return fail(E::invalid_input, "invalid finalized native animation binding");
  // Validate cross-section identity, graph support and Ozz archives before
  // publication. Temporary decoded resources do not cross preparation.
  native::DecodedAnimationRuntimeAsset check;
  if (!native::decode_animation_runtime_asset(out.artifact, check, errors))
    return NativeResult::failure(envelope, diagnostics(errors));
  if (cancelled(control)) return fail(E::cancelled, "native animation preparation cancelled");
  return NativeResult::success(envelope, std::move(out));
}

RuntimeResult prepare_runtime(const NativeResult &prepared) {
  if (!prepared.ok()) return RuntimeResult::failure(prepared.envelope(), prepared.diagnostics());
  native::DecodedAnimationRuntimeAsset out;
  native::Diagnostics errors;
  if (!native::decode_animation_runtime_asset(prepared.value()->artifact, out, errors))
    return RuntimeResult::failure(prepared.envelope(), diagnostics(errors));
  return RuntimeResult::success(prepared.envelope(), std::move(out));
}

portable::PreparedResult prepare_graphics(const NativeResult &prepared, const contracts::AssetDocument &geometry,
                                         const GraphicsMapping &mapping) {
  auto fail = [&](E code, const char *message) {
    return portable::PreparedResult::failure(prepared.envelope(), code, message);
  };
  if (!prepared.ok()) return portable::PreparedResult::failure(prepared.envelope(), prepared.diagnostics());
  if (mapping.rig.value.empty() || !geometry.rigs.empty() || !geometry.skins.empty() || !geometry.animations.empty())
    return fail(E::invalid_input, "native graphics projection requires static geometry and a rig ID");
  const auto &input = *prepared.value();
  native::DecodedAnimationRuntimeAsset decoded;
  native::Diagnostics errors;
  if (!native::decode_animation_runtime_asset(input.artifact, decoded, errors))
    return portable::PreparedResult::failure(prepared.envelope(), diagnostics(errors));
  native::BindingBake binding;
  if (!native::get_anim_binding_bake(input.artifact, binding))
    return fail(E::invalid_input, "native binding cannot be decoded");
  if (mapping.skin_prototypes.size() != binding.lods.size() ||
      mapping.rigid_prototypes.size() != binding.rigid_segments.size() ||
      mapping.attachment_prototypes.size() != binding.attachments.size())
    return fail(E::invalid_input, "native graphics mapping must cover all LODs and pieces");
  auto document = geometry;
  document.required_capabilities.push_back({"matter.asset.animation", 1});
  contracts::Rig rig;
  rig.id = mapping.rig;
  for (size_t i = 0; i < decoded.rig.joints.size(); ++i) {
    const auto &joint = decoded.rig.joints[i];
    rig.joints.push_back({{joint.name}, joint.parent == native::kInvalidJoint ? -1 : int32_t(joint.parent),
                          matrix(binding.inverse_bind_matrices[i])});
  }
  document.rigs.push_back(rig);
  for (size_t lod = 0; lod < binding.lods.size(); ++lod) {
    contracts::SkinWeights skin;
    skin.prototype = mapping.skin_prototypes[lod]; skin.rig = rig.id;
    for (const auto &vertex : binding.lods[lod].influences) {
      std::array<uint32_t, 4> joints{};
      float weights[4]{};
      for (size_t i = 0; i < 4; ++i) {
        joints[i] = vertex.weights[i] ? vertex.joints[i] : 0;
        weights[i] = float(vertex.weights[i]) / 65535.0f;
      }
      skin.joints.push_back(joints);
      skin.weights.push_back({weights[0],weights[1],weights[2],weights[3]});
    }
    document.skins.push_back(std::move(skin));
  }
  portable::Bindings bindings;
  bindings.requires_native_motion = true;
  for (size_t i = 0; i < binding.rigid_segments.size(); ++i) {
    const auto &segment = binding.rigid_segments[i];
    // A native group may bind several joints under the same name. Length
    // framing avoids ambiguous names while preserving a distinct piece ID.
    const auto instance = "rigid:" + std::to_string(segment.name.size()) + ":" +
                          segment.name + ":" + rig.joints[segment.joint].id.value;
    bindings.rigid.push_back({{instance}, mapping.rigid_prototypes[i], rig.id,
                              rig.joints[segment.joint].id, matrix(segment.bind_offset)});
  }
  for (size_t i = 0; i < binding.attachments.size(); ++i) {
    const auto &attachment = binding.attachments[i];
    contracts::SemanticId joint{attachment.target};
    auto offset = matrix(attachment.local);
    if (attachment.target_kind == native::AttachmentTargetKind::Socket) {
      const auto &sockets = decoded.rig.sockets;
      auto socket = std::find_if(sockets.begin(), sockets.end(), [&](const auto &s) { return s.name == attachment.target; });
      if (socket == sockets.end()) return fail(E::invalid_input, "native attachment socket is missing");
      joint = rig.joints[socket->joint].id;
      offset = mm::multiply(matrix(socket->local), offset);
    }
    bindings.rigid.push_back({{"attachment:" + attachment.name}, mapping.attachment_prototypes[i], rig.id, joint, offset});
  }
  return portable::prepare(prepared.envelope(), document, bindings);
}
} // namespace matter::component_adapters::animation
