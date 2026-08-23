#pragma once

#include "hydrology/fluid_gameplay_field.h"

#include <filesystem>
#include <functional>
#include <vector>

namespace hydrology {

struct ArtifactProvenance {
    std::uint32_t gpu_vendor = 0;
    std::uint32_t gpu_device = 0;
    std::uint32_t driver_version = 0;
    std::uint64_t physx_sdk_version = 0;
    std::uint64_t adapter_version = 0;
};

struct HydrologyArtifact {
    ProductKeys product_keys{};
    std::uint64_t semantic_key = 0;
    std::uint64_t particle_snapshot_digest = 0;
    float particle_radius_m = 0.0f;
    std::uint64_t payload_digest = 0;
    bool accepted = false;
    FluidBakeStats stats{};
    FillSensorResult sensor{};
    std::vector<FluidParticle> particles;
    gpu_meshing::MeshResult visual_mesh;
    gpu_meshing::MeshResult coarse_cpu_mesh;
    GameplayFieldLayout gameplay_layout{};
    std::vector<GameplaySample> gameplay_field;
    ArtifactProvenance provenance{};
};

using ArtifactBuilder = std::function<bool(HydrologyArtifact& artifact,
                                           gpu_meshing::Error& error)>;

bool serialize_artifact(const HydrologyArtifact& artifact,
                        std::vector<std::uint8_t>& bytes,
                        gpu_meshing::Error& error);
bool deserialize_artifact(const std::vector<std::uint8_t>& bytes,
                          HydrologyArtifact& artifact,
                          gpu_meshing::Error& error);
bool save_artifact_atomic(const std::filesystem::path& path,
                          const HydrologyArtifact& artifact,
                          gpu_meshing::Error& error);
bool load_artifact_validated(const std::filesystem::path& path,
                             std::uint64_t expected_visual_key,
                             HydrologyArtifact& artifact,
                             gpu_meshing::Error& error,
                             std::uint64_t expected_semantic_key = 0);
bool load_or_build_artifact(const std::filesystem::path& path,
                            std::uint64_t expected_visual_key,
                            const ArtifactBuilder& builder,
                            HydrologyArtifact& artifact,
                            gpu_meshing::Error& error,
                            std::uint64_t expected_semantic_key = 0);

}  // namespace hydrology
