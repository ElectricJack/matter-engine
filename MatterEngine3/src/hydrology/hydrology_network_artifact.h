#pragma once

#include "hydrology/hydrology_artifact.h"
#include "matter/bounds.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hydrology {

enum class HydrologyNetworkState : std::uint8_t {
    Incomplete,
    Failed,
    Ready,
};

enum class HydrologyFieldProductKind : std::uint8_t {
    Runtime = 0,
    Presentation = 1,
};

struct HydrologyFieldProductReference {
    HydrologyFieldProductKind kind = HydrologyFieldProductKind::Runtime;
    std::string relative_path;
    std::uint64_t payload_digest = 0;
};

struct HydrologyArtifactReference {
    std::string id;
    std::string relative_path;
    std::vector<std::string> dependencies;
    std::uint64_t semantic_key = 0;
    std::uint64_t payload_digest = 0;
};

struct HydrologyNetworkArtifact {
    HydrologyNetworkState state = HydrologyNetworkState::Incomplete;
    std::uint64_t network_key = 0;
    std::uint64_t terrain_revision = 0;
    std::uint64_t runtime_field_digest = 0;
    std::uint64_t presentation_field_digest = 0;
    std::vector<HydrologyFieldProductReference> field_products;
    std::vector<HydrologyArtifactReference> sections;
    std::vector<HydrologyArtifactReference> handoffs;
    std::vector<std::string> topological_order;
    matter::Aabb bounds_m{};
    std::uint64_t payload_digest = 0;
};

bool serialize_network_artifact(const HydrologyNetworkArtifact& artifact,
                                std::vector<std::uint8_t>& bytes,
                                gpu_meshing::Error& error);
bool deserialize_network_artifact(const std::vector<std::uint8_t>& bytes,
                                  HydrologyNetworkArtifact& artifact,
                                  gpu_meshing::Error& error);
bool save_network_artifact_atomic(const std::filesystem::path& path,
                                  const HydrologyNetworkArtifact& artifact,
                                  gpu_meshing::Error& error);
bool load_network_artifact_validated(
    const std::filesystem::path& path,
    std::uint64_t expected_network_key,
    std::uint64_t expected_terrain_revision,
    HydrologyNetworkArtifact& artifact,
    gpu_meshing::Error& error);

} // namespace hydrology
