#pragma once
#include <matter/contracts/generation.h>

namespace matter::services {
// A handle is scoped to one backend instance and one allocation generation.
// Zero is invalid. Backends reject foreign, released and stale handles.
struct GraphicsHandle { uint64_t owner = 0, slot = 0, generation = 0; };
struct UploadRequest {
  AssetOwner asset;
  ResolvedInputs resources;
  std::vector<data::Capability> required_capabilities;
};
inline UploadRequest upload_request(const AssetResult &result) {
  return {asset_owner(result), result.ok() ? result.value()->resources : ResolvedInputs{}, {}};
}
struct UploadTicket { GraphicsHandle handle; };
enum class UploadState { pending, ready };
struct UploadProduct {
  UploadState state = UploadState::pending;
  GraphicsHandle resource; // Valid only in ready state.
};
using UploadSubmission = JobResult<UploadTicket>;
using UploadResult = JobResult<UploadProduct>;
struct SceneInstance {
  data::SemanticId id, prototype;
  GraphicsHandle resource;
  data::Mat4 world_transform{}; // Coordinator composes parent * local.
};
struct SceneUpdate {
  uint64_t scene_revision = 0;
  std::vector<SceneInstance> upsert;
  std::vector<data::SemanticId> remove;
};
struct CameraState {
  data::Mat4 view{}, projection{};
  std::array<double, 3> region_origin_metres{};
};
enum class LightKind { directional, point, spot };
struct LightState {
  data::SemanticId id;
  LightKind kind = LightKind::directional;
  data::Vec3 position{}, direction{0, -1, 0}, color{1, 1, 1};
  float intensity = 1, range_metres = 100, cone_radians = 0.5f;
};
struct GraphicsObservation {
  data::SemanticId instance;
  float projected_error_pixels = 0;
  uint64_t resident_bytes = 0;
  std::optional<data::Capability> representation_demand;
};
struct GraphicsAcknowledgement {};
using GraphicsStatus = JobResult<GraphicsAcknowledgement>;
class GraphicsService {
public:
  virtual ~GraphicsService() = default;
  virtual std::vector<data::Capability> capabilities() const = 0;
  // Thread-confined application lane. Copies/retains owned upload inputs before
  // returning; holds them until terminal completion, including cancellation.
  // Ticket polling is explicit: no callback runs on a driver/device thread.
  // No usable resource is published on failure. Completion echoes submission
  // envelope; coordinator rejects stale completion before scene admission.
  virtual UploadSubmission upload(const JobEnvelope &, UploadRequest,
                                   const JobControl &) = 0;
  virtual UploadResult poll(const JobEnvelope &, UploadTicket) = 0;
  // Atomic scene transaction; deleting an instance retains other instances'
  // shared prototype resource. Stale scene revisions fail without publication.
  virtual GraphicsStatus update_scene(const JobEnvelope &, const SceneUpdate &) = 0;
  virtual GraphicsStatus set_camera(const JobEnvelope &, const CameraState &) = 0;
  virtual GraphicsStatus set_lights(const JobEnvelope &, const std::vector<LightState> &) = 0;
  virtual std::vector<GraphicsObservation> observations() const = 0;
  // Release one caller ownership; actual GPU retirement waits for all scene
  // references and device work. Releasing a ticket cancels future publication
  // and retires its inputs safely. Repeated/foreign release is invalid_input.
  virtual data::ValidationResult release_resource(GraphicsHandle) = 0;
  virtual data::ValidationResult release_ticket(UploadTicket) = 0;
};
} // namespace matter::services
