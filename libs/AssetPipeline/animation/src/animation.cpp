#include <matter/animation_asset/animation.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace matter::animation_asset {
namespace {
using E = data::ErrorCode;
data::ValidationResult error(E code, const char *message) {
  return {{{data::Severity::error, code, message, {}, {}, {}}}};
}
bool cancelled(const services::JobControl &control) {
  return control.cancellation && control.cancellation->load(std::memory_order_relaxed);
}
template <class T> const T *named(const std::vector<T> &values, const data::SemanticId &id) {
  auto it = std::find_if(values.begin(), values.end(), [&](const T &v) { return v.id == id; });
  return it == values.end() ? nullptr : &*it;
}
bool palette(const data::Mat4 &model, const data::Mat4 &inverse_bind, JointPalette &out) {
  out.position = mm::multiply(model, inverse_bind);
  if (!data::validate_transform(out.position).ok()) return false;
  data::Mat4 inverse;
  if (!mm::inverse(out.position, inverse)) return false;
  for (size_t r = 0; r < 4; ++r)
    for (size_t c = 0; c < 4; ++c) out.normal.m[r * 4 + c] = inverse.m[c * 4 + r];
  return true;
}
} // namespace

PreparedResult prepare(const services::JobEnvelope &envelope, const data::AssetDocument &doc,
                       const Bindings &bindings, const Options &options,
                       const services::JobControl &control) {
  auto fail = [&](E code, const char *message) { return PreparedResult::failure(envelope, code, message); };
  if (cancelled(control)) return fail(E::cancelled, "animation preparation cancelled");
  if (bindings.version.major != 1 || bindings.version.minor != 0)
    return fail(E::unsupported_version, "unsupported animation binding version");
  if (!options.max_joints_per_rig || !options.max_skinned_vertices)
    return fail(E::invalid_input, "animation limits must be positive");
  uint64_t vertices = 0;
  for (const auto &rig : doc.rigs)
    if (rig.joints.size() > options.max_joints_per_rig)
      return fail(E::resource_limit, "animation joint limit exceeded");
  for (const auto &skin : doc.skins) {
    if (skin.weights.size() > options.max_skinned_vertices - vertices)
      return fail(E::resource_limit, "animation skin vertex limit exceeded");
    vertices += skin.weights.size();
  }
  auto validation = options.validation;
  validation.supported_capabilities.push_back({"matter.asset.animation", 1});
  auto checked = data::validate(doc, validation);
  if (!checked.ok()) return PreparedResult::failure(envelope, std::move(checked.diagnostics));
  if (bindings.rigid.size() > validation.limits.max_instances)
    return fail(E::resource_limit, "animation rigid placement limit exceeded");
  std::set<std::string> ids;
  for (const auto &binding : bindings.rigid) {
    const auto *rig = named(doc.rigs, binding.rig);
    if (binding.instance.value.empty() || !ids.insert(binding.instance.value).second ||
        !named(doc.meshes, binding.prototype) || !rig || !named(rig->joints, binding.joint))
      return fail(E::invalid_input, "invalid or duplicate animation placement reference");
    if (!data::validate_transform(binding.offset).ok())
      return fail(E::invalid_input, "invalid animation pivot/offset");
  }
  Prepared out{doc, bindings, {}};
  for (const auto &rig : doc.rigs) {
    std::vector<data::Mat4> models;
    for (const auto &joint : rig.joints) {
      data::Mat4 model;
      if (!mm::inverse(joint.inverse_bind, model) || !data::validate_transform(model).ok())
        return fail(E::invalid_input, "invalid animation inverse bind");
      models.push_back(model);
    }
    out.bind_model.push_back(std::move(models));
    if (cancelled(control)) return fail(E::cancelled, "animation preparation cancelled");
  }
  if (cancelled(control)) return fail(E::cancelled, "animation preparation cancelled");
  return PreparedResult::success(envelope, std::move(out));
}

data::ValidationResult supports(const Prepared &prepared, Consumer consumer) {
  switch (consumer) {
  case Consumer::native_pose: return {};
  case Consumer::browser_pose:
    if (!prepared.bindings.requires_native_motion) return {};
    return error(E::unsupported_capability, "browser animation does not implement native motion graphs/Ozz clips");
  case Consumer::gltf_export:
    return error(E::unsupported_capability, "animation glTF export is not implemented; use the native artifact");
  }
  return error(E::unsupported_capability, "unknown animation consumer");
}

FrameResult graphics_frame(const PreparedResult &prepared, const PoseFrame &pose,
                           uint64_t expected_frame_serial) {
  auto fail = [&](E code, const char *message) { return FrameResult::failure(pose.asset, code, message); };
  if (!prepared.ok()) return FrameResult::failure(pose.asset, prepared.diagnostics());
  if (!(prepared.envelope() == pose.asset) || !expected_frame_serial ||
      pose.frame_serial != expected_frame_serial)
    return fail(E::stale_result, "animation asset revision or presentation frame no longer matches");
  const auto &asset = *prepared.value();
  if (pose.instance.value.empty() || pose.rigs.size() != asset.document.rigs.size() ||
      !data::validate_transform(pose.world).ok() || !data::validate_transform(pose.previous_world).ok())
    return fail(E::invalid_input, "invalid animation frame");
  std::set<std::string> seen;
  GraphicsFrame out{pose.instance, pose.frame_serial, {}, {}};
  for (const auto &p : pose.rigs) {
    const auto *rig = named(asset.document.rigs, p.rig);
    if (!rig || !seen.insert(p.rig.value).second || p.model.size() != rig->joints.size() ||
        p.previous_model.size() != p.model.size())
      return fail(E::invalid_input, "animation pose must cover every joint exactly once");
    SkinPose skin{p.rig, {}, {}};
    for (size_t i = 0; i < p.model.size(); ++i) {
      JointPalette current, previous;
      if (!data::validate_transform(p.model[i]).ok() || !data::validate_transform(p.previous_model[i]).ok() ||
          !palette(p.model[i], rig->joints[i].inverse_bind, current) ||
          !palette(p.previous_model[i], rig->joints[i].inverse_bind, previous))
        return fail(E::invalid_input, "animation pose/palette contains a singular or nonfinite transform");
      skin.current.push_back(current);
      skin.previous.push_back(previous);
    }
    out.skins.push_back(std::move(skin));
  }
  for (const auto &binding : asset.bindings.rigid) {
    const auto *found = named(asset.document.rigs, binding.rig);
    if (!found || !named(found->joints, binding.joint) ||
        !named(asset.document.meshes, binding.prototype) || binding.instance.value.empty() ||
        !data::validate_transform(binding.offset).ok())
      return fail(E::invalid_input, "invalid prepared animation placement");
    const auto &rig = *found;
    const auto joint = static_cast<size_t>(named(rig.joints, binding.joint) - rig.joints.data());
    const auto it = std::find_if(pose.rigs.begin(), pose.rigs.end(), [&](const RigPose &v) { return v.rig == rig.id; });
    RigidPose rigid{binding.instance, binding.prototype,
      mm::multiply(mm::multiply(pose.world, it->model[joint]), binding.offset),
      mm::multiply(mm::multiply(pose.previous_world, it->previous_model[joint]), binding.offset)};
    if (!data::validate_transform(rigid.world).ok() || !data::validate_transform(rigid.previous_world).ok())
      return fail(E::invalid_input, "animation placement overflow or singular transform");
    out.rigid.push_back(std::move(rigid));
  }
  return FrameResult::success(pose.asset, std::move(out));
}
} // namespace matter::animation_asset
