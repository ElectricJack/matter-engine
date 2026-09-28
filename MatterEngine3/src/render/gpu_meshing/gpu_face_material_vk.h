#pragma once
#include "face_material_bake.h"
#include <memory>
namespace matter { class VulkanDevice; }
namespace gpu_meshing {
// Persistent preparation service. Call on the Vulkan owner outside frames,
// or use an owned queued request as for solid face projection. Complete output
// only; batch size changes scheduling without changing sample points/identity.
class GpuFaceMaterialBaker {
public:
    explicit GpuFaceMaterialBaker(matter::VulkanDevice &);
    ~GpuFaceMaterialBaker();
    GpuFaceMaterialBaker(const GpuFaceMaterialBaker &)=delete;
    GpuFaceMaterialBaker &operator=(const GpuFaceMaterialBaker &)=delete;
    bool bake(const FaceMaterialJob &,FaceMaterialPatch &,FaceStats &,Error &,
              const BuildControl & = {}, std::uint32_t max_batch_pixels=16384);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gpu_meshing
