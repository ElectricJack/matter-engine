#pragma once
#include "matter/solid_face_projection.h"
#include <memory>
namespace matter {
class VulkanDevice;
}
namespace gpu_meshing {
// Persistent synchronous service, serialized on device-owner thread outside frames.
class GpuSolidFaceProjector {
  public:
    explicit GpuSolidFaceProjector(matter::VulkanDevice &);
    ~GpuSolidFaceProjector();
    GpuSolidFaceProjector(const GpuSolidFaceProjector &) = delete;
    GpuSolidFaceProjector &operator=(const GpuSolidFaceProjector &) = delete;
    bool project(const FaceJob &, FacePatch &, FaceStats &, Error &, const BuildControl & = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gpu_meshing
