#pragma once

#include "hydrology/physx_runtime.h"
#include "hydrology/hydrology_artifact.h"

namespace hydrology {

class PhysxFluidBake {
public:
    static bool run(const FluidBakeInput& input,
                    IFluidBakeBackend& backend,
                    const FluidBakeCallbacks& callbacks,
                    FluidBakeOutput& output,
                    FluidBakeError& error) noexcept;

    struct ProductBuildSettings {
        gpu_meshing::ParticleJob visual_job{};
        float particle_radius_m = 0.0f;
        float coarse_voxel_m = 0.0f;
        ProductIdentitySettings identity{};
        HydrologySemanticInputs semantic{};
        GameplayFieldLayout gameplay_layout{};
        ArtifactProvenance provenance{};
    };

    using VisualMesher = std::function<bool(
        const gpu_meshing::ParticleJob&, gpu_meshing::MeshResult&,
        gpu_meshing::Stats&, gpu_meshing::Error&, const gpu_meshing::BuildControl&)>;

    // Called only after run() has accepted the sorted final snapshot.  It is
    // deliberately renderer-agnostic: the caller supplies the existing Vulkan
    // particle mesher seam and no PhysX extraction API is involved.
    static bool build_accepted_artifact(
        const FluidBakeOutput& output, const ProductBuildSettings& settings,
        const TerrainHeightSampler& terrain, const VisualMesher& visual_mesher,
        HydrologyArtifact& artifact, FluidBakeError& error) noexcept;
};

}  // namespace hydrology
