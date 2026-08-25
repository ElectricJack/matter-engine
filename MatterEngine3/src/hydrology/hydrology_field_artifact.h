#pragma once

#include "hydrology/river_presentation_field.h"
#include "matter/gpu_visual_meshing.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hydrology {

enum class HydrologyFieldProductKind : std::uint8_t {
    Runtime = 0,
    Presentation = 1,
};

struct HydrologyFieldProduct {
    HydrologyFieldProductKind kind = HydrologyFieldProductKind::Runtime;
    GameplayFieldLayout layout{};
    std::vector<GameplaySample> gameplay;
    std::vector<PresentationSample> presentation;
    std::uint64_t payload_digest = 0u;
};

std::uint64_t hydrology_runtime_field_digest(
    const GameplayFieldLayout& layout,
    const std::vector<GameplaySample>& field) noexcept;

std::uint64_t hydrology_presentation_field_digest(
    const GameplayFieldLayout& layout,
    const std::vector<PresentationSample>& field) noexcept;

bool serialize_hydrology_field_product(
    const HydrologyFieldProduct& product,
    std::vector<std::uint8_t>& bytes,
    gpu_meshing::Error& error);

bool deserialize_hydrology_field_product(
    const std::vector<std::uint8_t>& bytes,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error);

bool save_hydrology_field_product_atomic(
    const std::filesystem::path& path,
    const HydrologyFieldProduct& product,
    gpu_meshing::Error& error);

bool load_hydrology_field_product_validated(
    const std::filesystem::path& path,
    HydrologyFieldProductKind expected_kind,
    std::uint64_t expected_payload_digest,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error);

}  // namespace hydrology
