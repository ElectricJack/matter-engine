#pragma once
#include <matter/graphics/backend.h>

namespace matter::graphics {
// The device adapter owns synchronization and native allocations. This boundary
// has no scene, world, source resolver, generator or material implementation.
enum class BufferUsage { vertex, index, texture_payload, uniform, indirect };
class DeviceBuffer {
public:
  virtual ~DeviceBuffer() = default; // Drain every submitted use before freeing.
  virtual data::ValidationResult poll(bool &upload_complete) = 0;
  virtual bool idle() const noexcept = 0;
  virtual uint64_t resident_bytes() const noexcept = 0;
};
class ResourceDevice {
public:
  virtual ~ResourceDevice() = default;
  // Retain/copy the byte owner through the last device read. A buffer can
  // outlive its containing frame/asset if a render submission retains it.
  // Install an output owner BEFORE submitting work, including on failure.
  virtual data::ValidationResult upload(BufferUsage, services::BytesOwner,
                                        std::shared_ptr<DeviceBuffer> &) = 0;
  virtual void drain() noexcept = 0;
};
struct MeshBuffers {
  data::SemanticId prototype;
  data::Bounds bounds;
  data::Winding winding = data::Winding::counterclockwise;
  uint32_t index_count = 0;
  std::shared_ptr<DeviceBuffer> positions, normals, tangents, indices;
  std::vector<std::shared_ptr<DeviceBuffer>> uv_sets;
  std::vector<data::Submesh> submeshes;
};
// Immutable renderer-owned asset. Device buffers and byte owners survive scene
// replacement through a frame snapshot. One mesh allocation per prototype.
struct SceneAsset {
  std::shared_ptr<ResourceDevice> device;
  services::UploadRequest inputs;
  std::vector<services::BytesOwner> bytes;
  std::vector<MeshBuffers> meshes;
  std::vector<std::shared_ptr<DeviceBuffer>> texture_payloads;
  uint64_t resident_bytes() const noexcept;
  bool idle() const noexcept;
};
struct DrawInstance {
  data::SemanticId instance; // Coordinator identity; never a content hash.
  std::vector<data::SemanticId> path; // Assembly child identities, in order.
  std::shared_ptr<const SceneAsset> asset;
  size_t mesh = 0;
  data::Mat4 world_transform{}, normal_transform{};
  bool mirrored = false;
};
struct SceneFrame {
  uint64_t revision = 0;
  services::CameraState camera;
  std::vector<services::LightState> lights;
  std::vector<DrawInstance> visible;
};
struct SceneOptions {
  uint64_t max_resident_bytes = 512ull * 1024 * 1024;
  size_t max_expanded_instances = 100000;
  uint32_t max_assembly_depth = 128;
};
// Concrete scene/visibility/resource backend for the Graphics facade. A render
// pass adapter consumes frame() and native DeviceBuffer implementations; it never
// consults PartStore or live world state. Thread confined like Graphics.
class SceneBackend final : public Backend {
public:
  explicit SceneBackend(std::shared_ptr<ResourceDevice>, SceneOptions = {});
  ~SceneBackend() override;
  std::vector<data::Capability> capabilities() const override;
  data::ValidationResult begin_upload(const services::UploadRequest &, std::unique_ptr<Upload> &) override;
  data::ValidationResult poll_upload(Upload &, UploadCompletion &) override;
  data::ValidationResult update_scene(const BackendScene &) override;
  data::ValidationResult set_camera(const services::CameraState &) override;
  data::ValidationResult set_lights(const std::vector<services::LightState> &) override;
  std::vector<services::GraphicsObservation> observations() const override;
  bool resource_idle(const Resource &) const noexcept override;
  void shutdown() noexcept override;
  // Vulkan zero-to-one clip depth, standard or reversed Z. Region origin is
  // metadata: the coordinator supplies matrices/placements in that same region.
  // Fails until a valid camera is provided; output is unchanged on failure.
  data::ValidationResult frame(SceneFrame &) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace matter::graphics
