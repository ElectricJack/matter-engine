#pragma once
#include <matter/contracts/graphics.h>

namespace matter::graphics {
namespace services = matter::services;
namespace data = matter::contracts;

// Backend-owned buffers, materials, BLAS and representation/render policy.
// No device layout or renderer object belongs in an AssetDocument.
class Resource {
public:
  virtual ~Resource() = default;
};
class Upload {
public:
  // Destruction cancels/drains this upload before returning, including partial
  // submission and failed polling. Inputs remain alive AFTER this destructor.
  virtual ~Upload() = default;
};
struct UploadCompletion {
  services::UploadState state = services::UploadState::pending;
  std::unique_ptr<Resource> resource; // Required for ready; absent for pending.
};
struct BackendInstance {
  services::SceneInstance instance;
  const Resource *resource = nullptr;
};
struct BackendScene {
  uint64_t revision = 0;
  std::vector<BackendInstance> instances;
};

// Thread-confined backend seam. Implementations own TLAS, synchronization,
// culling/detail selection and residency. No generator or world is injected.
class Backend {
public:
  virtual ~Backend() = default;
  virtual std::vector<data::Capability> capabilities() const = 0;
  // Request descriptors are borrowed through this call. Copy descriptors that
  // outlive it; immutable asset/payload storage remains owned by the facade.
  virtual data::ValidationResult begin_upload(const services::UploadRequest &,
                                               std::unique_ptr<Upload> &) = 0;
  virtual data::ValidationResult poll_upload(Upload &, UploadCompletion &) = 0;
  // Output owners must be installed before device submission, even on failure
  // or exception. Resource destructors drain their own work if destroyed before
  // idle (e.g. partial output from a failed upload).
  // Scene/camera/light updates are atomic, including on exception. Copy scene
  // descriptors before returning; resource pointers live through replacement,
  // retirement or shutdown. Backend scene acceleration and policies stay here.
  virtual data::ValidationResult update_scene(const BackendScene &) = 0;
  virtual data::ValidationResult set_camera(const services::CameraState &) = 0;
  virtual data::ValidationResult set_lights(const std::vector<services::LightState> &) = 0;
  virtual std::vector<services::GraphicsObservation> observations() const = 0;
  virtual bool resource_idle(const Resource &) const noexcept = 0;
  // Idempotent: drops scene references and drains ALL device work before return.
  // The facade destroys uploads/resources afterward, before destroying backend.
  virtual void shutdown() noexcept = 0;
};
} // namespace matter::graphics
