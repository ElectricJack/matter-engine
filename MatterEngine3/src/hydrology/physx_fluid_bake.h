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
    using GpuRunner = std::function<bool(
        const char* name, std::function<bool(std::string&)> work,
        std::string& error)>;

    // Called only after run() has accepted the sorted final snapshot.  It is
    // deliberately renderer-agnostic: the caller supplies the existing Vulkan
    // particle mesher seam and no PhysX extraction API is involved.
    static bool build_accepted_artifact(
        const FluidBakeOutput& output, const ProductBuildSettings& settings,
        const TerrainHeightSampler& terrain, const VisualMesher& visual_mesher,
        HydrologyArtifact& artifact, FluidBakeError& error) noexcept;

    // Renderer-thread form of the product seam. LocalProvider passes its
    // cfg.gpu_run and cfg.vk_particle_visual_bake here; this is intentionally
    // synchronous so Task 7 can own worker scheduling and cancellation.
    static bool build_accepted_artifact_on_renderer(
        const FluidBakeOutput& output, const ProductBuildSettings& settings,
        const TerrainHeightSampler& terrain, const GpuRunner& gpu_run,
        const VisualMesher& vk_particle_visual_bake,
        HydrologyArtifact& artifact, FluidBakeError& error) noexcept;
};

}  // namespace hydrology
