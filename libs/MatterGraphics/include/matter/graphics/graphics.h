#pragma once
#include <matter/graphics/backend.h>

namespace matter::graphics {
struct Options {
  data::ValidationLimits validation_limits;
  uint32_t max_tickets = 4096, max_resources = 4096, max_scene_instances = 100000;
};
// GraphicsService v1. Application-lane calls only; cancellation tokens may be
// signalled on other threads. A null backend creates a disposed service.
class Graphics final : public services::GraphicsService {
public:
  explicit Graphics(std::unique_ptr<Backend>, Options = {});
  ~Graphics() override;
  Graphics(const Graphics &) = delete;
  Graphics &operator=(const Graphics &) = delete;
  std::vector<data::Capability> capabilities() const override;
  services::UploadSubmission upload(const services::JobEnvelope &,
      services::UploadRequest, const services::JobControl &) override;
  services::UploadResult poll(const services::JobEnvelope &, services::UploadTicket) override;
  services::GraphicsStatus update_scene(const services::JobEnvelope &,
                                       const services::SceneUpdate &) override;
  services::GraphicsStatus set_camera(const services::JobEnvelope &,
                                     const services::CameraState &) override;
  services::GraphicsStatus set_lights(const services::JobEnvelope &,
                                    const std::vector<services::LightState> &) override;
  std::vector<services::GraphicsObservation> observations() const override;
  data::ValidationResult release_resource(services::GraphicsHandle) override;
  data::ValidationResult release_ticket(services::UploadTicket) override;
  // Check device retirement without publishing uploads or invoking callbacks.
  void collect_garbage() noexcept;
  // Idempotent synchronous drain; invalidates handles and releases all owners.
  void dispose() noexcept;
  bool disposed() const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace matter::graphics
