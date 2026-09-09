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
        HydrologySectionIdentity section{};
        gpu_meshing::ParticleJob visual_job{};
        float particle_radius_m = 0.0f;
        float coarse_voxel_m = 0.0f;
        ProductIdentitySettings identity{};
        HydrologySemanticInputs semantic{};
        GameplayFieldLayout gameplay_layout{};
        ArtifactProvenance provenance{};
    };

    struct ProductBuildTimings {
        double gpu_mesh_ms = 0.0;
        double cpu_mesh_ms = 0.0;
    };

    using VisualMesher = std::function<bool(
        const gpu_meshing::ParticleJob&, gpu_meshing::MeshResult&,
        gpu_meshing::Stats&, gpu_meshing::Error&, const gpu_meshing::BuildControl&)>;
    using GpuRunner = std::function<bool(
        const char* name, std::function<bool(std::string&)> work,
        std::string& error)>;

    // Resolve the authored validation domain to the finite particle envelope
    // actually consumed by the visual/CPU meshers. Cache verification uses
    // the same function so a warm artifact cannot silently change bounds.
    static gpu_meshing::ParticleJob resolved_visual_job(
        const std::vector<FluidParticle>& particles, float particle_radius_m,
        const gpu_meshing::ParticleJob& authored);

    // Uses the same cell-aligned halo/crop/weld strategy as accepted static
    // water, while retaining dual-phase ordering and weights. This keeps a
    // large animation frame at the authored voxel size instead of rejecting
    // its full-section dense grid or silently lowering resolution.
    static bool build_visual_job_chunks(
        const gpu_meshing::ParticleJob& root_job,
        const VisualMesher& visual_mesher,
        gpu_meshing::MeshResult& merged,
        gpu_meshing::Error& error) noexcept;

    // Called only after run() has accepted the sorted final snapshot.  It is
    // deliberately renderer-agnostic: the caller supplies the existing Vulkan
    // particle mesher seam and no PhysX extraction API is involved.
    static bool build_accepted_artifact(
        const FluidBakeOutput& output, const ProductBuildSettings& settings,
        const TerrainHeightSampler& terrain, const VisualMesher& visual_mesher,
        HydrologyArtifact& artifact, FluidBakeError& error,
        ProductBuildTimings* timings = nullptr) noexcept;

    // Renderer-thread form of the product seam. LocalProvider passes its
    // cfg.gpu_run and cfg.vk_particle_visual_bake here; this is intentionally
    // synchronous so Task 7 can own worker scheduling and cancellation.
    static bool build_accepted_artifact_on_renderer(
        const FluidBakeOutput& output, const ProductBuildSettings& settings,
        const TerrainHeightSampler& terrain, const GpuRunner& gpu_run,
        const VisualMesher& vk_particle_visual_bake,
        HydrologyArtifact& artifact, FluidBakeError& error,
        ProductBuildTimings* timings = nullptr) noexcept;

    // Diagnostic-only terminal-failure path.  It accepts only a finite host
    // snapshot from SensorNotReached or an over-budget escaped-particle
    // quarantine and produces exactly the existing material-4 visual mesh.
    // It has no artifact/gameplay/cache output by construction.
    static bool build_failed_debug_visual(
        const FluidBakeOutput& output, FluidBakeCode failure_code,
        const ProductBuildSettings& settings,
        const VisualMesher& visual_mesher,
        gpu_meshing::MeshResult& visual,
        FluidBakeError& error) noexcept;

    static bool build_failed_debug_visual_on_renderer(
        const FluidBakeOutput& output, FluidBakeCode failure_code,
        const ProductBuildSettings& settings, const GpuRunner& gpu_run,
        const VisualMesher& vk_particle_visual_bake,
        gpu_meshing::MeshResult& visual,
        FluidBakeError& error) noexcept;
};

}  // namespace hydrology
