#pragma once
#include "matter/solid_sdf_meshing.h"
#include <memory>
namespace matter {
class VulkanDevice;
}
namespace gpu_meshing {
// Persistent synchronous service. Call outside an active frame and serialize
// access on the device owner thread. Output is assigned only on success.
class GpuSolidMesher {
  public:
    explicit GpuSolidMesher(matter::VulkanDevice &);
    ~GpuSolidMesher();
    GpuSolidMesher(const GpuSolidMesher &) = delete;
    GpuSolidMesher &operator=(const GpuSolidMesher &) = delete;
    bool build(const SolidJob &, MeshResult &, SolidStats &, Error &,
               const BuildControl & = {});
    // Same production field dispatch, returning its lattice for conformance.
    bool debug_field(const SolidJob &, std::vector<float> &, GridLayout &, Error &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gpu_meshing
