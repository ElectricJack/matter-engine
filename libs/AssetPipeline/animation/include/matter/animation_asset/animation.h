#pragma once
#include <matter/contracts/jobs.h>

namespace matter::animation_asset {
namespace data = contracts;

// Owned, versioned animation placement extension. Offsets include any socket
// transform/pivot. Prototype IDs remain separate from per-piece instance IDs.
struct RigidPlacement {
  data::SemanticId instance, prototype, rig, joint;
  data::Mat4 offset{};
};
struct Bindings {
  data::SchemaVersion version;
  std::vector<RigidPlacement> rigid;
  // Native graph/IK/markers/additive/Ozz semantics live in the native artifact.
  // A pose consumer can use this product; exporting it as portable animation
  // would drop those semantics and must fail.
  bool requires_native_motion = false;
};
struct Prepared {
  data::AssetDocument document;
  Bindings bindings;
  // Parallel to document.rigs/joints; parent indices are NOT reordered.
  std::vector<std::vector<data::Mat4>> bind_model;
};
using PreparedResult = services::JobResult<Prepared>;
struct Options {
  data::ValidationOptions validation;
  uint32_t max_joints_per_rig = 256;
  uint64_t max_skinned_vertices = 2000000;
};
// Owns a copy on success, publishes no partial result. Geometry bytes remain
// with the asset consumer: this stage validates descriptors and animation data.
PreparedResult prepare(const services::JobEnvelope &, const data::AssetDocument &,
                       const Bindings & = {}, const Options & = {},
                       const services::JobControl & = {});

enum class Consumer { native_pose, browser_pose, gltf_export };
// Browser support is prepared poses only. Sampling/native motion graphs are
// deliberately not advertised. glTF animation export is not implemented.
data::ValidationResult supports(const Prepared &, Consumer);

struct RigPose {
  data::SemanticId rig;
  std::vector<data::Mat4> model, previous_model;
};
struct PoseFrame {
  services::JobEnvelope asset;
  data::SemanticId instance;
  uint64_t frame_serial = 0;
  data::Mat4 world{}, previous_world{};
  std::vector<RigPose> rigs;
};
struct JointPalette {
  data::Mat4 position{}, normal{}; // Row-major; normal = inverse transpose.
};
struct SkinPose {
  data::SemanticId rig;
  std::vector<JointPalette> current, previous;
};
struct RigidPose {
  data::SemanticId instance, prototype;
  data::Mat4 world{}, previous_world{};
};
struct GraphicsFrame {
  data::SemanticId instance;
  uint64_t frame_serial = 0;
  std::vector<SkinPose> skins;
  std::vector<RigidPose> rigid;
};
using FrameResult = services::JobResult<GraphicsFrame>;
// Caller supplies an evaluated exact-frame snapshot. No evaluator, ECS,
// generation service or renderer resource is retained or invoked here.
FrameResult graphics_frame(const PreparedResult &, const PoseFrame &,
                           uint64_t expected_frame_serial);
} // namespace matter::animation_asset
