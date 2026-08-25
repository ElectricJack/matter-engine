#pragma once

#include "hydrology/river_presentation_field.h"
#include "matter/gpu_visual_meshing.h"

#include <cstdint>
#include <filesystem>
#include <string>
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

std::string hydrology_field_product_relative_path(
    HydrologyFieldProductKind kind,
    std::uint64_t payload_digest);

using HydrologyFieldValidationTestHook = void (*)(
    const std::filesystem::path&, void*) noexcept;
void set_hydrology_field_validation_test_hook(
    HydrologyFieldValidationTestHook hook,
    void* context) noexcept;

enum class HydrologyFieldIoFailurePoint : std::uint8_t {
    None = 0,
    AfterRootHandle = 1,
    AfterDirectoryHandle = 2,
    AfterFileHandle = 3,
    AfterManifestParentHandle = 4,
    AfterManifestFileHandle = 5,
    BeforeManifestRename = 6,
};
void set_hydrology_field_io_failure_for_test(
    HydrologyFieldIoFailurePoint point) noexcept;

using HydrologyNamespaceValidationTestHook = void (*)(
    const std::filesystem::path&, void*) noexcept;
void set_hydrology_namespace_validation_test_hook(
    HydrologyNamespaceValidationTestHook hook,
    void* context) noexcept;

enum class HydrologyManifestPublicationTestStage : std::uint8_t {
    BeforeCommit = 0,
    AfterCommit = 1,
};
using HydrologyManifestPublicationTestHook = void (*)(
    HydrologyManifestPublicationTestStage,
    const std::filesystem::path&,
    void*) noexcept;
void set_hydrology_manifest_publication_test_hook(
    HydrologyManifestPublicationTestHook hook,
    void* context) noexcept;

struct HydrologyFileIdentity {
    std::uint64_t device = 0u;
    std::uint64_t file = 0u;
};
bool hydrology_file_identity_stable(
    HydrologyFileIdentity opened,
    HydrologyFileIdentity named_before,
    HydrologyFileIdentity named_after) noexcept;

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
